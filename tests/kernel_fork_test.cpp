// fork (a7 = 6), spec B3: the caller becomes a parent with a new child process.
//
// Expected behavior:
//   - Every page mapped in the parent is copied into a new frame for the child
//     (same virtual address and permissions). Afterwards they never see each
//     other's writes.
//   - Same registers and pc in both: both continue after the ecall. a0 is the
//     child's pid in the parent and 0 in the child.
//   - The child has the parent's break and initial_break.
//   - The child gets the next unused pid and joins the round robin.
//   - Not enough free frames to copy every mapped page: -ENOMEM, no child.
#include <cstdint>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "asm.hpp"
#include "cpu_helpers.hpp"
#include "elf_builder.hpp"
#include "kernel/kernel.hpp"
#include "loader/elf.hpp"
#include "machine/machine.hpp"
#include "test.hpp"

using namespace isa::reg;
using enum isa::Op;

namespace {

constexpr uint32_t kData = 0x20000;   // optional read-only data: a letter to print
constexpr uint32_t kInitialBreak = 0x11000;
constexpr int32_t kEnomem = 12;

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
        CHECK(e.has_value());
        const auto* exited = e ? std::get_if<kernel::Exited>(&*e) : nullptr;
        CHECK(exited != nullptr);
        return exited ? exited->exit_status : -12345;
    }
};

// Emits "fork; if a0 == 0 goto child". Returns the index of the branch, to patch.
size_t fork_and_branch_to_child(Asm& a) {
    a.syscall(SYS_FORK);
    return a.branch(BEQ, a0, zero);
}

}  // namespace

// ---- return values ----

TEST(fork_returns_child_pid_to_parent_and_zero_to_child) {
    // Parent exits with what fork returned (the child's pid); child exits with 100.
    System s;
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    a.exit_with_a0();                       // parent
    a.patch(to_child, a.here());
    a.exit_with(100);                       // child
    const uint32_t parent = s.spawn(elf(a));
    s.run();
    CHECK_EQ(s.status(parent), 2);
    CHECK_EQ(s.status(2), 100);
}

TEST(fork_child_gets_next_unused_pid) {
    System s;
    Asm forker;
    const size_t to_child = fork_and_branch_to_child(forker);
    forker.exit_with_a0();
    forker.patch(to_child, forker.here());
    forker.exit_with(0);
    Asm idle;
    idle.exit_with(0);
    const uint32_t parent = s.spawn(elf(forker));
    s.spawn(elf(idle));
    s.spawn(elf(idle));
    s.run();
    CHECK_EQ(s.status(parent), 4);          // pids 1-3 exist: the child is 4
    CHECK_EQ(s.status(4), 0);
}

TEST(fork_child_getpid) {
    System s;
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    a.exit_with(0);                         // parent
    a.patch(to_child, a.here());
    a.syscall(SYS_GETPID);                  // child: its own pid
    a.exit_with_a0();
    s.spawn(elf(a));
    s.run();
    CHECK_EQ(s.status(2), 2);
}

// ---- what the child inherits ----

TEST(fork_copies_registers) {
    System s;
    Asm a;
    a.li(s0, 7);
    a.li(s1, 30);
    const size_t to_child = fork_and_branch_to_child(a);
    a.emit({ADD, a0, s0, s1, 0});           // parent: 37
    a.exit_with_a0();
    a.patch(to_child, a.here());
    a.emit({SUB, a0, s1, s0, 0});           // child: 23
    a.exit_with_a0();
    const uint32_t parent = s.spawn(elf(a));
    s.run();
    CHECK_EQ(s.status(parent), 37);
    CHECK_EQ(s.status(2), 23);
}

TEST(fork_copies_memory_then_they_are_independent) {
    // Before fork: 5 is on the stack. Then the child overwrites it with 9.
    // The parent waits (yields) until the child is done, and must still read 5.
    System s;
    Asm a;
    a.li(t1, 5);
    a.emit({SW, 0, sp, t1, -4});
    const size_t to_child = fork_and_branch_to_child(a);
    a.syscall(SYS_YIELD);                   // parent: let the child run first
    a.emit({LW, a0, sp, 0, -4});
    a.exit_with_a0();
    a.patch(to_child, a.here());
    a.emit({LW, s0, sp, 0, -4});            // child sees the copy: 5
    a.li(t1, 9);
    a.emit({SW, 0, sp, t1, -4});
    a.emit({LW, t2, sp, 0, -4});            // its own write: 9
    a.emit({ADD, a0, s0, t2, 0});           // 5 + 9
    a.exit_with_a0();
    const uint32_t parent = s.spawn(elf(a));
    s.run();
    CHECK_EQ(s.status(parent), 5);
    CHECK_EQ(s.status(2), 14);
}

TEST(fork_child_write_does_not_reach_parent_heap) {
    // Same idea on a heap page, and the other way round: the parent writes
    // after fork, the child must still see the old value.
    System s;
    Asm a;
    a.sbrk(4096);
    a.emit({ADDI, s0, a0, 0, 0});
    a.li(t1, 1);
    a.emit({SW, 0, s0, t1, 0});             // heap[0] = 1, before fork
    const size_t to_child = fork_and_branch_to_child(a);
    a.li(t1, 2);
    a.emit({SW, 0, s0, t1, 0});             // parent: heap[0] = 2
    a.emit({LW, a0, s0, 0, 0});
    a.exit_with_a0();
    a.patch(to_child, a.here());
    a.emit({LW, a0, s0, 0, 0});             // child runs after the parent's write
    a.exit_with_a0();
    const uint32_t parent = s.spawn(elf(a));
    s.run();
    CHECK_EQ(s.status(parent), 2);
    CHECK_EQ(s.status(2), 1);
}

TEST(fork_child_has_parent_break) {
    // Untouched heap pages stay valid in the child, too.
    System s;
    Asm a;
    a.sbrk(3 * 4096);
    const size_t to_child = fork_and_branch_to_child(a);
    a.exit_with(0);                         // parent
    a.patch(to_child, a.here());
    a.sbrk(0);                              // child: same break as the parent
    a.emit({ADDI, s0, a0, 0, 0});
    a.li(t0, static_cast<int32_t>(kInitialBreak + 2 * 4096));
    a.emit({SW, 0, t0, t0, 0});             // touch a page nobody touched before fork
    a.emit({ADDI, a0, s0, 0, 0});
    a.exit_with_a0();
    s.spawn(elf(a));
    s.run();
    CHECK_EQ(s.status(2), static_cast<int32_t>(kInitialBreak + 3 * 4096));
}

TEST(fork_both_continue_and_print) {
    // Parent prints first (it keeps running after fork), then the child.
    System s;
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    a.write_stdout(kData, 1);
    a.exit_with(0);
    a.patch(to_child, a.here());
    a.write_stdout(kData + 1, 1);
    a.exit_with(0);
    s.spawn(elf(a, {'P', 'C'}));
    s.run();
    CHECK_EQ(s.out.str(), std::string("PC"));
}

TEST(fork_grandchild) {
    // fork, then fork again in both parent and child: a child, a second child
    // and a grandchild, so 4 processes in total, all of which exit.
    System s;
    Asm a;
    a.syscall(SYS_FORK);
    a.syscall(SYS_FORK);                    // runs in both: 4 processes
    a.exit_with(0);
    s.spawn(elf(a));
    s.run();
    for (uint32_t pid = 1; pid <= 4; pid++) CHECK_EQ(s.status(pid), 0);
    CHECK(!s.k.ending(5).has_value());
}

// ---- memory accounting ----

TEST(fork_copies_every_mapped_page_into_new_frames) {
    System s;
    Asm a;
    a.syscall(SYS_FORK);
    a.emit({JAL, zero, 0, 0, 0});           // both loop forever
    s.spawn(elf(a));
    const uint32_t free_before = s.m.frame_allocator().free_count();
    CHECK(std::holds_alternative<kernel::StepLimit>(s.k.run_all(5000)));
    const kernel::Process* parent = s.k.process(1);
    const kernel::Process* child = s.k.process(2);
    CHECK(parent != nullptr && child != nullptr);
    if (!parent || !child) return;
    const std::vector<uint32_t> pages = parent->page_table.mapped_pages();
    CHECK(pages == child->page_table.mapped_pages());          // same virtual layout
    CHECK_EQ(s.m.frame_allocator().free_count(), free_before - pages.size());
    for (uint32_t vpn : pages) {
        const auto p = parent->page_table.lookup(vpn);
        const auto c = child->page_table.lookup(vpn);
        CHECK(p && c);
        if (!p || !c) continue;
        CHECK(p->frame != c->frame);                            // own copy
        CHECK(p->read == c->read && p->write == c->write && p->exec == c->exec);
    }
}

TEST(fork_frames_are_freed_when_both_end) {
    System s;
    const uint32_t before = s.m.frame_allocator().free_count();
    Asm a;
    a.sbrk(4096);
    a.emit({SW, 0, a0, zero, 0});
    a.syscall(SYS_FORK);
    a.exit_with(0);
    s.spawn(elf(a));
    s.run();
    CHECK_EQ(s.m.frame_allocator().free_count(), before);
}

TEST(fork_without_enough_memory_returns_enomem) {
    // 15 processes x 17 frames = 255 of 256 frames used: copying 17 pages can't fit.
    System s;
    Asm forker;
    forker.syscall(SYS_FORK);
    forker.exit_with_a0();                  // -ENOMEM, and no child to run this
    const uint32_t pid = s.spawn(elf(forker));
    Asm idle;
    idle.exit_with(0);
    for (int i = 0; i < 14; i++) s.spawn(elf(idle));
    CHECK_EQ(s.m.frame_allocator().free_count(), 1);
    s.run();
    CHECK_EQ(s.status(pid), -kEnomem);
    CHECK(!s.k.ending(16).has_value());     // no child was ever created
    CHECK_EQ(s.m.frame_allocator().free_count(), mmu::kNumFrames);   // nothing leaked
}
