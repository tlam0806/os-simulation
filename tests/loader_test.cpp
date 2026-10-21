// ELF loader (spec B2). Test files are built in memory by elf_builder.hpp.
#include <cstdint>
#include <string>
#include <vector>

#include "elf_builder.hpp"
#include "loader/elf.hpp"
#include "test.hpp"

using namespace elf_builder;

namespace {

// A valid one-segment program: 8 bytes of code at 0x10000.
std::vector<uint8_t> simple_elf() {
    return make_elf(0x10000, {{0x10000, {0x13, 0x05, 0xa0, 0x02, 0x73, 0x00, 0x00, 0x00}}});
}

uint32_t entry_of(const std::variant<loader::Program, std::string>& r) {
    return std::get<loader::Program>(r).entry;
}

bool failed(const std::variant<loader::Program, std::string>& r) {
    return std::holds_alternative<std::string>(r);
}

uint32_t byte_at(const mem::Ram& ram, uint32_t addr) { return ram.read(addr, 1).value(); }

}  // namespace

// ---- Valid files ----

TEST(loader_copies_segment_and_returns_entry) {
    mem::Ram ram;
    const auto r = loader::load_elf(simple_elf(), ram);
    CHECK(!failed(r));
    CHECK_EQ(entry_of(r), 0x10000);
    CHECK_EQ(ram.read(0x10000, 4).value(), 0x02a00513);   // addi a0, zero, 42
    CHECK_EQ(ram.read(0x10004, 4).value(), 0x00000073);   // ecall
}

TEST(loader_entry_can_differ_from_segment_start) {
    mem::Ram ram;
    const auto r = loader::load_elf(make_elf(0x10004, {{0x10000, {1, 2, 3, 4, 5, 6, 7, 8}}}), ram);
    CHECK(!failed(r));
    CHECK_EQ(entry_of(r), 0x10004);
}

TEST(loader_zero_fills_bss) {
    mem::Ram ram;
    for (uint32_t a = 0x20000; a < 0x20010; a++) ram.write(a, 1, 0xff);   // old garbage
    const auto r = loader::load_elf(
        make_elf(0x20000, {{0x20000, {0xaa, 0xbb}, /*memsz=*/16}}), ram);
    CHECK(!failed(r));
    CHECK_EQ(byte_at(ram, 0x20000), 0xaa);
    CHECK_EQ(byte_at(ram, 0x20001), 0xbb);
    for (uint32_t a = 0x20002; a < 0x20010; a++) CHECK_EQ(byte_at(ram, a), 0);
    CHECK_EQ(byte_at(ram, 0x20010), 0);   // RAM after the segment untouched (was already 0)
}

TEST(loader_loads_every_segment) {
    // Like real lld output: code and data in separate segments.
    mem::Ram ram;
    const auto r = loader::load_elf(make_elf(0x110f4, {
        {0x110f4, {0x11, 0x22, 0x33, 0x44}},
        {0x1212c, {0x55, 0x66}, /*memsz=*/8},
    }), ram);
    CHECK(!failed(r));
    CHECK_EQ(ram.read(0x110f4, 4).value(), 0x44332211);
    CHECK_EQ(ram.read(0x1212c, 2).value(), 0x6655);
    CHECK_EQ(ram.read(0x1212e, 4).value(), 0);
}

TEST(loader_ignores_non_load_segments) {
    // GNU_STACK sits at address 0, which would be invalid if it were loaded.
    mem::Ram ram;
    const auto r = loader::load_elf(make_elf(0x10000, {
        {0, {}, /*memsz=*/0, PT_GNU_STACK},
        {0x10000, {0x13, 0, 0, 0}},
    }), ram);
    CHECK(!failed(r));
    CHECK_EQ(ram.read(0x10000, 4).value(), 0x13);
}

TEST(loader_segment_may_end_exactly_at_end_of_ram) {
    mem::Ram ram;
    const uint32_t start = mem::Ram::kRamSize - 4;
    const auto r = loader::load_elf(make_elf(start, {{start, {1, 2, 3, 4}}}), ram);
    CHECK(!failed(r));
    CHECK_EQ(ram.read(start, 4).value(), 0x04030201);
}

// ---- Invalid headers ----

TEST(loader_rejects_file_shorter_than_header) {
    mem::Ram ram;
    std::vector<uint8_t> f = simple_elf();
    f.resize(20);
    CHECK(failed(loader::load_elf(f, ram)));
    CHECK(failed(loader::load_elf(std::vector<uint8_t>{}, ram)));
}

TEST(loader_rejects_wrong_magic) {
    mem::Ram ram;
    std::vector<uint8_t> f = simple_elf();
    f[1] = 'X';
    CHECK(failed(loader::load_elf(f, ram)));
}

TEST(loader_rejects_64_bit_elf) {
    mem::Ram ram;
    std::vector<uint8_t> f = simple_elf();
    f[4] = 2;   // ELFCLASS64
    CHECK(failed(loader::load_elf(f, ram)));
}

TEST(loader_rejects_big_endian) {
    mem::Ram ram;
    std::vector<uint8_t> f = simple_elf();
    f[5] = 2;   // ELFDATA2MSB
    CHECK(failed(loader::load_elf(f, ram)));
}

TEST(loader_rejects_other_machines) {
    mem::Ram ram;
    std::vector<uint8_t> f = simple_elf();
    put16(f, E_MACHINE, 62);   // x86-64
    CHECK(failed(loader::load_elf(f, ram)));
}

TEST(loader_rejects_non_executables) {
    mem::Ram ram;
    std::vector<uint8_t> f = simple_elf();
    put16(f, E_TYPE, 1);   // ET_REL: an object file (.o), not linked yet
    CHECK(failed(loader::load_elf(f, ram)));
}

TEST(loader_rejects_program_headers_outside_file) {
    mem::Ram ram;
    std::vector<uint8_t> f = simple_elf();
    put32(f, E_PHOFF, static_cast<uint32_t>(f.size()));   // headers would start past the end
    CHECK(failed(loader::load_elf(f, ram)));

    f = simple_elf();
    put32(f, E_PHOFF, 0xffffffe0);   // offset + size wraps around 32 bits
    CHECK(failed(loader::load_elf(f, ram)));
}

// ---- Invalid segments ----

TEST(loader_rejects_segment_data_outside_file) {
    mem::Ram ram;
    std::vector<uint8_t> f = simple_elf();
    put32(f, kEhdrSize + P_FILESZ, 1000);   // claims more bytes than the file has
    put32(f, kEhdrSize + P_MEMSZ, 1000);
    CHECK(failed(loader::load_elf(f, ram)));

    f = simple_elf();
    put32(f, kEhdrSize + P_OFFSET, 0xfffffffc);   // offset + size wraps around 32 bits
    CHECK(failed(loader::load_elf(f, ram)));
}

TEST(loader_rejects_filesz_larger_than_memsz) {
    mem::Ram ram;
    std::vector<uint8_t> f = simple_elf();
    put32(f, kEhdrSize + P_MEMSZ, 4);   // file has 8 bytes, memory only 4
    CHECK(failed(loader::load_elf(f, ram)));
}

TEST(loader_rejects_segment_in_null_guard) {
    mem::Ram ram;
    CHECK(failed(loader::load_elf(make_elf(0, {{0, {1, 2, 3, 4}}}), ram)));
    CHECK(failed(loader::load_elf(make_elf(0xfffc, {{0xfffc, {1, 2, 3, 4}}}), ram)));
}

TEST(loader_rejects_segment_past_end_of_ram) {
    mem::Ram ram;
    const uint32_t start = mem::Ram::kRamSize - 3;   // 4 bytes: last one is outside
    CHECK(failed(loader::load_elf(make_elf(start, {{start, {1, 2, 3, 4}}}), ram)));
    // bss can also push a segment past the end
    const uint32_t start2 = mem::Ram::kRamSize - 16;
    CHECK(failed(loader::load_elf(make_elf(start2, {{start2, {1}, /*memsz=*/32}}), ram)));
}

TEST(loader_rejects_segment_address_that_wraps) {
    mem::Ram ram;
    CHECK(failed(loader::load_elf(make_elf(0x10000, {{0xfffff000, {1}, /*memsz=*/0x2000}}), ram)));
}

TEST(loader_failed_load_leaves_ram_unchanged) {
    // The first segment is fine, the second is not: nothing may be written.
    mem::Ram ram;
    const auto r = loader::load_elf(make_elf(0x10000, {
        {0x10000, {0xaa, 0xbb, 0xcc, 0xdd}},
        {0, {1, 2, 3, 4}},   // in the null guard
    }), ram);
    CHECK(failed(r));
    CHECK_EQ(ram.read(0x10000, 4).value(), 0);
}

TEST(loader_error_message_is_not_empty) {
    mem::Ram ram;
    const auto r = loader::load_elf(std::vector<uint8_t>{1, 2, 3}, ram);
    CHECK(failed(r));
    if (failed(r)) CHECK(!std::get<std::string>(r).empty());
}
