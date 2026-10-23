// getpid (a7 = 4) and yield (a7 = 5), spec B3.
//   getpid: a0 = the calling process's pid.
//   yield:  a0 = 0; the process gives up the rest of its turn and the next
//           process runs. With no other process, it simply continues.
// Like every system call: only a0 changes, and execution resumes after the ecall.
#include <cstdint>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "asm.hpp"
#include "elf_builder.hpp"
#include "kernel/kernel.hpp"
#include "machine/machine.hpp"
#include "test.hpp"

using namespace isa::reg;
using enum isa::Op;

namespace {

constexpr uint32_t kData = 0x20000;   // a read-only byte: the letter a program prints

std::vector<uint8_t> elf(const Asm& a, std::vector<uint8_t> data = {}) {
    using namespace elf_builder;
    std::vector<Segment> segments = {{0x10000, code(a.code)}};
    if (!data.empty()) segments.push_back({kData, std::move(data), 0, PT_LOAD, PF_R});
    return make_elf(0x10000, segments);
}

struct System {
    machine::Machine m;
    std::ostringstream out, err;
    kernel::Kernel k{m, out, err};

    uint32_t spawn(const std::vector<uint8_t>& file) {
        const auto r = k.spawn(file);
        CHECK(std::holds_alternative<uint32_t>(r));
        return std::holds_alternative<uint32_t>(r) ? std::get<uint32_t>(r) : 0;
    }

    void run() { CHECK(std::holds_alternative<kernel::AllDone>(k.run_all(1'000'000))); }

    int32_t status(uint32_t pid) {
        const auto e = k.ending(pid);
        const auto* exited = e ? std::get_if<kernel::Exited>(&e->reason) : nullptr;
        CHECK(exited != nullptr);
        return exited ? exited->exit_status : -12345;
    }
};

// Prints its letter, yields, prints it again, exits. Far shorter than a quantum,
// so only yield can make another process run in between.
Asm print_yield_print() {
    Asm a;
    a.write_stdout(kData, 1);
    a.syscall(SYS_YIELD);
    a.write_stdout(kData, 1);
    a.exit_with(0);
    return a;
}

}  // namespace

// ---- getpid ----

TEST(getpid_returns_own_pid) {
    System s;
    Asm a;
    a.syscall(SYS_GETPID);
    a.exit_with_a0();
    const uint32_t p1 = s.spawn(elf(a));
    const uint32_t p2 = s.spawn(elf(a));
    const uint32_t p3 = s.spawn(elf(a));
    s.run();
    CHECK_EQ(s.status(p1), 1);
    CHECK_EQ(s.status(p2), 2);
    CHECK_EQ(s.status(p3), 3);
}

TEST(getpid_changes_only_a0) {
    // a0 = pid + a1 + (a7 - 4): 1 + 40 + 0 if a1 and a7 survived the call.
    System s;
    Asm a;
    a.li(a1, 40);
    a.syscall(SYS_GETPID);
    a.emit({ADD, a0, a0, a1, 0});
    a.emit({ADDI, t1, a7, 0, -SYS_GETPID});
    a.emit({ADD, a0, a0, t1, 0});
    a.exit_with_a0();
    const uint32_t pid = s.spawn(elf(a));
    s.run();
    CHECK_EQ(s.status(pid), 41);
}

// ---- yield ----

TEST(yield_returns_zero) {
    System s;
    Asm a;
    a.li(a0, 99);
    a.syscall(SYS_YIELD);
    a.exit_with_a0();
    const uint32_t pid = s.spawn(elf(a));
    s.run();
    CHECK_EQ(s.status(pid), 0);
}

TEST(yield_alone_just_continues) {
    System s;
    s.spawn(elf(print_yield_print(), {'A'}));
    s.run();
    CHECK_EQ(s.out.str(), std::string("AA"));
}

TEST(yield_lets_the_next_process_run) {
    // Without yield both programs finish within one quantum: "AABB".
    // With yield they alternate.
    System s;
    s.spawn(elf(print_yield_print(), {'A'}));
    s.spawn(elf(print_yield_print(), {'B'}));
    s.run();
    CHECK_EQ(s.out.str(), std::string("ABAB"));
}

TEST(yield_resumes_after_the_ecall) {
    // If pc weren't moved past the ecall, the process would yield forever
    // and never print its second letter.
    System s;
    const uint32_t a = s.spawn(elf(print_yield_print(), {'A'}));
    const uint32_t b = s.spawn(elf(print_yield_print(), {'B'}));
    s.run();
    CHECK_EQ(s.status(a), 0);
    CHECK_EQ(s.status(b), 0);
    CHECK_EQ(s.m.steps() < 1000, true);   // two tiny programs: far fewer than 1000 steps
}

TEST(yield_keeps_registers) {
    // Each process keeps its own s0 across the switch.
    System s;
    Asm a;
    a.li(s0, 7);
    a.syscall(SYS_YIELD);
    a.emit({ADDI, a0, s0, 0, 0});
    a.exit_with_a0();
    Asm b;
    b.li(s0, 9);
    b.syscall(SYS_YIELD);
    b.emit({ADDI, a0, s0, 0, 0});
    b.exit_with_a0();
    const uint32_t pa = s.spawn(elf(a));
    const uint32_t pb = s.spawn(elf(b));
    s.run();
    CHECK_EQ(s.status(pa), 7);
    CHECK_EQ(s.status(pb), 9);
}

TEST(yield_in_a_loop_takes_turns) {
    // Three rounds of "print, yield" in each of three processes: ABCABCABC.
    System s;
    for (char c : {'A', 'B', 'C'}) {
        Asm a;
        a.li(s1, 3);
        const size_t loop = a.here();
        a.write_stdout(kData, 1);
        a.syscall(SYS_YIELD);
        a.emit({ADDI, s1, s1, 0, -1});
        a.patch(a.branch(BNE, s1, zero), loop);
        a.exit_with(0);
        s.spawn(elf(a, {static_cast<uint8_t>(c)}));
    }
    s.run();
    CHECK_EQ(s.out.str(), std::string("ABCABCABC"));
}
