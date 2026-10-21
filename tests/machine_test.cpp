// Machine: load an ELF program, then run it until a trap or a step limit.
// Uses two read-only getters on Machine:
//   const cpu::Cpu& cpu() const     the CPU, to check pc and registers
//   uint64_t steps() const          instructions attempted so far (spec A4)
#include <cstdint>
#include <vector>

#include "cpu_helpers.hpp"   // prints Cause in failure messages
#include "elf_builder.hpp"
#include "machine/machine.hpp"
#include "test.hpp"

using namespace elf_builder;
using namespace isa::reg;
using enum isa::Op;

namespace {

constexpr uint32_t kEntry = 0x10000;            // program_elf() puts code here
const isa::Instruction kLoopForever = {JAL, zero, 0, 0, 0};   // jumps to itself

}  // namespace

// ---- load ----

TEST(machine_load_sets_pc_sp_and_resets_steps) {
    machine::Machine m;
    CHECK(!m.load(make_elf(kEntry + 8, {{kEntry, code({kLoopForever, kLoopForever, kLoopForever})}})));
    CHECK_EQ(m.cpu().pc, kEntry + 8);                    // pc = ELF entry point
    CHECK_EQ(m.cpu().reg(sp), mem::Ram::kRamSize);       // sp = top of RAM (milestone notes)
    CHECK_EQ(m.cpu().reg(a0), 0);                        // other registers start at 0
    CHECK_EQ(m.steps(), 0);
}

TEST(machine_load_reports_errors) {
    machine::Machine m;
    const auto error = m.load(std::vector<uint8_t>{1, 2, 3});
    CHECK(error.has_value());
    if (error) CHECK(!error->empty());
}

// ---- run ----

TEST(machine_runs_until_ecall) {
    machine::Machine m;
    CHECK(!m.load(program_elf({
        {ADDI, a0, zero, 0, 42},
        {ECALL},
    })));
    const auto trap = m.run(1000);
    CHECK(trap.has_value());
    CHECK_EQ(trap->cause, cpu::Cause::ecall);
    CHECK_EQ(trap->pc, kEntry + 4);
    CHECK_EQ(m.cpu().reg(a0), 42);
    CHECK_EQ(m.steps(), 2);   // the trapping ecall counts as a step (spec A4)
}

TEST(machine_reports_other_traps) {
    // Jump past the code into zero-filled memory: the all-zero word is illegal.
    machine::Machine m;
    CHECK(!m.load(program_elf({{JAL, zero, 0, 0, 16}})));
    const auto trap = m.run(1000);
    CHECK(trap.has_value());
    CHECK_EQ(trap->cause, cpu::Cause::illegal_instruction);
    CHECK_EQ(trap->pc, kEntry + 16);
}

TEST(machine_stops_at_step_limit) {
    machine::Machine m;
    CHECK(!m.load(program_elf({kLoopForever})));
    CHECK(!m.run(100).has_value());   // no trap: the limit was reached
    CHECK_EQ(m.steps(), 100);         // exactly 100 instructions, not 101
}

TEST(machine_step_limit_is_per_call) {
    // run(n) runs up to n *more* instructions, so a stopped program can continue.
    machine::Machine m;
    CHECK(!m.load(program_elf({kLoopForever})));
    CHECK(!m.run(10).has_value());
    CHECK(!m.run(10).has_value());
    CHECK_EQ(m.steps(), 20);
}

TEST(machine_run_zero_does_nothing) {
    machine::Machine m;
    CHECK(!m.load(program_elf({{ADDI, a0, zero, 0, 1}, {ECALL}})));
    CHECK(!m.run(0).has_value());
    CHECK_EQ(m.steps(), 0);
    CHECK_EQ(m.cpu().pc, kEntry);
    CHECK_EQ(m.cpu().reg(a0), 0);
}

TEST(machine_can_resume_after_step_limit) {
    machine::Machine m;
    CHECK(!m.load(program_elf({
        {ADDI, a0, zero, 0, 1},
        {ADDI, a0, a0, 0, 1},
        {ADDI, a0, a0, 0, 1},
        {ECALL},
    })));
    CHECK(!m.run(2).has_value());
    CHECK_EQ(m.cpu().reg(a0), 2);
    const auto trap = m.run(10);
    CHECK(trap.has_value());
    CHECK_EQ(trap->cause, cpu::Cause::ecall);
    CHECK_EQ(m.cpu().reg(a0), 3);
    CHECK_EQ(m.steps(), 4);
}

TEST(machine_stack_is_usable) {
    // Push a value just below sp and read it back: sp must point at real RAM.
    machine::Machine m;
    CHECK(!m.load(program_elf({
        {ADDI, a1, zero, 0, 123},
        {SW, 0, sp, a1, -4},
        {LW, a0, sp, 0, -4},
        {ECALL},
    })));
    const auto trap = m.run(1000);
    CHECK(trap.has_value());
    CHECK_EQ(trap->cause, cpu::Cause::ecall);
    CHECK_EQ(m.cpu().reg(a0), 123);
}

TEST(machine_load_again_resets_steps) {
    machine::Machine m;
    CHECK(!m.load(program_elf({kLoopForever})));
    m.run(5);
    CHECK(!m.load(program_elf({kLoopForever})));
    CHECK_EQ(m.steps(), 0);
}
