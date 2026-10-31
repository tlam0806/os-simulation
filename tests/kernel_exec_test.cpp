// exec (a7 = 7), spec C3: replacing the caller's program with an ELF file read
// through the cache. Look first (walk, fetch every block, load into a new address
// space), act only when nothing can fail (drop the old memory, reset registers).
//
//   exec_basic_   the new program runs, the old one never returns; same pid and
//                 parent; fresh registers, memory and break; fds stay open
//   exec_block_   waiting for the file's blocks, indirect ones too; others run
//   exec_mem_     the old address space is freed
//   exec_err_     path errors, ENOEXEC; a failed exec leaves the old program as it was
//
// Programs are ELF files the tests put in the root directory, next to the
// usual /hello, /bin and /bin/prog (fs_system.hpp).
#include <string>
#include <vector>

#include "fs_system.hpp"

using namespace fs_system;

namespace {

constexpr int32_t kEnoexec = 8;
constexpr uint32_t kTop = 0x80000000;

// Puts `bytes` in the root directory as `name`.
void put(System& s, const std::string& name, const std::vector<uint8_t>& bytes) {
    CHECK(std::holds_alternative<uint32_t>(fs::create_file(s.m.disk(), fs::kRootInode, name, bytes)));
}

// Puts a program in the root directory.
void put(System& s, const std::string& name, const Asm& a, const Strings& str = {}) {
    put(s, name, elf(a, str.bytes));
}

// exec(path): a0 = -error if it fails; doesn't return otherwise.
void exec(Asm& a, uint32_t path) {
    a.li(a0, static_cast<int32_t>(path));
    a.syscall(SYS_EXEC);
}

// Writes "B" and exits with 7.
Asm prints_b() {
    Asm b;
    b.li(t0, 'B');
    b.li(t1, static_cast<int32_t>(kBuf));
    b.emit({SB, 0, t1, t0, 0});
    b.li(a0, 1);
    b.li(a1, static_cast<int32_t>(kBuf));
    b.li(a2, 1);
    b.syscall(SYS_WRITE);
    b.exit_with(7);
    return b;
}

// A program that calls exec(path) and exits with its result.
int32_t exec_one(System& s, const std::string& path) {
    Strings str;
    const uint32_t p = str.add(path);
    Asm a;
    exec(a, p);
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a, str.bytes);
    s.run();
    return s.status(pid);
}

int32_t exec_one(const std::string& path) {
    System s;
    return exec_one(s, path);
}

}  // namespace

// =================== the new program ===================

TEST(exec_basic_runs_the_new_program) {
    System s;
    put(s, "b", prints_b());
    CHECK_EQ(exec_one(s, "/b"), 7);
    CHECK_EQ(s.out.str(), std::string("B"));
}

TEST(exec_basic_does_not_return) {
    // The old program's code after the ECALL is gone: "A" is never written.
    System s;
    put(s, "b", prints_b());
    Strings str;
    const uint32_t p = str.add("/b");
    const uint32_t letter = str.add("A");
    Asm a;
    exec(a, p);
    a.write_stdout(letter, 1);
    a.exit_with(1);
    const uint32_t pid = s.spawn(a, str.bytes);
    s.run();
    CHECK_EQ(s.status(pid), 7);
    CHECK_EQ(s.out.str(), std::string("B"));
}

TEST(exec_basic_keeps_the_pid) {
    System s;
    Asm b;
    b.syscall(SYS_GETPID);
    b.exit_with_a0();
    put(s, "b", b);
    s.spawn(prints_b());           // pid 1, so the exec-er isn't pid 1 by chance
    Strings str;
    const uint32_t p = str.add("/b");
    Asm a;
    exec(a, p);
    a.exit_with(-1);
    const uint32_t pid = s.spawn(a, str.bytes);
    s.run();
    CHECK_EQ(s.status(pid), static_cast<int32_t>(pid));
}

TEST(exec_basic_keeps_the_parent) {
    // The parent forks; the child execs /b (exits 7); the parent's wait still
    // finds it as its child and gets its status.
    System s;
    put(s, "b", prints_b());
    Strings str;
    const uint32_t p = str.add("/b");
    Asm a;
    a.syscall(SYS_FORK);
    const size_t to_child = a.branch(BEQ, a0, zero);
    a.li(a0, -1);                  // parent: wait(-1, kBuf)
    a.li(a1, static_cast<int32_t>(kBuf));
    a.syscall(SYS_WAIT);
    a.li(t0, static_cast<int32_t>(kBuf));
    a.emit({LW, a0, t0, 0, 0});
    a.exit_with_a0();
    a.patch(to_child, a.here());
    exec(a, p);                    // child
    a.exit_with(-1);
    const uint32_t parent = s.spawn(a, str.bytes);
    s.run();
    CHECK_EQ(s.status(parent), 7);
}

TEST(exec_basic_registers_start_fresh) {
    // The old program fills some registers; the new one sums them, and sp - top: 0.
    System s;
    Asm b;
    b.li(t0, static_cast<int32_t>(kTop));
    b.emit({SUB, t1, sp, t0, 0});
    for (uint8_t r : {a0, a1, a2, s1, s2, ra, gp, tp, t2})
        b.emit({ADD, t1, t1, r, 0});
    b.emit({ADDI, a0, t1, 0, 0});
    b.exit_with_a0();
    put(s, "b", b);
    Strings str;
    const uint32_t p = str.add("/b");
    Asm a;
    for (uint8_t r : {a1, a2, s1, s2, ra, gp, tp, t2}) a.li(r, 1000);
    a.li(sp, 0x7FFF0000);
    exec(a, p);
    a.exit_with(-1);
    const uint32_t pid = s.spawn(a, str.bytes);
    s.run();
    CHECK_EQ(s.status(pid), 0);
}

TEST(exec_basic_memory_starts_fresh) {
    // The old program stores 0x55 at kBuf; the new one finds a zeroed stack there.
    System s;
    Asm b;
    b.li(t0, static_cast<int32_t>(kBuf));
    b.emit({LW, a0, t0, 0, 0});
    b.exit_with_a0();
    put(s, "b", b);
    Strings str;
    const uint32_t p = str.add("/b");
    Asm a;
    a.li(t0, static_cast<int32_t>(kBuf));
    a.li(t1, 0x55);
    a.emit({SW, 0, t0, t1, 0});
    exec(a, p);
    a.exit_with(-1);
    const uint32_t pid = s.spawn(a, str.bytes);
    s.run();
    CHECK_EQ(s.status(pid), 0);
}

TEST(exec_basic_break_is_the_new_programs) {
    // The old program has data at 0x20000 and grew its heap; the new one has
    // only code at 0x10000, so its initial break is 0x11000.
    System s;
    Asm b;
    b.sbrk(0);
    b.li(t0, 0x11000);
    b.emit({SUB, a0, a0, t0, 0});
    b.exit_with_a0();
    put(s, "b", b);
    Strings str;
    const uint32_t p = str.add("/b");
    Asm a;
    a.sbrk(3 * 4096);
    exec(a, p);
    a.exit_with(-1);
    const uint32_t pid = s.spawn(a, str.bytes);
    s.run();
    CHECK_EQ(s.status(pid), 0);
}

TEST(exec_basic_open_files_stay_open) {
    // The old program opens /hello and reads "h"; the new one reads on from fd 3.
    System s;
    Asm b;
    read_and_print(b, 3, 10);
    b.exit_with_a0();
    put(s, "b", b);
    Strings str;
    const uint32_t hello = str.add("/hello");
    const uint32_t p = str.add("/b");
    Asm a;
    open(a, hello);
    read_and_print(a, 3, 1);
    exec(a, p);
    a.exit_with(-1);
    const uint32_t pid = s.spawn(a, str.bytes);
    s.run();
    CHECK_EQ(s.status(pid), 1);
    CHECK_EQ(s.out.str(), std::string("hi"));
}

// =================== the file's blocks ===================

TEST(exec_block_caller_waits_for_the_file) {
    // Nothing is cached at the start: the exec-er blocks while pid 2 prints "A";
    // then the new program prints "B".
    System s;
    put(s, "b", prints_b());
    Strings str;
    const uint32_t p = str.add("/b");
    const uint32_t letter = str.add("A");
    Asm execer;
    exec(execer, p);
    execer.exit_with(-1);
    Asm other;
    other.write_stdout(letter, 1);
    other.exit_with(0);
    const uint32_t pid = s.spawn(execer, str.bytes);
    s.spawn(other, str.bytes);
    s.run();
    CHECK_EQ(s.status(pid), 7);
    CHECK_EQ(s.out.str(), std::string("AB"));
}

TEST(exec_block_big_program_through_the_indirect_block) {
    // 60 KiB of read-only data: the file has more than 12 blocks, so exec must
    // read the indirect block and the blocks it lists. The program checks a byte
    // near the end of its data.
    System s;
    constexpr uint32_t kSize = 60 * 1024, kAt = 55000;
    std::vector<uint8_t> data(kSize);
    for (uint32_t i = 0; i < kSize; i++) data[i] = static_cast<uint8_t>(i % 251);
    Asm b;
    b.li(t0, static_cast<int32_t>(kData + kAt));
    b.emit({LBU, a0, t0, 0, 0});
    b.exit_with_a0();
    const std::vector<uint8_t> file = elf(b, data);
    CHECK(file.size() > 12 * fs::kBlockSize);
    put(s, "big", file);
    CHECK_EQ(exec_one(s, "/big"), static_cast<int32_t>(kAt % 251));
}

TEST(exec_block_path_survives_the_re_runs) {
    // exec blocks several times (inode, directory, file blocks); a0 must still
    // hold the path each time, and a failing exec leaves the others alone.
    System s;
    Strings str;
    const uint32_t p = str.add("/hello");   // not an ELF: -ENOEXEC, after reading it
    Asm a;
    a.li(s2, 1000);
    a.li(a1, 20);
    exec(a, p);
    a.emit({ADD, a0, a0, s2, 0});
    a.emit({ADD, a0, a0, a1, 0});
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a, str.bytes);
    s.run();
    CHECK_EQ(s.status(pid), -kEnoexec + 1000 + 20);
}

// =================== memory ===================

TEST(exec_mem_old_address_space_is_freed) {
    // A chain of 20 programs, each exec-ing the next: /p0 -> /p1 -> ... -> /p19,
    // which exits with 42. Each holds 18 frames (code, data, 16 stack pages);
    // 20 x 18 is more than the 256 frames, so the old ones must be freed.
    // A program whose exec fails exits with 100 + its number.
    System s;
    constexpr int kChain = 20;
    for (int i = 0; i < kChain; i++) {
        Strings str;
        const uint32_t next = str.add("/p" + std::to_string(i + 1));
        Asm a;
        if (i + 1 < kChain) {
            exec(a, next);
            a.exit_with(100 + i);
        } else {
            a.exit_with(42);
        }
        put(s, "p" + std::to_string(i), a, str);
    }
    CHECK_EQ(exec_one(s, "/p0"), 42);
}

TEST(exec_mem_shared_pages_stay_with_the_parent) {
    // After fork the pages are shared copy-on-write. The child's exec drops its
    // mappings; the parent's memory must be untouched: it still reads its 0x55.
    System s;
    put(s, "b", prints_b());
    Strings str;
    const uint32_t p = str.add("/b");
    Asm a;
    a.li(t0, static_cast<int32_t>(kBuf));
    a.li(t1, 0x55);
    a.emit({SW, 0, t0, t1, 0});
    a.syscall(SYS_FORK);
    const size_t to_child = a.branch(BEQ, a0, zero);
    a.li(a0, -1);                  // parent: wait, then read kBuf
    a.li(a1, 0);
    a.syscall(SYS_WAIT);
    a.li(t0, static_cast<int32_t>(kBuf));
    a.emit({LW, a0, t0, 0, 0});
    a.exit_with_a0();
    a.patch(to_child, a.here());
    exec(a, p);                    // child
    a.exit_with(-1);
    const uint32_t parent = s.spawn(a, str.bytes);
    s.run();
    CHECK_EQ(s.status(parent), 0x55);
    CHECK_EQ(s.status(parent + 1), 7);
}

// =================== errors ===================

TEST(exec_err_path_errors_like_open) {
    CHECK_EQ(exec_one("/nope"), -kEnoent);
    CHECK_EQ(exec_one("/hello/x"), -kEnotdir);
    CHECK_EQ(exec_one("b"), -kEinval);
    CHECK_EQ(exec_one(""), -kEinval);
    CHECK_EQ(exec_one(std::string(300, '/')), -kEnametoolong);
}

TEST(exec_err_kernel_address_is_efault) {
    System s;
    Asm a;
    a.li(a0, static_cast<int32_t>(0x90000000));
    a.syscall(SYS_EXEC);
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(pid), -kEfault);
}

TEST(exec_err_directory_is_enoexec) {
    CHECK_EQ(exec_one("/bin"), -kEnoexec);
    CHECK_EQ(exec_one("/"), -kEnoexec);
}

TEST(exec_err_not_an_elf_is_enoexec) {
    CHECK_EQ(exec_one("/hello"), -kEnoexec);
    CHECK_EQ(exec_one("/bin/prog"), -kEnoexec);
}

TEST(exec_err_empty_file_is_enoexec) {
    System s;
    put(s, "empty", std::vector<uint8_t>{});
    CHECK_EQ(exec_one(s, "/empty"), -kEnoexec);
}

TEST(exec_err_bad_segment_is_enoexec) {
    // A valid ELF whose segment sits in the null guard, below 0x10000.
    System s;
    Asm b;
    b.exit_with(0);
    put(s, "low", elf_builder::make_elf(0x1000, {{0x1000, elf_builder::code(b.code)}}));
    CHECK_EQ(exec_one(s, "/low"), -kEnoexec);
}

TEST(exec_err_failed_exec_keeps_the_old_program) {
    // After a failed exec, the old program goes on with its memory, its heap and
    // its open files: it reads back 0x55, its break, and "hi" from fd 3.
    System s;
    Strings str;
    const uint32_t hello = str.add("/hello");
    Asm a;
    open(a, hello);
    a.sbrk(4096);
    a.emit({ADDI, s4, a0, 0, 0});              // the old break
    a.li(t0, static_cast<int32_t>(kBuf) + 64);
    a.li(t1, 0x55);
    a.emit({SW, 0, t0, t1, 0});
    exec(a, hello);                            // -ENOEXEC
    a.emit({ADDI, s1, a0, 0, 0});
    a.sbrk(0);                                 // break: old + 4096
    a.emit({SUB, a0, a0, s4, 0});
    a.emit({ADD, s1, s1, a0, 0});              // -ENOEXEC + 4096
    a.li(t0, static_cast<int32_t>(kBuf) + 64);
    a.emit({LW, a0, t0, 0, 0});
    a.emit({ADD, s1, s1, a0, 0});              // + 0x55
    read_and_print(a, 3, 10);                  // "hi", 2
    a.emit({ADD, a0, a0, s1, 0});
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a, str.bytes);
    s.run();
    CHECK_EQ(s.status(pid), -kEnoexec + 4096 + 0x55 + 2);
    CHECK_EQ(s.out.str(), std::string("hi"));
}
