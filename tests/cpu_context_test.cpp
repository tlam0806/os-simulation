// Saving and restoring the CPU context (registers + pc) for context switches.
#include <cstdint>

#include "cpu_helpers.hpp"
#include "test.hpp"

using namespace isa::reg;
using enum isa::Op;

// Let CHECK_EQ print a Context when a check fails.
namespace cpu {
std::ostream& operator<<(std::ostream& os, const Context& c) {
    os << "{pc=0x" << std::hex << c.pc;
    for (int i = 1; i < 32; i++)
        if (c.regs[i]) os << " x" << std::dec << i << "=0x" << std::hex << c.regs[i];
    return os << std::dec << "}";
}
}  // namespace cpu
namespace {

cpu::Context make_context(uint32_t pc, uint32_t base) {
    cpu::Context c;
    c.pc = pc;
    for (int i = 1; i < 32; i++) c.regs[i] = base + i;
    return c;
}

}  // namespace

TEST(context_of_new_cpu_is_all_zero) {
    Machine m;
    CHECK_EQ(m.cpu.context(), cpu::Context{});
}

TEST(context_reflects_registers_and_pc) {
    Machine m;
    m.cpu.pc = 0x1234;
    m.cpu.set_reg(a0, 7);
    m.cpu.set_reg(t6, 0xffffffff);
    const cpu::Context c = m.cpu.context();
    CHECK_EQ(c.pc, 0x1234);
    CHECK_EQ(c.regs[a0], 7);
    CHECK_EQ(c.regs[t6], 0xffffffff);
    CHECK_EQ(c.regs[zero], 0);
    CHECK_EQ(c.regs[a1], 0);
}

TEST(set_context_sets_every_register_and_pc) {
    Machine m;
    const cpu::Context c = make_context(0x4000, 1000);
    m.cpu.set_context(c);
    CHECK_EQ(m.cpu.pc, 0x4000);
    for (uint8_t i = 1; i < 32; i++) CHECK_EQ(m.cpu.reg(i), 1000 + i);
    CHECK_EQ(m.cpu.context(), c);
}

// A context with regs[0] != 0 is a kernel bug: set_context asserts on it.
// That can't be tested here, because a failed assert aborts the whole test run.

TEST(set_context_replaces_everything) {
    // Registers the new context has as 0 must become 0, not keep old values.
    Machine m;
    m.cpu.set_context(make_context(0x100, 500));
    m.cpu.set_context(cpu::Context{});
    CHECK_EQ(m.cpu.context(), cpu::Context{});
}

TEST(context_is_a_copy) {
    // Changing the CPU afterwards must not change a saved context.
    Machine m;
    m.cpu.set_reg(a0, 1);
    const cpu::Context saved = m.cpu.context();
    m.cpu.set_reg(a0, 2);
    m.cpu.pc = 0x500;
    CHECK_EQ(saved.regs[a0], 1);
    CHECK_EQ(saved.pc, 0);
}

TEST(context_switch_between_two_programs) {
    // Two "processes" run the same counting loop, each with its own a0.
    // Switching back and forth must not mix up their counts.
    Machine m;
    m.load(0, {{ADDI, a0, a0, 0, 1}, {JAL, zero, 0, 0, -4}});   // loop: a0++
    cpu::Context a = m.cpu.context();      // a0 = 0, pc = 0
    cpu::Context b = a;
    b.regs[a0] = 100;

    auto run = [&](cpu::Context& ctx, int steps) {
        m.cpu.set_context(ctx);
        for (int i = 0; i < steps; i++) CHECK(!m.cpu.step().has_value());
        ctx = m.cpu.context();
    };
    run(a, 3);   // addi, jal, addi
    run(b, 5);   // addi, jal, addi, jal, addi
    run(a, 3);   // jal, addi, jal
    CHECK_EQ(a.regs[a0], 3);
    CHECK_EQ(b.regs[a0], 103);
    CHECK_EQ(a.pc, 0);   // 6 steps: back at the addi
    CHECK_EQ(b.pc, 4);   // 5 steps: at the jal
}

TEST(restoring_a_context_replays_the_same_way) {
    // Determinism: run, restore the saved context, run again: same result.
    Machine m;
    m.load(0, {{ADDI, a0, a0, 0, 3}, {SLLI, a0, a0, 0, 2}, {ADD, a1, a0, a0, 0}});
    const cpu::Context start = m.cpu.context();
    for (int i = 0; i < 3; i++) m.cpu.step();
    const cpu::Context first = m.cpu.context();
    m.cpu.set_context(start);
    for (int i = 0; i < 3; i++) m.cpu.step();
    CHECK_EQ(m.cpu.context(), first);
    CHECK_EQ(first.regs[a1], 24);
}
