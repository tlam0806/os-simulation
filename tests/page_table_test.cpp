// Page table and translation (spec A2).
#include <cstdint>
#include <optional>
#include <vector>
#include <ostream>

#include "mmu/page_table.hpp"
#include "test.hpp"

using mmu::Access;
using mmu::Pte;

// Let CHECK_EQ print a Pte when a check fails.
namespace mmu {
std::ostream& operator<<(std::ostream& os, const Pte& p) {
    return os << "{frame=" << p.frame << " r=" << p.read << " w=" << p.write << " x=" << p.exec << "}";
}
}  // namespace mmu

namespace {

constexpr Pte kRwx = {.frame = 0, .read = true, .write = true, .exec = true};

Pte rwx(uint32_t frame) { Pte p = kRwx; p.frame = frame; return p; }
Pte perms(uint32_t frame, bool r, bool w, bool x) { return {frame, r, w, x}; }

// translate() as a plain number: the physical address, or kFault for a page fault.
constexpr int64_t kFault = -1;
int64_t phys(const mmu::PageTable& t, uint32_t vaddr, Access a) {
    const std::optional<uint32_t> p = mmu::translate(t, vaddr, a);
    return p ? int64_t{*p} : kFault;
}

}  // namespace

// ---- translation ----

TEST(page_table_empty_faults_everywhere) {
    mmu::PageTable t;
    for (Access a : {Access::fetch, Access::load, Access::store}) {
        CHECK_EQ(phys(t, 0, a), kFault);
        CHECK_EQ(phys(t, 0x10000, a), kFault);
        CHECK_EQ(phys(t, 0xffffffff, a), kFault);
    }
}

TEST(page_table_translates_frame_and_keeps_offset) {
    mmu::PageTable t;
    t.map(0x10, rwx(3));   // virtual 0x10000..0x10fff -> physical 0x3000..0x3fff
    CHECK_EQ(phys(t, 0x10000, Access::load), 0x3000);
    CHECK_EQ(phys(t, 0x10abc, Access::load), 0x3abc);
    CHECK_EQ(phys(t, 0x10fff, Access::load), 0x3fff);
}

TEST(page_table_neighbor_pages_are_not_mapped) {
    mmu::PageTable t;
    t.map(0x10, rwx(3));
    CHECK_EQ(phys(t, 0x0ffff, Access::load), kFault);   // last byte of page 0xf
    CHECK_EQ(phys(t, 0x11000, Access::load), kFault);   // first byte of page 0x11
}

TEST(page_table_highest_page) {
    mmu::PageTable t;
    t.map(0xfffff, rwx(255));   // the last virtual page -> the last physical frame
    CHECK_EQ(phys(t, 0xfffff000, Access::load), 0xff000);
    CHECK_EQ(phys(t, 0xffffffff, Access::load), 0xfffff);
}

TEST(page_table_frame_zero_is_a_real_frame) {
    // Frame 0 is valid: "mapped to frame 0" must not be confused with "not mapped".
    mmu::PageTable t;
    t.map(0x20, rwx(0));
    CHECK_EQ(phys(t, 0x20010, Access::store), 0x10);
}

TEST(page_table_pages_can_share_a_frame) {
    mmu::PageTable t;
    t.map(0x10, rwx(7));
    t.map(0x99, rwx(7));
    CHECK_EQ(phys(t, 0x10123, Access::load), 0x7123);
    CHECK_EQ(phys(t, 0x99123, Access::load), 0x7123);
}

// ---- permissions: X for fetch, R for load, W for store ----

TEST(page_table_read_only_page) {
    mmu::PageTable t;
    t.map(1, perms(5, true, false, false));
    CHECK_EQ(phys(t, 0x1004, Access::load), 0x5004);
    CHECK_EQ(phys(t, 0x1004, Access::store), kFault);
    CHECK_EQ(phys(t, 0x1004, Access::fetch), kFault);
}

TEST(page_table_code_page_is_read_and_execute) {
    // Typical .text: R+X. Storing into code must fault.
    mmu::PageTable t;
    t.map(1, perms(5, true, false, true));
    CHECK_EQ(phys(t, 0x1000, Access::fetch), 0x5000);
    CHECK_EQ(phys(t, 0x1000, Access::load), 0x5000);
    CHECK_EQ(phys(t, 0x1000, Access::store), kFault);
}

TEST(page_table_data_page_is_not_executable) {
    // Typical .data/stack: R+W. Jumping into data must fault.
    mmu::PageTable t;
    t.map(1, perms(5, true, true, false));
    CHECK_EQ(phys(t, 0x1000, Access::load), 0x5000);
    CHECK_EQ(phys(t, 0x1000, Access::store), 0x5000);
    CHECK_EQ(phys(t, 0x1000, Access::fetch), kFault);
}

TEST(page_table_execute_only_page) {
    // X without R: fetching is allowed, reading the bytes as data is not.
    mmu::PageTable t;
    t.map(1, perms(5, false, false, true));
    CHECK_EQ(phys(t, 0x1000, Access::fetch), 0x5000);
    CHECK_EQ(phys(t, 0x1000, Access::load), kFault);
}

TEST(page_table_no_permissions_faults_everything) {
    mmu::PageTable t;
    t.map(1, perms(5, false, false, false));
    for (Access a : {Access::fetch, Access::load, Access::store})
        CHECK_EQ(phys(t, 0x1000, a), kFault);
}

// ---- map / unmap / lookup ----

TEST(page_table_lookup) {
    mmu::PageTable t;
    CHECK(!t.lookup(0x10).has_value());
    t.map(0x10, perms(3, true, false, true));
    CHECK_EQ(t.lookup(0x10).value_or(Pte{}), perms(3, true, false, true));
    CHECK(!t.lookup(0x11).has_value());
}

TEST(page_table_map_replaces_existing_mapping) {
    mmu::PageTable t;
    t.map(0x10, rwx(1));
    t.map(0x10, perms(2, true, false, false));
    CHECK_EQ(phys(t, 0x10000, Access::load), 0x2000);
    CHECK_EQ(phys(t, 0x10000, Access::store), kFault);   // new permissions too
}

TEST(page_table_unmap) {
    mmu::PageTable t;
    t.map(0x10, rwx(1));
    t.map(0x11, rwx(2));
    t.unmap(0x10);
    CHECK_EQ(phys(t, 0x10000, Access::load), kFault);
    CHECK_EQ(phys(t, 0x11000, Access::load), 0x2000);   // others stay
    t.unmap(0x10);    // unmapping again: no effect, no crash
    t.unmap(0x500);   // never mapped: no effect, no crash
    CHECK(!t.lookup(0x10).has_value());
}

TEST(page_table_tables_are_independent) {
    // Two processes, two address spaces: the same virtual address, different frames.
    mmu::PageTable a, b;
    a.map(0x10, rwx(1));
    b.map(0x10, rwx(2));
    CHECK_EQ(phys(a, 0x10000, Access::load), 0x1000);
    CHECK_EQ(phys(b, 0x10000, Access::load), 0x2000);
}

TEST(page_table_mapped_pages) {
    // Used by the kernel to free every frame of a process that ends.
    mmu::PageTable t;
    CHECK(t.mapped_pages().empty());
    t.map(0x7ffff, rwx(1));
    t.map(0x10, rwx(2));
    t.map(0x11, rwx(3));
    t.unmap(0x11);
    const std::vector<uint32_t> pages = t.mapped_pages();
    CHECK_EQ(pages.size(), 2);
    if (pages.size() == 2) {
        CHECK_EQ(pages[0], 0x10);      // in increasing order
        CHECK_EQ(pages[1], 0x7ffff);
    }
}

TEST(page_table_mapped_pages_sorted_for_many_pages) {
    // With many pages, a hash table's own order is almost never sorted.
    mmu::PageTable t;
    for (uint32_t i = 0; i < 300; i++) t.map((i * 7919) % 100000, rwx(i % 256));   // scattered vpns
    const std::vector<uint32_t> pages = t.mapped_pages();
    CHECK_EQ(pages.size(), 300);
    bool sorted = true;
    for (size_t i = 1; i < pages.size(); i++) sorted = sorted && pages[i - 1] < pages[i];
    CHECK(sorted);
}
