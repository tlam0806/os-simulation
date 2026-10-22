// ELF loader (spec B1-B2): builds a program's address space.
// Test files are built in memory by elf_builder.hpp.
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "elf_builder.hpp"
#include "loader/elf.hpp"
#include "test.hpp"

using namespace elf_builder;

namespace {

using Result = std::variant<loader::Program, std::string>;

// Everything the loader works on: RAM, the free frames, and the new address space.
struct Space {
    mem::Ram ram;
    mmu::FrameAllocator frames;
    mmu::PageTable table;

    Result load(const std::vector<uint8_t>& file) {
        return loader::load_elf(file, ram, frames, table);
    }

    // Reads guest memory through the page table (ignoring permissions).
    // Returns 0xdeadbeef and fails the test if the page isn't mapped.
    uint32_t read(uint32_t vaddr, uint8_t size) {
        const auto pte = table.lookup(vaddr >> mmu::kPageShift);
        CHECK(pte.has_value());
        if (!pte) return 0xdeadbeef;
        return ram.read(pte->frame * mmu::kPageSize + (vaddr & (mmu::kPageSize - 1)), size).value();
    }

    bool mapped(uint32_t vaddr) const { return table.lookup(vaddr >> mmu::kPageShift).has_value(); }

    mmu::Pte pte(uint32_t vaddr) const {
        return table.lookup(vaddr >> mmu::kPageShift).value_or(mmu::Pte{9999, false, false, false});
    }

    // Fills all of RAM with 0xff, so "zero-filled" can't pass by accident.
    void dirty_ram() {
        for (uint32_t a = 0; a < mem::Ram::kRamSize; a += 4) ram.write(a, 4, 0xffffffff);
    }
};

// A valid one-segment program: 8 bytes of code at 0x10000.
std::vector<uint8_t> simple_elf() {
    return make_elf(0x10000, {{0x10000, {0x13, 0x05, 0xa0, 0x02, 0x73, 0x00, 0x00, 0x00}}});
}

bool failed(const Result& r) { return std::holds_alternative<std::string>(r); }

loader::Program program(const Result& r) {
    CHECK(!failed(r));
    return failed(r) ? loader::Program{0, 0} : std::get<loader::Program>(r);
}

constexpr uint32_t kStackPages = loader::kInitialStackPages;

}  // namespace

// ---- Loading code and data ----

TEST(loader_copies_segment_and_returns_entry) {
    Space s;
    const auto p = program(s.load(simple_elf()));
    CHECK_EQ(p.entry, 0x10000);
    CHECK_EQ(s.read(0x10000, 4), 0x02a00513);   // addi a0, zero, 42
    CHECK_EQ(s.read(0x10004, 4), 0x00000073);   // ecall
}

TEST(loader_entry_can_differ_from_segment_start) {
    Space s;
    const auto p = program(s.load(make_elf(0x10004, {{0x10000, {1, 2, 3, 4, 5, 6, 7, 8}}})));
    CHECK_EQ(p.entry, 0x10004);
}

TEST(loader_virtual_addresses_need_not_exist_in_ram) {
    // 0x40000000 is far beyond 1 MiB of RAM: the page is backed by some frame.
    Space s;
    program(s.load(make_elf(0x40000000, {{0x40000000, {0x11, 0x22, 0x33, 0x44}}})));
    CHECK_EQ(s.read(0x40000000, 4), 0x44332211);
    CHECK(s.pte(0x40000000).frame < mmu::kNumFrames);
}

TEST(loader_permissions_come_from_segment_flags) {
    Space s;
    program(s.load(make_elf(0x10000, {
        {0x10000, {1, 2, 3, 4}, 0, PT_LOAD, PF_R | PF_X},   // code
        {0x11000, {1, 2, 3, 4}, 0, PT_LOAD, PF_R},          // read-only data
        {0x12000, {1, 2, 3, 4}, 0, PT_LOAD, PF_R | PF_W},   // data
    })));
    const mmu::Pte code = s.pte(0x10000), rodata = s.pte(0x11000), data = s.pte(0x12000);
    CHECK(code.read && !code.write && code.exec);
    CHECK(rodata.read && !rodata.write && !rodata.exec);
    CHECK(data.read && data.write && !data.exec);
}

TEST(loader_shared_page_gets_both_permissions) {
    // Spec B2: two segments on one page -> the union of their permissions.
    Space s;
    program(s.load(make_elf(0x10000, {
        {0x10000, {1, 2, 3, 4}, 0, PT_LOAD, PF_R | PF_X},
        {0x10800, {5, 6, 7, 8}, 0, PT_LOAD, PF_R | PF_W},
    })));
    const mmu::Pte p = s.pte(0x10000);
    CHECK(p.read && p.write && p.exec);
    CHECK_EQ(s.read(0x10000, 4), 0x04030201);
    CHECK_EQ(s.read(0x10800, 4), 0x08070605);
}

TEST(loader_segment_spanning_pages) {
    // 0x1800 bytes starting halfway into page 0x10: covers pages 0x10 and 0x11.
    Space s;
    std::vector<uint8_t> data(0x1800);
    for (size_t i = 0; i < data.size(); i++) data[i] = static_cast<uint8_t>(i * 7);
    program(s.load(make_elf(0x10800, {{0x10800, data}})));
    CHECK(s.mapped(0x10000));
    CHECK(s.mapped(0x11000));
    CHECK(!s.mapped(0x12000));
    CHECK_EQ(s.read(0x10fff, 1), static_cast<uint8_t>(0x7ff * 7));   // last byte of page 0x10
    CHECK_EQ(s.read(0x11000, 1), static_cast<uint8_t>(0x800 * 7));   // first byte of page 0x11
    CHECK(s.pte(0x10000).frame != s.pte(0x11000).frame);
}

TEST(loader_zero_fills_bss_and_rest_of_page) {
    Space s;
    s.dirty_ram();
    program(s.load(make_elf(0x20000, {{0x20000, {0xaa, 0xbb}, /*memsz=*/16}})));
    CHECK_EQ(s.read(0x20000, 1), 0xaa);
    CHECK_EQ(s.read(0x20001, 1), 0xbb);
    for (uint32_t a = 0x20002; a < 0x20010; a++) CHECK_EQ(s.read(a, 1), 0);   // .bss
    CHECK_EQ(s.read(0x20ffc, 4), 0);   // rest of the page: new pages are always zero (spec B1)
}

TEST(loader_bss_can_span_extra_pages) {
    Space s;
    s.dirty_ram();
    program(s.load(make_elf(0x20000, {{0x20000, {0xaa}, /*memsz=*/0x2000}})));
    CHECK(s.mapped(0x21000));
    CHECK_EQ(s.read(0x21000, 4), 0);
    CHECK_EQ(s.read(0x21ffc, 4), 0);
}

TEST(loader_ignores_non_load_segments) {
    // GNU_STACK sits at address 0, which would be invalid if it were loaded.
    Space s;
    program(s.load(make_elf(0x10000, {
        {0, {}, /*memsz=*/0, PT_GNU_STACK},
        {0x10000, {0x13, 0, 0, 0}},
    })));
    CHECK(!s.mapped(0));
    CHECK_EQ(s.read(0x10000, 4), 0x13);
}

TEST(loader_each_page_gets_its_own_frame) {
    Space s;
    program(s.load(make_elf(0x10000, {{0x10000, std::vector<uint8_t>(3 * mmu::kPageSize, 1)}})));
    std::set<uint32_t> used;
    for (uint32_t va = 0x10000; va < 0x13000; va += mmu::kPageSize) used.insert(s.pte(va).frame);
    for (uint32_t i = 1; i <= kStackPages; i++) used.insert(s.pte(loader::kStackTop - i * mmu::kPageSize).frame);
    CHECK_EQ(used.size(), 3 + kStackPages);
    CHECK_EQ(s.frames.free_count(), mmu::kNumFrames - 3 - kStackPages);
}

// ---- Stack and heap ----

TEST(loader_maps_initial_stack) {
    Space s;
    s.dirty_ram();
    program(s.load(simple_elf()));
    for (uint32_t i = 1; i <= kStackPages; i++) {
        const uint32_t va = loader::kStackTop - i * mmu::kPageSize;
        const mmu::Pte p = s.pte(va);
        CHECK(p.read && p.write && !p.exec);   // stack is data: never executable
        CHECK_EQ(s.read(va, 4), 0);
    }
    CHECK(!s.mapped(loader::kStackTop));                                     // user space ends here
    CHECK(!s.mapped(loader::kStackTop - (kStackPages + 1) * mmu::kPageSize));   // below the stack
}

TEST(loader_initial_break_is_page_after_highest_segment) {
    Space s;
    auto p = program(s.load(make_elf(0x10000, {
        {0x10000, {1, 2, 3, 4}},
        {0x12000, {1}, /*memsz=*/0x345},   // ends at 0x12345
    })));
    CHECK_EQ(p.initial_break, 0x13000);

    Space s2;
    p = program(s2.load(make_elf(0x10000, {{0x10000, std::vector<uint8_t>(0x2000, 1)}})));   // ends at 0x12000
    CHECK_EQ(p.initial_break, 0x12000);
}

TEST(loader_segment_may_end_exactly_at_stack_region) {
    Space s;
    const uint32_t start = loader::kStackRegion - 4;
    program(s.load(make_elf(start, {{start, {1, 2, 3, 4}}})));
    CHECK_EQ(s.read(start, 4), 0x04030201);
}

// ---- Invalid headers ----

TEST(loader_rejects_file_shorter_than_header) {
    Space s;
    std::vector<uint8_t> f = simple_elf();
    f.resize(20);
    CHECK(failed(s.load(f)));
    CHECK(failed(s.load({})));
}

TEST(loader_rejects_wrong_magic) {
    Space s;
    std::vector<uint8_t> f = simple_elf();
    f[1] = 'X';
    CHECK(failed(s.load(f)));
}

TEST(loader_rejects_64_bit_elf) {
    Space s;
    std::vector<uint8_t> f = simple_elf();
    f[4] = 2;   // ELFCLASS64
    CHECK(failed(s.load(f)));
}

TEST(loader_rejects_big_endian) {
    Space s;
    std::vector<uint8_t> f = simple_elf();
    f[5] = 2;   // ELFDATA2MSB
    CHECK(failed(s.load(f)));
}

TEST(loader_rejects_other_machines) {
    Space s;
    std::vector<uint8_t> f = simple_elf();
    put16(f, E_MACHINE, 62);   // x86-64
    CHECK(failed(s.load(f)));
}

TEST(loader_rejects_non_executables) {
    Space s;
    std::vector<uint8_t> f = simple_elf();
    put16(f, E_TYPE, 1);   // ET_REL: an object file (.o), not linked yet
    CHECK(failed(s.load(f)));
}

TEST(loader_rejects_program_headers_outside_file) {
    Space s;
    std::vector<uint8_t> f = simple_elf();
    put32(f, E_PHOFF, static_cast<uint32_t>(f.size()));   // headers would start past the end
    CHECK(failed(s.load(f)));

    f = simple_elf();
    put32(f, E_PHOFF, 0xffffffe0);   // offset + size wraps around 32 bits
    CHECK(failed(s.load(f)));
}

// ---- Invalid segments ----

TEST(loader_rejects_segment_data_outside_file) {
    Space s;
    std::vector<uint8_t> f = simple_elf();
    put32(f, kEhdrSize + P_FILESZ, 1000);   // claims more bytes than the file has
    put32(f, kEhdrSize + P_MEMSZ, 1000);
    CHECK(failed(s.load(f)));

    f = simple_elf();
    put32(f, kEhdrSize + P_OFFSET, 0xfffffffc);   // offset + size wraps around 32 bits
    CHECK(failed(s.load(f)));
}

TEST(loader_rejects_filesz_larger_than_memsz) {
    Space s;
    std::vector<uint8_t> f = simple_elf();
    put32(f, kEhdrSize + P_MEMSZ, 4);   // file has 8 bytes, memory only 4
    CHECK(failed(s.load(f)));
}

TEST(loader_rejects_segment_in_null_guard) {
    Space s;
    CHECK(failed(s.load(make_elf(0, {{0, {1, 2, 3, 4}}}))));
    CHECK(failed(s.load(make_elf(0xfffc, {{0xfffc, {1, 2, 3, 4}}}))));
}

TEST(loader_rejects_segment_overlapping_stack_region) {
    Space s;
    const uint32_t start = loader::kStackRegion - 3;   // 4 bytes: the last one is in the stack region
    CHECK(failed(s.load(make_elf(start, {{start, {1, 2, 3, 4}}}))));
    // bss can also push a segment into it
    const uint32_t start2 = loader::kStackRegion - 16;
    CHECK(failed(s.load(make_elf(start2, {{start2, {1}, /*memsz=*/32}}))));
    // and kernel space is above it
    CHECK(failed(s.load(make_elf(0x80000000, {{0x80000000, {1}}}))));
}

TEST(loader_rejects_segment_address_that_wraps) {
    Space s;
    CHECK(failed(s.load(make_elf(0x10000, {{0xfffff000, {1}, /*memsz=*/0x2000}}))));
}

// ---- Failure changes nothing ----

TEST(loader_failed_load_changes_nothing) {
    // The first segment is fine, the second is not: no frame, mapping or byte may change.
    Space s;
    const auto r = s.load(make_elf(0x10000, {
        {0x10000, {0xaa, 0xbb, 0xcc, 0xdd}},
        {0, {1, 2, 3, 4}},   // in the null guard
    }));
    CHECK(failed(r));
    CHECK_EQ(s.frames.free_count(), mmu::kNumFrames);
    CHECK(!s.mapped(0x10000));
    CHECK(!s.mapped(loader::kStackTop - mmu::kPageSize));
    for (uint32_t a = 0; a < 0x2000; a += 4) CHECK_EQ(s.ram.read(a, 4).value(), 0);
}

TEST(loader_rejects_program_that_does_not_fit_in_ram) {
    // Leave exactly enough frames for the stack, but not for the code page.
    Space s;
    while (s.frames.free_count() > kStackPages) s.frames.allocate();
    const auto r = s.load(simple_elf());
    CHECK(failed(r));
    CHECK_EQ(s.frames.free_count(), kStackPages);   // nothing allocated
    CHECK(!s.mapped(0x10000));
    CHECK(!s.mapped(loader::kStackTop - mmu::kPageSize));
}

TEST(loader_error_message_is_not_empty) {
    Space s;
    const auto r = s.load({1, 2, 3});
    CHECK(failed(r));
    if (failed(r)) CHECK(!std::get<std::string>(r).empty());
}
