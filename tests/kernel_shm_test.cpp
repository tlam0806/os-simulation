// Shared memory (spec F2): the same frames mapped into several address spaces.
//
//   shm_create_   shmget: ids, errors, no frames yet
//   shm_attach_   shmat: where it goes; frames arrive on first touch
//   shm_share_    two processes see the same bytes
//   shm_fork_     a child keeps attachments, and they aren't copy-on-write
//   shm_detach_   shmdt
//   shm_destroy_  shmrm
//   shm_end_      exit, kill, exec and threads detach what's attached
//
// Programs keep a region's id in s1 and its address in s2.
#include <string>
#include <variant>
#include <vector>

#include "cpu_helpers.hpp"
#include "fs_system.hpp"
#include "mmu/frame_allocator.hpp"

using namespace fs_system;

namespace {

constexpr int32_t kEnomem = 12, kEbusy = 16, kEnosp = 28;
constexpr int32_t kPage = 4096;
constexpr int32_t kStackRegion = 0x7FF00000;
constexpr int32_t kOne = kStackRegion - kPage;  // where a first 1-page attachment goes
constexpr uint32_t kStatus = kBuf + 16;         // wait's status
constexpr uint32_t kVar = kBuf + 32;            // a stack word: ordinary memory
constexpr uint64_t kShort = 2'000'000;
constexpr uint64_t kDisk = 100'000'000;

void expect(Asm& a, uint8_t r, int32_t value, int32_t code) {
    a.li(t6, value);
    const size_t ok = a.branch(BEQ, r, t6);
    a.exit_with(code);
    a.patch(ok, a.here());
}

void shmget(Asm& a, int32_t size) {
    a.li(a0, size);
    a.syscall(SYS_SHMGET);
}

void shmat(Asm& a, int32_t id) {
    a.li(a0, id);
    a.syscall(SYS_SHMAT);
}

void shmdt(Asm& a, int32_t addr) {
    a.li(a0, addr);
    a.syscall(SYS_SHMDT);
}

void shmrm(Asm& a, int32_t id) {
    a.li(a0, id);
    a.syscall(SYS_SHMRM);
}

// s1 = a new region's id, s2 = where it's attached.
void make_region(Asm& a, int32_t size) {
    shmget(a, size);
    a.emit({ADDI, s1, a0, 0, 0});
    a.syscall(SYS_SHMAT);   // a0 still holds the id
    a.emit({ADDI, s2, a0, 0, 0});
}

// Word `index` of the region at s2.
void store_word(Asm& a, int32_t index, int32_t value) {
    a.li(t0, value);
    a.emit({SW, 0, s2, t0, index * 4});
}

void load_word(Asm& a, uint8_t rd, int32_t index) { a.emit({LW, rd, s2, 0, index * 4}); }

void fork_to_parent(Asm& a, size_t& to_parent) {
    a.syscall(SYS_FORK);
    to_parent = a.branch(BNE, a0, zero);
}

// wait(any, kStatus); a0 = the child's status.
void wait_status(Asm& a) {
    a.li(a0, -1);
    a.li(a1, static_cast<int32_t>(kStatus));
    a.syscall(SYS_WAIT);
    a.li(t6, static_cast<int32_t>(kStatus));
    a.emit({LW, a0, t6, 0, 0});
}

// sbrk so that the break becomes `target`; a0 = sbrk's result.
void grow_break_to(Asm& a, int32_t target) {
    a.sbrk(0);
    a.li(t0, target);
    a.emit({SUB, a0, t0, a0, 0});
    a.syscall(SYS_SBRK);
}

uint32_t here_addr(const Asm& a) { return 0x10000 + static_cast<uint32_t>(a.here()) * 4; }

int32_t run_one(System& s, const Asm& a, const Strings& str = {}, uint64_t limit = kShort) {
    const uint32_t pid = s.spawn(a, str.bytes);
    CHECK(std::holds_alternative<kernel::AllDone>(s.k.run_all(limit)));
    return s.status(pid);
}

int32_t run_one(const Asm& a) {
    System s;
    return run_one(s, a);
}

uint32_t free_frames(System& s) { return s.m.frame_allocator().free_count(); }

}  // namespace

// =================== Stage 1: the region table (shmget, shmrm). Filter: shm1_ ===================

TEST(shm1_create_ids_count_from_1) {
    Asm a;
    shmget(a, kPage);
    keep(a);
    shmget(a, 1);
    exit_with_both(a);
    CHECK_EQ(run_one(a), (1 << 8) + 2);
}

TEST(shm1_create_a_size_of_zero_or_less_is_einval) {
    Asm a;
    shmget(a, 0);
    expect(a, a0, -kEinval, 1);
    shmget(a, -1);
    expect(a, a0, -kEinval, 2);
    shmget(a, kPage);       // the failures used no id
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 1);
}

TEST(shm1_create_takes_no_frames) {
    System s;
    const uint32_t before = free_frames(s);
    Asm a;
    shmget(a, kPage + 1);
    a.exit_with_a0();
    CHECK_EQ(run_one(s, a), 1);
    CHECK_EQ(free_frames(s), before);
}

TEST(shm1_create_a_ninth_region_is_enospc_until_one_is_destroyed) {
    Asm a;
    for (int32_t i = 1; i <= 8; ++i) {
        shmget(a, kPage);
        expect(a, a0, i, i);
    }
    shmget(a, kPage);
    expect(a, a0, -kEnosp, 20);
    shmrm(a, 3);
    expect(a, a0, 0, 21);
    shmget(a, kPage);       // ids aren't reused, and the failure used none
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 9);
}

TEST(shm1_destroy_an_unknown_id_or_twice_is_einval) {
    Asm a;
    shmrm(a, 1);
    expect(a, a0, -kEinval, 1);
    shmget(a, kPage);
    shmrm(a, 1);
    expect(a, a0, 0, 2);
    shmrm(a, 1);
    expect(a, a0, -kEinval, 3);
    a.exit_with(7);
    CHECK_EQ(run_one(a), 7);
}

// =================== Stage 2: the attachment list (shmat, shmdt, sbrk limit). Filter: shm2_ ===================

TEST(shm2_create_a_region_can_be_bigger_than_memory) {
    System s;
    const uint32_t before = free_frames(s);
    Asm a;
    make_region(a, 300 * kPage);
    a.emit({ADDI, a0, s2, 0, 0});
    a.exit_with_a0();
    CHECK_EQ(run_one(s, a), kStackRegion - 300 * kPage);
    CHECK_EQ(free_frames(s), before);
}

TEST(shm2_attach_goes_just_below_the_stack_region) {
    Asm a;
    make_region(a, 1);
    a.emit({ADDI, a0, s2, 0, 0});
    a.exit_with_a0();
    CHECK_EQ(run_one(a), kOne);
}

TEST(shm2_attach_the_next_goes_below_the_last) {
    Asm a;
    make_region(a, kPage);
    expect(a, s2, kOne, 1);
    make_region(a, 2 * kPage);
    a.emit({ADDI, a0, s2, 0, 0});
    a.exit_with_a0();
    CHECK_EQ(run_one(a), kStackRegion - 3 * kPage);
}

TEST(shm2_attach_takes_the_highest_gap_that_fits) {
    Asm a;
    shmget(a, kPage);
    shmget(a, kPage);
    shmget(a, 2 * kPage);
    shmget(a, kPage);
    shmat(a, 1);
    expect(a, a0, kStackRegion - kPage, 1);
    shmat(a, 2);
    expect(a, a0, kStackRegion - 2 * kPage, 2);
    shmdt(a, kStackRegion - kPage);
    expect(a, a0, 0, 3);
    shmat(a, 3);            // 2 pages don't fit in the 1-page gap
    expect(a, a0, kStackRegion - 4 * kPage, 4);
    shmat(a, 4);            // this one does
    a.exit_with_a0();
    CHECK_EQ(run_one(a), kStackRegion - kPage);
}

TEST(shm2_attach_an_unknown_id_is_einval) {
    Asm a;
    shmget(a, kPage);
    for (int32_t id : {0, 2, -1}) {
        shmat(a, id);
        expect(a, a0, -kEinval, 1);
    }
    a.exit_with(7);
    CHECK_EQ(run_one(a), 7);
}

TEST(shm2_attach_stops_at_the_break) {
    Asm a;
    shmget(a, kPage);
    grow_break_to(a, kOne + 1);         // the heap has reached the page it would need
    shmat(a, 1);
    expect(a, a0, -kEnomem, 2);
    a.li(a0, -1);
    a.syscall(SYS_SBRK);                // the break is at kOne now, so it fits
    shmat(a, 1);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), kOne);
}

TEST(shm2_attach_stops_sbrk_at_the_lowest_attachment) {
    Asm a;
    make_region(a, kPage);
    grow_break_to(a, kOne);             // up to the attachment is fine
    const size_t ok = a.branch(BLT, zero, a0);
    a.exit_with(1);
    a.patch(ok, a.here());
    a.sbrk(1);
    expect(a, a0, -kEnomem, 2);
    a.emit({ADDI, a0, s2, 0, 0});
    a.syscall(SYS_SHMDT);
    a.sbrk(1);                          // and it's free again once detached
    a.exit_with_a0();
    CHECK_EQ(run_one(a), kOne);
}

TEST(shm2_detach_twice_is_einval) {
    Asm a;
    make_region(a, kPage);
    shmdt(a, kOne);
    expect(a, a0, 0, 1);
    shmdt(a, kOne);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), -kEinval);
}

TEST(shm2_destroy_then_attach_is_einval) {
    Asm a;
    shmget(a, kPage);
    shmrm(a, 1);
    shmat(a, 1);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), -kEinval);
}

TEST(shm2_destroy_while_attached_is_ebusy_until_detached) {
    Asm a;
    make_region(a, kPage);
    shmrm(a, 1);
    expect(a, a0, -kEbusy, 1);
    a.emit({ADDI, a0, s2, 0, 0});
    a.syscall(SYS_SHMDT);
    expect(a, a0, 0, 2);
    shmrm(a, 1);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 0);
}

TEST(shm2_detach_only_takes_the_start_of_an_attachment) {
    Asm a;
    make_region(a, 2 * kPage);
    shmdt(a, kStackRegion - 2 * kPage + 4);     // inside it
    expect(a, a0, -kEinval, 1);
    shmdt(a, kStackRegion - kPage);             // its second page
    expect(a, a0, -kEinval, 2);
    shmdt(a, kStackRegion - 2 * kPage);         // nothing was removed, so this works
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 0);
}

// =================== Stage 3: the page fault path. Filter: shm3_ ===================

TEST(shm3_attach_only_touched_pages_get_frames) {
    // Page 1 of 3 is touched, by a load. The program's end leaves only the region's frame.
    System s;
    const uint32_t before = free_frames(s);
    Asm a;
    make_region(a, 3 * kPage);
    a.li(t1, kPage);
    a.emit({ADD, t1, s2, t1, 0});
    a.emit({LW, a0, t1, 0, 0});
    a.exit_with_a0();
    CHECK_EQ(run_one(s, a), 0);
    CHECK_EQ(free_frames(s), before - 1);
}

TEST(shm3_attach_a_second_touch_of_a_page_takes_no_new_frame) {
    // Attached twice: both mappings use the page's one frame.
    System s;
    const uint32_t before = free_frames(s);
    Asm a;
    make_region(a, kPage);
    shmat(a, 1);
    a.emit({ADDI, s3, a0, 0, 0});
    store_word(a, 0, 1);
    a.emit({LW, a0, s3, 0, 0});
    a.exit_with_a0();
    CHECK_EQ(run_one(s, a), 1);
    CHECK_EQ(free_frames(s), before - 1);
}

TEST(shm3_attach_a_system_call_can_use_an_untouched_page) {
    // read's buffer is in the region, and nothing has touched it yet.
    System s;
    Strings str;
    const uint32_t hello = str.add("/hello");
    Asm a;
    make_region(a, kPage);
    open(a, hello);
    a.emit({ADDI, a1, s2, 0, 0});
    a.li(a2, 2);
    a.syscall(SYS_READ);
    expect(a, a0, 2, 1);
    a.write_stdout(kOne, 2);
    a.exit_with(7);
    CHECK_EQ(run_one(s, a, str, kDisk), 7);
    CHECK_EQ(s.out.str(), std::string("hi"));
}

TEST(shm3_attach_maps_every_page_read_and_write) {
    Asm a;
    make_region(a, 2 * kPage);
    store_word(a, 0, 11);
    a.emit({ADDI, s3, s2, 0, 0});
    a.li(t1, kPage);
    a.emit({ADD, s2, s2, t1, 0});
    store_word(a, 0, 22);
    load_word(a, t2, 0);
    a.emit({ADDI, s2, s3, 0, 0});
    load_word(a, a0, 0);
    a.emit({SLLI, a0, a0, 0, 8});
    a.emit({ADD, a0, a0, t2, 0});
    a.exit_with_a0();
    CHECK_EQ(run_one(a), (11 << 8) + 22);
}

TEST(shm3_attach_new_regions_are_zero_filled) {
    // The second region gets the frames the first one dirtied.
    Asm a;
    make_region(a, kPage);
    store_word(a, 5, 99);
    a.emit({ADDI, a0, s2, 0, 0});
    a.syscall(SYS_SHMDT);
    a.emit({ADDI, a0, s1, 0, 0});
    a.syscall(SYS_SHMRM);
    expect(a, a0, 0, 1);
    make_region(a, kPage);
    load_word(a, a0, 5);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 0);
}

TEST(shm3_attach_twice_shows_the_same_memory_at_two_addresses) {
    Asm a;
    make_region(a, kPage);
    expect(a, s2, kOne, 1);
    shmat(a, 1);
    expect(a, a0, kOne - kPage, 2);
    a.emit({ADDI, s3, a0, 0, 0});
    store_word(a, 0, 33);
    a.emit({LW, a0, s3, 0, 0});
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 33);
}

TEST(shm3_share_lecture_master_and_worker) {
    // L4's example. The two are unrelated: the worker only knows the id.
    System s;
    const uint32_t before = free_frames(s);

    Asm master;
    make_region(master, 40);
    store_word(master, 0, 0);
    const size_t wait = master.here();
    load_word(master, t0, 0);
    master.patch(master.branch(BEQ, t0, zero), wait);
    load_word(master, s3, 1);
    load_word(master, t0, 2);
    master.emit({ADD, s3, s3, t0, 0});
    load_word(master, t0, 3);
    master.emit({ADD, s3, s3, t0, 0});
    master.emit({ADDI, a0, s2, 0, 0});
    master.syscall(SYS_SHMDT);
    expect(master, a0, 0, 1);
    master.emit({ADDI, a0, s1, 0, 0});
    master.syscall(SYS_SHMRM);
    expect(master, a0, 0, 2);
    master.emit({ADDI, a0, s3, 0, 0});
    master.exit_with_a0();

    Asm worker;
    worker.busy_wait(100);
    shmat(worker, 1);
    worker.emit({ADDI, s2, a0, 0, 0});
    store_word(worker, 1, 10);
    store_word(worker, 2, 20);
    store_word(worker, 3, 30);
    store_word(worker, 0, 1);
    worker.emit({ADDI, a0, s2, 0, 0});
    worker.syscall(SYS_SHMDT);
    worker.exit_with_a0();

    const uint32_t m = s.spawn(master);
    const uint32_t w = s.spawn(worker);
    s.run();
    CHECK_EQ(s.status(m), 60);
    CHECK_EQ(s.status(w), 0);
    CHECK_EQ(free_frames(s), before);
}

TEST(shm3_share_a_region_is_never_swapped_out) {
    // The heap needs more frames than the machine has, so CLOCK evicts. A later
    // attachment still finds the bytes in the region's frames.
    System s;
    Asm a;
    make_region(a, 2 * kPage);
    store_word(a, 0, 77);
    a.sbrk(300 * kPage);
    a.emit({ADDI, s3, a0, 0, 0});
    a.li(s4, 300);
    const size_t touch = a.here();
    a.emit({SW, 0, s3, s4, 0});
    a.li(t0, kPage);
    a.emit({ADD, s3, s3, t0, 0});
    a.emit({ADDI, s4, s4, 0, -1});
    a.patch(a.branch(BNE, s4, zero), touch);
    shmat(a, 1);
    a.emit({ADDI, s3, a0, 0, 0});
    a.emit({LW, s3, s3, 0, 0});
    a.li(t1, kPage);                // an untouched page still gets a frame, by evicting
    a.emit({ADD, t1, s2, t1, 0});
    a.li(t0, 1);
    a.emit({SW, 0, t1, t0, 0});
    a.emit({LW, t0, t1, 0, 0});
    expect(a, t0, 1, 1);
    a.emit({ADDI, a0, s3, 0, 0});
    a.exit_with_a0();
    CHECK_EQ(run_one(s, a, {}, kDisk), 77);
}

TEST(shm3_share_touching_more_than_memory_kills_the_process) {
    // Region frames can't be evicted, so eventually nothing can. They stay with the
    // region after the process has gone, because nothing destroyed it.
    System s;
    Asm a;
    make_region(a, 300 * kPage);
    a.li(s4, 300);
    const size_t touch = a.here();
    a.emit({SW, 0, s2, s4, 0});
    a.li(t0, kPage);
    a.emit({ADD, s2, s2, t0, 0});
    a.emit({ADDI, s4, s4, 0, -1});
    a.patch(a.branch(BNE, s4, zero), touch);
    a.exit_with(1);
    const uint32_t pid = s.spawn(a);
    CHECK(std::holds_alternative<kernel::AllDone>(s.k.run_all(kDisk)));
    const auto e = s.k.ending(pid);
    CHECK(e.has_value());
    if (e) CHECK(std::holds_alternative<kernel::Killed>(e->status));
    CHECK_EQ(free_frames(s), 0u);
}

TEST(shm3_detach_a_later_access_faults) {
    System s;
    Asm a;
    make_region(a, kPage);
    store_word(a, 0, 1);
    a.emit({ADDI, a0, s2, 0, 0});
    a.syscall(SYS_SHMDT);
    expect(a, a0, 0, 1);
    load_word(a, t0, 0);
    a.exit_with(2);
    const uint32_t pid = s.spawn(a);
    s.run();
    const auto e = s.k.ending(pid);
    CHECK(e.has_value());
    if (e) CHECK(std::holds_alternative<kernel::Killed>(e->status));
}

TEST(shm3_detach_removes_every_page) {
    System s;
    Asm a;
    make_region(a, 2 * kPage);
    a.li(t0, kPage);
    a.emit({ADD, s3, s2, t0, 0});
    a.emit({SW, 0, s3, t0, 0});         // both pages are mapped
    store_word(a, 0, 1);
    a.emit({ADDI, a0, s2, 0, 0});
    a.syscall(SYS_SHMDT);
    a.emit({ADDI, s2, s3, 0, 0});
    load_word(a, t0, 0);
    a.exit_with(2);
    const uint32_t pid = s.spawn(a);
    s.run();
    const auto e = s.k.ending(pid);
    CHECK(e.has_value());
    if (e) CHECK(std::holds_alternative<kernel::Killed>(e->status));
}

TEST(shm3_detach_an_address_that_doesnt_start_an_attachment_is_einval) {
    Asm a;
    make_region(a, 2 * kPage);
    expect(a, s2, kStackRegion - 2 * kPage, 1);
    shmdt(a, kStackRegion - 2 * kPage + 4);     // inside it
    expect(a, a0, -kEinval, 2);
    shmdt(a, kStackRegion - kPage);             // its second page
    expect(a, a0, -kEinval, 3);
    shmdt(a, static_cast<int32_t>(kBuf));       // the stack
    expect(a, a0, -kEinval, 4);
    store_word(a, 0, 8);                        // nothing was removed
    load_word(a, a0, 0);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 8);
}

TEST(shm3_detach_keeps_the_bytes_in_the_region) {
    Asm a;
    make_region(a, kPage);
    store_word(a, 0, 42);
    shmdt(a, kOne);
    shmat(a, 1);
    a.emit({ADDI, s2, a0, 0, 0});
    load_word(a, a0, 0);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 42);
}

TEST(shm3_detach_removes_one_of_two_attachments_only) {
    Asm a;
    make_region(a, kPage);
    shmat(a, 1);
    a.emit({ADDI, s3, a0, 0, 0});
    store_word(a, 0, 12);
    a.emit({ADDI, a0, s2, 0, 0});
    a.syscall(SYS_SHMDT);
    a.emit({LW, a0, s3, 0, 0});
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 12);
}

TEST(shm3_destroy_gives_the_frames_back) {
    System s;
    Asm a;
    const uint32_t before = free_frames(s);
    make_region(a, 2 * kPage);
    a.li(t0, kPage);
    a.emit({ADD, t1, s2, t0, 0});
    a.emit({SW, 0, t1, t0, 0});
    store_word(a, 0, 1);
    a.emit({ADDI, a0, s2, 0, 0});
    a.syscall(SYS_SHMDT);
    shmrm(a, 1);
    a.exit_with_a0();
    CHECK_EQ(run_one(s, a), 0);
    CHECK_EQ(free_frames(s), before);
}

TEST(shm3_destroy_while_attached_is_ebusy_and_changes_nothing) {
    Asm a;
    make_region(a, kPage);
    store_word(a, 0, 3);
    shmrm(a, 1);
    expect(a, a0, -kEbusy, 1);
    load_word(a, a0, 0);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 3);
}

TEST(shm3_end_threads_share_an_attachment) {
    // One thread attaches and writes; the other uses the same address.
    Asm a;
    const size_t to_main = a.branch(JAL, zero, zero);
    const uint32_t body = here_addr(a);
    shmat(a, 1);
    a.emit({ADDI, s2, a0, 0, 0});
    store_word(a, 0, 6);
    a.li(a0, 0);
    a.syscall(SYS_THREAD_EXIT);
    a.patch(to_main, a.here());
    shmget(a, kPage);
    a.li(a0, static_cast<int32_t>(body));
    a.li(a1, 0);
    a.li(a2, 0);
    a.syscall(SYS_THREAD_CREATE);
    a.li(a1, 0);
    a.syscall(SYS_THREAD_JOIN);
    a.li(s2, kOne);
    load_word(a, a0, 0);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 6);
}

TEST(shm3_running_code_in_a_region_kills_the_process) {
    // The page is mapped read and write, not execute, so this fault can't be fixed.
    System s;
    Asm a;
    make_region(a, kPage);
    store_word(a, 0, 1);
    a.emit({JALR, ra, s2, 0, 0});
    a.exit_with(2);
    const uint32_t pid = s.spawn(a);
    CHECK(std::holds_alternative<kernel::AllDone>(s.k.run_all(kShort)));
    const auto e = s.k.ending(pid);
    CHECK(e.has_value());
    if (e) CHECK(std::holds_alternative<kernel::Killed>(e->status));
}

// =================== Stage 4: fork, exec and exit. Filter: shm4_ ===================

TEST(shm4_share_a_region_outlives_every_process) {
    // Nothing destroyed it, so the frames stay and the bytes are there later.
    System s;
    const uint32_t before = free_frames(s);
    Asm first;
    make_region(first, kPage);
    store_word(first, 0, 42);
    first.exit_with(0);
    CHECK_EQ(run_one(s, first), 0);
    CHECK_EQ(free_frames(s), before - 1);

    Asm second;
    shmat(second, 1);
    second.emit({ADDI, s2, a0, 0, 0});
    load_word(second, s3, 0);
    second.emit({ADDI, a0, s2, 0, 0});
    second.syscall(SYS_SHMDT);
    shmrm(second, 1);
    expect(second, a0, 0, 1);
    second.emit({ADDI, a0, s3, 0, 0});
    second.exit_with_a0();
    CHECK_EQ(run_one(s, second), 42);
    CHECK_EQ(free_frames(s), before);
}

TEST(shm4_fork_the_attachment_stays_shared_but_other_memory_is_copied) {
    Asm a;
    make_region(a, kPage);
    store_word(a, 0, 1);            // mapped before the fork
    a.li(t1, static_cast<int32_t>(kVar));
    a.li(t0, 1);
    a.emit({SW, 0, t1, t0, 0});
    size_t to_parent;
    fork_to_parent(a, to_parent);
    store_word(a, 0, 5);
    a.li(t1, static_cast<int32_t>(kVar));
    a.li(t0, 6);
    a.emit({SW, 0, t1, t0, 0});
    a.exit_with(0);
    a.patch(to_parent, a.here());
    wait_status(a);
    load_word(a, s3, 0);
    a.li(t1, static_cast<int32_t>(kVar));
    a.emit({LW, t0, t1, 0, 0});
    a.emit({SLLI, s3, s3, 0, 8});
    a.emit({ADD, a0, s3, t0, 0});
    a.exit_with_a0();
    CHECK_EQ(run_one(a), (5 << 8) + 1);
}

TEST(shm4_fork_a_page_untouched_at_the_fork_is_shared_too) {
    Asm a;
    make_region(a, kPage);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    store_word(a, 0, 5);
    a.exit_with(0);
    a.patch(to_parent, a.here());
    wait_status(a);
    load_word(a, a0, 0);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 5);
}

TEST(shm4_fork_a_page_that_already_had_a_frame_when_mapped_is_shared_too) {
    // The second mapping finds the frame in the region's table. Fork must still see
    // it as shared, or it turns the page into copy-on-write.
    Asm a;
    make_region(a, kPage);
    store_word(a, 0, 1);
    shmat(a, 1);
    a.emit({ADDI, s3, a0, 0, 0});
    a.emit({LW, t0, s3, 0, 0});
    size_t to_parent;
    fork_to_parent(a, to_parent);
    a.li(t0, 5);
    a.emit({SW, 0, s3, t0, 0});
    a.exit_with(0);
    a.patch(to_parent, a.here());
    wait_status(a);
    load_word(a, a0, 0);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 5);
}

TEST(shm4_fork_the_parent_can_write_after_the_child_ended) {
    // A shared page must not have been left read-only by fork.
    Asm a;
    make_region(a, kPage);
    store_word(a, 0, 1);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    a.exit_with(0);
    a.patch(to_parent, a.here());
    wait_status(a);
    store_word(a, 0, 9);
    load_word(a, a0, 0);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 9);
}

TEST(shm4_fork_counts_the_child_as_attached) {
    // The child's exit removes its own attachment and not the parent's.
    System s;
    const uint32_t before = free_frames(s);
    Asm a;
    make_region(a, kPage);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    a.exit_with(0);
    a.patch(to_parent, a.here());
    wait_status(a);
    shmrm(a, 1);
    expect(a, a0, -kEbusy, 1);
    a.emit({ADDI, a0, s2, 0, 0});
    a.syscall(SYS_SHMDT);
    expect(a, a0, 0, 2);
    shmrm(a, 1);
    expect(a, a0, 0, 3);
    a.exit_with(7);
    CHECK_EQ(run_one(s, a), 7);
    CHECK_EQ(free_frames(s), before);
}

TEST(shm4_fork_writing_a_shared_page_gives_its_frame_back_in_the_end) {
    // Both processes write the page after the fork. Once they've gone and the region
    // is destroyed, no reference to the frame may be left.
    System s;
    const uint32_t before = free_frames(s);
    Asm a;
    make_region(a, kPage);
    store_word(a, 0, 1);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    store_word(a, 0, 5);
    a.exit_with(0);
    a.patch(to_parent, a.here());
    wait_status(a);
    store_word(a, 0, 9);
    a.emit({ADDI, a0, s2, 0, 0});
    a.syscall(SYS_SHMDT);
    expect(a, a0, 0, 1);
    shmrm(a, 1);
    expect(a, a0, 0, 2);
    a.exit_with(7);
    CHECK_EQ(run_one(s, a), 7);
    CHECK_EQ(free_frames(s), before);
}

TEST(shm4_detach_leaves_another_process_attached) {
    // The parent detaches first; the child's mapping still works.
    Asm a;
    make_region(a, kPage);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    a.busy_wait(3000);
    store_word(a, 0, 15);
    load_word(a, a0, 0);
    a.exit_with_a0();
    a.patch(to_parent, a.here());
    a.emit({ADDI, a0, s2, 0, 0});
    a.syscall(SYS_SHMDT);
    expect(a, a0, 0, 1);
    wait_status(a);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 15);
}

TEST(shm4_destroy_while_another_process_is_attached_is_ebusy) {
    Asm a;
    make_region(a, kPage);
    store_word(a, 0, 1);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    a.emit({ADDI, a0, s2, 0, 0});
    a.syscall(SYS_SHMDT);
    expect(a, a0, 0, 1);
    shmrm(a, 1);
    expect(a, a0, -kEbusy, 2);
    a.exit_with(0);
    a.patch(to_parent, a.here());
    wait_status(a);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 0);
}

TEST(shm4_end_exit_detaches) {
    Asm a;
    shmget(a, kPage);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    shmat(a, 1);
    a.exit_with(0);                 // no shmdt
    a.patch(to_parent, a.here());
    wait_status(a);
    shmrm(a, 1);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 0);
}

TEST(shm4_end_being_killed_detaches) {
    Asm a;
    shmget(a, kPage);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    shmat(a, 1);
    a.emit({LW, t0, zero, 0, 0});   // the null guard
    a.exit_with(0);
    a.patch(to_parent, a.here());
    wait_status(a);
    shmrm(a, 1);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 0);
}

TEST(shm4_end_exec_detaches) {
    // The new program has no attachment: the region is free to destroy while it's
    // still running, and its access is killed.
    System s;
    Asm next;
    shmrm(next, 1);
    expect(next, a0, 0, 1);
    next.li(t1, kOne);
    next.emit({LW, t0, t1, 0, 0});
    next.exit_with(0);
    CHECK(std::holds_alternative<uint32_t>(fs::create_file(s.m.disk(), fs::kRootInode, "next", elf(next, {}))));
    Strings str;
    const uint32_t path = str.add("/next");
    Asm a;
    shmget(a, kPage);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    shmat(a, 1);
    a.li(a0, static_cast<int32_t>(path));
    a.syscall(SYS_EXEC);
    a.exit_with(0);
    a.patch(to_parent, a.here());
    wait_status(a);
    a.exit_with_a0();
    CHECK(run_one(s, a, str, kDisk) >= 128);
}

TEST(shm4_end_a_failed_exec_keeps_the_attachment) {
    System s;
    Strings str;
    const uint32_t path = str.add("/nothing");
    Asm a;
    make_region(a, kPage);
    a.li(a0, static_cast<int32_t>(path));
    a.syscall(SYS_EXEC);
    expect(a, a0, -kEnoent, 1);
    store_word(a, 0, 4);
    shmrm(a, 1);
    expect(a, a0, -kEbusy, 2);
    load_word(a, a0, 0);
    a.exit_with_a0();
    CHECK_EQ(run_one(s, a, str, kDisk), 4);
}

TEST(shm4_end_a_process_with_two_threads_detaches_once) {
    // The process's attachment count must fall by one, not one per thread, and
    // only when the whole process has ended.
    System s;
    Asm a;
    const size_t to_main = a.branch(JAL, zero, zero);
    const uint32_t body = here_addr(a);
    const size_t loop = a.here();
    a.patch(a.branch(JAL, zero, zero), loop);
    a.patch(to_main, a.here());
    make_region(a, kPage);
    a.li(a0, static_cast<int32_t>(body));
    a.li(a1, 0);
    a.li(a2, 0);
    a.syscall(SYS_THREAD_CREATE);
    a.syscall(SYS_YIELD);
    a.exit_with(3);
    CHECK_EQ(run_one(s, a), 3);

    Asm check;
    shmrm(check, 1);
    check.emit({ADDI, a0, a0, 0, 7});
    check.exit_with_a0();
    CHECK_EQ(run_one(s, check), 7);
}
