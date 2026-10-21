#include <random>
#include <string>

#include "isa/isa.hpp"
#include "test.hpp"

using namespace isa;
using namespace isa::reg;
using enum isa::Op;

// Let CHECK_EQ print Ops and Instructions when a check fails.
namespace isa {
std::ostream& operator<<(std::ostream& os, Op op) { return os << info(op).name; }
std::ostream& operator<<(std::ostream& os, const Instruction& in) {
    return os << "{" << in.op << " rd=" << +in.rd << " rs1=" << +in.rs1
              << " rs2=" << +in.rs2 << " imm=" << in.imm << "}";
}
}  // namespace isa

namespace {

// Reference encodings from LLVM's assembler, not from our own code. To check one:
//   echo "addi sp, sp, -16" | llvm-mc -triple=riscv32 -mattr=+m -show-encoding
// The cases include the largest and smallest immediate of each format.
struct Golden {
    uint32_t word;
    Instruction inst;
    const char* text;
};

const Golden kGolden[] = {
    {0x12345537, {LUI, a0, 0, 0, 0x12345000}, "lui a0, 0x12345"},
    {0xffffffb7, {LUI, t6, 0, 0, static_cast<int32_t>(0xfffff000)}, "lui t6, 0xfffff"},
    {0x00001097, {AUIPC, ra, 0, 0, 0x1000}, "auipc ra, 0x1"},
    {0x001000ef, {JAL, ra, 0, 0, 2048}, "jal ra, 2048"},
    {0x8000006f, {JAL, zero, 0, 0, -1048576}, "jal zero, -1048576"},
    {0x7ffff06f, {JAL, zero, 0, 0, 1048574}, "jal zero, 1048574"},
    {0x00008067, {JALR, zero, ra, 0, 0}, "jalr zero, 0(ra)"},
    {0x800302e7, {JALR, t0, t1, 0, -2048}, "jalr t0, -2048(t1)"},
    {0x00b50863, {BEQ, 0, a0, a1, 16}, "beq a0, a1, 16"},
    {0x80941063, {BNE, 0, s0, s1, -4096}, "bne s0, s1, -4096"},
    {0x00b540e3, {BLT, 0, a0, a1, 2048}, "blt a0, a1, 2048"},
    {0x7e62ffe3, {BGEU, 0, t0, t1, 4094}, "bgeu t0, t1, 4094"},
    {0xfff10503, {LB, a0, sp, 0, -1}, "lb a0, -1(sp)"},
    {0x00c12083, {LW, ra, sp, 0, 12}, "lw ra, 12(sp)"},
    {0x7ff75783, {LHU, a5, a4, 0, 2047}, "lhu a5, 2047(a4)"},
    {0x80a40023, {SB, 0, s0, a0, -2048}, "sb a0, -2048(s0)"},
    {0x00112623, {SW, 0, sp, ra, 12}, "sw ra, 12(sp)"},
    {0xff010113, {ADDI, sp, sp, 0, -16}, "addi sp, sp, -16"},
    {0x7ff00513, {ADDI, a0, zero, 0, 2047}, "addi a0, zero, 2047"},
    {0xfff33293, {SLTIU, t0, t1, 0, -1}, "sltiu t0, t1, -1"},
    {0x00151413, {SLLI, s0, a0, 0, 1}, "slli s0, a0, 1"},
    {0x01f55513, {SRLI, a0, a0, 0, 31}, "srli a0, a0, 31"},
    {0x41f55513, {SRAI, a0, a0, 0, 31}, "srai a0, a0, 31"},
    {0x40c58533, {SUB, a0, a1, a2, 0}, "sub a0, a1, a2"},
    {0x407352b3, {SRA, t0, t1, t2, 0}, "sra t0, t1, t2"},
    {0x02c586b3, {MUL, a3, a1, a2, 0}, "mul a3, a1, a2"},
    {0x02c5a533, {MULHSU, a0, a1, a2, 0}, "mulhsu a0, a1, a2"},
    {0x03eefe33, {REMU, t3, t4, t5, 0}, "remu t3, t4, t5"},
    {0x0ff0000f, {FENCE, 0, 0, 0, 0xff}, "fence"},
    {0x00000073, {ECALL}, "ecall"},
    {0x00100073, {EBREAK}, "ebreak"},
};

// A random instruction with every field in range, for round-trip testing.
Instruction random_instruction(Op op, std::mt19937& rng) {
    auto reg = [&] { return static_cast<uint8_t>(rng() % 32); };
    auto in_range = [&](int32_t lo, int32_t hi) {
        return std::uniform_int_distribution<int32_t>(lo, hi)(rng);
    };
    switch (info(op).format) {
    case Format::R:      return {op, reg(), reg(), reg(), 0};
    case Format::I:      return {op, reg(), reg(), 0, in_range(-2048, 2047)};
    case Format::Shift:  return {op, reg(), reg(), 0, in_range(0, 31)};
    case Format::S:      return {op, 0, reg(), reg(), in_range(-2048, 2047)};
    case Format::B:      return {op, 0, reg(), reg(), in_range(-2048, 2047) * 2};
    case Format::U:      return {op, reg(), 0, 0, static_cast<int32_t>(rng() & 0xfffff000)};
    case Format::J:      return {op, reg(), 0, 0, in_range(-524288, 524287) * 2};
    case Format::System: return {op};
    case Format::None:   break;
    }
    return {};
}

}  // namespace

TEST(decode_matches_llvm) {
    for (const Golden& g : kGolden) CHECK_EQ(decode(g.word), g.inst);
}

TEST(encode_matches_llvm) {
    for (const Golden& g : kGolden) CHECK_EQ(encode(g.inst), g.word);
}

TEST(disassemble_matches_llvm_syntax) {
    for (const Golden& g : kGolden) CHECK_EQ(disassemble(g.inst), std::string(g.text));
}

TEST(round_trip_every_op) {
    std::mt19937 rng(12345);  // fixed seed: the same "random" cases on every run
    for (int i = 1; i < kNumOps; i++) {
        const Op op = static_cast<Op>(i);
        for (int n = 0; n < 1000; n++) {
            const Instruction in = random_instruction(op, rng);
            CHECK_EQ(decode(encode(in)), in);
        }
    }
}

TEST(unsupported_encodings_are_illegal) {
    const uint32_t words[] = {
        0x00000000,  // all zeros: illegal by definition in RISC-V
        0xffffffff,  // all ones
        0x00004501,  // c.li a0, 0 (compressed)
        0x30529073,  // csrw mtvec, t0 (CSR access)
        0x30200073,  // mret
        0x10500073,  // wfi
        0x00100173,  // ebreak with a nonzero rd field
        0x0000100f,  // fence.i
        0x00b6252f,  // amoadd.w a0, a1, (a2) (atomics)
        0x00052507,  // flw fa0, 0(a0) (floating point)
        0x00053503,  // ld a0, 0(a0) (RV64 only)
        0x00a53023,  // sd a0, 0(a0) (RV64 only)
        0x0015051b,  // addiw a0, a0, 1 (RV64 only)
        0x02051513,  // slli a0, a0, 32 (shift amount > 31: RV64 only)
        0x00b52063,  // branch with funct3 = 2 (unassigned)
        0x00001067,  // jalr with funct3 = 1 (unassigned)
        0x40b51533,  // sll with the "alternate" bit set (unassigned)
    };
    for (uint32_t w : words) {
        CHECK_EQ(decode(w), Instruction{});
        CHECK_EQ(disassemble(decode(w)), std::string("illegal"));
    }
}

TEST(fence_variants_are_accepted) {
    // fence.tso is a FENCE with extra bits set. The spec says to treat unknown
    // FENCE variants as a plain FENCE, so it must not be illegal.
    CHECK_EQ(decode(0x8330000f).op, FENCE);
}

TEST(encode_rejects_bad_fields) {
    CHECK_THROWS(encode({ADDI, a0, a0, 0, 2048}));       // immediate too large
    CHECK_THROWS(encode({ADDI, a0, a0, 0, -2049}));      // immediate too small
    CHECK_THROWS(encode({SW, 0, sp, ra, 2048}));         // store offset too large
    CHECK_THROWS(encode({BEQ, 0, a0, a1, 3}));           // odd branch offset
    CHECK_THROWS(encode({BEQ, 0, a0, a1, 4096}));        // branch offset too large
    CHECK_THROWS(encode({JAL, ra, 0, 0, 1 << 20}));      // jump offset too large
    CHECK_THROWS(encode({LUI, a0, 0, 0, 0x123}));        // low 12 bits not zero
    CHECK_THROWS(encode({SLLI, a0, a0, 0, 32}));         // shift amount too large
    CHECK_THROWS(encode({ADD, 32, a0, a1, 0}));          // there is no register 32
    CHECK_THROWS(encode({ADD, a0, a1, a2, 5}));          // R-type has no immediate
    CHECK_THROWS(encode({SW, a0, sp, ra, 0}));           // S-type has no rd
    CHECK_THROWS(encode({ECALL, a0}));                   // ecall has no operands
    CHECK_THROWS(encode({Illegal}));
}

TEST(register_names) {
    CHECK_EQ(reg_name(zero), std::string_view("zero"));
    CHECK_EQ(reg_name(sp), std::string_view("sp"));
    CHECK_EQ(reg_name(a0), std::string_view("a0"));
    CHECK_EQ(reg_name(s11), std::string_view("s11"));
    CHECK_EQ(reg_name(t6), std::string_view("t6"));
    CHECK_EQ(reg_name(32), std::string_view("?"));
}
