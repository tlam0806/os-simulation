// lseek (a7 = 14), spec C3: setting an open file's offset from the start, the
// current offset or the end; only the end needs the inode, so only it may block.
//
//   lseek_basic_   SET, CUR, END; each open has its own offset; past the end is fine
//   lseek_block_   END may wait for the inode's block; SET and CUR never do
//   lseek_err_     EBADF, EINVAL; a failed lseek leaves the offset alone
//
// The disk (fs_system.hpp): /hello is 2 bytes; / has two entries, 64 bytes.
#include "fs_system.hpp"

using namespace fs_system;

namespace {

// Runs a program that first opens `path` (fd 3), then `body`, then exits with a0.
int32_t run_on(const std::string& path, void (*body)(Asm&)) {
    System s;
    Strings str;
    const uint32_t p = str.add(path);
    Asm a;
    open(a, p);
    body(a);
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a, str.bytes);
    s.run();
    return s.status(pid);
}

}  // namespace

// =================== SET, CUR, END ===================

TEST(lseek_basic_new_open_starts_at_0) {
    CHECK_EQ(run_on("/hello", [](Asm& a) { lseek(a, 3, 0, kCur); }), 0);
}

TEST(lseek_basic_set) {
    CHECK_EQ(run_on("/hello", [](Asm& a) { lseek(a, 3, 1, kSet); }), 1);
}

TEST(lseek_basic_cur_adds_to_the_offset) {
    CHECK_EQ(run_on("/hello", [](Asm& a) {
        lseek(a, 3, 5, kSet);
        lseek(a, 3, -2, kCur);
    }), 3);
}

TEST(lseek_basic_end_adds_to_the_size) {
    CHECK_EQ(run_on("/hello", [](Asm& a) { lseek(a, 3, 0, kEnd); }), 2);
    CHECK_EQ(run_on("/hello", [](Asm& a) { lseek(a, 3, -1, kEnd); }), 1);
    CHECK_EQ(run_on("/hello", [](Asm& a) { lseek(a, 3, 10, kEnd); }), 12);
}

TEST(lseek_basic_end_of_a_directory) {
    // A directory's size counts its entries: / holds hello and bin, 2 x 32 bytes.
    CHECK_EQ(run_on("/", [](Asm& a) { lseek(a, 3, 0, kEnd); }), 64);
}

TEST(lseek_basic_past_the_end_is_fine) {
    CHECK_EQ(run_on("/hello", [](Asm& a) { lseek(a, 3, 1000, kSet); }), 1000);
}

TEST(lseek_basic_largest_offset) {
    CHECK_EQ(run_on("/hello", [](Asm& a) { lseek(a, 3, 0x7FFFFFFF, kSet); }), 0x7FFFFFFF);
}

TEST(lseek_basic_each_open_has_its_own_offset) {
    // Two opens of one file are two entries: moving fd 3 leaves fd 4 at 0.
    System s;
    Strings str;
    const uint32_t p = str.add("/hello");
    Asm a;
    open(a, p);
    open(a, p);
    lseek(a, 3, 7, kSet);
    keep(a);                 // 7
    lseek(a, 4, 0, kCur);    // 0
    exit_with_both(a);
    const uint32_t pid = s.spawn(a, str.bytes);
    s.run();
    CHECK_EQ(s.status(pid), 7 << 8);
}

TEST(lseek_basic_only_a0_changes) {
    // END on "/" blocks and re-runs (see lseek_block_); fd, offset and whence
    // must still be in a0..a2 when it does, and the others stay as they were.
    System s;
    Strings str;
    const uint32_t p = str.add("/");
    Asm a;
    open(a, p);
    a.li(s2, 1000);
    lseek(a, 3, 5, kEnd);    // 69
    a.emit({ADD, a0, a0, s2, 0});
    a.emit({ADD, a0, a0, a1, 0});   // + 5
    a.emit({ADD, a0, a0, a2, 0});   // + 2
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a, str.bytes);
    s.run();
    CHECK_EQ(s.status(pid), 69 + 1000 + 5 + 2);
}

// =================== blocking ===================

TEST(lseek_block_end_may_wait_for_the_inode) {
    // open("/") reads nothing (the root is inode 1, known without the disk),
    // so END is the first call that needs inode block 2: it waits for the disk.
    System s;
    Strings str;
    const uint32_t p = str.add("/");
    Asm a;
    open(a, p);
    lseek(a, 3, 0, kEnd);
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a, str.bytes);
    s.run();
    CHECK_EQ(s.status(pid), 64);
    CHECK(s.m.steps() > 150);
}

TEST(lseek_block_set_and_cur_never_touch_the_disk) {
    System s;
    Strings str;
    const uint32_t p = str.add("/");
    Asm a;
    open(a, p);
    lseek(a, 3, 5, kSet);
    lseek(a, 3, 1, kCur);
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a, str.bytes);
    s.run();
    CHECK_EQ(s.status(pid), 6);
    CHECK(s.m.steps() < 100);
}

// =================== errors ===================

TEST(lseek_err_bad_fds_are_ebadf) {
    // Never opened, stdout, and out of range.
    CHECK_EQ(run_on("/hello", [](Asm& a) { lseek(a, 4, 0, kSet); }), -kEbadf);
    CHECK_EQ(run_on("/hello", [](Asm& a) { lseek(a, 1, 0, kSet); }), -kEbadf);
    CHECK_EQ(run_on("/hello", [](Asm& a) { lseek(a, 16, 0, kSet); }), -kEbadf);
    CHECK_EQ(run_on("/hello", [](Asm& a) { lseek(a, -1, 0, kSet); }), -kEbadf);
}

TEST(lseek_err_closed_fd_is_ebadf) {
    CHECK_EQ(run_on("/hello", [](Asm& a) {
        a.li(a0, 3);
        a.syscall(SYS_CLOSE);
        lseek(a, 3, 0, kSet);
    }), -kEbadf);
}

TEST(lseek_err_bad_fd_comes_before_bad_whence) {
    CHECK_EQ(run_on("/hello", [](Asm& a) { lseek(a, 4, 0, 3); }), -kEbadf);
}

TEST(lseek_err_bad_whence_is_einval) {
    CHECK_EQ(run_on("/hello", [](Asm& a) { lseek(a, 3, 0, 3); }), -kEinval);
    CHECK_EQ(run_on("/hello", [](Asm& a) { lseek(a, 3, 0, -1); }), -kEinval);
}

TEST(lseek_err_negative_result_is_einval) {
    CHECK_EQ(run_on("/hello", [](Asm& a) { lseek(a, 3, -1, kSet); }), -kEinval);
    CHECK_EQ(run_on("/hello", [](Asm& a) { lseek(a, 3, -1, kCur); }), -kEinval);
    CHECK_EQ(run_on("/hello", [](Asm& a) { lseek(a, 3, -3, kEnd); }), -kEinval);
}

TEST(lseek_err_result_above_0x7fffffff_is_einval) {
    // 0x7FFFFFFF + 1 would wrap to a negative int32: it must be caught, not wrapped.
    CHECK_EQ(run_on("/hello", [](Asm& a) {
        lseek(a, 3, 0x7FFFFFFF, kSet);
        lseek(a, 3, 1, kCur);
    }), -kEinval);
    CHECK_EQ(run_on("/hello", [](Asm& a) { lseek(a, 3, 0x7FFFFFFE, kEnd); }), -kEinval);
}

TEST(lseek_err_failed_lseek_leaves_the_offset) {
    CHECK_EQ(run_on("/hello", [](Asm& a) {
        lseek(a, 3, 5, kSet);
        lseek(a, 3, -10, kCur);   // -EINVAL
        lseek(a, 3, 0, kCur);     // still 5
    }), 5);
}
