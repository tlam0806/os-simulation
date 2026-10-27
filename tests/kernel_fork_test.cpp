// fork (a7 = 6), spec B3: the caller becomes a parent with a new child process.
//
// Expected behavior:
//   - Copy-on-write: the child maps the parent's frames (same virtual address).
//     Writable pages become read-only + cow in both; read-only pages are just
//     shared. Each shared frame's reference count goes up by one. Afterwards
//     they never see each other's writes (a store copies the page).
//   - Same registers and pc in both: both continue after the ecall. a0 is the
//     child's pid in the parent and 0 in the child.
//   - The child has the parent's break and initial_break.
//   - The child gets the next unused pid and joins the round robin.
//   - fork allocates no frames, so it never fails for lack of memory.
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
        const auto* exited = e ? std::get_if<kernel::Exited>(&e->reason) : nullptr;
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

// =================== copy-on-write, step 2: fork shares frames ===================
// No write faults yet: these programs never store after fork.

namespace {

// Touches one heap page, forks, then both loop forever (no stores after fork).
Asm touch_heap_fork_and_loop() {
    Asm a;
    a.sbrk(4096);
    a.emit({SW, 0, a0, zero, 0});           // heap page mapped before fork
    a.syscall(SYS_FORK);
    a.emit({JAL, zero, 0, 0, 0});           // loop forever
    return a;
}

// Runs until both processes are looping. Returns false if they aren't both alive.
bool run_until_looping(System& s) {
    CHECK(std::holds_alternative<kernel::StepLimit>(s.k.run_all(5000)));
    const bool alive = s.k.process(1) != nullptr && s.k.process(2) != nullptr;
    CHECK(alive);
    return alive;
}

}  // namespace

TEST(cow_share_fork_uses_no_new_frames) {
    // The only frame allocated after spawn is the heap page, touched before fork.
    System s;
    s.spawn(elf(touch_heap_fork_and_loop()));
    const uint32_t free_at_spawn = s.m.frame_allocator().free_count();
    if (!run_until_looping(s)) return;
    CHECK_EQ(s.m.frame_allocator().free_count(), free_at_spawn - 1);
}

TEST(cow_share_child_maps_the_same_frames) {
    System s;
    s.spawn(elf(touch_heap_fork_and_loop()));
    if (!run_until_looping(s)) return;
    const mmu::PageTable& p = s.k.process(1)->page_table;
    const mmu::PageTable& c = s.k.process(2)->page_table;
    const std::vector<uint32_t> pages = p.mapped_pages();
    CHECK(pages == c.mapped_pages());       // same virtual layout
    for (uint32_t vpn : pages) {
        const auto pp = p.lookup(vpn);
        const auto cp = c.lookup(vpn);
        CHECK(pp && cp);
        if (!pp || !cp) continue;
        CHECK_EQ(pp->frame, cp->frame);                                // shared
        CHECK_EQ(s.m.frame_allocator().ref_count(pp->frame), 2);       // two mappings
    }
}

TEST(cow_share_writable_pages_become_read_only_cow_in_both) {
    // Every page except the code page was read+write before fork (16 stack
    // pages + 1 heap page). After fork it is read-only + cow in both processes.
    System s;
    s.spawn(elf(touch_heap_fork_and_loop()));
    if (!run_until_looping(s)) return;
    for (uint32_t pid : {1u, 2u}) {
        const mmu::PageTable& t = s.k.process(pid)->page_table;
        size_t checked = 0;
        for (uint32_t vpn : t.mapped_pages()) {
            if (vpn == 0x10) continue;      // the code page
            const auto pte = t.lookup(vpn);
            CHECK(pte->read);
            CHECK(!pte->write);             // a store now faults
            CHECK(pte->cow);
            CHECK(!pte->exec);
            checked++;
        }
        CHECK_EQ(checked, 17);
    }
}

TEST(cow_share_read_only_pages_are_not_cow) {
    System s;
    s.spawn(elf(touch_heap_fork_and_loop(), {'x'}));   // code page + read-only data page
    if (!run_until_looping(s)) return;
    for (uint32_t vaddr : {0x10000u, kData}) {
        for (uint32_t pid : {1u, 2u}) {
            const auto pte = s.k.process(pid)->page_table.lookup(vaddr >> mmu::kPageShift);
            CHECK(pte.has_value());
            if (!pte) continue;
            CHECK(!pte->write);
            CHECK(!pte->cow);               // never writable: nothing to copy
        }
    }
    const auto code = s.k.process(1)->page_table.lookup(0x10);
    CHECK(code && code->exec && code->read);           // permissions unchanged
}

TEST(cow_share_parent_keeps_running_without_stores) {
    // fork with no store afterwards behaves exactly as before.
    System s;
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    a.exit_with_a0();                       // parent: the child's pid
    a.patch(to_child, a.here());
    a.exit_with(100);
    const uint32_t parent = s.spawn(elf(a));
    s.run();
    CHECK_EQ(s.status(parent), 2);
    CHECK_EQ(s.status(2), 100);
}

TEST(cow_share_four_processes_share_the_code_frame) {
    // fork twice: parent, two children and a grandchild all map the code page.
    System s;
    Asm a;
    a.syscall(SYS_FORK);
    a.syscall(SYS_FORK);
    a.emit({JAL, zero, 0, 0, 0});
    s.spawn(elf(a));
    CHECK(std::holds_alternative<kernel::StepLimit>(s.k.run_all(5000)));
    const kernel::Process* first = s.k.process(1);
    CHECK(first != nullptr);
    if (!first) return;
    const uint32_t frame = first->page_table.lookup(0x10)->frame;
    CHECK_EQ(s.m.frame_allocator().ref_count(frame), 4);
    for (uint32_t pid = 2; pid <= 4; pid++) {
        const kernel::Process* p = s.k.process(pid);
        CHECK(p != nullptr);
        if (p) CHECK_EQ(p->page_table.lookup(0x10)->frame, frame);
    }
}

TEST(cow_share_frames_are_freed_only_when_last_user_ends) {
    // Child exits at once; the parent still loops. Every frame the parent maps
    // is still in use, now with a single reference.
    System s;
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    a.emit({JAL, zero, 0, 0, 0});           // parent loops
    a.patch(to_child, a.here());
    a.exit_with(0);                         // child ends
    s.spawn(elf(a));
    CHECK(std::holds_alternative<kernel::StepLimit>(s.k.run_all(5000)));
    CHECK(s.k.ending(2).has_value());
    const kernel::Process* parent = s.k.process(1);
    CHECK(parent != nullptr);
    if (!parent) return;
    for (uint32_t vpn : parent->page_table.mapped_pages())
        CHECK_EQ(s.m.frame_allocator().ref_count(parent->page_table.lookup(vpn)->frame), 1);
    CHECK_EQ(s.m.frame_allocator().free_count(),
             mmu::kNumFrames - parent->page_table.mapped_pages().size());
}

TEST(cow_share_fork_succeeds_with_no_frame_to_copy) {
    // 15 processes x 17 frames = 255 of 256 frames used. Copying would need 17;
    // sharing needs none, so fork succeeds and both processes run.
    System s;
    Asm forker;
    const size_t to_child = fork_and_branch_to_child(forker);
    forker.exit_with_a0();                  // parent: the child's pid
    forker.patch(to_child, forker.here());
    forker.exit_with(7);
    const uint32_t pid = s.spawn(elf(forker));
    Asm idle;
    idle.exit_with(0);
    for (int i = 0; i < 14; i++) s.spawn(elf(idle));
    CHECK_EQ(s.m.frame_allocator().free_count(), 1);
    s.run();
    CHECK_EQ(s.status(pid), 16);
    CHECK_EQ(s.status(16), 7);
    CHECK_EQ(s.m.frame_allocator().free_count(), mmu::kNumFrames);   // nothing leaked
}

// =================== copy-on-write, step 3: a store copies the page ===================
// A store to a cow page is a store page fault. Frame still shared: copy it into
// a new frame, map that read+write for the writer, retry. Last user: just make
// it read+write. No free frame: the writer is killed.

namespace {

// The stack page holding sp - 4 (sp starts at 0x80000000).
constexpr uint32_t kStackVpn = (0x80000000u - 4) >> mmu::kPageShift;

bool killed_by_store_fault(System& s, uint32_t pid) {
    const auto e = s.k.ending(pid);
    CHECK(e.has_value());
    const auto* killed = e ? std::get_if<kernel::Killed>(&e->reason) : nullptr;
    CHECK(killed != nullptr);
    return killed && killed->trap.cause == cpu::Cause::store_page_fault;
}

}  // namespace

TEST(cow_write_child_store_gets_its_own_frame) {
    // Child stores to the stack page, then loops. The parent loops without storing.
    System s;
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    a.emit({JAL, zero, 0, 0, 0});           // parent loops
    a.patch(to_child, a.here());
    a.li(t1, 9);
    a.emit({SW, 0, sp, t1, -4});
    a.emit({JAL, zero, 0, 0, 0});           // child loops
    s.spawn(elf(a));
    const uint32_t free_at_spawn = s.m.frame_allocator().free_count();
    if (!run_until_looping(s)) return;
    const auto p = s.k.process(1)->page_table.lookup(kStackVpn);
    const auto c = s.k.process(2)->page_table.lookup(kStackVpn);
    CHECK(p && c);
    if (!p || !c) return;
    CHECK(p->frame != c->frame);                                   // child copied
    CHECK(c->write && c->read && !c->cow);                         // child: plain read+write
    CHECK(!p->write && p->cow);                                    // parent: untouched
    CHECK_EQ(s.m.frame_allocator().ref_count(p->frame), 1);
    CHECK_EQ(s.m.frame_allocator().ref_count(c->frame), 1);
    CHECK_EQ(s.m.frame_allocator().free_count(), free_at_spawn - 1);   // one copy
}

TEST(cow_write_only_the_written_page_is_copied) {
    System s;
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    a.emit({JAL, zero, 0, 0, 0});
    a.patch(to_child, a.here());
    a.emit({SW, 0, sp, zero, -4});
    a.emit({JAL, zero, 0, 0, 0});
    s.spawn(elf(a));
    if (!run_until_looping(s)) return;
    const mmu::PageTable& p = s.k.process(1)->page_table;
    const mmu::PageTable& c = s.k.process(2)->page_table;
    for (uint32_t vpn : p.mapped_pages()) {
        if (vpn == kStackVpn) continue;
        CHECK_EQ(p.lookup(vpn)->frame, c.lookup(vpn)->frame);    // still shared
    }
}

TEST(cow_write_loads_do_not_copy) {
    System s;
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    a.emit({JAL, zero, 0, 0, 0});
    a.patch(to_child, a.here());
    a.emit({LW, t1, sp, 0, -4});
    a.emit({JAL, zero, 0, 0, 0});
    s.spawn(elf(a));
    const uint32_t free_at_spawn = s.m.frame_allocator().free_count();
    if (!run_until_looping(s)) return;
    CHECK_EQ(s.m.frame_allocator().free_count(), free_at_spawn);
    const auto c = s.k.process(2)->page_table.lookup(kStackVpn);
    CHECK(c && c->cow && !c->write);
}

TEST(cow_write_copy_keeps_the_rest_of_the_page) {
    // Two words on one page before fork; the child overwrites one and must
    // still read the other.
    System s;
    Asm a;
    a.li(t1, 1111);
    a.emit({SW, 0, sp, t1, -4});
    a.li(t1, 2222);
    a.emit({SW, 0, sp, t1, -8});
    const size_t to_child = fork_and_branch_to_child(a);
    a.syscall(SYS_YIELD);                   // parent: let the child run first
    a.emit({LW, t1, sp, 0, -4});
    a.emit({LW, t2, sp, 0, -8});
    a.emit({ADD, a0, t1, t2, 0});           // 1111 + 2222, both untouched
    a.exit_with_a0();
    a.patch(to_child, a.here());
    a.li(t1, 5);
    a.emit({SW, 0, sp, t1, -4});            // copy, then write
    a.emit({LW, t2, sp, 0, -8});            // 2222 came along with the copy
    a.emit({LW, t1, sp, 0, -4});            // 5
    a.emit({ADD, a0, t1, t2, 0});
    a.exit_with_a0();
    const uint32_t parent = s.spawn(elf(a));
    s.run();
    CHECK_EQ(s.status(parent), 3333);
    CHECK_EQ(s.status(2), 2227);
}

TEST(cow_write_last_user_makes_page_writable_without_copy) {
    // Child ends at once. Then the parent writes: it's the only user left, so
    // the frame is reused, not copied.
    System s;
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    a.syscall(SYS_YIELD);                   // parent: let the child end first
    a.li(t1, 3);
    a.emit({SW, 0, sp, t1, -4});
    a.emit({JAL, zero, 0, 0, 0});           // parent loops
    a.patch(to_child, a.here());
    a.exit_with(0);
    s.spawn(elf(a));
    CHECK(std::holds_alternative<kernel::StepLimit>(s.k.run_all(5000)));
    CHECK(s.k.ending(2).has_value());
    const kernel::Process* parent = s.k.process(1);
    CHECK(parent != nullptr);
    if (!parent) return;
    const uint32_t mapped = parent->page_table.mapped_pages().size();
    CHECK_EQ(s.m.frame_allocator().free_count(), mmu::kNumFrames - mapped);   // no extra frame
    const auto pte = parent->page_table.lookup(kStackVpn);
    CHECK(pte && pte->write && !pte->cow);
    CHECK_EQ(s.m.frame_allocator().ref_count(pte->frame), 1);
}

TEST(cow_write_both_write_then_frames_balance) {
    // Both write the same page, then both exit: every frame comes back.
    System s;
    const uint32_t before = s.m.frame_allocator().free_count();
    Asm a;
    a.syscall(SYS_FORK);
    a.emit({SW, 0, sp, a0, -4});            // runs in both
    a.emit({LW, a0, sp, 0, -4});
    a.exit_with_a0();
    const uint32_t parent = s.spawn(elf(a));
    s.run();
    CHECK_EQ(s.status(parent), 2);          // each sees its own write
    CHECK_EQ(s.status(2), 0);
    CHECK_EQ(s.m.frame_allocator().free_count(), before);
}

TEST(cow_write_no_free_frame_kills_the_writer) {
    // 15 processes x 17 frames + 1 heap page = all 256 frames. After fork the
    // child stores to the stack: there is no frame for the copy, so it is
    // killed by the store page fault. The parent is unaffected.
    System s;
    Asm forker;
    forker.sbrk(4096);
    forker.emit({SW, 0, a0, zero, 0});      // takes the last free frame
    const size_t to_child = fork_and_branch_to_child(forker);
    forker.syscall(SYS_YIELD);
    forker.exit_with(0);                    // parent: no store
    forker.patch(to_child, forker.here());
    forker.emit({SW, 0, sp, zero, -4});     // needs a copy: none left
    forker.exit_with(1);
    const uint32_t pid = s.spawn(elf(forker));
    Asm idle;
    idle.busy_wait(2000);                   // keep their frames until after the fork
    idle.exit_with(0);
    for (int i = 0; i < 14; i++) s.spawn(elf(idle));
    s.run();
    CHECK(killed_by_store_fault(s, 16));
    CHECK_EQ(s.status(pid), 0);
    CHECK_EQ(s.m.frame_allocator().free_count(), mmu::kNumFrames);   // nothing leaked
}

TEST(cow_write_store_to_code_still_kills) {
    // A read-only page that was never writable is not cow: storing is a real fault.
    System s;
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    a.exit_with(0);
    a.patch(to_child, a.here());
    a.li(t0, 0x10000);
    a.emit({SW, 0, t0, zero, 0});
    a.exit_with(1);
    s.spawn(elf(a));
    s.run();
    CHECK(killed_by_store_fault(s, 2));
}

TEST(cow_write_kernel_store_breaks_cow) {
    // wait stores the status into a stack page the parent still shares with a
    // live child (B). The kernel must copy first: B keeps the old value, the
    // parent gets the status and keeps the rest of the page.
    System s;
    Asm a;
    a.li(t1, 77);
    a.emit({SW, 0, sp, t1, -8});            // status slot, before any fork
    a.li(t1, 100);
    a.emit({SW, 0, sp, t1, -12});           // a neighbour on the same page
    const size_t to_b = fork_and_branch_to_child(a);      // B = pid 2
    const size_t to_a = fork_and_branch_to_child(a);      // A = pid 3
    a.li(a0, 3);
    a.emit({ADDI, a1, sp, 0, -8});
    a.syscall(SYS_WAIT);                    // parent: status of A into sp - 8
    a.emit({LW, t1, sp, 0, -8});            // 5
    a.emit({LW, t2, sp, 0, -12});           // 100
    a.emit({ADD, a0, t1, t2, 0});
    a.exit_with_a0();
    a.patch(to_a, a.here());
    a.exit_with(5);                         // A
    a.patch(to_b, a.here());
    a.busy_wait(3000);                      // B: still alive during the wait
    a.emit({LW, a0, sp, 0, -8});            // the old 77, not A's status
    a.exit_with_a0();
    const uint32_t parent = s.spawn(elf(a));
    s.run();
    CHECK_EQ(s.status(parent), 105);
    CHECK_EQ(s.status(2), 77);
    CHECK_EQ(s.m.frame_allocator().free_count(), mmu::kNumFrames);
}
