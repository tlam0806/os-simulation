// Scheduler: Kernel::run_all() runs every process, round robin, switching on
// each timer interrupt (every kernel::kQuantum instructions).
#include <cstdint>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "asm.hpp"
#include "cpu_helpers.hpp"
#include "elf_builder.hpp"
#include "kernel/kernel.hpp"
#include "machine/machine.hpp"
#include "test.hpp"

using namespace isa::reg;
using enum isa::Op;

namespace {

constexpr uint32_t kData = 0x20000;   // optional read-only data segment

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

    bool all_done(uint64_t max_steps = 1'000'000) {
        return std::holds_alternative<kernel::AllDone>(k.run_all(max_steps));
    }

    // Exit status of an ended process; fails the test if it didn't exit.
    int32_t status(uint32_t pid) {
        const auto e = k.ending(pid);
        CHECK(e.has_value());
        const auto* exited = e ? std::get_if<kernel::Exited>(&*e) : nullptr;
        CHECK(exited != nullptr);
        return exited ? exited->exit_status : -12345;
    }

    cpu::Trap killed_by(uint32_t pid) {
        const auto e = k.ending(pid);
        CHECK(e.has_value());
        const auto* killed = e ? std::get_if<kernel::Killed>(&*e) : nullptr;
        CHECK(killed != nullptr);
        return killed ? killed->trap : cpu::Trap{};
    }
};

Asm exits_with(int32_t status) { Asm a; a.exit_with(status); return a; }

Asm loops_forever() { Asm a; a.emit({JAL, zero, 0, 0, 0}); return a; }

// Prints the letter stored at kData three times, spinning ~1500 instructions
// (longer than one quantum) after each print, then exits.
Asm prints_letter_three_times() {
    Asm a;
    a.li(s1, 3);
    const size_t loop = a.here();
    a.write_stdout(kData, 1);
    a.busy_wait(750);
    a.emit({ADDI, s1, s1, 0, -1});
    a.patch(a.branch(BNE, s1, zero), loop);
    a.exit_with(0);
    return a;
}

// Sums 1..n in s0 (about 3n instructions, so it spans many quanta), exits with the sum.
Asm sums_to(int32_t n) {
    Asm a;
    a.li(s0, 0);
    a.li(s1, n);
    const size_t loop = a.here();
    a.emit({ADD, s0, s0, s1, 0});
    a.emit({ADDI, s1, s1, 0, -1});
    a.patch(a.branch(BNE, s1, zero), loop);
    a.emit({ADDI, a0, s0, 0, 0});
    a.exit_with_a0();
    return a;
}

}  // namespace

// ---- one process ----

TEST(run_all_with_no_processes_is_all_done) {
    System s;
    CHECK(s.all_done());
}

TEST(run_all_records_exit_and_removes_process) {
    System s;
    const uint32_t pid = s.spawn(elf(exits_with(7)));
    CHECK(s.all_done());
    CHECK_EQ(s.status(pid), 7);
    CHECK_EQ(s.k.process_count(), 0);
    CHECK(s.k.process(pid) == nullptr);
}

TEST(run_all_records_kill) {
    System s;
    Asm a;
    a.emit({EBREAK});
    const uint32_t pid = s.spawn(elf(a));
    CHECK(s.all_done());
    const cpu::Trap t = s.killed_by(pid);
    CHECK_EQ(t.cause, cpu::Cause::breakpoint);
    CHECK_EQ(t.pc, 0x10000);
    CHECK_EQ(s.k.process_count(), 0);
}

TEST(run_all_frees_all_frames_of_ended_processes) {
    // Includes heap pages from sbrk, not just what the loader mapped.
    System s;
    const uint32_t before = s.m.frame_allocator().free_count();
    Asm grows;
    grows.sbrk(8192);
    grows.exit_with(0);
    s.spawn(elf(grows));
    s.spawn(elf(exits_with(1)));
    CHECK(s.all_done());
    CHECK_EQ(s.m.frame_allocator().free_count(), before);
}

TEST(ending_is_empty_for_running_or_unknown_pids) {
    System s;
    const uint32_t pid = s.spawn(elf(exits_with(0)));
    CHECK(!s.k.ending(pid).has_value());       // not run yet
    CHECK(!s.k.ending(pid + 1).has_value());   // never existed
}

// ---- several processes ----

TEST(run_all_starts_in_pid_order) {
    System s;
    for (char c : {'A', 'B', 'C'}) {
        Asm a;
        a.write_stdout(kData, 1);
        a.exit_with(0);
        s.spawn(elf(a, {static_cast<uint8_t>(c)}));
    }
    CHECK(s.all_done());
    CHECK_EQ(s.out.str(), std::string("ABC"));
}

TEST(run_all_takes_turns) {
    // Each process prints, then spins longer than a quantum, three times.
    // Taking turns gives ABABAB; running each to the end would give AAABBB.
    // Both read their letter from the same virtual address, so this also checks
    // that write() reads the memory of the process that called it.
    System s;
    const uint32_t a = s.spawn(elf(prints_letter_three_times(), {'A'}));
    const uint32_t b = s.spawn(elf(prints_letter_three_times(), {'B'}));
    CHECK(s.all_done());
    CHECK_EQ(s.out.str(), std::string("ABABAB"));
    CHECK_EQ(s.status(a), 0);
    CHECK_EQ(s.status(b), 0);
}

TEST(run_all_keeps_each_process_registers) {
    // Both sums take several quanta; switching must save and restore s0 and s1.
    System s;
    const uint32_t a = s.spawn(elf(sums_to(2000)));
    const uint32_t b = s.spawn(elf(sums_to(1000)));
    CHECK(s.all_done());
    CHECK_EQ(s.status(a), 2001000);
    CHECK_EQ(s.status(b), 500500);
}

TEST(run_all_preempts_a_process_that_never_stops) {
    // Process 1 never calls the kernel; the timer still lets process 2 run.
    System s;
    const uint32_t hog = s.spawn(elf(loops_forever()));
    const uint32_t other = s.spawn(elf(exits_with(5)));
    const auto r = s.k.run_all(100000);
    CHECK(std::holds_alternative<kernel::StepLimit>(r));
    CHECK_EQ(s.status(other), 5);
    CHECK(!s.k.ending(hog).has_value());
    CHECK_EQ(s.k.process_count(), 1);
}

TEST(run_all_killed_process_does_not_stop_others) {
    System s;
    Asm bad;
    bad.emit({EBREAK});
    const uint32_t a = s.spawn(elf(bad));
    const uint32_t b = s.spawn(elf(exits_with(3)));
    CHECK(s.all_done());
    CHECK_EQ(s.killed_by(a).cause, cpu::Cause::breakpoint);
    CHECK_EQ(s.status(b), 3);
}

TEST(run_all_sbrk_is_per_process) {
    // Each process has its own break. Process 1 is preempted between its two
    // sbrk calls while process 2 grows its own heap.
    System s;
    Asm one;
    one.sbrk(4096);
    one.busy_wait(1000);   // longer than a quantum
    one.sbrk(0);
    one.exit_with_a0();
    Asm two;
    two.sbrk(8192);
    two.sbrk(0);
    two.exit_with_a0();
    const uint32_t p1 = s.spawn(elf(one));
    const uint32_t p2 = s.spawn(elf(two));
    CHECK(s.all_done());
    CHECK_EQ(s.status(p1), 0x12000);   // 0x11000 + 4096, unaffected by process 2
    CHECK_EQ(s.status(p2), 0x13000);   // 0x11000 + 8192
}

// ---- step limit ----

TEST(run_all_step_limit_counts_all_processes) {
    System s;
    s.spawn(elf(loops_forever()));
    s.spawn(elf(loops_forever()));
    const auto r = s.k.run_all(5000);
    CHECK(std::holds_alternative<kernel::StepLimit>(r));
    CHECK_EQ(s.m.steps(), 5000);
    CHECK_EQ(s.k.process_count(), 2);
}

TEST(run_all_continues_after_step_limit) {
    System s;
    Asm a;
    a.busy_wait(1500);   // about 3000 instructions
    a.exit_with(9);
    const uint32_t pid = s.spawn(elf(a));
    CHECK(std::holds_alternative<kernel::StepLimit>(s.k.run_all(1000)));
    CHECK(!s.k.ending(pid).has_value());
    CHECK(s.all_done());
    CHECK_EQ(s.status(pid), 9);
}
