// fork and open files, spec C3 (L10, "Process Sharing File"): the child's fd
// table points to the parent's open-file entries, so they share each offset;
// an entry lives until the last fd pointing to it is closed.
//
//   forkfile_share_   one offset for both: reads and lseeks in one move the other
//   forkfile_refs_    the child closing or ending doesn't close the parent's file
//   forkfile_apart_   opens after the fork are separate; so are the fd tables
#include <string>

#include "fs_system.hpp"

using namespace fs_system;

namespace {

// fork; then "if a0 == 0 goto child". Returns the branch, to patch.
size_t fork_and_branch_to_child(Asm& a) {
    a.syscall(SYS_FORK);
    return a.branch(BEQ, a0, zero);
}

// wait(-1, 0): waits for any child, keeps no status.
void wait_any(Asm& a) {
    a.li(a0, -1);
    a.li(a1, 0);
    a.syscall(SYS_WAIT);
}

struct Result {
    int32_t status;   // the parent's
    std::string out;
};

// Spawns the program, runs everything, returns the first process's status and stdout.
Result run(const Asm& a, const Strings& str) {
    System s;
    const uint32_t pid = s.spawn(a, str.bytes);
    s.run();
    return {s.status(pid), s.out.str()};
}

}  // namespace

// =================== one shared offset ===================

TEST(forkfile_share_child_read_moves_the_parents_offset) {
    // The child reads "h"; after waiting, the parent goes on from there: "i".
    Strings str;
    const uint32_t p = str.add("/hello");
    Asm a;
    open(a, p);
    const size_t to_child = fork_and_branch_to_child(a);
    wait_any(a);                 // parent
    read_and_print(a, 3, 1);
    a.exit_with_a0();
    a.patch(to_child, a.here());
    read_and_print(a, 3, 1);     // child
    a.exit_with(0);
    const Result r = run(a, str);
    CHECK_EQ(r.out, std::string("hi"));
    CHECK_EQ(r.status, 1);
}

TEST(forkfile_share_parent_read_before_fork_is_seen_by_child) {
    // The offset is in the shared entry, not copied: the child starts at 1.
    Strings str;
    const uint32_t p = str.add("/hello");
    Asm a;
    open(a, p);
    read_and_print(a, 3, 1);
    const size_t to_child = fork_and_branch_to_child(a);
    wait_any(a);                 // parent: the file is now at its end
    read(a, 3, kBuf, 10);
    a.exit_with_a0();
    a.patch(to_child, a.here());
    read_and_print(a, 3, 10);    // child
    a.exit_with(0);
    const Result r = run(a, str);
    CHECK_EQ(r.out, std::string("hi"));
    CHECK_EQ(r.status, 0);
}

TEST(forkfile_share_child_lseek_moves_the_parents_offset) {
    Strings str;
    const uint32_t p = str.add("/hello");
    Asm a;
    open(a, p);
    const size_t to_child = fork_and_branch_to_child(a);
    wait_any(a);                 // parent
    lseek(a, 3, 0, kCur);
    a.exit_with_a0();
    a.patch(to_child, a.here());
    lseek(a, 3, 1, kSet);        // child
    a.exit_with(0);
    CHECK_EQ(run(a, str).status, 1);
}

// =================== the entry outlives one fd ===================

TEST(forkfile_refs_child_exit_keeps_the_parents_file) {
    // The child ends without closing: its fd 3 goes, the parent's stays.
    Strings str;
    const uint32_t p = str.add("/hello");
    Asm a;
    open(a, p);
    const size_t to_child = fork_and_branch_to_child(a);
    wait_any(a);                 // parent
    read_and_print(a, 3, 10);
    a.exit_with_a0();
    a.patch(to_child, a.here());
    a.exit_with(0);              // child
    const Result r = run(a, str);
    CHECK_EQ(r.status, 2);
    CHECK_EQ(r.out, std::string("hi"));
}

TEST(forkfile_refs_child_close_keeps_the_parents_file) {
    Strings str;
    const uint32_t p = str.add("/hello");
    Asm a;
    open(a, p);
    const size_t to_child = fork_and_branch_to_child(a);
    wait_any(a);                 // parent
    read_and_print(a, 3, 10);
    a.exit_with_a0();
    a.patch(to_child, a.here());
    close(a, 3);                 // child: 0
    a.exit_with_a0();
    System s;
    const uint32_t parent = s.spawn(a, str.bytes);
    s.run();
    CHECK_EQ(s.status(parent), 2);
    CHECK_EQ(s.status(parent + 1), 0);
    CHECK_EQ(s.out.str(), std::string("hi"));
}

TEST(forkfile_refs_child_exit_does_not_free_the_entry) {
    // If the child's exit freed the shared entry, the parent's next open would
    // take that same entry for /bin/prog, and fd 3 would then read "elf".
    Strings str;
    const uint32_t hello = str.add("/hello");
    const uint32_t prog = str.add("/bin/prog");
    Asm a;
    open(a, hello);
    const size_t to_child = fork_and_branch_to_child(a);
    wait_any(a);                 // parent
    open(a, prog);               // fd 4, in a new entry
    read_and_print(a, 3, 10);
    a.exit_with_a0();
    a.patch(to_child, a.here());
    a.exit_with(0);              // child
    const Result r = run(a, str);
    CHECK_EQ(r.out, std::string("hi"));
    CHECK_EQ(r.status, 2);
}

TEST(forkfile_refs_child_close_does_not_free_the_entry) {
    Strings str;
    const uint32_t hello = str.add("/hello");
    const uint32_t prog = str.add("/bin/prog");
    Asm a;
    open(a, hello);
    const size_t to_child = fork_and_branch_to_child(a);
    wait_any(a);                 // parent
    open(a, prog);
    read_and_print(a, 3, 10);
    a.exit_with_a0();
    a.patch(to_child, a.here());
    close(a, 3);                 // child
    a.exit_with(0);
    const Result r = run(a, str);
    CHECK_EQ(r.out, std::string("hi"));
    CHECK_EQ(r.status, 2);
}

TEST(forkfile_refs_child_close_then_parent_close) {
    // Both fds can be closed, once each: the second close isn't EBADF.
    Strings str;
    const uint32_t p = str.add("/hello");
    Asm a;
    open(a, p);
    const size_t to_child = fork_and_branch_to_child(a);
    wait_any(a);                 // parent
    close(a, 3);
    a.exit_with_a0();
    a.patch(to_child, a.here());
    close(a, 3);                 // child
    a.exit_with(0);
    CHECK_EQ(run(a, str).status, 0);
}

TEST(forkfile_refs_entry_is_freed_when_both_are_gone) {
    // 70 rounds of: open, fork (the child exits at once), wait, close. Each
    // entry must be freed after both fds are gone, or open hits ENFILE (64).
    Strings str;
    const uint32_t p = str.add("/hello");
    Asm a;
    for (int i = 0; i < 70; i++) {
        open(a, p);
        const size_t to_child = fork_and_branch_to_child(a);
        wait_any(a);             // parent
        close(a, 3);
        const size_t over_child = a.here();
        a.emit({JAL, zero, 0, 0, 0});
        a.patch(to_child, a.here());
        a.exit_with(0);          // child
        a.patch(over_child, a.here());
    }
    open(a, p);
    a.exit_with_a0();
    CHECK_EQ(run(a, str).status, 3);
}

TEST(forkfile_refs_three_processes_share_one_entry) {
    // Parent and two children: each reads one byte of "hi" in turn; the third
    // read finds the end. Then everyone has closed it.
    Strings str;
    const uint32_t p = str.add("/hello");
    Asm a;
    open(a, p);
    const size_t first = fork_and_branch_to_child(a);
    wait_any(a);                 // parent, after child 1
    const size_t second = fork_and_branch_to_child(a);
    wait_any(a);                 // parent, after child 2
    read(a, 3, kBuf, 10);        // 0: the end
    a.exit_with_a0();
    a.patch(first, a.here());
    read_and_print(a, 3, 1);     // child 1: "h"
    a.exit_with(0);
    a.patch(second, a.here());
    read_and_print(a, 3, 1);     // child 2: "i"
    a.exit_with(0);
    const Result r = run(a, str);
    CHECK_EQ(r.out, std::string("hi"));
    CHECK_EQ(r.status, 0);
}

// =================== what is not shared ===================

TEST(forkfile_apart_opens_after_fork_are_separate) {
    // Each opens /hello after the fork: two entries, each reads from 0.
    Strings str;
    const uint32_t p = str.add("/hello");
    Asm a;
    const size_t to_child = fork_and_branch_to_child(a);
    wait_any(a);                 // parent
    open(a, p);
    read_and_print(a, 3, 10);
    a.exit_with_a0();
    a.patch(to_child, a.here());
    open(a, p);                  // child
    read_and_print(a, 3, 10);
    a.exit_with(0);
    const Result r = run(a, str);
    CHECK_EQ(r.out, std::string("hihi"));
    CHECK_EQ(r.status, 2);
}

TEST(forkfile_apart_fd_tables_are_copies) {
    // Both have fd 3 from before the fork; each next open gets fd 4 in its own table.
    Strings str;
    const uint32_t p = str.add("/hello");
    Asm a;
    open(a, p);
    const size_t to_child = fork_and_branch_to_child(a);
    wait_any(a);                 // parent: the child's open took nothing here
    open(a, p);
    a.exit_with_a0();
    a.patch(to_child, a.here());
    open(a, p);                  // child
    a.exit_with_a0();
    System s;
    const uint32_t parent = s.spawn(a, str.bytes);
    s.run();
    CHECK_EQ(s.status(parent), 4);
    CHECK_EQ(s.status(parent + 1), 4);
}

TEST(forkfile_apart_child_close_leaves_the_parents_fd) {
    // Closing the child's fd 3 empties a slot in the child's table only: the
    // parent's next open still finds 3 taken and gets 4.
    Strings str;
    const uint32_t p = str.add("/hello");
    Asm a;
    open(a, p);
    const size_t to_child = fork_and_branch_to_child(a);
    wait_any(a);                 // parent
    open(a, p);
    a.exit_with_a0();
    a.patch(to_child, a.here());
    close(a, 3);                 // child
    a.exit_with(0);
    CHECK_EQ(run(a, str).status, 4);
}
