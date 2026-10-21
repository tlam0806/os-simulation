// Register-register instructions: ADD SUB SLL SLT SLTU XOR SRL SRA OR AND,
// and the M extension: MUL MULH MULHSU MULHU DIV DIVU REM REMU.
// The M-extension expected values were computed independently in Python.
#include "cpu_helpers.hpp"
#include "test.hpp"

using namespace isa::reg;
using enum isa::Op;

namespace {

// Runs one instruction "op a0, a1, a2" with a1 = x and a2 = y; returns a0.
// Also checks that it didn't trap and that pc moved to the next instruction.
uint32_t run(isa::Op op, uint32_t x, uint32_t y) {
    Machine m;
    m.load(0, {{op, a0, a1, a2, 0}});
    m.cpu.set_reg(a1, x);
    m.cpu.set_reg(a2, y);
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.cpu.pc, 4);
    return m.cpu.reg(a0);
}

}  // namespace

TEST(add) {
    CHECK_EQ(run(ADD, 5, 3), 8);
    CHECK_EQ(run(ADD, 0x7fffffff, 1), 0x80000000);   // overflow wraps, no trap
    CHECK_EQ(run(ADD, 0xffffffff, 1), 0);
}

TEST(sub) {
    CHECK_EQ(run(SUB, 5, 3), 2);
    CHECK_EQ(run(SUB, 3, 5), 0xfffffffe);
    CHECK_EQ(run(SUB, 0x80000000, 1), 0x7fffffff);
}

TEST(sll_uses_low_5_bits_of_rs2) {
    CHECK_EQ(run(SLL, 1, 4), 16);
    CHECK_EQ(run(SLL, 1, 31), 0x80000000);
    CHECK_EQ(run(SLL, 1, 33), 2);              // 33 & 31 = 1
    CHECK_EQ(run(SLL, 1, 0xffffffe0), 1);      // low 5 bits are 0
}

TEST(slt_compares_signed) {
    CHECK_EQ(run(SLT, 0xffffffff, 0), 1);      // -1 < 0
    CHECK_EQ(run(SLT, 0, 0xffffffff), 0);
    CHECK_EQ(run(SLT, 5, 5), 0);
}

TEST(sltu_compares_unsigned) {
    CHECK_EQ(run(SLTU, 0, 0xffffffff), 1);
    CHECK_EQ(run(SLTU, 0xffffffff, 0), 0);
    CHECK_EQ(run(SLTU, 5, 5), 0);
}

TEST(xor_or_and) {
    CHECK_EQ(run(XOR, 0xff00ff00, 0x0ff00ff0), 0xf0f0f0f0);
    CHECK_EQ(run(OR, 0xf0f0f0f0, 0x0f0f0f0f), 0xffffffff);
    CHECK_EQ(run(AND, 0xff00ff00, 0x0ff00ff0), 0x0f000f00);
}

TEST(srl_fills_with_zeros) {
    CHECK_EQ(run(SRL, 0x80000000, 31), 1);
    CHECK_EQ(run(SRL, 0x80000000, 36), 0x08000000);   // 36 & 31 = 4
}

TEST(sra_fills_with_sign_bit) {
    CHECK_EQ(run(SRA, 0x80000000, 4), 0xf8000000);
    CHECK_EQ(run(SRA, 0x80000000, 36), 0xf8000000);   // 36 & 31 = 4
    CHECK_EQ(run(SRA, 0x7fffffff, 30), 1);
}

TEST(mul_low_32_bits) {
    CHECK_EQ(run(MUL, 6, 7), 42);
    CHECK_EQ(run(MUL, 0xffffffff, 0xffffffff), 1);    // -1 * -1
    CHECK_EQ(run(MUL, 0x10000, 0x10000), 0);          // 2^32: only high bits set
}

TEST(mulh_signed_times_signed) {
    CHECK_EQ(run(MULH, 0xffffffff, 0xffffffff), 0);   // -1 * -1 = 1
    CHECK_EQ(run(MULH, 0x80000000, 0x80000000), 0x40000000);
    CHECK_EQ(run(MULH, 0xffffffff, 1), 0xffffffff);   // -1: high half is all ones
    CHECK_EQ(run(MULH, 0x7fffffff, 0x7fffffff), 0x3fffffff);
    CHECK_EQ(run(MULH, 2, 0x80000000), 0xffffffff);   // 2 * -2^31
}

TEST(mulhsu_signed_times_unsigned) {
    CHECK_EQ(run(MULHSU, 0xffffffff, 0xffffffff), 0xffffffff);   // -1 * (2^32 - 1)
    CHECK_EQ(run(MULHSU, 2, 0x80000000), 1);                       // 2 * 2^31 (rs2 unsigned)
    CHECK_EQ(run(MULHSU, 0x80000000, 0xffffffff), 0x80000000);
}

TEST(mulhu_unsigned_times_unsigned) {
    CHECK_EQ(run(MULHU, 0xffffffff, 0xffffffff), 0xfffffffe);
    CHECK_EQ(run(MULHU, 0x80000000, 2), 1);
    CHECK_EQ(run(MULHU, 0xffffffff, 1), 0);
}

TEST(div_rounds_toward_zero) {
    CHECK_EQ(run(DIV, 20, 6), 3);
    CHECK_EQ(run(DIV, static_cast<uint32_t>(-20), 6), static_cast<uint32_t>(-3));
    CHECK_EQ(run(DIV, 20, static_cast<uint32_t>(-6)), static_cast<uint32_t>(-3));
}

TEST(div_special_cases) {
    CHECK_EQ(run(DIV, 20, 0), 0xffffffff);                   // by zero: all ones, no trap
    CHECK_EQ(run(DIV, 0x80000000, 0xffffffff), 0x80000000);  // -2^31 / -1 overflows
}

TEST(divu) {
    CHECK_EQ(run(DIVU, 20, 6), 3);
    CHECK_EQ(run(DIVU, 0xffffffff, 2), 0x7fffffff);
    CHECK_EQ(run(DIVU, 20, 0), 0xffffffff);                  // by zero: all ones
}

TEST(rem_takes_sign_of_dividend) {
    CHECK_EQ(run(REM, 20, 6), 2);
    CHECK_EQ(run(REM, static_cast<uint32_t>(-20), 6), static_cast<uint32_t>(-2));
    CHECK_EQ(run(REM, 20, static_cast<uint32_t>(-6)), 2);
}

TEST(rem_special_cases) {
    CHECK_EQ(run(REM, 20, 0), 20);                           // by zero: the dividend
    CHECK_EQ(run(REM, 0x80000000, 0xffffffff), 0);           // overflow case
}

TEST(remu) {
    CHECK_EQ(run(REMU, 20, 6), 2);
    CHECK_EQ(run(REMU, 0xffffffff, 10), 5);
    CHECK_EQ(run(REMU, 20, 0), 20);                          // by zero: the dividend
}

TEST(alu_reg_same_register_everywhere) {
    Machine m;
    m.load(0, {{ADD, a0, a0, a0, 0}});
    m.cpu.set_reg(a0, 21);
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.cpu.reg(a0), 42);
}

TEST(alu_reg_write_to_x0_is_ignored) {
    Machine m;
    m.load(0, {{MUL, zero, a1, a2, 0}});
    m.cpu.set_reg(a1, 6);
    m.cpu.set_reg(a2, 7);
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.cpu.reg(zero), 0);
}
