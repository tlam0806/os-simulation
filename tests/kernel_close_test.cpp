// close (a7 = 12), spec C3: emptying an fd slot, freeing the open-file entry
// once nothing points to it, and closing every fd when a process ends.
//
//   close_basic_   the result, the fd and the entry are free again
//   close_end_     an ending process closes its fds
//   close_err_     EBADF for fds that aren't open files; never blocks
#include "fs_system.hpp"

using namespace fs_system;

namespace {

// Runs one program and returns its exit status.
int32_t run_one(const Asm& a, const Strings& str) {
    System s;
    const uint32_t pid = s.spawn(a, str.bytes);
    s.run();
    return s.status(pid);
}

}  // namespace

// =================== the result, and what is freed ===================

TEST(close_basic_returns_0) {
    Strings str;
    const uint32_t p = str.add("/hello");
    Asm a;
    open(a, p);
    close(a, 3);
    a.exit_with_a0();
    CHECK_EQ(run_one(a, str), 0);
}

TEST(close_basic_fd_can_be_reused) {
    // The lowest empty fd is taken: after closing 3, the next open is 3 again.
    Strings str;
    const uint32_t p = str.add("/hello");
    Asm a;
    open(a, p);
    close(a, 3);
    open(a, p);
    a.exit_with_a0();
    CHECK_EQ(run_one(a, str), 3);
}

TEST(close_basic_lowest_fd_first) {
    // Open 3, 4, 5; close 4: the next open takes 4, not 6.
    Strings str;
    const uint32_t p = str.add("/hello");
    Asm a;
    for (int i = 0; i < 3; i++) open(a, p);
    close(a, 4);
    open(a, p);
    a.exit_with_a0();
    CHECK_EQ(run_one(a, str), 4);
}

TEST(close_basic_frees_the_open_file_entry) {
    // 70 opens, each closed again: more than the 64 entries of the open-file
    // table, so each close must free its entry, or an open fails with ENFILE.
    Strings str;
    const uint32_t p = str.add("/hello");
    Asm a;
    for (int i = 0; i < 70; i++) {
        open(a, p);
        close(a, 3);
    }
    open(a, p);
    a.exit_with_a0();
    CHECK_EQ(run_one(a, str), 3);
}

TEST(close_basic_other_fds_stay_open) {
    // Closing 3 leaves 4 alone: closing 4 afterwards still works.
    Strings str;
    const uint32_t p = str.add("/hello");
    Asm a;
    open(a, p);
    open(a, p);
    close(a, 3);
    keep(a);                 // 0
    close(a, 4);             // 0
    exit_with_both(a);
    CHECK_EQ(run_one(a, str), 0);
}

// =================== an ending process closes its fds ===================

TEST(close_end_exit_frees_the_entries) {
    // Five processes in turn each open 13 files and exit without closing:
    // 65 opens, more than 64 entries. The last one must still get fd 15.
    System s;
    Strings str;
    const uint32_t p = str.add("/hello");
    Asm a;
    for (int i = 0; i < 13; i++) open(a, p);
    a.exit_with_a0();
    for (int i = 0; i < 5; i++) {
        const uint32_t pid = s.spawn(a, str.bytes);
        s.run();
        CHECK_EQ(s.status(pid), 15);
    }
}

TEST(close_end_killed_process_frees_the_entries) {
    // Same, but each process dies on a bad load instead of exiting.
    System s;
    Strings str;
    const uint32_t p = str.add("/hello");
    Asm a;
    for (int i = 0; i < 13; i++) open(a, p);
    a.li(t0, 0);
    a.emit({LW, t1, t0, 0, 0});   // load from address 0: killed
    a.exit_with(0);
    for (int i = 0; i < 5; i++) {
        s.spawn(a, str.bytes);
        s.run();
    }
    Asm b;
    for (int i = 0; i < 13; i++) open(b, p);
    b.exit_with_a0();
    const uint32_t pid = s.spawn(b, str.bytes);
    s.run();
    CHECK_EQ(s.status(pid), 15);
}

// =================== errors ===================

TEST(close_err_unopened_fd_is_ebadf) {
    Asm a;
    close(a, 3);
    a.exit_with_a0();
    CHECK_EQ(run_one(a, {}), -kEbadf);
}

TEST(close_err_twice_is_ebadf) {
    Strings str;
    const uint32_t p = str.add("/hello");
    Asm a;
    open(a, p);
    close(a, 3);
    close(a, 3);
    a.exit_with_a0();
    CHECK_EQ(run_one(a, str), -kEbadf);
}

TEST(close_err_std_fds_are_ebadf) {
    // 0, 1 and 2 never point to an open-file entry, and stdout still works after.
    System s;
    Strings str;
    const uint32_t msg = str.add("ok");
    Asm a;
    close(a, 0);
    keep(a);
    close(a, 1);
    a.emit({ADD, s1, s1, a0, 0});
    close(a, 2);
    a.emit({ADD, s1, s1, a0, 0});
    a.write_stdout(msg, 2);
    a.emit({ADDI, a0, s1, 0, 0});
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a, str.bytes);
    s.run();
    CHECK_EQ(s.status(pid), -3 * kEbadf);
    CHECK_EQ(s.out.str(), std::string("ok"));
}

TEST(close_err_out_of_range_is_ebadf) {
    Asm a;
    close(a, 16);
    keep(a);
    close(a, -1);
    a.emit({ADD, a0, a0, s1, 0});
    a.exit_with_a0();
    CHECK_EQ(run_one(a, {}), -2 * kEbadf);
}

TEST(close_err_never_touches_the_disk) {
    // close needs no block: with nothing opened, the clock never reaches a
    // disk completion.
    System s;
    Asm a;
    for (int fd = 0; fd < 20; fd++) close(a, fd);
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(pid), -kEbadf);
    CHECK(s.m.steps() < 200);
}
