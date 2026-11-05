// Shared by the file-system call tests (kernel_open_test, kernel_close_test, ...):
// a machine whose disk holds a small file system, and helpers to build programs
// that call open, close, read and lseek.
//
// The disk holds:   /hello      (a file, "hi")
//                   /bin        (a directory)
//                   /bin/prog   (a file, "elf")
#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "asm.hpp"
#include "disk/disk.hpp"
#include "elf_builder.hpp"
#include "fs/layout.hpp"
#include "fs/ops.hpp"
#include "kernel/kernel.hpp"
#include "machine/machine.hpp"
#include "test.hpp"

namespace fs_system {

using namespace isa::reg;
using enum isa::Op;

constexpr uint32_t kData = 0x20000;   // read-only data: the paths
constexpr int32_t kEnoent = 2, kEbadf = 9, kEfault = 14, kEnotdir = 20, kEinval = 22, kEnfile = 23,
                  kEmfile = 24, kEnametoolong = 36;

// Zero-terminated strings packed into the data segment.
struct Strings {
    std::vector<uint8_t> bytes;
    // The address of s (with its terminating zero) once loaded.
    uint32_t add(const std::string& s) {
        const uint32_t at = kData + static_cast<uint32_t>(bytes.size());
        bytes.insert(bytes.end(), s.begin(), s.end());
        bytes.push_back(0);
        return at;
    }
};

inline std::vector<uint8_t> elf(const Asm& a, const std::vector<uint8_t>& data) {
    using namespace elf_builder;
    std::vector<Segment> segments = {{0x10000, code(a.code)}};
    if (!data.empty()) segments.push_back({kData, data, 0, PT_LOAD, PF_R});
    return make_elf(0x10000, segments);
}

struct System {
    machine::Machine m;
    std::ostringstream out, err;
    kernel::Kernel k{m, out, err};

    System() {
        disk::Disk& d = m.disk();
        fs::format(d);
        const std::vector<uint8_t> hi = {'h', 'i'};
        CHECK(std::holds_alternative<uint32_t>(fs::create_file(d, fs::kRootInode, "hello", hi)));
        const auto bin = fs::make_directory(d, fs::kRootInode, "bin");
        CHECK(std::holds_alternative<uint32_t>(bin));
        const std::vector<uint8_t> elf = {'e', 'l', 'f'};
        if (std::holds_alternative<uint32_t>(bin))
            CHECK(std::holds_alternative<uint32_t>(fs::create_file(d, std::get<uint32_t>(bin), "prog", elf)));
    }

    uint32_t spawn(const Asm& a, const std::vector<uint8_t>& data = {}) {
        const auto r = k.spawn(elf(a, data));
        CHECK(std::holds_alternative<uint32_t>(r));
        return std::holds_alternative<uint32_t>(r) ? std::get<uint32_t>(r) : 0;
    }

    void run() { CHECK(std::holds_alternative<kernel::AllDone>(k.run_all(10'000'000))); }

    int32_t status(uint32_t pid) {
        const auto e = k.ending(pid);
        CHECK(e.has_value());
        const auto* exited = e ? std::get_if<kernel::Exited>(&e->status) : nullptr;
        CHECK(exited != nullptr);
        return exited ? exited->exit_status : -12345;
    }
};

// open(path): a0 = the fd or -error.
inline void open(Asm& a, uint32_t path) {
    a.li(a0, static_cast<int32_t>(path));
    a.syscall(SYS_OPEN);
}

// s1 = a0, to keep a result while making another call.
inline void keep(Asm& a) { a.emit({ADDI, s1, a0, 0, 0}); }

// exit((s1 << 8) + a0): two results in one status.
inline void exit_with_both(Asm& a) {
    a.emit({SLLI, s1, s1, 0, 8});
    a.emit({ADD, a0, a0, s1, 0});
    a.exit_with_a0();
}

constexpr uint32_t kBuf = 0x80000000 - 0x2000;   // on the stack, mapped by the loader
constexpr int32_t kSet = 0, kCur = 1, kEnd = 2;  // lseek's whence

// close(fd): a0 = 0 or -error.
inline void close(Asm& a, int32_t fd) {
    a.li(a0, fd);
    a.syscall(SYS_CLOSE);
}

// read(fd, buffer, count): a0 = bytes read or -error.
inline void read(Asm& a, int32_t fd, uint32_t buffer, int32_t count) {
    a.li(a0, fd);
    a.li(a1, static_cast<int32_t>(buffer));
    a.li(a2, count);
    a.syscall(SYS_READ);
}

// lseek(fd, offset, whence): a0 = the new offset or -error.
inline void lseek(Asm& a, int32_t fd, int32_t offset, int32_t whence) {
    a.li(a0, fd);
    a.li(a1, offset);
    a.li(a2, whence);
    a.syscall(SYS_LSEEK);
}

// read(fd, kBuf, count), then writes the a0 bytes it read to stdout. a0 stays
// read's result (kept in s3 meanwhile).
inline void read_and_print(Asm& a, int32_t fd, int32_t count) {
    read(a, fd, kBuf, count);
    a.emit({ADDI, s3, a0, 0, 0});
    a.emit({ADDI, a2, a0, 0, 0});
    a.li(a0, 1);
    a.li(a1, static_cast<int32_t>(kBuf));
    a.syscall(SYS_WRITE);
    a.emit({ADDI, a0, s3, 0, 0});
}

}  // namespace fs_system
