// Decoding works straight from the RISC-V spec's tables and does not use the
// kOps table in isa.cpp. Having two independent sources lets the tests check
// one against the other (decode(encode(x)) == x).
#include "isa/isa.hpp"

namespace isa {
namespace {

// Bits hi..lo of w, shifted down to bit 0.
constexpr uint32_t field(uint32_t w, int hi, int lo) {
    return (w >> lo) & ((1u << (hi - lo + 1)) - 1);
}

// Sign-extend the low `width` bits of v.
constexpr int32_t sign_extend(uint32_t v, int width) {
    const int shift = 32 - width;
    return static_cast<int32_t>(v << shift) >> shift;
}

// Immediates are scattered across the word differently in each format.
// The bit positions below are from the spec's "Immediate Encoding Variants" figure.
int32_t imm_i(uint32_t w) { return sign_extend(field(w, 31, 20), 12); }
int32_t imm_s(uint32_t w) { return sign_extend(field(w, 31, 25) << 5 | field(w, 11, 7), 12); }
int32_t imm_u(uint32_t w) { return static_cast<int32_t>(w & 0xfffff000); }

int32_t imm_b(uint32_t w) {
    return sign_extend(field(w, 31, 31) << 12 | field(w, 7, 7) << 11 |
                       field(w, 30, 25) << 5 | field(w, 11, 8) << 1, 13);
}

int32_t imm_j(uint32_t w) {
    return sign_extend(field(w, 31, 31) << 20 | field(w, 19, 12) << 12 |
                       field(w, 20, 20) << 11 | field(w, 30, 21) << 1, 21);
}

using enum Op;

// May return an Instruction whose op is Illegal but whose fields are not 0;
// decode() cleans that up.
Instruction decode_fields(uint32_t w) {
    const uint8_t rd = field(w, 11, 7);
    const uint8_t rs1 = field(w, 19, 15);
    const uint8_t rs2 = field(w, 24, 20);
    const uint32_t funct3 = field(w, 14, 12);
    const uint32_t funct7 = field(w, 31, 25);

    auto r_type = [&](Op op) { return Instruction{op, rd, rs1, rs2, 0}; };
    auto i_type = [&](Op op) { return Instruction{op, rd, rs1, 0, imm_i(w)}; };
    auto shift  = [&](Op op) { return Instruction{op, rd, rs1, 0, static_cast<int32_t>(rs2)}; };
    auto s_type = [&](Op op) { return Instruction{op, 0, rs1, rs2, imm_s(w)}; };
    auto b_type = [&](Op op) { return Instruction{op, 0, rs1, rs2, imm_b(w)}; };
    auto u_type = [&](Op op) { return Instruction{op, rd, 0, 0, imm_u(w)}; };
    auto j_type = [&](Op op) { return Instruction{op, rd, 0, 0, imm_j(w)}; };

    switch (field(w, 6, 0)) {  // opcode
    case 0x37: return u_type(LUI);
    case 0x17: return u_type(AUIPC);
    case 0x6f: return j_type(JAL);
    case 0x67: return i_type(funct3 == 0 ? JALR : Illegal);

    case 0x63: {
        static constexpr Op ops[8] = {BEQ, BNE, Illegal, Illegal, BLT, BGE, BLTU, BGEU};
        return b_type(ops[funct3]);
    }
    case 0x03: {
        static constexpr Op ops[8] = {LB, LH, LW, Illegal, LBU, LHU, Illegal, Illegal};
        return i_type(ops[funct3]);
    }
    case 0x23: {
        static constexpr Op ops[8] = {SB, SH, SW, Illegal, Illegal, Illegal, Illegal, Illegal};
        return s_type(ops[funct3]);
    }

    case 0x13:
        // For shifts the top 7 bits must be exactly 0x00 or 0x20. Any other
        // value includes bit 25, which would be a 6th shift-amount bit: legal
        // on RV64, illegal on RV32.
        if (funct3 == 1) return shift(funct7 == 0x00 ? SLLI : Illegal);
        if (funct3 == 5) return shift(funct7 == 0x00 ? SRLI : funct7 == 0x20 ? SRAI : Illegal);
        {
            static constexpr Op ops[8] = {ADDI, Illegal, SLTI, SLTIU, XORI, Illegal, ORI, ANDI};
            return i_type(ops[funct3]);
        }

    case 0x33:
        switch (funct7) {
        case 0x00: {
            static constexpr Op ops[8] = {ADD, SLL, SLT, SLTU, XOR, SRL, OR, AND};
            return r_type(ops[funct3]);
        }
        case 0x20: return r_type(funct3 == 0 ? SUB : funct3 == 5 ? SRA : Illegal);
        case 0x01: {
            static constexpr Op ops[8] = {MUL, MULH, MULHSU, MULHU, DIV, DIVU, REM, REMU};
            return r_type(ops[funct3]);
        }
        default: return {};
        }

    // funct3 = 1 here is FENCE.I, which we don't support. The spec says plain
    // FENCE must ignore its rd and rs1 fields, so any values are accepted.
    case 0x0f: return i_type(funct3 == 0 ? FENCE : Illegal);

    // Everything else under this opcode (CSR access, mret, wfi, ...) is illegal.
    case 0x73:
        if (w == 0x00000073) return {ECALL};
        if (w == 0x00100073) return {EBREAK};
        return {};

    default: return {};
    }
}

}  // namespace

Instruction decode(uint32_t word) {
    Instruction inst = decode_fields(word);
    if (inst.op == Illegal) return {};
    return inst;
}

}  // namespace isa
