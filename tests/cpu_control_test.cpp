// Instructions that use or change pc: LUI AUIPC JAL JALR and the branches,
// plus the system instructions FENCE ECALL EBREAK.
#include "cpu_helpers.hpp"
#include "test.hpp"

using namespace isa::reg;
using enum isa::Op;

namespace {

constexpr uint32_t kPc = 0x100;   // run at a nonzero pc so pc-relative math is visible

// Runs a branch "op a1, a2, offset" at kPc; returns the pc afterwards.
uint32_t branch(isa::Op op, uint32_t x, uint32_t y, int32_t offset = 16) {
    Machine m;
    m.load(kPc, {{op, 0, a1, a2, offset}});
    m.cpu.set_reg(a1, x);
    m.cpu.set_reg(a2, y);
    CHECK(!m.cpu.step().has_value());
    return m.cpu.pc;
}

constexpr uint32_t kTaken = kPc + 16;
constexpr uint32_t kNotTaken = kPc + 4;
constexpr uint32_t kMinusOne = 0xffffffff;

}  // namespace

TEST(lui) {
    Machine m;
    m.load(kPc, {{LUI, a0, 0, 0, 0x12345000}, {LUI, a1, 0, 0, static_cast<int32_t>(0xfffff000)}});
    CHECK(!m.cpu.step().has_value());
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.cpu.reg(a0), 0x12345000);
    CHECK_EQ(m.cpu.reg(a1), 0xfffff000);
    CHECK_EQ(m.cpu.pc, kPc + 8);
}

TEST(auipc_adds_to_its_own_pc) {
    Machine m;
    m.load(kPc, {{AUIPC, a0, 0, 0, 0x1000}, {AUIPC, a1, 0, 0, -4096}});
    CHECK(!m.cpu.step().has_value());
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.cpu.reg(a0), kPc + 0x1000);
    CHECK_EQ(m.cpu.reg(a1), kPc + 4 - 0x1000);   // second one runs at kPc + 4; wraps below 0
    CHECK_EQ(m.cpu.pc, kPc + 8);
}

TEST(jal_links_and_jumps) {
    Machine m;
    m.load(kPc, {{JAL, ra, 0, 0, 16}});
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.cpu.reg(ra), kPc + 4);   // return address = the next instruction
    CHECK_EQ(m.cpu.pc, kPc + 16);
}

TEST(jal_backwards_without_link) {
    Machine m;
    m.load(kPc, {{JAL, zero, 0, 0, -8}});
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.cpu.pc, kPc - 8);
    CHECK_EQ(m.cpu.reg(zero), 0);
}

TEST(jal_misaligned_target_traps_without_side_effects) {
    Machine m;
    m.load(kPc, {{JAL, ra, 0, 0, 2}});
    const auto trap = m.cpu.step();
    CHECK(trap.has_value());
    CHECK_EQ(trap->cause, cpu::Cause::instruction_address_misaligned);
    CHECK_EQ(trap->pc, kPc);
    CHECK_EQ(trap->tval, kPc + 2);   // tval = the bad target
    CHECK_EQ(m.cpu.pc, kPc);         // trap is reported on the jump itself
    CHECK_EQ(m.cpu.reg(ra), 0);      // and the link register is not written
}

TEST(jalr_jumps_to_register_plus_offset) {
    Machine m;
    m.load(kPc, {{JALR, ra, a1, 0, 8}});
    m.cpu.set_reg(a1, 0x200);
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.cpu.pc, 0x208);
    CHECK_EQ(m.cpu.reg(ra), kPc + 4);
}

TEST(jalr_negative_offset) {
    Machine m;
    m.load(kPc, {{JALR, zero, a1, 0, -16}});
    m.cpu.set_reg(a1, 0x210);
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.cpu.pc, 0x200);
}

TEST(jalr_clears_bit_0_of_target) {
    Machine m;
    m.load(kPc, {{JALR, zero, a1, 0, 0}});
    m.cpu.set_reg(a1, 0x201);
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.cpu.pc, 0x200);
}

TEST(jalr_rd_same_as_rs1_uses_old_value) {
    // The target must come from a1 *before* a1 is overwritten with the return address.
    Machine m;
    m.load(kPc, {{JALR, a1, a1, 0, 0}});
    m.cpu.set_reg(a1, 0x200);
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.cpu.pc, 0x200);
    CHECK_EQ(m.cpu.reg(a1), kPc + 4);
}

TEST(jalr_misaligned_target_traps_without_side_effects) {
    Machine m;
    m.load(kPc, {{JALR, ra, a1, 0, 0}});
    m.cpu.set_reg(a1, 0x202);
    const auto trap = m.cpu.step();
    CHECK(trap.has_value());
    CHECK_EQ(trap->cause, cpu::Cause::instruction_address_misaligned);
    CHECK_EQ(trap->tval, 0x202);
    CHECK_EQ(m.cpu.pc, kPc);
    CHECK_EQ(m.cpu.reg(ra), 0);
}

TEST(beq_bne) {
    CHECK_EQ(branch(BEQ, 5, 5), kTaken);
    CHECK_EQ(branch(BEQ, 5, 6), kNotTaken);
    CHECK_EQ(branch(BNE, 5, 6), kTaken);
    CHECK_EQ(branch(BNE, 5, 5), kNotTaken);
}

TEST(blt_bge_compare_signed) {
    CHECK_EQ(branch(BLT, kMinusOne, 0), kTaken);      // -1 < 0
    CHECK_EQ(branch(BLT, 0, kMinusOne), kNotTaken);
    CHECK_EQ(branch(BLT, 5, 5), kNotTaken);
    CHECK_EQ(branch(BGE, 0, kMinusOne), kTaken);
    CHECK_EQ(branch(BGE, 5, 5), kTaken);              // greater *or equal*
    CHECK_EQ(branch(BGE, kMinusOne, 0), kNotTaken);
}

TEST(bltu_bgeu_compare_unsigned) {
    CHECK_EQ(branch(BLTU, 0, kMinusOne), kTaken);     // 0 < 0xffffffff
    CHECK_EQ(branch(BLTU, kMinusOne, 0), kNotTaken);
    CHECK_EQ(branch(BGEU, kMinusOne, 0), kTaken);
    CHECK_EQ(branch(BGEU, 5, 5), kTaken);
    CHECK_EQ(branch(BGEU, 0, 1), kNotTaken);
}

TEST(branch_backwards) {
    CHECK_EQ(branch(BEQ, 1, 1, -16), kPc - 16);
}

TEST(branch_misaligned_target_traps_only_when_taken) {
    Machine m;
    m.load(kPc, {{BEQ, 0, a1, a2, 6}});   // target kPc + 6 is not a multiple of 4
    const auto trap = m.cpu.step();       // a1 == a2 == 0: taken
    CHECK(trap.has_value());
    CHECK_EQ(trap->cause, cpu::Cause::instruction_address_misaligned);
    CHECK_EQ(trap->tval, kPc + 6);
    CHECK_EQ(m.cpu.pc, kPc);

    CHECK_EQ(branch(BNE, 1, 1, 6), kNotTaken);   // not taken: no trap
}

TEST(loop_program_sums_1_to_5) {
    Machine m;
    m.load(0, {
        {ADDI, a0, zero, 0, 5},     // 0x00  a0 = 5
        {ADDI, a1, zero, 0, 0},     // 0x04  a1 = 0
        {ADD, a1, a1, a0, 0},       // 0x08  loop: a1 += a0
        {ADDI, a0, a0, 0, -1},      // 0x0c        a0 -= 1
        {BNE, 0, a0, zero, -8},     // 0x10        if a0 != 0 goto loop
        {ECALL},                    // 0x14
    });
    std::optional<cpu::Trap> trap;
    for (int i = 0; i < 100 && !trap; i++) trap = m.cpu.step();
    CHECK(trap.has_value());
    CHECK_EQ(trap->cause, cpu::Cause::ecall);
    CHECK_EQ(trap->pc, 0x14);
    CHECK_EQ(m.cpu.reg(a1), 15);
}

TEST(fence_is_a_no_op) {
    Machine m;
    m.load(0, {{FENCE, 0, 0, 0, 0xff}});
    m.cpu.set_reg(a0, 7);
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.cpu.pc, 4);
    CHECK_EQ(m.cpu.reg(a0), 7);
}

TEST(ecall_traps_at_its_own_pc) {
    // The kernel resumes at pc + 4 after handling it (spec A3).
    Machine m;
    m.load(kPc, {{ECALL}});
    m.cpu.set_reg(a7, 93);
    const auto trap = m.cpu.step();
    CHECK(trap.has_value());
    CHECK_EQ(trap->cause, cpu::Cause::ecall);
    CHECK_EQ(trap->pc, kPc);
    CHECK_EQ(trap->tval, 0);
    CHECK_EQ(m.cpu.pc, kPc);
    CHECK_EQ(m.cpu.reg(a7), 93);
}

TEST(ebreak_traps) {
    Machine m;
    m.load(kPc, {{EBREAK}});
    const auto trap = m.cpu.step();
    CHECK(trap.has_value());
    CHECK_EQ(trap->cause, cpu::Cause::breakpoint);
    CHECK_EQ(trap->pc, kPc);
    CHECK_EQ(trap->tval, 0);
    CHECK_EQ(m.cpu.pc, kPc);
}
