#include "cpu_helpers.hpp"
#include "test.hpp"

using namespace isa::reg;

TEST(cpu_registers_start_zero_and_x0_ignores_writes) {
    mem::Ram ram;
    cpu::Cpu c(ram);
    CHECK_EQ(c.pc, 0);
    CHECK_EQ(c.reg(a0), 0);
    c.set_reg(a0, 42);
    CHECK_EQ(c.reg(a0), 42);
    c.set_reg(zero, 42);
    CHECK_EQ(c.reg(zero), 0);
}

TEST(cpu_step_valid_instruction_advances_pc) {
    mem::Ram ram;
    ram.write(0, 4, isa::encode({isa::Op::ADDI, a0, zero, 0, 5}));
    cpu::Cpu c(ram);
    CHECK(!c.step().has_value());
    CHECK_EQ(c.pc, 4);
}

TEST(cpu_step_illegal_instruction_traps) {
    mem::Ram ram;
    ram.write(8, 4, 0xffffffff);   // all ones: not a valid instruction
    cpu::Cpu c(ram);
    c.pc = 8;
    const auto trap = c.step();
    CHECK(trap.has_value());
    CHECK_EQ(trap->cause, cpu::Cause::illegal_instruction);
    CHECK_EQ(trap->pc, 8);
    CHECK_EQ(trap->tval, 0xffffffff);   // tval = the instruction word
    CHECK_EQ(c.pc, 8);                  // a trapping instruction changes nothing
}

TEST(cpu_step_zero_word_is_illegal) {
    mem::Ram ram;   // all zeros
    cpu::Cpu c(ram);
    const auto trap = c.step();
    CHECK(trap.has_value());
    CHECK_EQ(trap->cause, cpu::Cause::illegal_instruction);
    CHECK_EQ(trap->tval, 0);
    CHECK_EQ(c.pc, 0);
}

TEST(cpu_step_misaligned_pc_traps) {
    mem::Ram ram;
    cpu::Cpu c(ram);
    c.pc = 2;
    const auto trap = c.step();
    CHECK(trap.has_value());
    CHECK_EQ(trap->cause, cpu::Cause::instruction_address_misaligned);
    CHECK_EQ(trap->pc, 2);
    CHECK_EQ(trap->tval, 2);
    CHECK_EQ(c.pc, 2);
}

TEST(cpu_step_pc_outside_ram_traps) {
    mem::Ram ram;
    cpu::Cpu c(ram);
    for (uint32_t pc : {static_cast<uint32_t>(mem::Ram::kRamSize), 0xfffffffcu}) {
        c.pc = pc;
        const auto trap = c.step();
        CHECK(trap.has_value());
        CHECK_EQ(trap->cause, cpu::Cause::instruction_page_fault);
        CHECK_EQ(trap->pc, pc);
        CHECK_EQ(trap->tval, pc);
        CHECK_EQ(c.pc, pc);
    }
}

TEST(cpu_step_runs_consecutive_instructions) {
    mem::Ram ram;
    const uint32_t nop = isa::encode({isa::Op::ADDI, zero, zero, 0, 0});
    for (uint32_t addr = 0; addr < 12; addr += 4) ram.write(addr, 4, nop);
    cpu::Cpu c(ram);
    for (int i = 0; i < 3; i++) CHECK(!c.step().has_value());
    CHECK_EQ(c.pc, 12);
    const auto trap = c.step();   // address 12 holds zeros: illegal
    CHECK(trap.has_value());
    CHECK_EQ(trap->cause, cpu::Cause::illegal_instruction);
    CHECK_EQ(c.pc, 12);
}
