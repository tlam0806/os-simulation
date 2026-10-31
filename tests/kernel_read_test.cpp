// read (a7 = 13), spec C3: copying from an open file at its offset into user
// memory, at most up to the end of the offset's block, through the cache.
//
//   read_basic_   the bytes, the count, the offset moving; EOF; directories
//   read_block_   never past a block's end; indirect blocks; waiting for the disk
//   read_err_     EBADF, EFAULT; a failed read leaves the offset alone
//
// The disk (fs_system.hpp) plus /big: 13 blocks and 100 bytes, byte i = i % 251,
// so its file block 12 is the first one reached through the indirect block.
#include <string>

#include "fs_system.hpp"

using namespace fs_system;

namespace {

constexpr uint32_t kBigSize = 13 * fs::kBlockSize + 100;

uint8_t big_byte(uint32_t i) { return static_cast<uint8_t>(i % 251); }

// The bytes [from, from + n) of /big.
std::string big(uint32_t from, uint32_t n) {
    std::string s;
    for (uint32_t i = from; i < from + n; i++) s.push_back(static_cast<char>(big_byte(i)));
    return s;
}

void add_big(System& s) {
    std::vector<uint8_t> bytes(kBigSize);
    for (uint32_t i = 0; i < kBigSize; i++) bytes[i] = big_byte(i);
    CHECK(std::holds_alternative<uint32_t>(fs::create_file(s.m.disk(), fs::kRootInode, "big", bytes)));
}

struct Result {
    int32_t status;
    std::string out;
};

// Runs a program that opens `path` (fd 3), then `body`, then exits with a0.
Result run_on(const std::string& path, void (*body)(Asm&)) {
    System s;
    add_big(s);
    Strings str;
    const uint32_t p = str.add(path);
    Asm a;
    open(a, p);
    body(a);
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a, str.bytes);
    s.run();
    return {s.status(pid), s.out.str()};
}

// The clock when a program like run_on's has finished.
uint64_t steps_on(const std::string& path, void (*body)(Asm&)) {
    System s;
    add_big(s);
    Strings str;
    const uint32_t p = str.add(path);
    Asm a;
    open(a, p);
    body(a);
    a.exit_with_a0();
    s.spawn(a, str.bytes);
    s.run();
    return s.m.steps();
}

}  // namespace

// =================== the bytes and the offset ===================

TEST(read_basic_whole_small_file) {
    // Asking for more than there is is fine: it returns what there is.
    const Result r = run_on("/hello", [](Asm& a) { read_and_print(a, 3, 100); });
    CHECK_EQ(r.status, 2);
    CHECK_EQ(r.out, std::string("hi"));
}

TEST(read_basic_offset_moves_forward) {
    const Result r = run_on("/hello", [](Asm& a) {
        read_and_print(a, 3, 1);
        read_and_print(a, 3, 1);
        keep(a);                 // 1
        read_and_print(a, 3, 1); // 0: the end
        exit_with_both(a);
    });
    CHECK_EQ(r.status, 1 << 8);
    CHECK_EQ(r.out, std::string("hi"));
}

TEST(read_basic_at_the_end_returns_0) {
    const Result r = run_on("/hello", [](Asm& a) {
        lseek(a, 3, 0, kEnd);
        read_and_print(a, 3, 10);
    });
    CHECK_EQ(r.status, 0);
    CHECK_EQ(r.out, std::string(""));
}

TEST(read_basic_past_the_end_returns_0) {
    const Result r = run_on("/hello", [](Asm& a) {
        lseek(a, 3, 1000, kSet);
        read_and_print(a, 3, 10);
    });
    CHECK_EQ(r.status, 0);
}

TEST(read_basic_after_lseek) {
    const Result r = run_on("/hello", [](Asm& a) {
        lseek(a, 3, 1, kSet);
        read_and_print(a, 3, 10);
    });
    CHECK_EQ(r.status, 1);
    CHECK_EQ(r.out, std::string("i"));
}

TEST(read_basic_count_0_reads_nothing) {
    const Result r = run_on("/hello", [](Asm& a) {
        read_and_print(a, 3, 0);
        keep(a);                 // 0
        read_and_print(a, 3, 10);
        exit_with_both(a);
    });
    CHECK_EQ(r.status, 2);
    CHECK_EQ(r.out, std::string("hi"));
}

TEST(read_basic_offset_is_per_open) {
    // Two opens of /hello are two entries: reading fd 3 doesn't move fd 4.
    System s;
    Strings str;
    const uint32_t p = str.add("/hello");
    Asm a;
    open(a, p);
    open(a, p);
    read_and_print(a, 3, 1);
    read_and_print(a, 4, 2);
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a, str.bytes);
    s.run();
    CHECK_EQ(s.status(pid), 2);
    CHECK_EQ(s.out.str(), std::string("hhi"));
}

TEST(read_basic_directory_gives_raw_entries) {
    // /bin holds one entry: prog's inode number, then "prog" padded to 28 bytes.
    System s;
    const auto prog = fs::resolve(s.m.disk(), "/bin/prog");
    CHECK(std::holds_alternative<uint32_t>(prog));
    const auto entry = fs::encode_dir_entry({std::holds_alternative<uint32_t>(prog) ? std::get<uint32_t>(prog) : 0, "prog"});
    Strings str;
    const uint32_t p = str.add("/bin");
    Asm a;
    open(a, p);
    read_and_print(a, 3, 100);
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a, str.bytes);
    s.run();
    CHECK_EQ(s.status(pid), 32);
    CHECK_EQ(s.out.str(), std::string(entry.begin(), entry.end()));
}

TEST(read_basic_untouched_stack_page_is_mapped) {
    // 0x7FF00000 is in the stack region but below the 16 pages the loader maps.
    const Result r = run_on("/hello", [](Asm& a) {
        read(a, 3, 0x7FF00000, 2);
        keep(a);
        a.li(t0, 0x7FF00000);
        a.emit({LBU, a0, t0, 0, 1});   // 'i'
        exit_with_both(a);
    });
    CHECK_EQ(r.status, (2 << 8) + 'i');
}

// =================== blocks ===================

TEST(read_block_never_past_the_end_of_a_block) {
    // 10 bytes before the end of block 0: one read gets those 10, the next
    // read starts block 1.
    const Result r = run_on("/big", [](Asm& a) {
        lseek(a, 3, fs::kBlockSize - 10, kSet);
        read_and_print(a, 3, 100);
        keep(a);                 // 10
        read_and_print(a, 3, 100);
        exit_with_both(a);
    });
    CHECK_EQ(r.status, (10 << 8) + 100);
    CHECK_EQ(r.out, big(fs::kBlockSize - 10, 110));
}

TEST(read_block_whole_block_at_most) {
    const Result r = run_on("/big", [](Asm& a) { read_and_print(a, 3, 3 * fs::kBlockSize); });
    CHECK_EQ(r.status, static_cast<int32_t>(fs::kBlockSize));
    CHECK_EQ(r.out, big(0, fs::kBlockSize));
}

TEST(read_block_last_direct_block) {
    const Result r = run_on("/big", [](Asm& a) {
        lseek(a, 3, 11 * fs::kBlockSize + 7, kSet);
        read_and_print(a, 3, 50);
    });
    CHECK_EQ(r.status, 50);
    CHECK_EQ(r.out, big(11 * fs::kBlockSize + 7, 50));
}

TEST(read_block_through_the_indirect_block) {
    // File block 12 is the first one listed in the indirect block.
    const Result r = run_on("/big", [](Asm& a) {
        lseek(a, 3, 12 * fs::kBlockSize + 5, kSet);
        read_and_print(a, 3, 50);
    });
    CHECK_EQ(r.status, 50);
    CHECK_EQ(r.out, big(12 * fs::kBlockSize + 5, 50));
}

TEST(read_block_last_partial_block) {
    // Block 13 holds only 100 bytes: from byte 90 there are 10 left.
    const Result r = run_on("/big", [](Asm& a) {
        lseek(a, 3, 13 * fs::kBlockSize + 90, kSet);
        read_and_print(a, 3, 100);
        keep(a);                 // 10
        read_and_print(a, 3, 100);
        exit_with_both(a);       // then 0
    });
    CHECK_EQ(r.status, 10 << 8);
    CHECK_EQ(r.out, big(13 * fs::kBlockSize + 90, 10));
}

TEST(read_block_data_block_needs_the_disk) {
    // open leaves the inode and the directory cached, not the file's data:
    // read waits at least the 64 steps it takes to transfer one block.
    const uint64_t open_only = steps_on("/hello", [](Asm&) {});
    const uint64_t with_read = steps_on("/hello", [](Asm& a) { read(a, 3, kBuf, 2); });
    CHECK(with_read > open_only + 60);
}

TEST(read_block_second_read_is_a_cache_hit) {
    const uint64_t once = steps_on("/hello", [](Asm& a) { read(a, 3, kBuf, 2); });
    const uint64_t twice = steps_on("/hello", [](Asm& a) {
        read(a, 3, kBuf, 2);
        lseek(a, 3, 0, kSet);
        read(a, 3, kBuf, 2);
    });
    CHECK(twice > once);
    CHECK(twice - once < 50);
}

TEST(read_block_only_a0_changes) {
    // read blocks and re-runs: fd, buffer and count must still be in a0..a2.
    const Result r = run_on("/big", [](Asm& a) {
        a.li(s2, 1000);
        lseek(a, 3, 12 * fs::kBlockSize, kSet);
        read(a, 3, kBuf, 7);              // 7
        a.emit({ADD, a0, a0, s2, 0});     // + 1000
        a.emit({ADD, a0, a0, a2, 0});     // + 7
        a.li(t0, static_cast<int32_t>(kBuf));
        a.emit({SUB, t1, a1, t0, 0});
        a.emit({ADD, a0, a0, t1, 0});     // + 0 if a1 is still kBuf
    });
    CHECK_EQ(r.status, 7 + 1000 + 7);
}

// =================== errors ===================

TEST(read_err_bad_fds_are_ebadf) {
    // Never opened, stdin, stdout, out of range.
    CHECK_EQ(run_on("/hello", [](Asm& a) { read(a, 4, kBuf, 1); }).status, -kEbadf);
    CHECK_EQ(run_on("/hello", [](Asm& a) { read(a, 0, kBuf, 1); }).status, -kEbadf);
    CHECK_EQ(run_on("/hello", [](Asm& a) { read(a, 1, kBuf, 1); }).status, -kEbadf);
    CHECK_EQ(run_on("/hello", [](Asm& a) { read(a, 16, kBuf, 1); }).status, -kEbadf);
}

TEST(read_err_closed_fd_is_ebadf) {
    CHECK_EQ(run_on("/hello", [](Asm& a) {
        a.li(a0, 3);
        a.syscall(SYS_CLOSE);
        read(a, 3, kBuf, 1);
    }).status, -kEbadf);
}

TEST(read_err_bad_fd_comes_before_bad_buffer) {
    CHECK_EQ(run_on("/hello", [](Asm& a) { read(a, 4, 0x90000000, 1); }).status, -kEbadf);
}

TEST(read_err_kernel_buffer_is_efault) {
    CHECK_EQ(run_on("/hello", [](Asm& a) { read(a, 3, 0x90000000, 2); }).status, -kEfault);
}

TEST(read_err_read_only_buffer_is_efault) {
    // The data segment is read-only: the bytes would go into it.
    CHECK_EQ(run_on("/hello", [](Asm& a) { read(a, 3, kData, 2); }).status, -kEfault);
}

TEST(read_err_buffer_running_off_the_stack_is_efault) {
    // The second byte would be at 0x80000000, the kernel's.
    CHECK_EQ(run_on("/hello", [](Asm& a) { read(a, 3, 0x80000000 - 1, 2); }).status, -kEfault);
}

TEST(read_err_only_the_bytes_copied_are_checked) {
    // count is 100, but /hello has 2 bytes: only 0x7FFFFFFE and 0x7FFFFFFF are written.
    CHECK_EQ(run_on("/hello", [](Asm& a) { read(a, 3, 0x80000000 - 2, 100); }).status, 2);
    // At the end there is nothing to copy, so nothing to check.
    CHECK_EQ(run_on("/hello", [](Asm& a) {
        lseek(a, 3, 0, kEnd);
        read(a, 3, 0x90000000, 100);
    }).status, 0);
}

TEST(read_err_failed_read_leaves_the_offset) {
    const Result r = run_on("/hello", [](Asm& a) {
        read(a, 3, kData, 2);     // -EFAULT
        read_and_print(a, 3, 2);  // still from 0
    });
    CHECK_EQ(r.status, 2);
    CHECK_EQ(r.out, std::string("hi"));
}
