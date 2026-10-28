// Parent links and wait (a7 = 8), spec B3. Three groups, one per step:
//
//   parent_       5.2  each process knows its parent; orphans get parent 0
//   wait_ended_   5.4  wait when a matching child has already ended
//   wait_blocks_  5.5  wait when the child is still running: the caller sleeps
//
// wait(a0 = pid or -1 for any child, a1 = address for the status or 0):
//   - returns the pid of a child of the caller that has ended, and stores its
//     status at a1: the exit status, or 128 + trap cause if it was killed.
//     That child is then collected: no later wait returns it again.
//   - no child of the caller matches a0 (ended or running): -ECHILD at once.
//   - a1 can't be written: -EFAULT, and the child is not collected.
//   - matching children exist but none has ended: the caller blocks until one does.
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

constexpr uint32_t kData = 0x20000;   // read-only bytes: letters to print
constexpr int32_t kEchild = 10, kEfault = 14;

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

    uint32_t spawn(const Asm& a, std::vector<uint8_t> data = {}) {
        const auto r = k.spawn(elf(a, std::move(data)));
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

// fork; if a0 == 0 goto child. Returns the branch to patch with the child's code.
size_t fork_and_branch_to_child(Asm& a) {
    a.syscall(SYS_FORK);
    return a.branch(BEQ, a0, zero);
}

// wait(pid, sp - 8): the status goes to the stack. Afterwards s1 = returned
// value and s2 = stored status.
void wait_into_s1_s2(Asm& a, int32_t pid) {
    a.li(a0, pid);
    a.emit({ADDI, a1, sp, 0, -8});
    a.syscall(SYS_WAIT);
    a.emit({ADDI, s1, a0, 0, 0});
    a.emit({LW, s2, sp, 0, -8});
}

// exit(s1 * 1000 + s2): both values in one exit status.
void exit_with_s1_and_s2(Asm& a) {
    a.li(t0, 1000);
    a.emit({MUL, t1, s1, t0, 0});
    a.emit({ADD, a0, t1, s2, 0});
    a.exit_with_a0();
}

void spin_forever(Asm& a) { a.emit({JAL, zero, 0, 0, 0}); }

}  // namespace

// =================== 5.2: parent links ===================

TEST(parent_of_spawned_process_is_zero) {
    System s;
    Asm a;
    spin_forever(a);
    const uint32_t pid = s.spawn(a);
    const kernel::Process* p = s.k.process(pid);
    CHECK(p != nullptr);
    if (p) CHECK_EQ(p->parent_pid, 0);
}

TEST(parent_of_forked_child_is_the_forker) {
    System s;
    Asm a;
    a.syscall(SYS_FORK);
    a.syscall(SYS_FORK);    // runs in both: pid 1 -> 2 and 3, pid 2 -> 4
    spin_forever(a);
    s.spawn(a);
    CHECK(std::holds_alternative<kernel::StepLimit>(s.k.run_all(20000)));
    const uint32_t expected_parent[] = {0, 0, 1, 1, 2};   // index = pid
    for (uint32_t pid = 1; pid <= 4; pid++) {
        const kernel::Process* p = s.k.process(pid);
        CHECK(p != nullptr);
        if (p) CHECK_EQ(p->parent_pid, expected_parent[pid]);
    }
}

TEST(parent_ends_first_child_becomes_orphan) {
    System s;
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    a.exit_with(0);                       // parent ends right away
    a.patch(to_child, a.here());
    spin_forever(a);                      // child keeps running
    s.spawn(a);
    CHECK(std::holds_alternative<kernel::StepLimit>(s.k.run_all(20000)));
    const kernel::Process* child = s.k.process(2);
    CHECK(child != nullptr);
    if (child) CHECK_EQ(child->parent_pid, 0);
}

// =================== 5.4: wait, child already ended ===================
// The parent yields after fork, so the (short) child runs and ends first.

TEST(wait_ended_returns_pid_and_exit_status) {
    System s;
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    a.syscall(SYS_YIELD);                 // let the child finish
    wait_into_s1_s2(a, -1);
    exit_with_s1_and_s2(a);               // 2 * 1000 + 42
    a.patch(to_child, a.here());
    a.exit_with(42);
    const uint32_t parent = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(parent), 2042);
}

TEST(wait_ended_negative_exit_status) {
    System s;
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    a.syscall(SYS_YIELD);
    wait_into_s1_s2(a, -1);
    a.emit({ADDI, a0, s2, 0, 0});
    a.exit_with_a0();
    a.patch(to_child, a.here());
    a.exit_with(-5);
    const uint32_t parent = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(parent), -5);
}

TEST(wait_ended_killed_child_status_is_128_plus_cause) {
    System s;
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    a.syscall(SYS_YIELD);
    wait_into_s1_s2(a, -1);
    a.emit({ADDI, a0, s2, 0, 0});
    a.exit_with_a0();
    a.patch(to_child, a.here());
    a.emit({EBREAK});                     // killed: breakpoint, cause 3
    const uint32_t parent = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(parent), 131);
}

TEST(wait_ended_for_a_specific_pid) {
    // Two children: pid 2 exits with 1, pid 3 with 2. Wait for 3 first, then 2.
    System s;
    Asm a;
    const size_t to_first = fork_and_branch_to_child(a);
    const size_t to_second = fork_and_branch_to_child(a);
    a.syscall(SYS_YIELD);
    a.syscall(SYS_YIELD);
    wait_into_s1_s2(a, 3);
    a.emit({ADDI, s3, s1, 0, 0});         // 3
    a.emit({ADDI, s4, s2, 0, 0});         // 2
    wait_into_s1_s2(a, 2);                // s1 = 2, s2 = 1
    // exit(3*1000 + 2*100 + 2*10 + 1) = 3221
    a.li(t0, 1000);
    a.emit({MUL, a0, s3, t0, 0});
    a.li(t0, 100);
    a.emit({MUL, t1, s4, t0, 0});
    a.emit({ADD, a0, a0, t1, 0});
    a.li(t0, 10);
    a.emit({MUL, t1, s1, t0, 0});
    a.emit({ADD, a0, a0, t1, 0});
    a.emit({ADD, a0, a0, s2, 0});
    a.exit_with_a0();
    a.patch(to_first, a.here());          // pid 2 jumps here right after the first fork
    a.exit_with(1);
    a.patch(to_second, a.here());         // pid 3, from the second fork
    a.exit_with(2);
    const uint32_t parent = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(parent), 3221);
}

TEST(wait_ended_without_status_address) {
    // a1 = 0: nothing is stored, the pid is still returned.
    System s;
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    a.syscall(SYS_YIELD);
    a.li(a0, -1);
    a.li(a1, 0);
    a.syscall(SYS_WAIT);
    a.exit_with_a0();
    a.patch(to_child, a.here());
    a.exit_with(42);
    const uint32_t parent = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(parent), 2);
}

TEST(wait_ended_child_is_collected_only_once) {
    System s;
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    a.syscall(SYS_YIELD);
    wait_into_s1_s2(a, -1);               // collects pid 2
    wait_into_s1_s2(a, -1);               // nothing left: -ECHILD
    a.emit({ADDI, a0, s1, 0, 0});
    a.exit_with_a0();
    a.patch(to_child, a.here());
    a.exit_with(0);
    const uint32_t parent = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(parent), -kEchild);
}

TEST(wait_ended_no_children_is_echild) {
    System s;
    Asm a;
    wait_into_s1_s2(a, -1);
    a.emit({ADDI, a0, s1, 0, 0});
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(pid), -kEchild);
}

TEST(wait_ended_only_own_children) {
    // pid 1 waits for pid 2, which is still running but isn't its child:
    // -ECHILD at once. (Blocking here would wait for someone else's process.)
    System s;
    Asm waiter;
    wait_into_s1_s2(waiter, 2);
    waiter.emit({ADDI, a0, s1, 0, 0});
    waiter.exit_with_a0();
    Asm other;
    other.busy_wait(1500);
    other.exit_with(0);
    const uint32_t pid = s.spawn(waiter);
    s.spawn(other);
    s.run();
    CHECK_EQ(s.status(pid), -kEchild);
}

TEST(wait_ended_not_grandchildren) {
    // pid 1 forks pid 2, which forks pid 3. pid 1 can't wait for pid 3.
    System s;
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    a.syscall(SYS_YIELD);
    a.syscall(SYS_YIELD);
    wait_into_s1_s2(a, 3);
    a.emit({ADDI, a0, s1, 0, 0});
    a.exit_with_a0();
    a.patch(to_child, a.here());
    a.syscall(SYS_FORK);                  // pid 2 forks pid 3
    a.exit_with(0);
    const uint32_t parent = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(parent), -kEchild);
}

TEST(wait_ended_status_into_untouched_heap) {
    // The status address is a heap page nobody touched: the kernel maps it.
    System s;
    Asm a;
    a.sbrk(4096);
    a.emit({ADDI, s3, a0, 0, 0});
    const size_t to_child = fork_and_branch_to_child(a);
    a.syscall(SYS_YIELD);
    a.li(a0, -1);
    a.emit({ADDI, a1, s3, 0, 0});
    a.syscall(SYS_WAIT);
    a.emit({LW, a0, s3, 0, 0});
    a.exit_with_a0();
    a.patch(to_child, a.here());
    a.exit_with(77);
    const uint32_t parent = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(parent), 77);
}

TEST(wait_ended_bad_status_address_is_efault_and_not_collected) {
    // Code is read-only: storing the status there fails. The child stays
    // uncollected, so a second wait with a good address still gets it.
    System s;
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    a.syscall(SYS_YIELD);
    a.li(a0, -1);
    a.li(a1, 0x10000);                    // the code page
    a.syscall(SYS_WAIT);
    a.emit({ADDI, s3, a0, 0, 0});         // -EFAULT
    wait_into_s1_s2(a, -1);               // s1 = 2, s2 = 9
    // exit(-s3 * 10000 + s1 * 1000 + s2) = 14 * 10000 + 2009 = 142009
    a.li(t0, -10000);
    a.emit({MUL, a0, s3, t0, 0});
    a.li(t0, 1000);
    a.emit({MUL, t1, s1, t0, 0});
    a.emit({ADD, a0, a0, t1, 0});
    a.emit({ADD, a0, a0, s2, 0});
    a.exit_with_a0();
    a.patch(to_child, a.here());
    a.exit_with(9);
    const uint32_t parent = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(parent), kEfault * 10000 + 2009);
}

// =================== 5.5: wait blocks ===================

TEST(wait_blocks_until_child_exits) {
    // The parent waits right after fork; the child works for longer than a
    // quantum first. The parent must sleep, then get the child's status.
    System s;
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    wait_into_s1_s2(a, -1);
    exit_with_s1_and_s2(a);               // 2 * 1000 + 7
    a.patch(to_child, a.here());
    a.busy_wait(1500);                    // ~3000 instructions
    a.exit_with(7);
    const uint32_t parent = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(parent), 2007);
}

TEST(wait_blocks_parent_does_not_run_while_waiting) {
    // Child prints C, works, prints C. Parent prints P only after wait returns.
    System s;
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    wait_into_s1_s2(a, -1);
    a.write_stdout(kData, 1);             // "P"
    a.exit_with(0);
    a.patch(to_child, a.here());
    a.write_stdout(kData + 1, 1);         // "C"
    a.busy_wait(1500);
    a.write_stdout(kData + 1, 1);
    a.exit_with(0);
    s.spawn(a, {'P', 'C'});
    s.run();
    CHECK_EQ(s.out.str(), std::string("CCP"));
}

TEST(wait_blocks_returns_the_child_that_ends_first) {
    // Child pid 2 works long, child pid 3 exits at once: wait(-1) returns 3.
    System s;
    Asm a;
    const size_t to_slow = fork_and_branch_to_child(a);
    const size_t to_fast = fork_and_branch_to_child(a);
    wait_into_s1_s2(a, -1);
    exit_with_s1_and_s2(a);               // 3 * 1000 + 5
    a.patch(to_slow, a.here());
    a.busy_wait(3000);
    a.exit_with(4);
    a.patch(to_fast, a.here());
    a.exit_with(5);
    const uint32_t parent = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(parent), 3005);
}

TEST(wait_blocks_chain_of_waits) {
    // pid 1 waits for pid 2, which waits for pid 3. pid 3 exits with 3;
    // pid 2 exits with its child's status + 10; pid 1 gets 13.
    System s;
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    wait_into_s1_s2(a, -1);
    a.emit({ADDI, a0, s2, 0, 0});
    a.exit_with_a0();
    a.patch(to_child, a.here());
    const size_t to_grandchild = fork_and_branch_to_child(a);
    wait_into_s1_s2(a, -1);
    a.emit({ADDI, a0, s2, 0, 10});
    a.exit_with_a0();
    a.patch(to_grandchild, a.here());
    a.busy_wait(1000);
    a.exit_with(3);
    const uint32_t parent = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(parent), 13);
}

TEST(wait_blocks_others_keep_running) {
    // While pid 1 is blocked, an unrelated process still gets turns.
    System s;
    Asm parent;
    const size_t to_child = fork_and_branch_to_child(parent);
    wait_into_s1_s2(parent, -1);
    parent.exit_with(0);
    parent.patch(to_child, parent.here());
    parent.busy_wait(3000);
    parent.exit_with(0);
    Asm other;
    other.write_stdout(kData, 1);
    other.exit_with(0);
    s.spawn(parent);
    s.spawn(other, {'X'});
    s.run();
    CHECK_EQ(s.out.str(), std::string("X"));
}

// =================== 5.6: real blocking (the caller sleeps) ===================
// A wait with nothing to collect marks the caller blocked: the scheduler skips
// it and it runs no instructions. The end of a matching child (exit or kill)
// makes it runnable; then its wait runs again and collects that child.

namespace {

// Parent forks and waits for any child; the child busy-waits, then exits 0.
// Returns how many instructions the whole run took.
uint64_t steps_with_busy_child(int32_t iterations) {
    System s;
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    wait_into_s1_s2(a, -1);
    a.exit_with(0);
    a.patch(to_child, a.here());
    a.busy_wait(iterations);
    a.exit_with(0);
    s.spawn(a);
    s.run();
    return s.m.steps();
}

}  // namespace

TEST(wait_sleeps_parent_is_blocked_while_child_runs) {
    System s;
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    wait_into_s1_s2(a, -1);
    a.exit_with(0);
    a.patch(to_child, a.here());
    a.emit({JAL, zero, 0, 0, 0});         // child loops forever
    s.spawn(a);
    CHECK(std::holds_alternative<kernel::StepLimit>(s.k.run_all(5000)));
    const kernel::Process* parent = s.k.process(1);
    const kernel::Process* child = s.k.process(2);
    CHECK(parent != nullptr && child != nullptr);
    if (!parent || !child) return;
    CHECK(parent->blocked == kernel::BlockReason::Wait);
    CHECK(child->blocked == kernel::BlockReason::None);
}

TEST(wait_sleeps_blocked_parent_runs_no_instructions) {
    // The child's loop is 2 instructions per iteration. Making it 4000
    // iterations longer must cost exactly 8000 more instructions: a blocked
    // parent adds none, however many turns the child takes.
    const uint64_t short_run = steps_with_busy_child(3000);
    const uint64_t long_run = steps_with_busy_child(7000);
    CHECK_EQ(long_run - short_run, 8000u);
}

TEST(wait_sleeps_child_exit_makes_parent_runnable) {
    // After the child exits, the parent collects it and loops: not blocked.
    System s;
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    wait_into_s1_s2(a, -1);
    a.emit({JAL, zero, 0, 0, 0});         // parent loops after its wait
    a.patch(to_child, a.here());
    a.busy_wait(1500);
    a.exit_with(0);
    s.spawn(a);
    CHECK(std::holds_alternative<kernel::StepLimit>(s.k.run_all(20000)));
    const kernel::Process* parent = s.k.process(1);
    CHECK(parent != nullptr);
    if (!parent) return;
    CHECK(parent->blocked == kernel::BlockReason::None);
    const auto ending = s.k.ending(2);
    CHECK(ending.has_value() && ending->collected);
}

TEST(wait_sleeps_other_child_does_not_wake) {
    // The parent waits for pid 3, which loops forever. pid 2 exiting must not
    // wake it.
    System s;
    Asm a;
    const size_t to_two = fork_and_branch_to_child(a);
    const size_t to_three = fork_and_branch_to_child(a);
    wait_into_s1_s2(a, 3);
    a.exit_with(0);
    a.patch(to_two, a.here());
    a.exit_with(0);                       // pid 2 ends at once
    a.patch(to_three, a.here());
    a.emit({JAL, zero, 0, 0, 0});         // pid 3 never ends
    s.spawn(a);
    CHECK(std::holds_alternative<kernel::StepLimit>(s.k.run_all(5000)));
    CHECK(s.k.ending(2).has_value());
    const kernel::Process* parent = s.k.process(1);
    CHECK(parent != nullptr);
    if (parent) CHECK(parent->blocked == kernel::BlockReason::Wait);
}

TEST(wait_sleeps_grandchild_end_does_not_wake) {
    // pid 1 waits for its child pid 2, which loops. pid 2's own child (pid 3)
    // exits: that is not pid 1's child, so pid 1 stays blocked.
    System s;
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    wait_into_s1_s2(a, -1);
    a.exit_with(0);
    a.patch(to_child, a.here());
    const size_t to_grandchild = fork_and_branch_to_child(a);
    a.emit({JAL, zero, 0, 0, 0});         // pid 2 loops
    a.patch(to_grandchild, a.here());
    a.exit_with(0);                       // pid 3 ends
    s.spawn(a);
    CHECK(std::holds_alternative<kernel::StepLimit>(s.k.run_all(5000)));
    CHECK(s.k.ending(3).has_value());
    const kernel::Process* parent = s.k.process(1);
    CHECK(parent != nullptr);
    if (parent) CHECK(parent->blocked == kernel::BlockReason::Wait);
}

TEST(wait_sleeps_killed_child_wakes_parent) {
    // The child is killed by a store to the code page (store page fault, 15)
    // while the parent sleeps: status 128 + 15.
    System s;
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    wait_into_s1_s2(a, -1);
    exit_with_s1_and_s2(a);               // 2 * 1000 + 143
    a.patch(to_child, a.here());
    a.busy_wait(1500);                    // the parent is asleep by now
    a.li(t0, 0x10000);
    a.emit({SW, 0, t0, zero, 0});
    a.exit_with(0);
    const uint32_t parent = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(parent), 2143);
}

TEST(wait_sleeps_wakeup_with_bad_address_is_efault) {
    // The parent sleeps in wait(-1, code page). When the child ends, the retried
    // wait can't store: -EFAULT, child not collected. A second wait gets it.
    System s;
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    a.li(a0, -1);
    a.li(a1, 0x10000);
    a.syscall(SYS_WAIT);
    a.emit({ADDI, s3, a0, 0, 0});         // -EFAULT
    wait_into_s1_s2(a, -1);               // s1 = 2, s2 = 9
    a.li(t0, -10000);
    a.emit({MUL, a0, s3, t0, 0});
    a.li(t0, 1000);
    a.emit({MUL, t1, s1, t0, 0});
    a.emit({ADD, a0, a0, t1, 0});
    a.emit({ADD, a0, a0, s2, 0});
    a.exit_with_a0();
    a.patch(to_child, a.here());
    a.busy_wait(1500);
    a.exit_with(9);
    const uint32_t parent = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(parent), kEfault * 10000 + 2009);
}
