// Threads (spec Part E): several threads of control in one process.
//
//   thread_create_  a new thread's first registers; ids
//   thread_shared_  memory, the break and fds are shared; registers and stacks aren't
//   thread_join_    collecting a thread's status; its errors
//   thread_end_     thread_exit ends one thread; exit or a fault ends them all
//   thread_sched_   kernel threads: each is scheduled and blocked on its own
//   thread_fork_    fork copies only the caller; any thread can wait for the child
//   thread_exec_    exec ends the other threads
//   thread_swap_    two threads, one page table, swapping
//   thread_race_    a race condition, made on purpose
//
// A program starts with `to_main`, a jump over the thread bodies. A body's entry
// address is known once it is written, so main comes after the bodies.
#include <set>
#include <string>
#include <vector>

#include "cpu_helpers.hpp"
#include "fs_system.hpp"

using namespace fs_system;

namespace {

constexpr int32_t kEsrch = 3, kEchild = 10;
constexpr int32_t kPage = 4096;
constexpr uint64_t kShort = 2'000'000;         // for programs with no long disk waits
constexpr uint64_t kForever = 4'000'000'000;
constexpr int32_t kBad = 100;                  // exit codes above this: a check failed

uint32_t here_addr(const Asm& a) { return 0x10000 + static_cast<uint32_t>(a.here()) * 4; }

// Jumps over the bodies; patch it when main starts.
size_t jump_over_bodies(Asm& a) { return a.branch(JAL, zero, zero); }

// tid in a0. stack and arg are registers (zero: don't care).
void thread_create(Asm& a, uint32_t entry, uint8_t stack, uint8_t arg) {
    a.emit({ADDI, a1, stack, 0, 0});
    a.emit({ADDI, a2, arg, 0, 0});
    a.li(a0, static_cast<int32_t>(entry));
    a.syscall(SYS_THREAD_CREATE);
}

void thread_exit(Asm& a, int32_t status) {
    a.li(a0, status);
    a.syscall(SYS_THREAD_EXIT);
}

void thread_exit_with_a0(Asm& a) { a.syscall(SYS_THREAD_EXIT); }

// thread_join(tid, status_addr): a0 = tid or -error.
void join(Asm& a, uint8_t tid, uint32_t status_addr) {
    a.emit({ADDI, a0, tid, 0, 0});
    a.li(a1, static_cast<int32_t>(status_addr));
    a.syscall(SYS_THREAD_JOIN);
}

// If r != value, the process exits with code.
void expect(Asm& a, uint8_t r, int32_t value, int32_t code) {
    a.li(t6, value);
    const size_t ok = a.branch(BEQ, r, t6);
    a.exit_with(code);
    a.patch(ok, a.here());
}

// If r != other, the process exits with code.
void expect_same(Asm& a, uint8_t r, uint8_t other, int32_t code) {
    const size_t ok = a.branch(BEQ, r, other);
    a.exit_with(code);
    a.patch(ok, a.here());
}

// Joins tid (it must succeed); a0 = its status.
void join_status(Asm& a, uint8_t tid) {
    join(a, tid, kBuf);
    expect_same(a, a0, tid, kBad);
    a.li(t6, static_cast<int32_t>(kBuf));
    a.emit({LW, a0, t6, 0, 0});
}

// s1 = a new heap of `pages` pages.
void heap(Asm& a, int32_t pages) {
    a.sbrk(pages * kPage);
    a.emit({ADDI, s1, a0, 0, 0});
}

// write(1, letter's address, 1); uses a0..a2.
void print(Asm& a, uint32_t letter) { a.write_stdout(letter, 1); }

// Loops forever.
void spin(Asm& a) {
    const size_t loop = a.here();
    a.patch(a.branch(JAL, zero, zero), loop);
}

// disk_read(sector, buffer in r, 1).
void disk_read(Asm& a, uint8_t buffer) {
    a.emit({ADDI, a1, buffer, 0, 0});
    a.li(a0, 100);
    a.li(a2, 1);
    a.syscall(SYS_DISK_READ);
}

// Runs one program to the end; its status.
int32_t run_one(System& s, const Asm& a, const Strings& str = {}, uint64_t limit = kShort) {
    const uint32_t pid = s.spawn(a, str.bytes);
    CHECK(std::holds_alternative<kernel::AllDone>(s.k.run_all(limit)));
    return s.status(pid);
}

int32_t run_one(const Asm& a) {
    System s;
    return run_one(s, a);
}

void put(System& s, const std::string& name, const Asm& a) {
    CHECK(std::holds_alternative<uint32_t>(fs::create_file(s.m.disk(), fs::kRootInode, name, elf(a, {}))));
}

// Exits with 7 and does nothing else: the program exec'd below.
Asm exits_7() {
    Asm b;
    b.exit_with(7);
    return b;
}

}  // namespace

// =================== thread_create ===================

TEST(thread_create_starts_with_entry_stack_and_arg) {
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    expect(a, a0, 0x1234, 101);
    expect(a, sp, 0x5000, 102);
    expect(a, s1, 0, 103);                  // main's s1 isn't copied
    expect(a, ra, 0, 104);
    thread_exit(a, 7);
    a.patch(to_main, a.here());
    a.li(s1, 99);
    a.li(s2, 0x5000);
    a.li(s3, 0x1234);
    thread_create(a, body, s2, s3);
    a.emit({ADDI, s4, a0, 0, 0});
    join_status(a, s4);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 7);
}

TEST(thread_create_caller_goes_on_after_the_ecall) {
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    thread_exit(a, 0);
    a.patch(to_main, a.here());
    a.li(s1, 5);
    thread_create(a, body, zero, zero);
    a.emit({ADDI, s4, a0, 0, 0});
    join(a, s4, 0);
    a.emit({ADDI, a0, s1, 0, 0});           // the caller's registers are as they were
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 5);
}

TEST(thread_create_ids_and_a_shared_pid) {
    // The thread stores getpid and gettid; main compares them with its own.
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    a.emit({ADDI, s1, a0, 0, 0});
    a.syscall(SYS_GETPID);
    a.emit({SW, 0, s1, a0, 0});
    a.syscall(SYS_GETTID);
    a.emit({SW, 0, s1, a0, 4});
    thread_exit(a, 0);
    a.patch(to_main, a.here());
    heap(a, 1);
    thread_create(a, body, zero, s1);
    a.emit({ADDI, s4, a0, 0, 0});
    join(a, s4, 0);
    a.syscall(SYS_GETPID);
    a.emit({ADDI, s5, a0, 0, 0});
    a.emit({LW, t0, s1, 0, 0});
    expect_same(a, t0, s5, 1);              // same pid
    a.emit({LW, t0, s1, 0, 4});
    expect_same(a, t0, s4, 2);              // its id is what thread_create returned
    a.syscall(SYS_GETTID);
    expect_same(a, a0, s5, 3);              // the first thread's id is the pid
    const size_t differ = a.branch(BNE, s4, s5);
    a.exit_with(4);
    a.patch(differ, a.here());
    a.exit_with(0);
    CHECK_EQ(run_one(a), 0);
}

// =================== what is shared ===================

TEST(thread_shared_heap_both_ways) {
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    a.emit({LW, t0, a0, 0, 0});             // sees main's 5
    a.emit({ADDI, t0, t0, 0, 1});
    a.emit({SW, 0, a0, t0, 0});
    thread_exit(a, 0);
    a.patch(to_main, a.here());
    heap(a, 1);
    a.li(t0, 5);
    a.emit({SW, 0, s1, t0, 0});
    thread_create(a, body, zero, s1);
    a.emit({ADDI, s4, a0, 0, 0});
    join(a, s4, 0);
    a.emit({LW, a0, s1, 0, 0});             // sees the thread's 6
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 6);
}

TEST(thread_shared_page_touched_first_by_the_thread) {
    // Lazily mapped once, for both: main sees it, with no fault that loses data.
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    a.li(t0, 9);
    a.emit({SW, 0, a0, t0, 0});
    thread_exit(a, 0);
    a.patch(to_main, a.here());
    heap(a, 1);
    thread_create(a, body, zero, s1);
    a.emit({ADDI, s4, a0, 0, 0});
    join(a, s4, 0);
    a.emit({LW, a0, s1, 0, 0});
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 9);
}

TEST(thread_shared_break) {
    // The thread grows the heap; main sees the new break and can use the page.
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    a.emit({ADDI, s1, a0, 0, 0});
    a.sbrk(kPage);
    a.emit({SW, 0, s1, a0, 0});             // the old break: where the new page starts
    thread_exit(a, 0);
    a.patch(to_main, a.here());
    heap(a, 1);
    thread_create(a, body, zero, s1);
    a.emit({ADDI, s4, a0, 0, 0});
    join(a, s4, 0);
    a.emit({LW, s5, s1, 0, 0});
    a.sbrk(0);
    a.li(t0, kPage);
    a.emit({ADD, t0, s5, t0, 0});
    expect_same(a, a0, t0, 1);
    a.li(t0, 8);
    a.emit({SW, 0, s5, t0, 0});
    a.emit({LW, a0, s5, 0, 0});
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 8);
}

TEST(thread_shared_fds) {
    // The thread opens a file; main reads it and closes it with the same fd.
    System s;
    Strings str;
    const uint32_t path = str.add("/hello");
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    open(a, path);
    thread_exit_with_a0(a);
    a.patch(to_main, a.here());
    thread_create(a, body, zero, zero);
    a.emit({ADDI, s4, a0, 0, 0});
    join_status(a, s4);
    expect(a, a0, 3, 1);                    // the fd
    read(a, 3, kBuf, 2);
    expect(a, a0, 2, 2);
    close(a, 3);
    expect(a, a0, 0, 3);
    open(a, path);                          // fd 3 is free again, for all threads
    a.exit_with_a0();
    CHECK_EQ(run_one(s, a, str, kForever), 3);
}

TEST(thread_shared_ending_thread_releases_nothing) {
    // A thread ends: its process's memory and fds stay. They come back only with
    // the process.
    System s;
    Strings str;
    const uint32_t path = str.add("/hello");
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    a.li(t0, 6);
    a.emit({SW, 0, a0, t0, 0});
    thread_exit(a, 0);
    a.patch(to_main, a.here());
    heap(a, 1);
    open(a, path);
    thread_create(a, body, zero, s1);
    a.emit({ADDI, s4, a0, 0, 0});
    join(a, s4, 0);
    read(a, 3, kBuf, 2);
    expect(a, a0, 2, 1);                    // the fd is still open
    a.emit({LW, a0, s1, 0, 0});
    a.exit_with_a0();
    CHECK_EQ(run_one(s, a, str, kForever), 6);
}

TEST(thread_shared_not_the_registers) {
    // Each thread counts to 2000 in s3, past many timer interrupts, with its id
    // in s2. A switch that mixes up registers shows in the sum.
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    a.emit({ADDI, s2, a0, 0, 0});
    a.li(s3, 0);
    a.li(s4, 2000);
    const size_t loop = a.here();
    a.emit({ADDI, s3, s3, 0, 1});
    a.patch(a.branch(BNE, s3, s4), loop);
    a.emit({ADD, a0, s2, s3, 0});
    thread_exit_with_a0(a);
    a.patch(to_main, a.here());
    a.li(s5, 1);
    thread_create(a, body, zero, s5);
    a.emit({ADDI, s6, a0, 0, 0});
    a.li(s5, 2);
    thread_create(a, body, zero, s5);
    a.emit({ADDI, s7, a0, 0, 0});
    join_status(a, s6);
    expect(a, a0, 2001, 1);
    join_status(a, s7);
    expect(a, a0, 2002, 2);
    a.exit_with(0);
    CHECK_EQ(run_one(a), 0);
}

TEST(thread_shared_stacks_are_private_but_reachable) {
    // Each thread uses its own stack. Main can still read a thread's stack: it is
    // ordinary memory of the process, with no protection.
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    a.emit({SW, 0, sp, a0, -4});
    a.busy_wait(1500);                      // the other thread runs in between
    a.emit({LW, a0, sp, 0, -4});
    thread_exit_with_a0(a);
    a.patch(to_main, a.here());
    heap(a, 2);
    a.li(t0, kPage);
    a.emit({ADD, s2, s1, t0, 0});           // top of the first stack
    a.emit({ADD, s3, s2, t0, 0});           // top of the second
    a.li(s5, 11);
    thread_create(a, body, s2, s5);
    a.emit({ADDI, s6, a0, 0, 0});
    a.li(s5, 22);
    thread_create(a, body, s3, s5);
    a.emit({ADDI, s7, a0, 0, 0});
    join_status(a, s6);
    expect(a, a0, 11, 1);
    join_status(a, s7);
    expect(a, a0, 22, 2);
    a.emit({LW, a0, s2, 0, -4});
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 11);
}

// =================== thread_join ===================

TEST(thread_join_blocks_until_the_thread_ends) {
    System s;
    Strings str;
    const uint32_t w = str.add("W"), m = str.add("M");
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    a.busy_wait(3000);                      // a few quanta
    print(a, w);
    thread_exit(a, 0);
    a.patch(to_main, a.here());
    thread_create(a, body, zero, zero);
    a.emit({ADDI, s4, a0, 0, 0});
    join(a, s4, 0);
    print(a, m);
    a.exit_with(0);
    CHECK_EQ(run_one(s, a, str), 0);
    CHECK_EQ(s.out.str(), std::string("WM"));
}

TEST(thread_join_a_thread_that_already_ended) {
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    thread_exit(a, 7);
    a.patch(to_main, a.here());
    thread_create(a, body, zero, zero);
    a.emit({ADDI, s4, a0, 0, 0});
    a.busy_wait(3000);                      // it has ended by now
    join_status(a, s4);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 7);
}

TEST(thread_join_status_may_be_skipped) {
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    thread_exit(a, 7);
    a.patch(to_main, a.here());
    thread_create(a, body, zero, zero);
    a.emit({ADDI, s4, a0, 0, 0});
    join(a, s4, 0);
    expect_same(a, a0, s4, 1);
    a.exit_with(0);
    CHECK_EQ(run_one(a), 0);
}

TEST(thread_join_collects_once) {
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    thread_exit(a, 7);
    a.patch(to_main, a.here());
    thread_create(a, body, zero, zero);
    a.emit({ADDI, s4, a0, 0, 0});
    join(a, s4, 0);
    join(a, s4, 0);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), -kEsrch);
}

TEST(thread_join_unknown_id_is_esrch) {
    Asm a;
    a.li(s4, 999);
    join(a, s4, 0);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), -kEsrch);
}

TEST(thread_join_thread_of_another_process_is_esrch) {
    // Process 1 is spinning in the table; process 2 tries to join it.
    System s;
    Asm one;
    one.busy_wait(3000);
    one.exit_with(0);
    s.spawn(one);
    Asm two;
    two.li(s4, 1);
    join(two, s4, 0);
    two.exit_with_a0();
    CHECK_EQ(run_one(s, two), -kEsrch);
}

TEST(thread_join_itself_is_einval) {
    Asm a;
    a.syscall(SYS_GETTID);
    a.emit({ADDI, s4, a0, 0, 0});
    join(a, s4, 0);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), -kEinval);
}

TEST(thread_join_bad_status_pointer_does_not_collect) {
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    thread_exit(a, 7);
    a.patch(to_main, a.here());
    thread_create(a, body, zero, zero);
    a.emit({ADDI, s4, a0, 0, 0});
    join(a, s4, 0x10);                      // not writable
    expect(a, a0, -kEfault, 1);
    join_status(a, s4);                     // still there
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 7);
}

TEST(thread_join_a_thread_can_join_the_first_thread) {
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    a.emit({ADDI, s4, a0, 0, 0});           // arg: the first thread's id
    join_status(a, s4);
    a.emit({ADDI, a0, a0, 0, 1});
    a.exit_with_a0();                       // 7 + 1
    a.patch(to_main, a.here());
    a.syscall(SYS_GETTID);
    a.emit({ADDI, s5, a0, 0, 0});
    thread_create(a, body, zero, s5);
    thread_exit(a, 7);
    CHECK_EQ(run_one(a), 8);
}

TEST(thread_join_each_other_is_a_deadlock) {
    // They stay blocked. Nothing crashes: the clock runs out.
    System s;
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    a.emit({ADDI, s4, a0, 0, 0});
    join(a, s4, 0);
    thread_exit(a, 0);
    a.patch(to_main, a.here());
    a.syscall(SYS_GETTID);
    a.emit({ADDI, s5, a0, 0, 0});
    thread_create(a, body, zero, s5);
    a.emit({ADDI, s4, a0, 0, 0});
    join(a, s4, 0);
    a.exit_with(0);
    const uint32_t pid = s.spawn(a);
    CHECK(std::holds_alternative<kernel::StepLimit>(s.k.run_all(200'000)));
    CHECK(!s.k.ending(pid).has_value());
}

// =================== how threads and processes end ===================

TEST(thread_end_first_thread_may_leave_the_others_running) {
    // The process ends with the last thread's status; all its frames come back.
    System s;
    Strings str;
    const uint32_t w = str.add("W");
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    a.busy_wait(3000);
    print(a, w);
    thread_exit(a, 9);
    a.patch(to_main, a.here());
    thread_create(a, body, zero, zero);
    thread_exit(a, 1);
    CHECK_EQ(run_one(s, a, str), 9);
    CHECK_EQ(s.out.str(), std::string("W"));
    CHECK_EQ(s.m.frame_allocator().free_count(), mmu::kNumFrames);
}

TEST(thread_end_a_single_thread_process_can_thread_exit) {
    System s;
    Asm a;
    thread_exit(a, 5);
    CHECK_EQ(run_one(s, a), 5);
    CHECK_EQ(s.m.frame_allocator().free_count(), mmu::kNumFrames);
}

TEST(thread_end_wait_collects_after_the_last_thread) {
    // The child's first thread ends early. wait must keep blocking until the
    // second one ends, and return its status.
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    a.busy_wait(3000);
    thread_exit(a, 5);
    a.patch(to_main, a.here());
    a.syscall(SYS_FORK);
    const size_t to_child = a.branch(BEQ, a0, zero);
    a.emit({ADDI, s4, a0, 0, 0});
    a.emit({ADDI, a0, s4, 0, 0});
    a.li(a1, static_cast<int32_t>(kBuf));
    a.syscall(SYS_WAIT);
    expect_same(a, a0, s4, 1);
    a.li(t0, static_cast<int32_t>(kBuf));
    a.emit({LW, a0, t0, 0, 0});
    a.exit_with_a0();
    a.patch(to_child, a.here());
    thread_create(a, body, zero, zero);
    thread_exit(a, 1);
    CHECK_EQ(run_one(a), 5);
}

TEST(thread_end_exit_ends_the_whole_process) {
    // A thread spinning forever ends with main's exit.
    System s;
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    spin(a);
    a.patch(to_main, a.here());
    thread_create(a, body, zero, zero);
    a.busy_wait(2000);
    a.exit_with(3);
    CHECK_EQ(run_one(s, a), 3);
    CHECK_EQ(s.k.process_count(), 0u);
    CHECK_EQ(s.m.frame_allocator().free_count(), mmu::kNumFrames);
}

TEST(thread_end_exit_from_any_thread_ends_a_blocked_first_thread) {
    // Main is blocked in join; the thread calls exit. Main prints nothing.
    System s;
    Strings str;
    const uint32_t m = str.add("M");
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    a.exit_with(4);
    a.patch(to_main, a.here());
    thread_create(a, body, zero, zero);
    a.emit({ADDI, s4, a0, 0, 0});
    join(a, s4, 0);
    print(a, m);
    a.exit_with(0);
    CHECK_EQ(run_one(s, a, str), 4);
    CHECK_EQ(s.out.str(), std::string(""));
}

TEST(thread_end_a_fault_kills_the_whole_process) {
    System s;
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    a.emit({LW, t0, zero, 0, 0});           // address 0: never accessible
    thread_exit(a, 0);
    a.patch(to_main, a.here());
    thread_create(a, body, zero, zero);
    spin(a);
    const uint32_t pid = s.spawn(a);
    CHECK(std::holds_alternative<kernel::AllDone>(s.k.run_all(kShort)));
    const auto e = s.k.ending(pid);
    CHECK(e.has_value());
    const auto* killed = e ? std::get_if<kernel::Killed>(&e->status) : nullptr;
    CHECK(killed != nullptr);
    if (killed) CHECK_EQ(std::get<cpu::Trap>(killed->reason).cause, cpu::Cause::load_page_fault);
    CHECK_EQ(s.m.frame_allocator().free_count(), mmu::kNumFrames);
}

TEST(thread_end_a_bad_entry_kills_the_whole_process) {
    System s;
    Asm a;
    thread_create(a, 0x40000000, zero, zero);    // nothing there
    spin(a);
    const uint32_t pid = s.spawn(a);
    CHECK(std::holds_alternative<kernel::AllDone>(s.k.run_all(kShort)));
    const auto e = s.k.ending(pid);
    CHECK(e.has_value());
    const auto* killed = e ? std::get_if<kernel::Killed>(&e->status) : nullptr;
    CHECK(killed != nullptr);
    if (killed) CHECK_EQ(std::get<cpu::Trap>(killed->reason).cause, cpu::Cause::instruction_page_fault);
}

TEST(thread_end_exit_with_a_sibling_waiting_on_the_disk) {
    // The disk is about to write into the sibling's page, so the process can't be
    // freed yet. It ends once the disk is done; the sibling never runs again.
    System s;
    Strings str;
    const uint32_t x = str.add("X");
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    disk_read(a, a0);
    print(a, x);
    thread_exit(a, 0);
    a.patch(to_main, a.here());
    heap(a, 1);
    thread_create(a, body, zero, s1);
    a.syscall(SYS_YIELD);                   // the sibling runs, and blocks
    a.exit_with(3);
    CHECK_EQ(run_one(s, a, str, kForever), 3);
    CHECK_EQ(s.out.str(), std::string(""));
    CHECK_EQ(s.m.disk().completed().size(), 1u);
    CHECK_EQ(s.m.frame_allocator().free_count(), mmu::kNumFrames);
}

// =================== scheduling: kernel threads ===================

TEST(thread_sched_a_blocked_thread_leaves_the_others_running) {
    // User threads would all stop here. With kernel threads, main prints first.
    System s;
    Strings str;
    const uint32_t x = str.add("A"), y = str.add("B");
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    disk_read(a, a0);
    print(a, x);
    thread_exit(a, 0);
    a.patch(to_main, a.here());
    heap(a, 1);
    thread_create(a, body, zero, s1);
    a.emit({ADDI, s4, a0, 0, 0});
    a.syscall(SYS_YIELD);
    print(a, y);
    join(a, s4, 0);
    a.exit_with(0);
    CHECK_EQ(run_one(s, a, str, kForever), 0);
    CHECK_EQ(s.out.str(), std::string("BA"));
}

TEST(thread_sched_the_timer_switches_between_threads) {
    // Main spins until the thread sets a flag: only preemption lets it run.
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    a.li(t0, 6);
    a.emit({SW, 0, a0, t0, 0});
    thread_exit(a, 0);
    a.patch(to_main, a.here());
    heap(a, 1);
    thread_create(a, body, zero, s1);
    const size_t loop = a.here();
    a.emit({LW, a0, s1, 0, 0});
    a.patch(a.branch(BEQ, a0, zero), loop);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 6);
}

TEST(thread_sched_yield_goes_to_the_next_thread) {
    System s;
    Strings str;
    const uint32_t x = str.add("A"), y = str.add("B"), z = str.add("C");
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    print(a, y);
    thread_exit(a, 0);
    a.patch(to_main, a.here());
    thread_create(a, body, zero, zero);
    a.emit({ADDI, s4, a0, 0, 0});
    print(a, x);
    a.syscall(SYS_YIELD);
    print(a, z);
    join(a, s4, 0);
    a.exit_with(0);
    CHECK_EQ(run_one(s, a, str), 0);
    CHECK_EQ(s.out.str(), std::string("ABC"));
}

// =================== fork and wait ===================

TEST(thread_fork_copies_only_the_caller) {
    // The worker forks. The child prints C and exits: it has no copy of main,
    // which would print a second M. Main (any thread) waits for it.
    System s;
    Strings str;
    const uint32_t c = str.add("C"), m = str.add("M");
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    a.emit({ADDI, s1, a0, 0, 0});
    a.syscall(SYS_FORK);
    const size_t to_child = a.branch(BEQ, a0, zero);
    a.emit({SW, 0, s1, a0, 0});             // the child's pid, for main
    thread_exit(a, 0);
    a.patch(to_child, a.here());
    print(a, c);
    a.exit_with(5);
    a.patch(to_main, a.here());
    heap(a, 1);
    thread_create(a, body, zero, s1);
    a.emit({ADDI, s4, a0, 0, 0});
    join(a, s4, 0);
    a.emit({LW, s5, s1, 0, 0});
    a.emit({ADDI, a0, s5, 0, 0});
    a.li(a1, static_cast<int32_t>(kBuf));
    a.syscall(SYS_WAIT);
    expect_same(a, a0, s5, 1);
    a.li(t0, static_cast<int32_t>(kBuf));
    a.emit({LW, s6, t0, 0, 0});
    expect(a, s6, 5, 2);
    print(a, m);
    a.exit_with(0);
    CHECK_EQ(run_one(s, a, str), 0);
    CHECK_EQ(s.out.str(), std::string("CM"));
}

TEST(thread_fork_child_memory_is_a_copy) {
    // The child's store goes to its own copy; the threads of the parent, which
    // share theirs, don't see it. The second thread's store is seen by main only.
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    a.emit({ADDI, s1, a0, 0, 0});
    a.syscall(SYS_FORK);
    const size_t to_child = a.branch(BEQ, a0, zero);
    a.emit({ADDI, s5, a0, 0, 0});
    a.emit({ADDI, a0, s5, 0, 0});
    a.li(a1, 0);
    a.syscall(SYS_WAIT);
    a.li(t0, 2);
    a.emit({SW, 0, s1, t0, 4});
    thread_exit(a, 0);
    a.patch(to_child, a.here());
    a.li(t0, 1);
    a.emit({SW, 0, s1, t0, 0});
    a.emit({LW, a0, s1, 0, 4});             // 0: the parent's store isn't here
    a.exit_with_a0();
    a.patch(to_main, a.here());
    heap(a, 1);
    a.emit({SW, 0, s1, zero, 0});           // touched before the fork
    thread_create(a, body, zero, s1);
    a.emit({ADDI, s4, a0, 0, 0});
    join(a, s4, 0);
    a.emit({LW, t0, s1, 0, 0});
    expect(a, t0, 0, 1);                    // the child's store isn't here
    a.emit({LW, a0, s1, 0, 4});
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 2);
}

TEST(thread_fork_two_threads_waiting_for_one_child) {
    // Both wake when the child ends. One collects it; the other re-runs and gets
    // -ECHILD, and neither hangs.
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    a.li(a0, -1);
    a.li(a1, 0);
    a.syscall(SYS_WAIT);
    thread_exit_with_a0(a);
    a.patch(to_main, a.here());
    a.syscall(SYS_FORK);
    const size_t to_child = a.branch(BEQ, a0, zero);
    a.emit({ADDI, s5, a0, 0, 0});
    thread_create(a, body, zero, zero);
    a.emit({ADDI, s4, a0, 0, 0});
    a.li(a0, -1);
    a.li(a1, 0);
    a.syscall(SYS_WAIT);
    a.emit({ADDI, s6, a0, 0, 0});
    join_status(a, s4);
    a.emit({ADD, a0, a0, s6, 0});           // pid + -ECHILD, in either order
    a.emit({SUB, a0, a0, s5, 0});
    a.exit_with_a0();
    a.patch(to_child, a.here());
    a.busy_wait(3000);
    a.exit_with(0);
    CHECK_EQ(run_one(a), -kEchild);
}

// =================== exec ===================

TEST(thread_exec_from_the_first_thread_ends_the_others) {
    // The sibling spins forever. exec ends it, or the system never finishes.
    System s;
    put(s, "seven", exits_7());
    Strings str;
    const uint32_t p = str.add("/seven");
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    spin(a);
    a.patch(to_main, a.here());
    thread_create(a, body, zero, zero);
    a.li(a0, static_cast<int32_t>(p));
    a.syscall(SYS_EXEC);
    a.exit_with(1);
    CHECK_EQ(run_one(s, a, str, kForever), 7);
    CHECK_EQ(s.k.process_count(), 0u);
}

TEST(thread_exec_from_another_thread_keeps_the_pid) {
    System s;
    put(s, "seven", exits_7());
    Strings str;
    const uint32_t p = str.add("/seven");
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    a.li(a0, static_cast<int32_t>(p));
    a.syscall(SYS_EXEC);
    a.exit_with(1);
    a.patch(to_main, a.here());
    thread_create(a, body, zero, zero);
    spin(a);
    CHECK_EQ(run_one(s, a, str, 5'000'000), 7);    // the status under the first pid
}

TEST(thread_exec_leaves_only_the_caller) {
    // The new program spins, so the system is still running when the test looks:
    // the worker that called exec is the only thread left in the process.
    System s;
    Asm forever;
    spin(forever);
    put(s, "forever", forever);
    Strings str;
    const uint32_t p = str.add("/forever");
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    a.li(a0, static_cast<int32_t>(p));
    a.syscall(SYS_EXEC);
    a.exit_with(1);
    a.patch(to_main, a.here());
    thread_create(a, body, zero, zero);
    spin(a);
    const uint32_t pid = s.spawn(a, str.bytes);
    CHECK(std::holds_alternative<kernel::StepLimit>(s.k.run_all(500'000)));
    CHECK(s.k.process(pid) != nullptr);
    CHECK(s.k.thread(pid) == nullptr);
    CHECK(s.k.thread(pid + 1) != nullptr);
}

TEST(thread_exec_that_fails_leaves_the_others_running) {
    System s;
    Strings str;
    const uint32_t p = str.add("/nope");
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    a.busy_wait(3000);
    thread_exit(a, 9);
    a.patch(to_main, a.here());
    thread_create(a, body, zero, zero);
    a.emit({ADDI, s4, a0, 0, 0});
    a.li(a0, static_cast<int32_t>(p));
    a.syscall(SYS_EXEC);
    expect(a, a0, -kEnoent, 1);
    join_status(a, s4);
    a.exit_with_a0();
    CHECK_EQ(run_one(s, a, str, kForever), 9);
}

TEST(thread_exec_waits_for_a_sibling_on_the_disk) {
    // The disk writes into the old address space until the sibling's read is done,
    // so the old program must not be replaced before that. The file is read once
    // first: its blocks are cached, so exec itself needs no disk and reaches its
    // act phase while the sibling is still waiting. The new program spins, and the
    // system runs in small slices, so the test sees the moment of the replacement:
    // the break grows with sbrk, then falls back when the new program replaces it.
    System s;
    Asm forever;
    spin(forever);
    put(s, "forever", forever);
    Strings str;
    const uint32_t p = str.add("/forever"), x = str.add("X");
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    disk_read(a, a0);
    print(a, x);
    thread_exit(a, 0);
    a.patch(to_main, a.here());
    open(a, p);
    read(a, 3, kBuf, kPage);
    heap(a, 1);
    thread_create(a, body, zero, s1);
    a.syscall(SYS_YIELD);
    a.li(a0, static_cast<int32_t>(p));
    a.syscall(SYS_EXEC);
    a.exit_with(1);
    const uint32_t pid = s.spawn(a, str.bytes);
    const uint32_t first_break = s.k.process(pid)->brk;
    bool grew = false, replaced = false;
    for (int slice = 0; slice < 100'000 && !replaced; slice++) {
        s.k.run_all(20);
        const kernel::Process* process = s.k.process(pid);
        if (!process) break;
        if (process->brk > first_break) grew = true;
        replaced = grew && process->brk <= first_break;
    }
    CHECK(replaced);
    bool sibling_read_finished = false;
    for (const disk::Request& r : s.m.disk().completed())
        if (r.first_sector == 100) sibling_read_finished = true;
    CHECK(sibling_read_finished);       // checked at the moment of the replacement
    CHECK(s.k.thread(pid + 1) == nullptr);
    CHECK_EQ(s.out.str(), std::string(""));
}

// =================== swapping ===================

TEST(thread_swap_each_thread_keeps_its_pages) {
    // Two threads, 150 pages each: 300 in all, more than RAM. Each writes its
    // pages and checks them twice. They evict each other's pages in one table.
    System s;
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    a.emit({ADDI, s1, a0, 0, 0});
    a.li(s2, 150);
    for (int pass = 0; pass < 3; pass++) {
        a.li(s3, 0);
        const size_t top = a.here();
        a.emit({SLLI, t0, s3, 0, 12});
        a.emit({ADD, t0, t0, s1, 0});
        a.emit({ADDI, t2, s3, 0, 3});
        if (pass == 0) {
            a.emit({SW, 0, t0, t2, 0});
        } else {
            a.emit({LW, t1, t0, 0, 0});
            const size_t ok = a.branch(BEQ, t1, t2);
            thread_exit(a, 1);
            a.patch(ok, a.here());
        }
        a.emit({ADDI, s3, s3, 0, 1});
        a.patch(a.branch(BNE, s3, s2), top);
    }
    thread_exit(a, 0);
    a.patch(to_main, a.here());
    heap(a, 300);
    a.emit({ADDI, s5, s1, 0, 0});
    a.li(t0, 150 * kPage);
    a.emit({ADD, s6, s1, t0, 0});
    thread_create(a, body, zero, s5);
    a.emit({ADDI, s7, a0, 0, 0});
    thread_create(a, body, zero, s6);
    a.emit({ADDI, s8, a0, 0, 0});
    join_status(a, s7);
    expect(a, a0, 0, 1);
    join_status(a, s8);
    expect(a, a0, 0, 2);
    a.exit_with(0);
    CHECK_EQ(run_one(s, a, {}, kForever), 0);
    CHECK_EQ(s.m.frame_allocator().free_count(), mmu::kNumFrames);
}

TEST(thread_swap_two_faults_on_one_page_read_it_once) {
    // Page 0 is swapped out. Main faults on it and blocks on the disk; then the
    // thread faults on it too. One read, and both see the word.
    System s;
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    a.emit({LW, a0, a0, 0, 0});
    thread_exit_with_a0(a);
    a.patch(to_main, a.here());
    heap(a, 300);
    a.li(s2, 300);
    a.li(s3, 0);
    const size_t top = a.here();
    a.emit({SLLI, t0, s3, 0, 12});
    a.emit({ADD, t0, t0, s1, 0});
    a.li(t2, 5);
    a.emit({SW, 0, t0, t2, 0});
    a.emit({ADDI, s3, s3, 0, 1});
    a.patch(a.branch(BNE, s3, s2), top);
    thread_create(a, body, zero, s1);
    a.emit({ADDI, s4, a0, 0, 0});
    a.emit({LW, s5, s1, 0, 0});             // blocks, and the thread runs
    join_status(a, s4);
    a.emit({ADD, a0, a0, s5, 0});
    a.exit_with_a0();
    CHECK_EQ(run_one(s, a, {}, kForever), 10);
    std::set<uint32_t> seen;
    for (const disk::Request& r : s.m.disk().completed()) {
        if (r.op == disk::Op::read && r.first_sector >= fs::kSwapStart * fs::kSectorsPerBlock) {
            CHECK(seen.insert(r.first_sector).second);
        }
    }
}

// =================== a race condition ===================

TEST(thread_race_two_threads_lose_updates) {
    // counter++ is three instructions: load, add, store. The timer can stop a
    // thread in the middle, and the other's updates are then overwritten. The
    // loop is 7 instructions so the quantum (1000) cuts it at every point in turn.
    Asm a;
    const size_t to_main = jump_over_bodies(a);
    const uint32_t body = here_addr(a);
    a.li(s3, 0);
    a.li(s2, 5000);
    const size_t top = a.here();
    a.emit({LW, t0, a0, 0, 0});
    a.emit({ADDI, t0, t0, 0, 1});
    a.emit({SW, 0, a0, t0, 0});
    a.emit({ADDI, s3, s3, 0, 1});
    a.emit({ADDI, zero, zero, 0, 0});
    a.emit({ADDI, zero, zero, 0, 0});
    a.patch(a.branch(BNE, s3, s2), top);
    thread_exit(a, 0);
    a.patch(to_main, a.here());
    heap(a, 1);
    thread_create(a, body, zero, s1);
    a.emit({ADDI, s4, a0, 0, 0});
    thread_create(a, body, zero, s1);
    a.emit({ADDI, s5, a0, 0, 0});
    join(a, s4, 0);
    join(a, s5, 0);
    a.emit({LW, a0, s1, 0, 0});
    a.exit_with_a0();
    const int32_t counter = run_one(a);
    CHECK(counter >= 5000);
    CHECK(counter < 10000);
}
