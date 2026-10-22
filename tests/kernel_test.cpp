// Kernel: runs a program, handles its traps and system calls (spec A3, B3).
#include <cstdint>
#include <initializer_list>
#include <sstream>
#include <variant>

#include "cpu_helpers.hpp"   // prints Cause in failure messages
#include "elf_builder.hpp"
#include "kernel/kernel.hpp"
#include "machine/machine.hpp"
#include "test.hpp"

using namespace isa::reg;
using enum isa::Op;

namespace {

using Result = std::variant<kernel::Exited, kernel::Killed, kernel::StepLimit>;

constexpr uint32_t kEntry = 0x10000;   // program_elf() puts code here
constexpr int32_t SYS_EXIT = 1;        // spec B3
constexpr int32_t kUnknownSyscall = 999;
constexpr int32_t kEnosys = 38;      // not ENOSYS: that name is a macro in <errno.h>

// Instructions for "exit(a0)": a7 = 1; ecall.
const isa::Instruction kSetExit = {ADDI, a7, zero, 0, SYS_EXIT};
const isa::Instruction kEcall = {ECALL};

Result run_program(machine::Machine& m, std::initializer_list<isa::Instruction> program,
                   uint64_t max_steps = 1000) {
    CHECK(!m.load(elf_builder::program_elf(program)));
    std::ostringstream out, err;   // these tests don't look at output
    kernel::Kernel k(m, out, err);
    return k.run(max_steps);
}

// The exit status, or a value no test uses if the program didn't exit.
int32_t exit_status(const Result& r) {
    const auto* exited = std::get_if<kernel::Exited>(&r);
    CHECK(exited != nullptr);
    return exited ? exited->exit_status : -12345;
}

cpu::Trap killed_by(const Result& r) {
    const auto* killed = std::get_if<kernel::Killed>(&r);
    CHECK(killed != nullptr);
    return killed ? killed->trap : cpu::Trap{};
}

}  // namespace

// ---- exit ----

TEST(kernel_exit_returns_status) {
    machine::Machine m;
    const Result r = run_program(m, {{ADDI, a0, zero, 0, 3}, kSetExit, kEcall});
    CHECK_EQ(exit_status(r), 3);
}

TEST(kernel_exit_status_is_signed) {
    machine::Machine m;
    const Result r = run_program(m, {{ADDI, a0, zero, 0, -1}, kSetExit, kEcall});
    CHECK_EQ(exit_status(r), -1);
}

// ---- other system calls ----

TEST(kernel_unknown_syscall_returns_enosys_and_continues) {
    // The program keeps running after the bad call, and exits with what a0 held.
    machine::Machine m;
    const Result r = run_program(m, {
        {ADDI, a7, zero, 0, kUnknownSyscall},
        kEcall,                               // a0 = -ENOSYS
        kSetExit,
        kEcall,                               // exit(a0)
    });
    CHECK_EQ(exit_status(r), -kEnosys);
}

TEST(kernel_syscall_resumes_after_the_ecall) {
    // If pc weren't moved past the ecall, it would run again and again.
    machine::Machine m;
    const Result r = run_program(m, {
        {ADDI, a7, zero, 0, kUnknownSyscall},
        kEcall,
        {ADDI, a0, zero, 0, 7},
        kSetExit,
        kEcall,
    });
    CHECK_EQ(exit_status(r), 7);
    CHECK_EQ(m.steps(), 5);   // each instruction ran exactly once
}

TEST(kernel_syscall_changes_only_a0) {
    // a1 and a7 must survive the call (spec B3: the kernel changes no other register).
    machine::Machine m;
    const Result r = run_program(m, {
        {ADDI, a1, zero, 0, 40},
        {ADDI, a7, zero, 0, kUnknownSyscall},
        kEcall,
        {ADDI, a7, a7, 0, -kUnknownSyscall + 2},   // a7 = 2 only if a7 was still 999
        {ADD, a0, a1, a7, 0},                       // a0 = 40 + 2
        kSetExit,
        kEcall,
    });
    CHECK_EQ(exit_status(r), 42);
}

// ---- faults kill the program ----

TEST(kernel_illegal_instruction_kills) {
    // Jump past the code into zero-filled memory.
    machine::Machine m;
    const cpu::Trap t = killed_by(run_program(m, {{JAL, zero, 0, 0, 16}}));
    CHECK_EQ(t.cause, cpu::Cause::illegal_instruction);
    CHECK_EQ(t.pc, kEntry + 16);
}

TEST(kernel_page_fault_kills) {
    machine::Machine m;
    const cpu::Trap t = killed_by(run_program(m, {
        {LUI, a1, 0, 0, 0x100000},   // a1 = end of RAM
        {LW, a0, a1, 0, 0},
    }));
    CHECK_EQ(t.cause, cpu::Cause::load_page_fault);
    CHECK_EQ(t.pc, kEntry + 4);
    CHECK_EQ(t.tval, 0x100000);
}

TEST(kernel_ebreak_kills) {
    machine::Machine m;
    const cpu::Trap t = killed_by(run_program(m, {{EBREAK}}));
    CHECK_EQ(t.cause, cpu::Cause::breakpoint);
}

// ---- step limit ----

TEST(kernel_stops_at_step_limit) {
    machine::Machine m;
    const Result r = run_program(m, {{JAL, zero, 0, 0, 0}}, 100);   // loops forever
    CHECK(std::holds_alternative<kernel::StepLimit>(r));
    CHECK_EQ(m.steps(), 100);
}

TEST(kernel_step_limit_counts_across_syscalls) {
    // A loop that makes a system call every time around: the limit still applies.
    machine::Machine m;
    const Result r = run_program(m, {
        {ADDI, a7, zero, 0, kUnknownSyscall},
        kEcall,
        {JAL, zero, 0, 0, -4},   // back to the ecall
    }, 101);
    CHECK(std::holds_alternative<kernel::StepLimit>(r));
    CHECK_EQ(m.steps(), 101);
}

TEST(kernel_run_zero_steps) {
    machine::Machine m;
    const Result r = run_program(m, {{ADDI, a0, zero, 0, 3}, kSetExit, kEcall}, 0);
    CHECK(std::holds_alternative<kernel::StepLimit>(r));
    CHECK_EQ(m.steps(), 0);
}
