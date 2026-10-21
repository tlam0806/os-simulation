// The RV32IM instruction set: the list of instructions, their decoded form,
// and conversion between 32-bit machine words and that form.
// See docs/machine-spec.md, section A1.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace isa {

// Every instruction the CPU understands, plus Illegal for everything else.
enum class Op : uint8_t {
    Illegal,
    LUI, AUIPC,
    JAL, JALR,
    BEQ, BNE, BLT, BGE, BLTU, BGEU,
    LB, LH, LW, LBU, LHU,
    SB, SH, SW,
    ADDI, SLTI, SLTIU, XORI, ORI, ANDI, SLLI, SRLI, SRAI,
    ADD, SUB, SLL, SLT, SLTU, XOR, SRL, SRA, OR, AND,
    MUL, MULH, MULHSU, MULHU, DIV, DIVU, REM, REMU,
    FENCE, ECALL, EBREAK,
};
inline constexpr int kNumOps = static_cast<int>(Op::EBREAK) + 1;

// How an instruction's operands are laid out in its 32 bits.
// R, I, S, B, U, J are the standard RISC-V formats. Shift is I-format with a
// 5-bit shift amount instead of an immediate. System has no operands.
enum class Format : uint8_t { None, R, I, Shift, S, B, U, J, System };

// A decoded instruction. Fields that the format doesn't use are always 0.
//
// imm is the value the instruction actually uses, already sign-extended:
//   I, S   immediate or address offset, -2048..2047
//   Shift  shift amount, 0..31
//   B, J   byte offset from this instruction's pc (always even)
//   U      the full 32-bit value, low 12 bits zero (lui a0, 0x12345 -> 0x12345000)
struct Instruction {
    Op op = Op::Illegal;
    uint8_t rd = 0;
    uint8_t rs1 = 0;
    uint8_t rs2 = 0;
    int32_t imm = 0;

    bool operator==(const Instruction&) const = default;
};

struct OpInfo {
    Op op;
    std::string_view name;   // assembly mnemonic, e.g. "addi"
    Format format;
    uint32_t match;          // the fixed bits that identify it: opcode, funct3, funct7
};

const OpInfo& info(Op op);

// Never fails: unknown encodings decode to Op::Illegal with all fields 0.
Instruction decode(uint32_t word);

// Throws std::invalid_argument if a field is out of range, if a field the
// format doesn't use is nonzero, or for Op::Illegal.
uint32_t encode(const Instruction& inst);

// Assembly text, e.g. "addi sp, sp, -16" or "sw ra, 12(sp)".
// Branch and jump targets are shown as offsets from pc.
std::string disassemble(const Instruction& inst);

// ABI register name: 0 -> "zero", 2 -> "sp", 10 -> "a0".
std::string_view reg_name(unsigned reg);

// Register numbers by ABI name, for writing instructions by hand.
namespace reg {
enum : uint8_t {
    zero, ra, sp, gp, tp, t0, t1, t2, s0, s1,
    a0, a1, a2, a3, a4, a5, a6, a7,
    s2, s3, s4, s5, s6, s7, s8, s9, s10, s11,
    t3, t4, t5, t6,
};
}  // namespace reg

}  // namespace isa
