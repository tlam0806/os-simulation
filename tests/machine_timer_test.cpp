// Timer interrupt (spec A4): the clock is Machine::steps(); when it reaches the
// deadline, run() returns a timer interrupt before running the next instruction.
#include <cstdint>
#include <optional>

#include "cpu_helpers.hpp"   // prints Cause in failure messages
#include "elf_builder.hpp"
#include "machine/machine.hpp"
#include "test.hpp"

using namespace elf_builder;
using namespace isa::reg;
using enum isa::Op;

namespace {

constexpr uint32_t kEntry = 0x10000;

// a0 counts up forever: "loop: addi a0, a0, 1; jal zero, loop".
// After an even number of instructions, pc is back at kEntry.
std::vector<uint8_t> counter_program() {
    return program_elf({{ADDI, a0, a0, 0, 1}, {JAL, zero, 0, 0, -4}});
}

bool is_timer(const std::optional<cpu::Trap>& t) {
    return t.has_value() && t->cause == cpu::Cause::timer_interrupt;
}

}  // namespace

TEST(timer_is_off_by_default) {
    machine::Machine m;
    CHECK(!m.load(counter_program()));
    CHECK(!m.run(1000).has_value());   // only the step limit stops it
    CHECK_EQ(m.steps(), 1000);
}

TEST(timer_fires_at_deadline) {
    machine::Machine m;
    CHECK(!m.load(counter_program()));
    m.set_timer(10);
    const auto t = m.run(1000);
    CHECK(is_timer(t));
    CHECK_EQ(m.steps(), 10);              // exactly 10 instructions ran
    CHECK_EQ(m.cpu().reg(a0), 5);         // 5 of them were the addi
    CHECK_EQ(t->pc, kEntry);              // pc = the instruction that did NOT run yet
    CHECK_EQ(m.cpu().pc, kEntry);
    CHECK_EQ(t->tval, 0);
}

TEST(timer_interrupt_does_not_advance_clock) {
    // Fires again right away if the deadline isn't moved, and no instruction runs.
    machine::Machine m;
    CHECK(!m.load(counter_program()));
    m.set_timer(10);
    CHECK(is_timer(m.run(1000)));
    CHECK(is_timer(m.run(1000)));
    CHECK(is_timer(m.run(1000)));
    CHECK_EQ(m.steps(), 10);
    CHECK_EQ(m.cpu().reg(a0), 5);
}

TEST(timer_rearmed_fires_again_later) {
    // This is what a kernel does on each tick: deadline = clock + quantum.
    machine::Machine m;
    CHECK(!m.load(counter_program()));
    m.set_timer(10);
    CHECK(is_timer(m.run(1000)));
    m.set_timer(m.steps() + 6);
    CHECK(is_timer(m.run(1000)));
    CHECK_EQ(m.steps(), 16);
    CHECK_EQ(m.cpu().reg(a0), 8);
}

TEST(timer_deadline_already_passed_fires_immediately) {
    machine::Machine m;
    CHECK(!m.load(counter_program()));
    m.set_timer(0);
    CHECK(is_timer(m.run(1000)));
    CHECK_EQ(m.steps(), 0);
    CHECK_EQ(m.cpu().reg(a0), 0);
}

TEST(timer_cleared_stops_firing) {
    machine::Machine m;
    CHECK(!m.load(counter_program()));
    m.set_timer(10);
    CHECK(is_timer(m.run(1000)));
    m.clear_timer();
    CHECK(!m.run(100).has_value());
    CHECK_EQ(m.steps(), 110);
}

TEST(timer_is_checked_before_the_instruction) {
    // The 3rd instruction is ecall. With deadline 2, the timer fires first and
    // the ecall has not run yet; after clearing the timer, the ecall happens.
    machine::Machine m;
    CHECK(!m.load(program_elf({{ADDI, a0, zero, 0, 1}, {ADDI, a0, a0, 0, 1}, {ECALL}})));
    m.set_timer(2);
    const auto t = m.run(1000);
    CHECK(is_timer(t));
    CHECK_EQ(t->pc, kEntry + 8);
    m.clear_timer();
    const auto t2 = m.run(1000);
    CHECK(t2.has_value());
    CHECK_EQ(t2->cause, cpu::Cause::ecall);
    CHECK_EQ(t2->pc, kEntry + 8);
    CHECK_EQ(m.steps(), 3);
}

TEST(timer_and_step_limit) {
    // The step limit counts instructions run in this call; a pending interrupt
    // is reported even if the limit would allow more.
    machine::Machine m;
    CHECK(!m.load(counter_program()));
    m.set_timer(50);
    CHECK(!m.run(20).has_value());   // limit reached before the deadline
    CHECK_EQ(m.steps(), 20);
    CHECK(is_timer(m.run(1000)));    // continues and fires at 50
    CHECK_EQ(m.steps(), 50);
}

TEST(timer_run_zero_runs_nothing) {
    machine::Machine m;
    CHECK(!m.load(counter_program()));
    m.set_timer(0);
    CHECK(!m.run(0).has_value());    // no instruction may run, so no check happens either
    CHECK_EQ(m.steps(), 0);
}
