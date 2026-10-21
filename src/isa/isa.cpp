#include "isa/isa.hpp"

#include <array>

namespace isa {
namespace {

// Fixed bits of an instruction: opcode in bits 6..0, funct3 in 14..12, funct7 in 31..25.
constexpr uint32_t bits(uint32_t opcode, uint32_t funct3 = 0, uint32_t funct7 = 0) {
    return opcode | funct3 << 12 | funct7 << 25;
}

using enum Op;
using enum Format;

// Indexed by Op. The opcode/funct3/funct7 values come from the RISC-V spec's
// "RV32/64G Instruction Set Listings" chapter.
constexpr std::array<OpInfo, kNumOps> kOps = {{
    {Illegal, "illegal", None, 0},

    {LUI,   "lui",   U, bits(0x37)},
    {AUIPC, "auipc", U, bits(0x17)},

    {JAL,  "jal",  J, bits(0x6f)},
    {JALR, "jalr", I, bits(0x67, 0)},

    {BEQ,  "beq",  B, bits(0x63, 0)},
    {BNE,  "bne",  B, bits(0x63, 1)},
    {BLT,  "blt",  B, bits(0x63, 4)},
    {BGE,  "bge",  B, bits(0x63, 5)},
    {BLTU, "bltu", B, bits(0x63, 6)},
    {BGEU, "bgeu", B, bits(0x63, 7)},

    {LB,  "lb",  I, bits(0x03, 0)},
    {LH,  "lh",  I, bits(0x03, 1)},
    {LW,  "lw",  I, bits(0x03, 2)},
    {LBU, "lbu", I, bits(0x03, 4)},
    {LHU, "lhu", I, bits(0x03, 5)},

    {SB, "sb", S, bits(0x23, 0)},
    {SH, "sh", S, bits(0x23, 1)},
    {SW, "sw", S, bits(0x23, 2)},

    {ADDI,  "addi",  I,     bits(0x13, 0)},
    {SLTI,  "slti",  I,     bits(0x13, 2)},
    {SLTIU, "sltiu", I,     bits(0x13, 3)},
    {XORI,  "xori",  I,     bits(0x13, 4)},
    {ORI,   "ori",   I,     bits(0x13, 6)},
    {ANDI,  "andi",  I,     bits(0x13, 7)},
    {SLLI,  "slli",  Shift, bits(0x13, 1, 0x00)},
    {SRLI,  "srli",  Shift, bits(0x13, 5, 0x00)},
    {SRAI,  "srai",  Shift, bits(0x13, 5, 0x20)},

    {ADD,  "add",  R, bits(0x33, 0, 0x00)},
    {SUB,  "sub",  R, bits(0x33, 0, 0x20)},
    {SLL,  "sll",  R, bits(0x33, 1, 0x00)},
    {SLT,  "slt",  R, bits(0x33, 2, 0x00)},
    {SLTU, "sltu", R, bits(0x33, 3, 0x00)},
    {XOR,  "xor",  R, bits(0x33, 4, 0x00)},
    {SRL,  "srl",  R, bits(0x33, 5, 0x00)},
    {SRA,  "sra",  R, bits(0x33, 5, 0x20)},
    {OR,   "or",   R, bits(0x33, 6, 0x00)},
    {AND,  "and",  R, bits(0x33, 7, 0x00)},

    {MUL,    "mul",    R, bits(0x33, 0, 0x01)},
    {MULH,   "mulh",   R, bits(0x33, 1, 0x01)},
    {MULHSU, "mulhsu", R, bits(0x33, 2, 0x01)},
    {MULHU,  "mulhu",  R, bits(0x33, 3, 0x01)},
    {DIV,    "div",    R, bits(0x33, 4, 0x01)},
    {DIVU,   "divu",   R, bits(0x33, 5, 0x01)},
    {REM,    "rem",    R, bits(0x33, 6, 0x01)},
    {REMU,   "remu",   R, bits(0x33, 7, 0x01)},

    {FENCE,  "fence",  I,      bits(0x0f, 0)},
    {ECALL,  "ecall",  System, 0x00000073},
    {EBREAK, "ebreak", System, 0x00100073},
}};

// Catch a table row that is missing or out of order at compile time.
constexpr bool table_matches_enum() {
    for (int i = 0; i < kNumOps; i++)
        if (kOps[i].op != static_cast<Op>(i)) return false;
    return true;
}
static_assert(table_matches_enum(), "kOps rows must be in the same order as enum Op");

constexpr std::array<std::string_view, 32> kRegNames = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
    "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
    "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
    "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

}  // namespace

const OpInfo& info(Op op) { return kOps[static_cast<int>(op)]; }

std::string_view reg_name(unsigned reg) { return reg < 32 ? kRegNames[reg] : "?"; }

}  // namespace isa
