#include "cpu/cpu.hpp"

#include <cstdint>

namespace cpu {
namespace {

int32_t as_signed(uint32_t v) { return static_cast<int32_t>(v); }

// High 32 bits of a 64-bit product (MULH, MULHSU, MULHU).
uint32_t high_bits(int64_t product) { return static_cast<uint32_t>(static_cast<uint64_t>(product) >> 32); }
uint32_t high_bits(uint64_t product) { return static_cast<uint32_t>(product >> 32); }

// Division never traps in RISC-V. These cases are undefined behavior in C++,
// so they are handled before dividing (spec A1, "Rules that are easy to get wrong").
uint32_t div_signed(uint32_t a, uint32_t b) {
    if (b == 0) return 0xffffffff;
    if (a == 0x80000000 && b == 0xffffffff) return a;   // -2^31 / -1 overflows
    return static_cast<uint32_t>(as_signed(a) / as_signed(b));
}

uint32_t rem_signed(uint32_t a, uint32_t b) {
    if (b == 0) return a;
    if (a == 0x80000000 && b == 0xffffffff) return 0;
    return static_cast<uint32_t>(as_signed(a) % as_signed(b));
}

uint32_t div_unsigned(uint32_t a, uint32_t b) { return b == 0 ? 0xffffffff : a / b; }
uint32_t rem_unsigned(uint32_t a, uint32_t b) { return b == 0 ? a : a % b; }

}  // namespace

// Runs one instruction. Every check that can trap happens before any register,
// memory or pc changes, so a trapping instruction has no effect (spec A3).
std::optional<Trap> Cpu::step() noexcept {
    auto fetch_result = fetch();
    if (std::holds_alternative<Trap>(fetch_result)) {
        return std::get<Trap>(fetch_result);
    }
    uint32_t word = std::get<uint32_t>(fetch_result);
    isa::Instruction decoded = isa::decode(word);

    using enum isa::Op;
    const uint8_t rd = decoded.rd;
    const uint32_t rs1 = reg(decoded.rs1);         // read both sources first: rd may be
    const uint32_t rs2 = reg(decoded.rs2);         // the same register as rs1 or rs2
    const int32_t imm = decoded.imm;               // already sign-extended by decode
    const uint32_t uimm = static_cast<uint32_t>(imm);
    const uint32_t shamt = rs2 & 31;               // register shifts use the low 5 bits
    uint32_t next_pc = pc + 4;

    auto trap = [&](Cause cause, uint32_t tval) { return Trap{cause, pc, tval}; };

    // Taken branch: pc + imm. A misaligned target traps on the branch itself.
    auto branch = [&](bool taken) -> std::optional<Trap> {
        if (!taken) return std::nullopt;
        const uint32_t target = pc + uimm;
        if (target % 4 != 0) return trap(Cause::instruction_address_misaligned, target);
        next_pc = target;
        return std::nullopt;
    };

    // Loads and stores: the address must be aligned to the access size (spec A1).
    const uint32_t addr = rs1 + uimm;

    switch (decoded.op) {
    case Illegal:
        return trap(Cause::illegal_instruction, word);

    // Register-immediate
    case ADDI:  set_reg(rd, rs1 + uimm); break;    // unsigned add wraps, as RISC-V requires
    case SLTI:  set_reg(rd, as_signed(rs1) < imm); break;
    case SLTIU: set_reg(rd, rs1 < uimm); break;
    case XORI:  set_reg(rd, rs1 ^ uimm); break;
    case ORI:   set_reg(rd, rs1 | uimm); break;
    case ANDI:  set_reg(rd, rs1 & uimm); break;
    case SLLI:  set_reg(rd, rs1 << imm); break;    // imm is the shift amount, 0..31
    case SRLI:  set_reg(rd, rs1 >> imm); break;    // unsigned shift fills with zeros
    case SRAI:  set_reg(rd, static_cast<uint32_t>(as_signed(rs1) >> imm)); break;

    // Register-register
    case ADD:   set_reg(rd, rs1 + rs2); break;
    case SUB:   set_reg(rd, rs1 - rs2); break;
    case SLL:   set_reg(rd, rs1 << shamt); break;
    case SLT:   set_reg(rd, as_signed(rs1) < as_signed(rs2)); break;
    case SLTU:  set_reg(rd, rs1 < rs2); break;
    case XOR:   set_reg(rd, rs1 ^ rs2); break;
    case SRL:   set_reg(rd, rs1 >> shamt); break;
    case SRA:   set_reg(rd, static_cast<uint32_t>(as_signed(rs1) >> shamt)); break;
    case OR:    set_reg(rd, rs1 | rs2); break;
    case AND:   set_reg(rd, rs1 & rs2); break;

    // Multiply / divide
    case MUL:    set_reg(rd, rs1 * rs2); break;
    case MULH:   set_reg(rd, high_bits(int64_t{as_signed(rs1)} * int64_t{as_signed(rs2)})); break;
    case MULHSU: set_reg(rd, high_bits(int64_t{as_signed(rs1)} * int64_t{rs2})); break;
    case MULHU:  set_reg(rd, high_bits(uint64_t{rs1} * uint64_t{rs2})); break;
    case DIV:    set_reg(rd, div_signed(rs1, rs2)); break;
    case DIVU:   set_reg(rd, div_unsigned(rs1, rs2)); break;
    case REM:    set_reg(rd, rem_signed(rs1, rs2)); break;
    case REMU:   set_reg(rd, rem_unsigned(rs1, rs2)); break;

    // Upper immediate (imm already holds the value shifted into the top 20 bits)
    case LUI:   set_reg(rd, uimm); break;
    case AUIPC: set_reg(rd, pc + uimm); break;

    // Jumps: check the target before writing the return address.
    case JAL: {
        const uint32_t target = pc + uimm;
        if (target % 4 != 0) return trap(Cause::instruction_address_misaligned, target);
        set_reg(rd, pc + 4);
        next_pc = target;
        break;
    }
    case JALR: {
        const uint32_t target = (rs1 + uimm) & ~1u;
        if (target % 4 != 0) return trap(Cause::instruction_address_misaligned, target);
        set_reg(rd, pc + 4);
        next_pc = target;
        break;
    }

    // Branches
    case BEQ:  if (auto t = branch(rs1 == rs2)) return t; break;
    case BNE:  if (auto t = branch(rs1 != rs2)) return t; break;
    case BLT:  if (auto t = branch(as_signed(rs1) < as_signed(rs2))) return t; break;
    case BGE:  if (auto t = branch(as_signed(rs1) >= as_signed(rs2))) return t; break;
    case BLTU: if (auto t = branch(rs1 < rs2)) return t; break;
    case BGEU: if (auto t = branch(rs1 >= rs2)) return t; break;

    // Loads: LB/LH sign-extend the value, LBU/LHU zero-extend it.
    case LB: case LH: case LW: case LBU: case LHU: {
        const uint8_t size = (decoded.op == LB || decoded.op == LBU) ? 1
                           : (decoded.op == LH || decoded.op == LHU) ? 2 : 4;
        if (addr % size != 0) return trap(Cause::load_address_misaligned, addr);
        const auto paddr = to_physical(addr, mmu::Access::load);
        const auto value = paddr ? ram.read(*paddr, size) : std::nullopt;
        if (!value) return trap(Cause::load_page_fault, addr);   // tval stays virtual
        uint32_t v = *value;
        if (decoded.op == LB) v = static_cast<uint32_t>(int32_t{static_cast<int8_t>(v)});
        if (decoded.op == LH) v = static_cast<uint32_t>(int32_t{static_cast<int16_t>(v)});
        set_reg(rd, v);
        break;
    }

    // Stores: Ram::write changes nothing when it fails.
    case SB: case SH: case SW: {
        const uint8_t size = decoded.op == SB ? 1 : decoded.op == SH ? 2 : 4;
        if (addr % size != 0) return trap(Cause::store_address_misaligned, addr);
        const auto paddr = to_physical(addr, mmu::Access::store);
        if (!paddr || !ram.write(*paddr, size, rs2)) return trap(Cause::store_page_fault, addr);
        break;
    }

    // System
    case FENCE:  break;   // one core, no caches: nothing to order (spec A1)
    case ECALL:  return trap(Cause::ecall, 0);
    case EBREAK: return trap(Cause::breakpoint, 0);
    }

    pc = next_pc;
    return std::nullopt;
}

std::variant<uint32_t, Trap> Cpu::fetch() noexcept {
    if (pc % 4) {
        return Trap{
            Cause::instruction_address_misaligned,
            pc,
            pc
        };
    }
    const std::optional<uint32_t> paddr = to_physical(pc, mmu::Access::fetch);
    std::optional<uint32_t> result = paddr ? ram.read(*paddr, 4) : std::nullopt;
    if (result) {
        return result.value();
    } else {
        return Trap{
            Cause::instruction_page_fault,
            pc,
            pc
        };
    }
}

void Cpu::set_page_table(const mmu::PageTable* ptr) noexcept {
    page_table = ptr;
}

std::optional<uint32_t> Cpu::to_physical(uint32_t vaddr, mmu::Access access) const noexcept {
    if (page_table == nullptr) {
        return vaddr;
    }
    return translate(*page_table, vaddr, access);
}

}  // namespace cpu
