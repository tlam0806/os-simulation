// Register-immediate ALU instructions: ADDI SLTI SLTIU XORI ORI ANDI SLLI SRLI SRAI.
// Expected values follow the RISC-V spec, chapter "Integer Computational Instructions".
#include "cpu/cpu.hpp"
#include "isa/isa.hpp"
#include "test.hpp"

using namespace isa::reg;
using enum isa::Op;

namespace {

// Runs one instruction "op a0, a1, imm" with a1 = rs1_value; returns a0.
// Also checks that it didn't trap and that pc moved to the next instruction.
uint32_t run(isa::Op op, uint32_t rs1_value, int32_t imm) {
    mem::Ram ram;
    cpu::Cpu c(ram);
    ram.write(0, 4, isa::encode({op, a0, a1, 0, imm}));
    c.set_reg(a1, rs1_value);
    const auto trap = c.step();
    CHECK(!trap.has_value());
    CHECK_EQ(c.pc, 4);
    return c.reg(a0);
}

}  // namespace

TEST(addi) {
    CHECK_EQ(run(ADDI, 5, 3), 8);
    CHECK_EQ(run(ADDI, 5, -3), 2);
    CHECK_EQ(run(ADDI, 0, -1), 0xffffffff);
    CHECK_EQ(run(ADDI, 0, -2048), 0xfffff800);          // smallest immediate, sign-extended
    CHECK_EQ(run(ADDI, 0, 2047), 0x7ff);                // largest immediate
    CHECK_EQ(run(ADDI, 0x7fffffff, 1), 0x80000000);     // overflow wraps, no trap
    CHECK_EQ(run(ADDI, 0xffffffff, 1), 0);
}

TEST(slti_compares_signed) {
    CHECK_EQ(run(SLTI, 5, 10), 1);
    CHECK_EQ(run(SLTI, 10, 5), 0);
    CHECK_EQ(run(SLTI, 5, 5), 0);
    CHECK_EQ(run(SLTI, 0xffffffff, 0), 1);              // -1 < 0
    CHECK_EQ(run(SLTI, 0, -1), 0);                      // 0 < -1 is false
    CHECK_EQ(run(SLTI, 0x80000000, -2048), 1);          // most negative number
}

TEST(sltiu_compares_unsigned) {
    CHECK_EQ(run(SLTIU, 5, 10), 1);
    CHECK_EQ(run(SLTIU, 0xffffffff, 1), 0);             // 0xffffffff is huge, not -1
    CHECK_EQ(run(SLTIU, 5, -1), 1);                     // imm -1 sign-extends to 0xffffffff
    CHECK_EQ(run(SLTIU, 0, 1), 1);                      // "sltiu rd, rs, 1" means rd = (rs == 0)
    CHECK_EQ(run(SLTIU, 7, 1), 0);
}

TEST(xori) {
    CHECK_EQ(run(XORI, 0xff00ff00, 0x0f0), 0xff00fff0);
    CHECK_EQ(run(XORI, 0x12345678, -1), 0xedcba987);    // xor with -1 = bitwise NOT
}

TEST(ori) {
    CHECK_EQ(run(ORI, 0x12340000, 0x5a), 0x1234005a);
    CHECK_EQ(run(ORI, 0, -2048), 0xfffff800);           // immediate is sign-extended first
}

TEST(andi) {
    CHECK_EQ(run(ANDI, 0x12345678, 0xff), 0x78);
    CHECK_EQ(run(ANDI, 0x12345678, -16), 0x12345670);   // -16 = 0xfffffff0
}

TEST(slli) {
    CHECK_EQ(run(SLLI, 1, 0), 1);
    CHECK_EQ(run(SLLI, 1, 31), 0x80000000);
    CHECK_EQ(run(SLLI, 0xffffffff, 4), 0xfffffff0);     // bits shifted out are lost
}

TEST(srli_fills_with_zeros) {
    CHECK_EQ(run(SRLI, 0x80000000, 4), 0x08000000);
    CHECK_EQ(run(SRLI, 0x80000000, 31), 1);
    CHECK_EQ(run(SRLI, 0x12345678, 0), 0x12345678);
}

TEST(srai_fills_with_sign_bit) {
    CHECK_EQ(run(SRAI, 0x80000000, 4), 0xf8000000);
    CHECK_EQ(run(SRAI, 0x80000000, 31), 0xffffffff);
    CHECK_EQ(run(SRAI, 0x40000000, 4), 0x04000000);     // positive: same as SRLI
}

TEST(alu_imm_write_to_x0_is_ignored) {
    mem::Ram ram;
    cpu::Cpu c(ram);
    ram.write(0, 4, isa::encode({ADDI, zero, a1, 0, 5}));
    c.set_reg(a1, 10);
    CHECK(!c.step().has_value());
    CHECK_EQ(c.reg(zero), 0);
    CHECK_EQ(c.pc, 4);
}

TEST(alu_imm_rd_same_as_rs1) {
    mem::Ram ram;
    cpu::Cpu c(ram);
    ram.write(0, 4, isa::encode({ADDI, a0, a0, 0, 1}));
    c.set_reg(a0, 41);
    CHECK(!c.step().has_value());
    CHECK_EQ(c.reg(a0), 42);
}

TEST(alu_imm_changes_only_rd) {
    mem::Ram ram;
    cpu::Cpu c(ram);
    ram.write(0, 4, isa::encode({ADDI, a0, a1, 0, 1}));
    c.set_reg(a1, 7);
    c.set_reg(a2, 99);
    CHECK(!c.step().has_value());
    CHECK_EQ(c.reg(a0), 8);
    CHECK_EQ(c.reg(a1), 7);
    CHECK_EQ(c.reg(a2), 99);
}

TEST(alu_imm_program) {
    // a0 = ((0 + 1) + 2) << 3 = 24
    mem::Ram ram;
    ram.write(0, 4, isa::encode({ADDI, a0, zero, 0, 1}));
    ram.write(4, 4, isa::encode({ADDI, a0, a0, 0, 2}));
    ram.write(8, 4, isa::encode({SLLI, a0, a0, 0, 3}));
    cpu::Cpu c(ram);
    for (int i = 0; i < 3; i++) CHECK(!c.step().has_value());
    CHECK_EQ(c.reg(a0), 24);
    CHECK_EQ(c.pc, 12);
}
