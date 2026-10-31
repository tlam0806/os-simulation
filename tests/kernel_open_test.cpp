// open (a7 = 11), spec C2 + C3, stage 1: the block cache without eviction,
// block-and-re-run, reading the path from user memory, walking the path,
// the fd table.
//
//   open_basic_   which fd comes back; files, directories, nested paths
//   open_block_   the caller blocks on cache misses; others run; cache hits
//                 don't touch the disk; the disk queue is shared with disk_read
//   open_err_     ENOENT, ENOTDIR, EINVAL, EFAULT, ENAMETOOLONG, EMFILE
//
// Every test's disk holds:   /hello      (a file, "hi")
//                            /bin        (a directory)
//                            /bin/prog   (a file)
#include <string>

#include "fs_system.hpp"

using namespace fs_system;

namespace {

// Runs one program that opens `path` and exits with open's result.
int32_t open_one(const std::string& path) {
    System s;
    Strings str;
    const uint32_t p = str.add(path);
    Asm a;
    open(a, p);
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a, str.bytes);
    s.run();
    return s.status(pid);
}

}  // namespace

// =================== which fd comes back ===================

TEST(open_basic_file_in_root_is_fd_3) {
    // 0, 1 and 2 are stdin, stdout and stderr.
    CHECK_EQ(open_one("/hello"), 3);
}

TEST(open_basic_second_open_is_fd_4) {
    // Two opens of the same file are two entries: fds 3 and 4.
    System s;
    Strings str;
    const uint32_t p = str.add("/hello");
    Asm a;
    open(a, p);
    keep(a);
    open(a, p);
    exit_with_both(a);
    const uint32_t pid = s.spawn(a, str.bytes);
    s.run();
    CHECK_EQ(s.status(pid), (3 << 8) + 4);
}

TEST(open_basic_nested_path) {
    CHECK_EQ(open_one("/bin/prog"), 3);
}

TEST(open_basic_directories_can_be_opened) {
    CHECK_EQ(open_one("/"), 3);
    CHECK_EQ(open_one("/bin"), 3);
}

TEST(open_basic_extra_slashes) {
    CHECK_EQ(open_one("//bin///prog"), 3);
    CHECK_EQ(open_one("/bin/"), 3);
}

TEST(open_basic_each_process_has_its_own_fds) {
    System s;
    Strings str;
    const uint32_t p = str.add("/hello");
    Asm a;
    open(a, p);
    a.exit_with_a0();
    const uint32_t first = s.spawn(a, str.bytes);
    const uint32_t second = s.spawn(a, str.bytes);
    s.run();
    CHECK_EQ(s.status(first), 3);
    CHECK_EQ(s.status(second), 3);
}

TEST(open_basic_only_a0_changes) {
    // The call blocks and re-runs several times; the kernel must keep the
    // path in a0 meanwhile, and leave every other register alone.
    System s;
    Strings str;
    const uint32_t p = str.add("/bin/prog");
    Asm a;
    a.li(s2, 77);
    a.li(a1, 1000);
    a.li(a2, 20000);
    open(a, p);
    a.emit({ADD, a0, a0, s2, 0});
    a.emit({ADD, a0, a0, a1, 0});
    a.emit({ADD, a0, a0, a2, 0});
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a, str.bytes);
    s.run();
    CHECK_EQ(s.status(pid), 3 + 77 + 1000 + 20000);
}

// =================== blocking and the cache ===================

TEST(open_block_caller_is_blocked_on_a_miss) {
    // The first block it needs isn't cached: the caller blocks, its read is
    // on the disk. Nothing else runs, so the kernel idles until run_all's limit.
    System s;
    Strings str;
    const uint32_t p = str.add("/hello");
    Asm a;
    open(a, p);
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a, str.bytes);
    CHECK(std::holds_alternative<kernel::StepLimit>(s.k.run_all(20)));
    const kernel::Process* proc = s.k.process(pid);
    CHECK(proc != nullptr);
    if (proc) CHECK(proc->blocked == kernel::BlockReason::IO);
    CHECK(s.m.disk().busy());
    s.run();
    CHECK_EQ(s.status(pid), 3);
}

TEST(open_block_others_run_meanwhile) {
    // pid 1 opens, then prints A. pid 2 prints B at once. B comes first.
    System s;
    Strings str;
    const uint32_t p = str.add("/bin/prog");
    const uint32_t letters = str.add("AB");
    Asm opener;
    open(opener, p);
    opener.write_stdout(letters, 1);
    opener.exit_with(0);
    Asm other;
    other.write_stdout(letters + 1, 1);
    other.exit_with(0);
    s.spawn(opener, str.bytes);
    s.spawn(other, str.bytes);
    s.run();
    CHECK_EQ(s.out.str(), std::string("BA"));
}

TEST(open_block_cache_hits_need_no_disk) {
    // A second open of the same path finds every block cached: it costs a
    // few instructions, not another wait for the disk.
    auto steps = [](int opens) {
        System s;
        Strings str;
        const uint32_t p = str.add("/bin/prog");
        Asm a;
        for (int i = 0; i < opens; i++) open(a, p);
        a.exit_with_a0();
        s.spawn(a, str.bytes);
        s.run();
        return s.m.steps();
    };
    const uint64_t one = steps(1);
    const uint64_t two = steps(2);
    CHECK(one > 200);          // the first open really waited for the disk
    CHECK(two > one);
    CHECK(two - one < 50);     // the second one didn't
}

TEST(open_block_shares_the_queue_with_disk_read) {
    // pid 1 reads a far sector first; pid 2's open needs blocks after it.
    // First come, first served: 1 finishes before 2.
    System s;
    Strings str;
    const uint32_t p = str.add("/hello");
    const uint32_t digits = str.add("12");
    Asm reader;
    reader.li(a0, 120 * 1024);
    reader.li(a1, 0x80000000 - 512);
    reader.li(a2, 1);
    reader.syscall(SYS_DISK_READ);
    reader.write_stdout(digits, 1);
    reader.exit_with(0);
    Asm opener;
    open(opener, p);
    opener.write_stdout(digits + 1, 1);
    opener.exit_with(0);
    s.spawn(reader, str.bytes);
    s.spawn(opener, str.bytes);
    s.run();
    CHECK_EQ(s.out.str(), std::string("12"));
}

// =================== errors ===================

TEST(open_err_missing_name_is_enoent) {
    CHECK_EQ(open_one("/nope"), -kEnoent);
    CHECK_EQ(open_one("/bin/nope"), -kEnoent);
    CHECK_EQ(open_one("/Hello"), -kEnoent);   // case matters
}

TEST(open_err_file_in_the_middle_is_enotdir) {
    CHECK_EQ(open_one("/hello/x"), -kEnotdir);
    CHECK_EQ(open_one("/bin/prog/"), 3);       // a trailing slash is just skipped
}

TEST(open_err_bad_paths_are_einval) {
    CHECK_EQ(open_one(""), -kEinval);
    CHECK_EQ(open_one("hello"), -kEinval);                     // relative
    CHECK_EQ(open_one("/" + std::string(28, 'n')), -kEinval);  // name too long
    CHECK_EQ(open_one("/bin/" + std::string(28, 'n')), -kEinval);
}

TEST(open_err_kernel_address_is_efault) {
    System s;
    Asm a;
    open(a, 0x90000000);
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(pid), -kEfault);
}

TEST(open_err_path_running_off_the_data_is_efault) {
    // "/hello" with no zero after it, in the last bytes of the data page: the
    // kernel reads on into the next page, which isn't in the address space.
    System s;
    std::vector<uint8_t> data(4096, 'x');
    const std::string path = "/hello";
    std::copy(path.begin(), path.end(), data.end() - static_cast<long>(path.size()));
    Asm a;
    open(a, kData + 4096 - static_cast<uint32_t>(path.size()));
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a, data);
    s.run();
    CHECK_EQ(s.status(pid), -kEfault);
}

TEST(open_err_path_of_256_bytes_is_enametoolong) {
    // 251 slashes + "hello": 256 bytes with no zero among them.
    CHECK_EQ(open_one(std::string(251, '/') + "hello"), -kEnametoolong);
}

TEST(open_err_longest_path_is_fine) {
    // 250 slashes + "hello" + the zero: exactly 256 bytes.
    CHECK_EQ(open_one(std::string(250, '/') + "hello"), 3);
}

TEST(open_err_path_errors_do_not_touch_the_disk) {
    // Errors found while reading the path, or in its form, come back at once:
    // the clock never reaches a disk completion.
    System s;
    Strings str;
    const uint32_t empty = str.add("");
    const uint32_t relative = str.add("hello");
    const uint32_t long_path = str.add(std::string(300, '/'));
    Asm a;
    open(a, 0x90000000);
    open(a, empty);
    open(a, relative);
    open(a, long_path);
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a, str.bytes);
    s.run();
    CHECK_EQ(s.status(pid), -kEnametoolong);
    CHECK(s.m.steps() < 100);
}

TEST(open_err_failed_open_takes_no_fd) {
    System s;
    Strings str;
    const uint32_t nope = str.add("/nope");
    const uint32_t hello = str.add("/hello");
    Asm a;
    open(a, nope);
    open(a, hello);
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a, str.bytes);
    s.run();
    CHECK_EQ(s.status(pid), 3);
}

TEST(open_err_full_fd_table_is_emfile) {
    // fds 3..15 are 13 opens; the 14th finds no empty slot.
    System s;
    Strings str;
    const uint32_t p = str.add("/hello");
    Asm a;
    for (int i = 0; i < 13; i++) open(a, p);
    keep(a);                 // 15
    open(a, p);              // -EMFILE
    exit_with_both(a);
    const uint32_t pid = s.spawn(a, str.bytes);
    s.run();
    CHECK_EQ(s.status(pid), (15 << 8) - kEmfile);
}
