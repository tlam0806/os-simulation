// The accessed (A) and dirty (D) bits, spec A2: set by the MMU on every
// successful translation, for the CPU's accesses and the kernel's accesses
// through user addresses alike. Page replacement (Part D) will read them.
//
//   mmu_bits_    translate() on a page table directly
//   cpu_bits_    a CPU running instructions through its page table
//   kbits_       the kernel: loader, lazy pages, system calls touching user memory
#include <cstdint>

#include "cpu_helpers.hpp"
#include "fs_system.hpp"
#include "mmu/page_table.hpp"
#include "test.hpp"

using mmu::Access;
using mmu::Pte;

namespace {

constexpr Pte RW(uint32_t frame) { return {frame, true, true, false}; }
constexpr Pte R(uint32_t frame) { return {frame, true, false, false}; }
constexpr Pte RX(uint32_t frame) { return {frame, true, false, true}; }

// The entry for the page holding vaddr (an unmapped page reads as an all-clear entry).
Pte entry(const mmu::PageTable& t, uint32_t vaddr) { return t.lookup(vaddr >> mmu::kPageShift).value_or(Pte{}); }

}  // namespace

// =================== translate() ===================

TEST(mmu_bits_new_mapping_is_clean) {
    mmu::PageTable t;
    t.map(0x20, RW(6));
    CHECK(!entry(t, 0x20000).accessed);
    CHECK(!entry(t, 0x20000).dirty);
}

TEST(mmu_bits_load_sets_accessed_only) {
    mmu::PageTable t;
    t.map(0x20, RW(6));
    CHECK(mmu::translate(t, 0x20010, Access::load).has_value());
    CHECK(entry(t, 0x20000).accessed);
    CHECK(!entry(t, 0x20000).dirty);
}

TEST(mmu_bits_fetch_sets_accessed_only) {
    mmu::PageTable t;
    t.map(0x10, RX(5));
    CHECK(mmu::translate(t, 0x10000, Access::fetch).has_value());
    CHECK(entry(t, 0x10000).accessed);
    CHECK(!entry(t, 0x10000).dirty);
}

TEST(mmu_bits_store_sets_both) {
    mmu::PageTable t;
    t.map(0x20, RW(6));
    CHECK(mmu::translate(t, 0x20ffc, Access::store).has_value());
    CHECK(entry(t, 0x20000).accessed);
    CHECK(entry(t, 0x20000).dirty);
}

TEST(mmu_bits_fault_sets_nothing) {
    // A store to a read-only page faults: no bit changes. A fault on an
    // unmapped page creates no entry.
    mmu::PageTable t;
    t.map(0x20, R(6));
    CHECK(!mmu::translate(t, 0x20000, Access::store).has_value());
    CHECK(!entry(t, 0x20000).accessed);
    CHECK(!entry(t, 0x20000).dirty);
    CHECK(!mmu::translate(t, 0x30000, Access::load).has_value());
    CHECK(!t.lookup(0x30).has_value());
}

TEST(mmu_bits_only_the_page_accessed) {
    mmu::PageTable t;
    t.map(0x20, RW(6));
    t.map(0x21, RW(7));
    CHECK(mmu::translate(t, 0x21000, Access::store).has_value());
    CHECK(!entry(t, 0x20000).accessed);
    CHECK(!entry(t, 0x20000).dirty);
    CHECK(entry(t, 0x21000).dirty);
}

TEST(mmu_bits_stay_set) {
    // A later load doesn't clear D: only the kernel clears bits.
    mmu::PageTable t;
    t.map(0x20, RW(6));
    CHECK(mmu::translate(t, 0x20000, Access::store).has_value());
    CHECK(mmu::translate(t, 0x20000, Access::load).has_value());
    CHECK(entry(t, 0x20000).dirty);
}

TEST(mmu_bits_translation_is_unchanged) {
    // Setting the bits doesn't change the answer: same frame, same offset.
    mmu::PageTable t;
    t.map(0x20, RW(6));
    CHECK_EQ(mmu::translate(t, 0x20123, Access::store).value_or(0), 0x6123u);
    CHECK_EQ(mmu::translate(t, 0x20123, Access::store).value_or(0), 0x6123u);
    CHECK_EQ(entry(t, 0x20000).frame, 6u);
    CHECK(entry(t, 0x20000).read && entry(t, 0x20000).write && !entry(t, 0x20000).exec);
}

TEST(mmu_bits_kernel_clears_by_mapping_again) {
    mmu::PageTable t;
    t.map(0x20, RW(6));
    CHECK(mmu::translate(t, 0x20000, Access::store).has_value());
    Pte p = entry(t, 0x20000);
    p.accessed = false;
    t.map(0x20, p);
    CHECK(!entry(t, 0x20000).accessed);
    CHECK(entry(t, 0x20000).dirty);
}

// =================== the CPU ===================

namespace {

using namespace isa::reg;
using enum isa::Op;

constexpr uint32_t kCode = 0x10000, kCodeFrame = 5;
constexpr uint32_t kDataPage = 0x20000, kDataFrame = 6;

// Code page -> frame 5 (R+X) holding `inst`; data page -> frame 6 with `data`.
void setup(Machine& m, mmu::PageTable& t, isa::Instruction inst, Pte data) {
    t.map(kCode >> mmu::kPageShift, RX(kCodeFrame));
    t.map(kDataPage >> mmu::kPageShift, data);
    m.ram.write(kCodeFrame << mmu::kPageShift, 4, isa::encode(inst));
    m.cpu.pc = kCode;
    m.cpu.set_page_table(&t);
    m.cpu.set_reg(t0, kDataPage);
}

}  // namespace

TEST(cpu_bits_fetch_marks_the_code_page) {
    Machine m;
    mmu::PageTable t;
    setup(m, t, {ADDI, a0, zero, 0, 1}, RW(kDataFrame));
    CHECK(!m.cpu.step().has_value());
    CHECK(entry(t, kCode).accessed);
    CHECK(!entry(t, kCode).dirty);
    CHECK(!entry(t, kDataPage).accessed);
}

TEST(cpu_bits_load_marks_accessed) {
    Machine m;
    mmu::PageTable t;
    setup(m, t, {LW, a0, t0, 0, 8}, R(kDataFrame));
    CHECK(!m.cpu.step().has_value());
    CHECK(entry(t, kDataPage).accessed);
    CHECK(!entry(t, kDataPage).dirty);
}

TEST(cpu_bits_store_marks_dirty) {
    Machine m;
    mmu::PageTable t;
    setup(m, t, {SB, 0, t0, a0, 5}, RW(kDataFrame));
    CHECK(!m.cpu.step().has_value());
    CHECK(entry(t, kDataPage).accessed);
    CHECK(entry(t, kDataPage).dirty);
}

TEST(cpu_bits_faulting_store_marks_nothing) {
    // The fetch succeeded (code page accessed); the store faulted (data page clean).
    Machine m;
    mmu::PageTable t;
    setup(m, t, {SW, 0, t0, a0, 0}, R(kDataFrame));
    CHECK(m.cpu.step().has_value());
    CHECK(entry(t, kCode).accessed);
    CHECK(!entry(t, kDataPage).accessed);
    CHECK(!entry(t, kDataPage).dirty);
}

// =================== the kernel ===================

namespace {

using namespace fs_system;

constexpr uint32_t kOther = 0x80000000 - 0x3000;   // a stack page the programs never touch themselves

// Runs `a` for a while; it must end with a long spin so it is still alive.
// Returns its page table as the kernel holds it.
mmu::PageTable run_then_look(System& s, Asm& a, const Strings& str = {}) {
    a.busy_wait(1'000'000);
    a.exit_with(0);
    const uint32_t pid = s.spawn(a, str.bytes);
    s.k.run_all(30'000);
    const kernel::Process* p = s.k.process(pid);
    CHECK(p != nullptr);
    return p ? p->page_table : mmu::PageTable{};
}

}  // namespace

TEST(kbits_loader_maps_pages_clean) {
    // The loader writes the code, data and stack frames by physical address.
    System s;
    Strings str;
    str.add("data");
    Asm a;
    a.exit_with(0);
    const uint32_t pid = s.spawn(a, str.bytes);
    const kernel::Process* p = s.k.process(pid);
    CHECK(p != nullptr);
    if (!p) return;
    CHECK(!p->page_table.mapped_pages().empty());
    for (uint32_t vpn : p->page_table.mapped_pages()) {
        CHECK(!p->page_table.lookup(vpn)->accessed);
        CHECK(!p->page_table.lookup(vpn)->dirty);
    }
}

TEST(kbits_program_store_marks_its_page) {
    System s;
    Asm a;
    a.li(t0, static_cast<int32_t>(kBuf));
    a.li(t1, 1);
    a.emit({SW, 0, t0, t1, 0});
    const mmu::PageTable t = run_then_look(s, a);
    CHECK(entry(t, kBuf).accessed);
    CHECK(entry(t, kBuf).dirty);
    CHECK(entry(t, 0x10000).accessed);       // the code page: fetched
    CHECK(!entry(t, 0x10000).dirty);
    CHECK(!entry(t, kOther).accessed);       // untouched
}

TEST(kbits_lazy_heap_page_is_marked_by_the_retried_store) {
    // The store faults, the kernel maps a clean page, the store runs again and
    // sets the bits.
    System s;
    Asm a;
    a.sbrk(4096);                            // a0 = the old break: the new page
    a.li(t1, 7);
    a.emit({SW, 0, a0, t1, 0});
    a.emit({ADDI, s1, a0, 0, 0});
    const mmu::PageTable t = run_then_look(s, a);
    CHECK(entry(t, 0x11000).accessed);       // code at 0x10000 only: heap starts at 0x11000
    CHECK(entry(t, 0x11000).dirty);
}

TEST(kbits_read_into_a_buffer_makes_it_dirty) {
    // The kernel's copy into the buffer goes through the MMU like a store.
    System s;
    Strings str;
    const uint32_t p = str.add("/hello");
    Asm a;
    open(a, p);
    read(a, 3, kOther, 2);
    const mmu::PageTable t = run_then_look(s, a, str);
    CHECK(entry(t, kOther).accessed);
    CHECK(entry(t, kOther).dirty);
}

TEST(kbits_write_from_a_buffer_only_accesses_it) {
    // write reads the buffer: accessed, not dirty.
    System s;
    Asm a;
    a.li(a0, 1);
    a.li(a1, static_cast<int32_t>(kOther));
    a.li(a2, 4);
    a.syscall(SYS_WRITE);
    const mmu::PageTable t = run_then_look(s, a);
    CHECK(entry(t, kOther).accessed);
    CHECK(!entry(t, kOther).dirty);
}

TEST(kbits_open_path_is_accessed) {
    // open reads the path from the read-only data page.
    System s;
    Strings str;
    const uint32_t p = str.add("/hello");
    Asm a;
    open(a, p);
    const mmu::PageTable t = run_then_look(s, a, str);
    CHECK(entry(t, kData).accessed);
    CHECK(!entry(t, kData).dirty);
}

TEST(kbits_disk_read_buffer_is_dirty) {
    // The disk writes the frame directly (DMA); the page is marked dirty when
    // the kernel checks the buffer.
    System s;
    Asm a;
    a.li(a0, 0);
    a.li(a1, static_cast<int32_t>(kOther));
    a.li(a2, 1);
    a.syscall(SYS_DISK_READ);
    const mmu::PageTable t = run_then_look(s, a);
    CHECK(entry(t, kOther).accessed);
    CHECK(entry(t, kOther).dirty);
}

TEST(kbits_disk_write_buffer_is_only_accessed) {
    System s;
    Asm a;
    a.li(a0, 100);
    a.li(a1, static_cast<int32_t>(kOther));
    a.li(a2, 1);
    a.syscall(SYS_DISK_WRITE);
    const mmu::PageTable t = run_then_look(s, a);
    CHECK(entry(t, kOther).accessed);
    CHECK(!entry(t, kOther).dirty);
}
