// Pipes (spec Part F): a bounded byte buffer in the kernel, with two ends.
//
//   pipe_create_  the fds it returns; its errors
//   pipe_data_    bytes go through in order; short reads and writes; wrong ends
//   pipe_block_   readers wait when it's empty, writers when it's full
//   pipe_close_   end of file, -EPIPE, and who holds an end open
//   pipe_share_   fork, exec and threads
//
// Most programs make one pipe first, in a process with no other files, so its
// fds are kRead and kWrite. pipe_create_ checks that.
#include <string>
#include <vector>

#include "cpu_helpers.hpp"
#include "fs_system.hpp"

using namespace fs_system;

namespace {

constexpr int32_t kEspipe = 29, kEpipe = 32;
constexpr int32_t kRead = 3, kWrite = 4;
constexpr int32_t kPipeSize = 4096;
constexpr int32_t kPage = 4096;
constexpr uint32_t kFds = kBuf;                // on the stack: pipe's two words
constexpr uint32_t kStatus = kBuf + 16;        // wait's status
constexpr uint32_t kIn = kBuf + 64;            // where programs read bytes into
constexpr uint64_t kShort = 2'000'000;
constexpr uint64_t kDisk = 100'000'000;        // for programs that open files
constexpr int32_t kBad = 100;                  // exit codes from here on: a check failed

void expect(Asm& a, uint8_t r, int32_t value, int32_t code) {
    a.li(t6, value);
    const size_t ok = a.branch(BEQ, r, t6);
    a.exit_with(code);
    a.patch(ok, a.here());
}

// pipe(fds_addr): a0 = 0 or -error.
void pipe_at(Asm& a, uint32_t fds_addr) {
    a.li(a0, static_cast<int32_t>(fds_addr));
    a.syscall(SYS_PIPE);
}

// Makes the program's pipe: it must get kRead and kWrite.
// signal(SIGPIPE, ignore): a write with no reader returns -EPIPE instead of
// ending the writer (spec F3). Before signals exist, the call is just -ENOSYS.
void ignore_sigpipe(Asm& a) {
    a.li(a0, 13);
    a.li(a1, 1);
    a.li(a2, 0);
    a.syscall(SYS_SIGNAL);
}

void make_pipe(Asm& a) {
    pipe_at(a, kFds);
    expect(a, a0, 0, kBad);
}

// write(fd, buffer, count): a0 = bytes written or -error.
void write_to(Asm& a, int32_t fd, uint32_t buffer, int32_t count) {
    a.li(a0, fd);
    a.li(a1, static_cast<int32_t>(buffer));
    a.li(a2, count);
    a.syscall(SYS_WRITE);
}

// write(fd, the address in r, count).
void write_from(Asm& a, int32_t fd, uint8_t r, int32_t count) {
    a.emit({ADDI, a1, r, 0, 0});
    a.li(a0, fd);
    a.li(a2, count);
    a.syscall(SYS_WRITE);
}

// read(fd, kIn, count), then writes what it got to stdout; a0 stays read's result.
void read_and_show(Asm& a, int32_t count) {
    read(a, kRead, kIn, count);
    a.emit({ADDI, s3, a0, 0, 0});
    a.emit({ADDI, a2, a0, 0, 0});
    a.li(a0, 1);
    a.li(a1, static_cast<int32_t>(kIn));
    a.syscall(SYS_WRITE);
    a.emit({ADDI, a0, s3, 0, 0});
}

void print(Asm& a, uint32_t letter) { a.write_stdout(letter, 1); }

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

// s1 = a new heap of `pages` pages.
void heap(Asm& a, int32_t pages) {
    a.sbrk(pages * kPage);
    a.emit({ADDI, s1, a0, 0, 0});
}

void spin(Asm& a) {
    const size_t loop = a.here();
    a.patch(a.branch(JAL, zero, zero), loop);
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

// Fills the pipe from a heap page (uses s1): it must take all 4096 bytes.
void fill_pipe(Asm& a) {
    heap(a, 1);
    write_from(a, kWrite, s1, kPipeSize);
    expect(a, a0, kPipeSize, kBad + 1);
}

}  // namespace

// =================== pipe ===================

TEST(pipe_create_returns_the_lowest_fds_read_end_first) {
    Asm a;
    pipe_at(a, kFds);
    expect(a, a0, 0, 1);
    a.li(t0, static_cast<int32_t>(kFds));
    a.emit({LW, s2, t0, 0, 0});
    a.emit({LW, s3, t0, 0, 4});
    expect(a, s2, 3, 2);
    expect(a, s3, 4, 3);
    a.exit_with(7);
    CHECK_EQ(run_one(a), 7);
}

TEST(pipe_create_skips_fds_in_use) {
    System s;
    Strings str;
    const uint32_t path = str.add("/hello");
    Asm a;
    open(a, path);                          // fd 3
    pipe_at(a, kFds);
    expect(a, a0, 0, 1);
    a.li(t0, static_cast<int32_t>(kFds));
    a.emit({LW, s2, t0, 0, 0});
    a.emit({LW, s3, t0, 0, 4});
    expect(a, s2, 4, 2);
    expect(a, s3, 5, 3);
    a.exit_with(7);
    CHECK_EQ(run_one(s, a, str, kDisk), 7);
}

TEST(pipe_create_a_bad_pointer_is_efault_and_makes_nothing) {
    Asm a;
    pipe_at(a, kFds + 1);                   // not aligned
    expect(a, a0, -kEfault, 1);
    pipe_at(a, 0);                          // not writable
    expect(a, a0, -kEfault, 2);
    pipe_at(a, 0x10000);                    // the code: read-only
    expect(a, a0, -kEfault, 3);
    make_pipe(a);                           // the fds are still free
    a.li(t0, static_cast<int32_t>(kFds));
    a.emit({LW, s2, t0, 0, 0});
    expect(a, s2, 3, 4);
    a.exit_with(7);
    CHECK_EQ(run_one(a), 7);
}

TEST(pipe_create_fewer_than_two_fds_is_emfile_and_makes_nothing) {
    // 12 opens leave only fd 15: pipe fails, and fd 15 is still free.
    System s;
    Strings str;
    const uint32_t path = str.add("/hello");
    Asm a;
    for (int i = 0; i < 12; i++) open(a, path);
    expect(a, a0, 14, 1);
    pipe_at(a, kFds);
    expect(a, a0, -kEmfile, 2);
    open(a, path);
    expect(a, a0, 15, 3);
    a.exit_with(7);
    CHECK_EQ(run_one(s, a, str, kDisk), 7);
}

TEST(pipe_create_fewer_than_two_entries_is_enfile_and_makes_nothing) {
    // 5 processes hold 6 pipes each: 60 of the 64 entries. The last one opens 3
    // files, which leaves one entry: too few for a pipe, enough for one more file.
    System s;
    Strings str;
    const uint32_t path = str.add("/hello");
    Asm holder;
    for (int i = 0; i < 6; i++) pipe_at(holder, kFds);
    holder.busy_wait(200'000);
    holder.exit_with(0);
    for (int i = 0; i < 5; i++) s.spawn(holder);
    Asm a;
    a.busy_wait(5000);                      // the holders have their pipes by now
    for (int i = 0; i < 3; i++) open(a, path);
    expect(a, a0, 5, 1);
    pipe_at(a, kFds);
    expect(a, a0, -kEnfile, 2);
    open(a, path);
    expect(a, a0, 6, 3);
    a.exit_with(7);
    CHECK_EQ(run_one(s, a, str, kDisk), 7);
}

// =================== bytes through a pipe ===================

TEST(pipe_data_comes_out_in_order) {
    System s;
    Strings str;
    const uint32_t abc = str.add("abc"), de = str.add("de");
    Asm a;
    make_pipe(a);
    write_to(a, kWrite, abc, 3);
    expect(a, a0, 3, 1);
    write_to(a, kWrite, de, 2);
    expect(a, a0, 2, 2);
    read_and_show(a, 2);
    expect(a, a0, 2, 3);
    read_and_show(a, 3);
    expect(a, a0, 3, 4);
    a.exit_with(7);
    CHECK_EQ(run_one(s, a, str), 7);
    CHECK_EQ(s.out.str(), std::string("abcde"));
}

TEST(pipe_data_read_returns_what_is_there_without_waiting_for_more) {
    System s;
    Strings str;
    const uint32_t ab = str.add("ab");
    Asm a;
    make_pipe(a);
    write_to(a, kWrite, ab, 2);
    read_and_show(a, 100);
    a.exit_with_a0();
    CHECK_EQ(run_one(s, a, str), 2);
    CHECK_EQ(s.out.str(), std::string("ab"));
}

TEST(pipe_data_a_count_of_zero_returns_0_without_blocking) {
    // Empty pipe, then full pipe: neither call has anything to wait for.
    Asm a;
    make_pipe(a);
    read(a, kRead, kIn, 0);
    expect(a, a0, 0, 1);
    fill_pipe(a);
    write_to(a, kWrite, kIn, 0);
    expect(a, a0, 0, 2);
    a.exit_with(7);
    CHECK_EQ(run_one(a), 7);
}

TEST(pipe_data_write_takes_only_what_fits) {
    Asm a;
    make_pipe(a);
    heap(a, 2);
    write_from(a, kWrite, s1, kPipeSize + 100);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), kPipeSize);
}

TEST(pipe_data_wraps_around_the_buffer) {
    // 100 bytes at a time, 100 times: 10000 bytes through 4096. Each round has its
    // own first and last byte, so a wrong position in the circle shows.
    Asm a;
    make_pipe(a);
    heap(a, 1);
    a.li(s2, 0);
    a.li(s4, 100);
    const size_t top = a.here();
    a.emit({SB, 0, s1, s2, 0});
    a.emit({SB, 0, s1, s2, 99});
    write_from(a, kWrite, s1, 100);
    expect(a, a0, 100, 1);
    read(a, kRead, kIn, 100);
    expect(a, a0, 100, 2);
    a.li(t0, static_cast<int32_t>(kIn));
    a.emit({LBU, t1, t0, 0, 0});
    const size_t first_ok = a.branch(BEQ, t1, s2);
    a.exit_with(3);
    a.patch(first_ok, a.here());
    a.emit({LBU, t1, t0, 0, 99});
    const size_t last_ok = a.branch(BEQ, t1, s2);
    a.exit_with(4);
    a.patch(last_ok, a.here());
    a.emit({ADDI, s2, s2, 0, 1});
    a.patch(a.branch(BNE, s2, s4), top);
    a.exit_with(7);
    CHECK_EQ(run_one(a), 7);
}

TEST(pipe_data_the_wrong_end_is_ebadf) {
    System s;
    Strings str;
    const uint32_t x = str.add("x"), path = str.add("/hello");
    Asm a;
    make_pipe(a);
    write_to(a, kRead, x, 1);
    expect(a, a0, -kEbadf, 1);
    read(a, kWrite, kIn, 1);
    expect(a, a0, -kEbadf, 2);
    open(a, path);                          // fd 5: a file still can't be written
    write_to(a, 5, x, 1);
    expect(a, a0, -kEbadf, 3);
    a.exit_with(7);
    CHECK_EQ(run_one(s, a, str, kDisk), 7);
}

TEST(pipe_data_lseek_is_espipe) {
    Asm a;
    make_pipe(a);
    lseek(a, kRead, 0, kSet);
    expect(a, a0, -kEspipe, 1);
    lseek(a, kWrite, 0, kSet);
    expect(a, a0, -kEspipe, 2);
    a.exit_with(7);
    CHECK_EQ(run_one(a), 7);
}

TEST(pipe_data_write_from_a_bad_buffer_is_efault_and_adds_nothing) {
    System s;
    Strings str;
    const uint32_t ab = str.add("ab");
    Asm a;
    make_pipe(a);
    write_to(a, kWrite, 0, 1);
    expect(a, a0, -kEfault, 1);
    write_to(a, kWrite, ab, 2);
    read_and_show(a, 100);
    a.exit_with_a0();
    CHECK_EQ(run_one(s, a, str), 2);
    CHECK_EQ(s.out.str(), std::string("ab"));
}

TEST(pipe_data_read_into_a_bad_buffer_is_efault_and_takes_nothing) {
    System s;
    Strings str;
    const uint32_t ab = str.add("ab");
    Asm a;
    make_pipe(a);
    write_to(a, kWrite, ab, 2);
    read(a, kRead, 0x10000, 2);             // the code: read-only
    expect(a, a0, -kEfault, 1);
    read_and_show(a, 100);
    a.exit_with_a0();
    CHECK_EQ(run_one(s, a, str), 2);
    CHECK_EQ(s.out.str(), std::string("ab"));
}

TEST(pipe_data_read_maps_an_untouched_heap_page) {
    Asm a;
    make_pipe(a);
    a.li(s2, 9);
    a.li(t0, static_cast<int32_t>(kIn));
    a.emit({SB, 0, t0, s2, 0});
    write_to(a, kWrite, kIn, 1);
    heap(a, 1);
    a.emit({ADDI, a1, s1, 0, 0});
    a.li(a0, kRead);
    a.li(a2, 1);
    a.syscall(SYS_READ);
    expect(a, a0, 1, 1);
    a.emit({LBU, a0, s1, 0, 0});
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 9);
}

// =================== blocking ===================

TEST(pipe_block_a_reader_waits_for_the_writer) {
    // The parent reads first, but can't get past read until the child has written.
    System s;
    Strings str;
    const uint32_t c = str.add("C"), hi = str.add("hi");
    Asm a;
    make_pipe(a);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    a.busy_wait(2000);
    print(a, c);
    write_to(a, kWrite, hi, 2);
    a.exit_with(0);
    a.patch(to_parent, a.here());
    read_and_show(a, 2);
    expect(a, a0, 2, 1);
    a.exit_with(7);
    CHECK_EQ(run_one(s, a, str), 7);
    CHECK_EQ(s.out.str(), std::string("Chi"));
}

TEST(pipe_block_a_writer_waits_for_room) {
    // The pipe is full, so the parent's next write can't finish until the child
    // has taken a byte. It then writes the one byte there's room for.
    System s;
    Strings str;
    const uint32_t c = str.add("C"), p = str.add("P");
    Asm a;
    make_pipe(a);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    a.busy_wait(2000);
    print(a, c);
    read(a, kRead, kIn, 1);
    a.exit_with(0);
    a.patch(to_parent, a.here());
    fill_pipe(a);
    write_to(a, kWrite, p, 1);
    expect(a, a0, 1, 1);
    print(a, p);
    a.exit_with(7);
    CHECK_EQ(run_one(s, a, str), 7);
    CHECK_EQ(s.out.str(), std::string("CP"));
}

TEST(pipe_block_a_blocked_reader_is_not_runnable) {
    System s;
    Asm a;
    make_pipe(a);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    spin(a);                                // the child holds the write end open
    a.patch(to_parent, a.here());
    read(a, kRead, kIn, 1);
    a.exit_with(1);
    const uint32_t pid = s.spawn(a);
    CHECK(std::holds_alternative<kernel::StepLimit>(s.k.run_all(20'000)));
    const kernel::Thread* thread = s.k.thread(pid);
    CHECK(thread != nullptr);
    if (thread) CHECK(thread->blocked == kernel::BlockReason::Pipe);
}

TEST(pipe_block_a_blocked_write_moved_nothing_when_re_run) {
    // Woken and re-run, a write must add its bytes once only: 4096 + 1 in all.
    System s;
    Strings str;
    const uint32_t x = str.add("x");
    Asm a;
    make_pipe(a);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    close(a, kWrite);
    a.busy_wait(2000);
    a.li(s5, 0);
    const size_t top = a.here();
    read(a, kRead, kIn, 1000);
    a.emit({ADD, s5, s5, a0, 0});
    a.patch(a.branch(BLT, zero, a0), top);
    a.emit({ADDI, a0, s5, 0, 0});
    a.exit_with_a0();
    a.patch(to_parent, a.here());
    close(a, kRead);
    fill_pipe(a);
    write_to(a, kWrite, x, 1);
    close(a, kWrite);
    wait_status(a);
    a.exit_with_a0();
    CHECK_EQ(run_one(s, a, str), kPipeSize + 1);
}

TEST(pipe_block_everything_arrives_through_a_small_buffer) {
    // L4's producer and consumer: 5 pages, each byte of page i worth i, through a
    // 4096-byte buffer. The writer loops on short writes; the reader reads to the
    // end of file and returns the sum of all the bytes.
    Asm a;
    make_pipe(a);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    close(a, kWrite);
    a.li(s5, 0);
    const size_t next_read = a.here();
    read(a, kRead, kIn, 1000);
    const size_t at_end = a.branch(BEQ, a0, zero);
    a.emit({ADDI, s3, a0, 0, 0});
    a.li(s4, static_cast<int32_t>(kIn));
    const size_t next_byte = a.here();
    a.emit({LBU, t0, s4, 0, 0});
    a.emit({ADD, s5, s5, t0, 0});
    a.emit({ADDI, s4, s4, 0, 1});
    a.emit({ADDI, s3, s3, 0, -1});
    a.patch(a.branch(BNE, s3, zero), next_byte);
    a.patch(a.branch(JAL, zero, zero), next_read);
    a.patch(at_end, a.here());
    a.emit({ADDI, a0, s5, 0, 0});
    a.exit_with_a0();
    a.patch(to_parent, a.here());
    close(a, kRead);
    heap(a, 1);
    a.li(s2, 1);
    a.li(s6, 6);
    const size_t next_page = a.here();
    a.li(s3, 0);
    a.li(s4, kPage);
    const size_t fill = a.here();
    a.emit({ADD, t0, s1, s3, 0});
    a.emit({SB, 0, t0, s2, 0});
    a.emit({ADDI, s3, s3, 0, 1});
    a.patch(a.branch(BNE, s3, s4), fill);
    a.li(s3, 0);
    const size_t again = a.here();
    a.emit({ADD, a1, s1, s3, 0});
    a.emit({SUB, a2, s4, s3, 0});
    a.li(a0, kWrite);
    a.syscall(SYS_WRITE);
    const size_t wrote = a.branch(BLT, zero, a0);
    a.exit_with(1);
    a.patch(wrote, a.here());
    a.emit({ADD, s3, s3, a0, 0});
    a.patch(a.branch(BNE, s3, s4), again);
    a.emit({ADDI, s2, s2, 0, 1});
    a.patch(a.branch(BNE, s2, s6), next_page);
    close(a, kWrite);
    wait_status(a);
    a.exit_with_a0();
    System s;
    CHECK_EQ(run_one(s, a, {}, 10'000'000), kPage * (1 + 2 + 3 + 4 + 5));
}

// =================== closing ===================

TEST(pipe_close_the_reader_gets_the_bytes_then_end_of_file) {
    System s;
    Strings str;
    const uint32_t ab = str.add("ab");
    Asm a;
    make_pipe(a);
    write_to(a, kWrite, ab, 2);
    close(a, kWrite);
    read_and_show(a, 100);
    expect(a, a0, 2, 1);
    read(a, kRead, kIn, 100);
    a.exit_with_a0();
    CHECK_EQ(run_one(s, a, str), 0);
    CHECK_EQ(s.out.str(), std::string("ab"));
}

TEST(pipe_close_writing_with_no_reader_is_epipe) {
    System s;
    Strings str;
    const uint32_t x = str.add("x");
    Asm a;
    ignore_sigpipe(a);
    make_pipe(a);
    close(a, kRead);
    write_to(a, kWrite, x, 1);
    expect(a, a0, -kEpipe, 1);
    write_to(a, kWrite, x, 0);              // even for no bytes
    expect(a, a0, -kEpipe, 2);
    a.exit_with(7);
    CHECK_EQ(run_one(s, a, str), 7);
}

TEST(pipe_close_wakes_a_blocked_reader) {
    // The child blocks on the empty pipe. The parent closes the last write end.
    Asm a;
    make_pipe(a);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    close(a, kWrite);
    read(a, kRead, kIn, 1);
    a.emit({ADDI, a0, a0, 0, 7});           // end of file: 0 + 7
    a.exit_with_a0();
    a.patch(to_parent, a.here());
    a.busy_wait(2000);
    close(a, kWrite);
    wait_status(a);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 7);
}

TEST(pipe_close_wakes_a_blocked_writer) {
    // The child fills the pipe and blocks on one more byte. The parent closes the
    // last read end, so the child's write returns -EPIPE.
    Asm a;
    ignore_sigpipe(a);                      // the child keeps it
    make_pipe(a);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    close(a, kRead);
    fill_pipe(a);
    write_to(a, kWrite, kIn, 1);
    a.emit({SUB, a0, zero, a0, 0});         // -EPIPE becomes the status EPIPE
    a.exit_with_a0();
    a.patch(to_parent, a.here());
    a.busy_wait(20000);
    close(a, kRead);
    wait_status(a);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), kEpipe);
}

TEST(pipe_close_an_ending_process_closes_its_ends) {
    // The child never calls close: its exit closes the last write end, and that
    // gives the blocked parent its end of file.
    Asm a;
    make_pipe(a);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    a.busy_wait(2000);
    a.exit_with(0);
    a.patch(to_parent, a.here());
    close(a, kWrite);
    read(a, kRead, kIn, 1);
    a.emit({ADDI, a0, a0, 0, 7});
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 7);
}

TEST(pipe_close_a_reader_holding_the_write_end_blocks_forever) {
    // L4's example closes the end it doesn't use, and this is why. Nobody will ever
    // write, but the write end is still open, so there is no end of file.
    System s;
    Asm a;
    make_pipe(a);
    read(a, kRead, kIn, 1);
    a.exit_with(1);
    const uint32_t pid = s.spawn(a);
    CHECK(std::holds_alternative<kernel::StepLimit>(s.k.run_all(100'000)));
    CHECK(s.k.process(pid) != nullptr);
    const kernel::Thread* thread = s.k.thread(pid);
    CHECK(thread != nullptr);
    if (thread) CHECK(thread->blocked == kernel::BlockReason::Pipe);
}

TEST(pipe_close_frees_the_entries) {
    // 40 pipes need 80 entries, more than the 64 there are, if none were freed.
    Asm a;
    a.li(s2, 0);
    a.li(s4, 40);
    const size_t top = a.here();
    pipe_at(a, kFds);
    expect(a, a0, 0, 1);
    close(a, kRead);
    close(a, kWrite);
    a.emit({ADDI, s2, s2, 0, 1});
    a.patch(a.branch(BNE, s2, s4), top);
    a.exit_with(7);
    CHECK_EQ(run_one(a), 7);
}

// =================== fork, exec, threads ===================

TEST(pipe_share_the_lecture_example) {
    // L4: the parent writes a string, and the child reads and prints it. Each
    // closes the end it doesn't use.
    System s;
    Strings str;
    const uint32_t hello = str.add("Hello There!");
    Asm a;
    make_pipe(a);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    close(a, kWrite);
    read_and_show(a, 100);
    expect(a, a0, 12, 1);
    close(a, kRead);
    a.exit_with(0);
    a.patch(to_parent, a.here());
    close(a, kRead);
    write_to(a, kWrite, hello, 12);
    close(a, kWrite);
    wait_status(a);
    a.exit_with_a0();
    CHECK_EQ(run_one(s, a, str), 0);
    CHECK_EQ(s.out.str(), std::string("Hello There!"));
}

TEST(pipe_share_a_child_closing_leaves_the_parent_end_open) {
    // After fork each end has two fds. One closing isn't the end closing.
    System s;
    Strings str;
    const uint32_t ab = str.add("ab");
    Asm a;
    make_pipe(a);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    close(a, kRead);
    close(a, kWrite);
    a.exit_with(0);
    a.patch(to_parent, a.here());
    wait_status(a);
    write_to(a, kWrite, ab, 2);
    expect(a, a0, 2, 1);
    read_and_show(a, 2);
    a.exit_with_a0();
    CHECK_EQ(run_one(s, a, str), 2);
    CHECK_EQ(s.out.str(), std::string("ab"));
}

TEST(pipe_share_exec_keeps_the_ends) {
    // The fd table survives exec: the new program can write to the pipe its old
    // self made. This is how a shell connects programs.
    System s;
    Strings words;
    const uint32_t hi = words.add("hi");
    Asm next;
    write_to(next, kWrite, hi, 2);
    next.exit_with_a0();
    CHECK(std::holds_alternative<uint32_t>(fs::create_file(s.m.disk(), fs::kRootInode, "next", elf(next, words.bytes))));
    Strings str;
    const uint32_t path = str.add("/next");
    Asm a;
    make_pipe(a);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    a.li(a0, static_cast<int32_t>(path));
    a.syscall(SYS_EXEC);
    a.exit_with(1);
    a.patch(to_parent, a.here());
    close(a, kWrite);
    read_and_show(a, 2);
    expect(a, a0, 2, 1);
    read(a, kRead, kIn, 1);                 // the program's exit closed the last end
    a.emit({ADDI, a0, a0, 0, 7});
    a.exit_with_a0();
    CHECK_EQ(run_one(s, a, str, kDisk), 7);
    CHECK_EQ(s.out.str(), std::string("hi"));
}

TEST(pipe_share_between_threads) {
    // Threads share the fd table, so no fork is needed.
    System s;
    Strings str;
    const uint32_t hi = str.add("hi");
    Asm a;
    const size_t to_main = a.branch(JAL, zero, zero);
    const uint32_t body = here_addr(a);
    a.busy_wait(2000);
    write_to(a, kWrite, hi, 2);
    a.li(a0, 0);
    a.syscall(SYS_THREAD_EXIT);
    a.patch(to_main, a.here());
    make_pipe(a);
    a.li(a0, static_cast<int32_t>(body));
    a.li(a1, 0);
    a.li(a2, 0);
    a.syscall(SYS_THREAD_CREATE);
    read_and_show(a, 2);
    a.exit_with_a0();
    CHECK_EQ(run_one(s, a, str), 2);
    CHECK_EQ(s.out.str(), std::string("hi"));
}

TEST(pipe_share_exit_ends_a_sibling_blocked_on_a_pipe_at_once) {
    // It isn't waiting on the disk, so the process doesn't have to wait for it.
    System s;
    Asm a;
    const size_t to_main = a.branch(JAL, zero, zero);
    const uint32_t body = here_addr(a);
    read(a, kRead, kIn, 1);
    a.exit_with(1);
    a.patch(to_main, a.here());
    make_pipe(a);
    a.li(a0, static_cast<int32_t>(body));
    a.li(a1, 0);
    a.li(a2, 0);
    a.syscall(SYS_THREAD_CREATE);
    a.syscall(SYS_YIELD);
    a.exit_with(3);
    const uint32_t pid = s.spawn(a);
    CHECK(std::holds_alternative<kernel::AllDone>(s.k.run_all(kShort)));
    CHECK_EQ(s.status(pid), 3);
    CHECK(s.k.thread(pid + 1) == nullptr);
    CHECK_EQ(s.m.frame_allocator().free_count(), mmu::kNumFrames);
}
