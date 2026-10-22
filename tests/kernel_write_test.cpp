// The write system call (spec B3): a7 = 2, a0 = fd, a1 = buffer, a2 = length.
// Each test program ends with exit(a0), so the exit status is write's return value.
#include <cstdint>
#include <initializer_list>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "cpu_helpers.hpp"
#include "elf_builder.hpp"
#include "kernel/kernel.hpp"
#include "machine/machine.hpp"
#include "test.hpp"

using namespace isa::reg;
using enum isa::Op;

namespace {

constexpr uint32_t kCode = 0x10000;
constexpr uint32_t kData = 0x11000;   // where the text to print is placed
constexpr int32_t SYS_EXIT = 1, SYS_WRITE = 2, kStdout = 1, kStderr = 2;
constexpr int32_t kEbadf = 9, kEfault = 14;   // not EBADF/EFAULT: those are macros in <errno.h>

// What the guest printed, and how it ended.
struct Output {
    std::string out;
    std::string err;
    int32_t status = -12345;   // a value no test expects, if the program didn't exit
};

// Loads `program` at kCode and `data` at kData, runs it, and collects the output.
Output run(std::initializer_list<isa::Instruction> program, const std::string& data,
           machine::Machine& m) {
    using namespace elf_builder;
    std::vector<Segment> segments = {{kCode, code(program)}};
    if (!data.empty()) segments.push_back({kData, std::vector<uint8_t>(data.begin(), data.end())});
    CHECK(!m.load(make_elf(kCode, segments)));

    std::ostringstream out, err;
    kernel::Kernel k(m, out, err);
    const auto result = k.run(1000);
    const auto* exited = std::get_if<kernel::Exited>(&result);
    CHECK(exited != nullptr);
    return {out.str(), err.str(), exited ? exited->exit_status : -12345};
}

Output run(std::initializer_list<isa::Instruction> program, const std::string& data = "") {
    machine::Machine m;
    return run(program, data, m);
}

const isa::Instruction kBufAtData = {LUI, a1, 0, 0, kData};   // a1 = kData
const isa::Instruction kSetWrite = {ADDI, a7, zero, 0, SYS_WRITE};
const isa::Instruction kSetExit = {ADDI, a7, zero, 0, SYS_EXIT};
const isa::Instruction kEcall = {ECALL};

}  // namespace

// ---- printing ----

TEST(write_prints_to_stdout_and_returns_length) {
    const Output o = run({
        {ADDI, a0, zero, 0, kStdout}, kBufAtData, {ADDI, a2, zero, 0, 3}, kSetWrite, kEcall,
        kSetExit, kEcall,
    }, "hi\n");
    CHECK_EQ(o.out, std::string("hi\n"));
    CHECK_EQ(o.err, std::string(""));
    CHECK_EQ(o.status, 3);
}

TEST(write_prints_to_stderr) {
    const Output o = run({
        {ADDI, a0, zero, 0, kStderr}, kBufAtData, {ADDI, a2, zero, 0, 4}, kSetWrite, kEcall,
        kSetExit, kEcall,
    }, "oops");
    CHECK_EQ(o.out, std::string(""));
    CHECK_EQ(o.err, std::string("oops"));
    CHECK_EQ(o.status, 4);
}

TEST(write_prints_exactly_len_bytes) {
    const Output o = run({
        {ADDI, a0, zero, 0, kStdout}, kBufAtData, {ADDI, a2, zero, 0, 2}, kSetWrite, kEcall,
        kSetExit, kEcall,
    }, "hello");
    CHECK_EQ(o.out, std::string("he"));
    CHECK_EQ(o.status, 2);
}

TEST(write_prints_raw_bytes_including_zero) {
    // A '\0' in the buffer is just a byte; it doesn't end the text.
    const std::string data("a\0b", 3);
    const Output o = run({
        {ADDI, a0, zero, 0, kStdout}, kBufAtData, {ADDI, a2, zero, 0, 3}, kSetWrite, kEcall,
        kSetExit, kEcall,
    }, data);
    CHECK_EQ(o.out, data);
    CHECK_EQ(o.status, 3);
}

TEST(write_twice_program_continues) {
    const Output o = run({
        {ADDI, a0, zero, 0, kStdout}, kBufAtData, {ADDI, a2, zero, 0, 2}, kSetWrite, kEcall,
        {ADDI, a0, zero, 0, kStdout}, {ADDI, a1, a1, 0, 2}, {ADDI, a2, zero, 0, 5}, kEcall,
        kSetExit, kEcall,
    }, "hithere");
    CHECK_EQ(o.out, std::string("hithere"));
    CHECK_EQ(o.status, 5);
}

TEST(write_zero_length_prints_nothing) {
    // Length 0 succeeds even when the buffer address is outside RAM.
    const Output o = run({
        {ADDI, a0, zero, 0, kStdout}, {LUI, a1, 0, 0, 0x200000}, {ADDI, a2, zero, 0, 0},
        kSetWrite, kEcall, kSetExit, kEcall,
    });
    CHECK_EQ(o.out, std::string(""));
    CHECK_EQ(o.status, 0);
}

TEST(write_changes_only_a0) {
    // After the call: a0 = a2 + a7 + (a1 - kData) = 3 + 2 + 0 if all three survived.
    const Output o = run({
        {ADDI, a0, zero, 0, kStdout}, kBufAtData, {ADDI, a2, zero, 0, 3}, kSetWrite, kEcall,
        {ADD, a0, a2, a7, 0},
        {LUI, t0, 0, 0, kData},
        {SUB, t1, a1, t0, 0},
        {ADD, a0, a0, t1, 0},
        kSetExit, kEcall,
    }, "abc");
    CHECK_EQ(o.status, 5);
}

// ---- errors: reported in a0, nothing printed, program keeps running ----

TEST(write_bad_fd_returns_ebadf) {
    for (int32_t fd : {0, 3, -1}) {
        const Output o = run({
            {ADDI, a0, zero, 0, fd}, kBufAtData, {ADDI, a2, zero, 0, 2}, kSetWrite, kEcall,
            kSetExit, kEcall,
        }, "hi");
        CHECK_EQ(o.out, std::string(""));
        CHECK_EQ(o.err, std::string(""));
        CHECK_EQ(o.status, -kEbadf);
    }
}

TEST(write_buffer_partly_unmapped_returns_efault) {
    // "hi" sits in the last 2 bytes of page 0x11; page 0x12 is not mapped.
    // The first 2 bytes are readable, the last 2 are not: print nothing at all.
    machine::Machine m;
    std::ostringstream out, err;
    CHECK(!m.load(elf_builder::make_elf(kCode, {
        {kCode, elf_builder::code({
            {ADDI, a0, zero, 0, kStdout}, {LUI, a1, 0, 0, 0x12000}, {ADDI, a1, a1, 0, -2},
            {ADDI, a2, zero, 0, 4}, kSetWrite, kEcall,
            kSetExit, kEcall,
        })},
        {0x11ffe, {'h', 'i'}, 0, elf_builder::PT_LOAD, elf_builder::PF_R},
    })));
    kernel::Kernel k(m, out, err);
    const auto r = k.run(1000);
    CHECK_EQ(out.str(), std::string(""));
    CHECK(std::holds_alternative<kernel::Exited>(r));
    if (const auto* e = std::get_if<kernel::Exited>(&r)) CHECK_EQ(e->exit_status, -kEfault);
}

TEST(write_buffer_across_pages_in_unrelated_frames) {
    // "hello" starts 2 bytes before the end of page 0x11 and continues in page 0x12.
    // The loader gives frames in file order: page 0x10 (code), then page 0x12
    // (the first data segment), then page 0x11. So virtual pages 0x11 and 0x12
    // are NOT in neighboring frames, and reading "past the end" of page 0x11's
    // frame would print the wrong bytes.
    machine::Machine m;
    std::ostringstream out, err;
    CHECK(!m.load(elf_builder::make_elf(kCode, {
        {kCode, elf_builder::code({
            {ADDI, a0, zero, 0, kStdout}, {LUI, a1, 0, 0, 0x12000}, {ADDI, a1, a1, 0, -2},
            {ADDI, a2, zero, 0, 5}, kSetWrite, kEcall,
            kSetExit, kEcall,
        })},
        {0x12100, {'x'}, 0, elf_builder::PT_LOAD, elf_builder::PF_R},   // maps page 0x12 first
        {0x11ffe, {'h', 'e', 'l', 'l', 'o'}, 0, elf_builder::PT_LOAD, elf_builder::PF_R},
    })));
    kernel::Kernel k(m, out, err);
    const auto r = k.run(1000);
    CHECK_EQ(out.str(), std::string("hello"));
    if (const auto* e = std::get_if<kernel::Exited>(&r)) CHECK_EQ(e->exit_status, 5);
}

TEST(write_buffer_must_be_readable) {
    // Execute-only memory can't be read, not even by the kernel on the program's behalf.
    machine::Machine m;
    std::ostringstream out, err;
    CHECK(!m.load(elf_builder::make_elf(kCode, {
        {kCode, elf_builder::code({
            {ADDI, a0, zero, 0, kStdout}, kBufAtData, {ADDI, a2, zero, 0, 2}, kSetWrite, kEcall,
            kSetExit, kEcall,
        })},
        {kData, {'h', 'i'}, 0, elf_builder::PT_LOAD, elf_builder::PF_X},
    })));
    kernel::Kernel k(m, out, err);
    const auto r = k.run(1000);
    CHECK_EQ(out.str(), std::string(""));
    if (const auto* e = std::get_if<kernel::Exited>(&r)) CHECK_EQ(e->exit_status, -kEfault);
}

TEST(write_buffer_that_wraps_returns_efault) {
    const Output o = run({
        {ADDI, a0, zero, 0, kStdout}, {ADDI, a1, zero, 0, -1},   // a1 = 0xffffffff
        {ADDI, a2, zero, 0, 2}, kSetWrite, kEcall,
        kSetExit, kEcall,
    });
    CHECK_EQ(o.out, std::string(""));
    CHECK_EQ(o.status, -kEfault);
}

TEST(write_huge_length_returns_efault) {
    // A program can pass any length. The simulator must not crash or try to
    // allocate 4 GiB; the buffer can't fit in RAM, so it's -EFAULT.
    const Output o = run({
        {ADDI, a0, zero, 0, kStdout}, kBufAtData, {ADDI, a2, zero, 0, -1},   // a2 = 0xffffffff
        kSetWrite, kEcall,
        kSetExit, kEcall,
    }, "hi");
    CHECK_EQ(o.out, std::string(""));
    CHECK_EQ(o.status, -kEfault);
}
