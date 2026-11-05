// Swapping (spec Part D): when RAM is full, the page-fault handler evicts a page
// chosen by CLOCK to a swap slot, and brings swapped-out pages back on demand.
//
//   swap_layout_   the swap area at the end of the disk (D1)
//   swap_mmu_      an entry with V clear always faults (D2)
//   swap_out_      programs bigger than RAM: pages go to slots, with the right bytes
//   swap_in_       pages come back intact; clean pages are dropped, dirty ones written
//   swap_slots_    slots are freed with their pages; a full swap area kills
//   swap_fork_     fork shares slots: one process's exit doesn't free the other's
//   swap_kaccess_  system calls touching swapped-out pages (D7)
//
// RAM has 256 frames. The programs below use a heap of N pages and touch each
// page in order ("passes"), so with N = 300 every pass goes through more pages
// than fit in RAM. Page i holds 7i + 3 in its word 0 and 7i + 4 in its word at
// byte 2044; a check pass compares both and exits with i + 1 at the first wrong
// page. 0 means every page was right.
#include <functional>
#include <set>
#include <vector>

#include "fs_system.hpp"
#include "mmu/page_table.hpp"
#include "util/bytes.hpp"

using namespace fs_system;

namespace {

constexpr uint64_t kForever = 4'000'000'000;   // run_all's limit: disk waits are long
constexpr int32_t kPage = 4096;
constexpr uint32_t kHeap = 0x11000;            // the heap of a code-only program starts here

// Pages moved to (write) or from (read) the swap area so far: the disk's
// completed requests there (spec D6).
uint32_t swap_transfers(System& s, disk::Op op) {
    uint32_t n = 0;
    for (const disk::Request& r : s.m.disk().completed())
        if (r.op == op && r.first_sector >= fs::kSwapStart * fs::kSectorsPerBlock) n++;
    return n;
}
uint32_t outs(System& s) { return swap_transfers(s, disk::Op::write); }
uint32_t ins(System& s) { return swap_transfers(s, disk::Op::read); }

// for (s3 = 0; s3 != s2; s3++) { t0 = s1 + (s3 << 12); body(); }
void for_each_page(Asm& a, const std::function<void()>& body) {
    a.li(s3, 0);
    const size_t top = a.here();
    a.emit({SLLI, t0, s3, 0, 12});
    a.emit({ADD, t0, t0, s1, 0});
    body();
    a.emit({ADDI, s3, s3, 0, 1});
    a.patch(a.branch(BNE, s3, s2), top);
}

// t2 = 7 * s3 + 3 + extra: page s3's word 0.
void expected(Asm& a, int32_t extra) {
    a.li(t3, 7);
    a.emit({MUL, t2, s3, t3, 0});
    a.emit({ADDI, t2, t2, 0, 3 + extra});
}

void write_pass(Asm& a, int32_t extra = 0) {
    for_each_page(a, [&] {
        expected(a, extra);
        a.emit({SW, 0, t0, t2, 0});
        a.emit({ADDI, t2, t2, 0, 1});
        a.emit({SW, 0, t0, t2, 2044});
    });
}

// Adds a branch to `fails` for each wrong word.
void check_pass(Asm& a, std::vector<size_t>& fails, int32_t extra = 0) {
    for_each_page(a, [&] {
        expected(a, extra);
        a.emit({LW, t1, t0, 0, 0});
        fails.push_back(a.branch(BNE, t1, t2));
        a.emit({ADDI, t2, t2, 0, 1});
        a.emit({LW, t1, t0, 0, 2044});
        fails.push_back(a.branch(BNE, t1, t2));
    });
}

// Both words += 1000: makes every page dirty.
void modify_pass(Asm& a) {
    for_each_page(a, [&] {
        for (int32_t off : {0, 2044}) {
            a.emit({LW, t1, t0, 0, off});
            a.emit({ADDI, t1, t1, 0, 1000});
            a.emit({SW, 0, t0, t1, off});
        }
    });
}

// The common start: s1 = a new heap of `pages` pages, s2 = pages.
void grow_heap(Asm& a, int32_t pages) {
    a.sbrk(pages * kPage);
    a.emit({ADDI, s1, a0, 0, 0});
    a.li(s2, pages);
}

// The common end: exit(0), or exit(page + 1) from a failed check.
void finish(Asm& a, const std::vector<size_t>& fails) {
    a.exit_with(0);
    for (size_t f : fails) a.patch(f, a.here());
    a.emit({ADDI, a0, s3, 0, 1});
    a.exit_with_a0();
}

// Writes `pages` pages, then `checks` check passes; with `modify`, then a
// modify pass and a final check.
Asm heap_program(int32_t pages, int checks, bool modify = false) {
    Asm a;
    std::vector<size_t> fails;
    grow_heap(a, pages);
    write_pass(a);
    for (int i = 0; i < checks; i++) check_pass(a, fails);
    if (modify) {
        modify_pass(a);
        check_pass(a, fails, 1000);
    }
    finish(a, fails);
    return a;
}

int32_t run_one(System& s, const Asm& a) {
    const uint32_t pid = s.spawn(a);
    CHECK(std::holds_alternative<kernel::AllDone>(s.k.run_all(kForever)));
    return s.status(pid);
}

}  // namespace

// =================== the swap area ===================

TEST(swap_layout_format_reserves_the_swap_area) {
    System s;
    CHECK(fs::block_in_use(s.m.disk(), fs::kSwapStart));
    CHECK(fs::block_in_use(s.m.disk(), fs::kSwapStart + 100));
    CHECK(fs::block_in_use(s.m.disk(), fs::kNumBlocks - 1));
    CHECK(!fs::block_in_use(s.m.disk(), fs::kSwapStart - 1));
}

// =================== V ===================

TEST(swap_mmu_invalid_entry_always_faults) {
    // Permissions say yes, V says no: every access faults and sets no bit.
    mmu::PageTable t;
    mmu::Pte p{.frame = 6, .read = true, .write = true, .exec = true};
    p.valid = false;
    p.swap_slot = 5;
    t.map(0x20, p);
    for (mmu::Access a : {mmu::Access::fetch, mmu::Access::load, mmu::Access::store})
        CHECK(!mmu::translate(t, 0x20000, a).has_value());
    const mmu::Pte back = t.lookup(0x20).value_or(mmu::Pte{});
    CHECK(!back.accessed);
    CHECK(!back.dirty);
    CHECK_EQ(back.swap_slot.value_or(0), 5u);
}

TEST(swap_mmu_new_entries_are_valid) {
    const mmu::Pte p{};
    CHECK(p.valid);
    CHECK(!p.swap_slot.has_value());
}

// =================== the test programs themselves ===================

TEST(swap_program_that_fits_needs_no_swap) {
    // 100 pages fit in RAM: this passes without swapping, which shows the test
    // programs are right (and that a wrong page would be caught).
    System s;
    CHECK_EQ(run_one(s, heap_program(100, 2, true)), 0);
    CHECK_EQ(outs(s), 0u);
    CHECK_EQ(ins(s), 0u);
}

TEST(swap_program_detects_a_wrong_page) {
    // The check pass expects 7i + 3 + 1000 after no modify pass: page 0 is wrong.
    System s;
    Asm a;
    std::vector<size_t> fails;
    grow_heap(a, 10);
    write_pass(a);
    check_pass(a, fails, 1000);
    finish(a, fails);
    CHECK_EQ(run_one(s, a), 1);
}

// =================== swapping out ===================

TEST(swap_out_pages_go_to_slots_with_their_bytes) {
    // 300 pages written once, then the program spins. RAM can't hold them all:
    // some heap entries are now V clear, each with its own slot, and that slot's
    // block on the disk holds the page's bytes.
    System s;
    Asm a;
    grow_heap(a, 300);
    write_pass(a);
    a.busy_wait(1'000'000'000);
    a.exit_with(0);
    const uint32_t pid = s.spawn(a);
    // Each swap write takes under 20 ms (A6), so 5 s of clock is plenty; the
    // spin keeps the process alive past it.
    CHECK(std::holds_alternative<kernel::StepLimit>(s.k.run_all(5'000'000)));
    const kernel::Process* p = s.k.process(pid);
    CHECK(p != nullptr);
    if (!p) return;
    uint32_t resident = 0, swapped = 0;
    std::set<uint32_t> slots;
    for (uint32_t i = 0; i < 300; i++) {
        const auto e = p->page_table.lookup((kHeap >> mmu::kPageShift) + i);
        CHECK(e.has_value());
        if (!e) continue;
        if (e->valid) { resident++; continue; }
        swapped++;
        CHECK(e->swap_slot.has_value());
        if (!e->swap_slot) continue;
        CHECK(*e->swap_slot < fs::kSwapBlocks);
        slots.insert(*e->swap_slot);
        const auto block = fs::read_block(s.m.disk(), fs::kSwapStart + *e->swap_slot);
        CHECK(block.has_value());
        if (!block) continue;
        CHECK_EQ(util::get32(*block, 0), 7 * i + 3);
        CHECK_EQ(util::get32(*block, 2044), 7 * i + 4);
    }
    CHECK(resident <= 256);
    CHECK(swapped >= 300 - 256);
    CHECK_EQ(slots.size(), swapped);                  // one slot each
    CHECK(outs(s) >= swapped);
}

TEST(swap_out_program_bigger_than_ram_runs) {
    System s;
    CHECK_EQ(run_one(s, heap_program(300, 1)), 0);
}

// =================== swapping in ===================

TEST(swap_in_pages_survive_many_passes) {
    System s;
    CHECK_EQ(run_one(s, heap_program(300, 3)), 0);
    CHECK(ins(s) >= 3 * (300 - 256));
}

TEST(swap_in_clean_pages_are_dropped_not_written) {
    // After the write pass, the program only reads. A page read back from its
    // slot and not stored to since has D clear: evicting it needs no write.
    // Each page is written at most once (+ code and stack pages), yet hundreds
    // of pages come back: their later evictions wrote nothing.
    System s;
    CHECK_EQ(run_one(s, heap_program(300, 2)), 0);
    CHECK(outs(s) <= 300 + 20);
    CHECK(ins(s) >= 300);
}

TEST(swap_in_dirty_pages_are_written_again) {
    // The modify pass stores to every page after it came back: each eviction
    // then must write the new bytes, or the final check would read old ones.
    System s;
    CHECK_EQ(run_one(s, heap_program(300, 1, true)), 0);
    CHECK(outs(s) > 300 + 100);
}

TEST(swap_in_two_processes_share_ram) {
    // 2 x 200 pages: each fits alone, not together. The timer interleaves them.
    System s;
    const Asm a = heap_program(200, 2);
    const uint32_t first = s.spawn(a);
    const uint32_t second = s.spawn(a);
    CHECK(std::holds_alternative<kernel::AllDone>(s.k.run_all(kForever)));
    CHECK_EQ(s.status(first), 0);
    CHECK_EQ(s.status(second), 0);
    CHECK(outs(s) > 0);
}

// =================== slots ===================

TEST(swap_slots_are_freed_when_the_process_ends) {
    // Each run leaves about 60+ pages in slots at its exit; with slots never
    // freed, 10 runs would need more than the 512 slots.
    System s;
    for (int i = 0; i < 10; i++) CHECK_EQ(run_one(s, heap_program(300, 1)), 0);
}

TEST(swap_slots_full_kills_the_process) {
    // 900 pages > 256 frames + 512 slots: a fault finds no free slot.
    System s;
    Asm a;
    grow_heap(a, 900);
    write_pass(a);
    a.exit_with(0);
    const uint32_t pid = s.spawn(a);
    CHECK(std::holds_alternative<kernel::AllDone>(s.k.run_all(kForever)));
    const auto e = s.k.ending(pid);
    CHECK(e.has_value() && std::holds_alternative<kernel::Killed>(e->status));
    // Its slots are free again: a normal program runs.
    CHECK_EQ(run_one(s, heap_program(300, 1)), 0);
}

// =================== fork ===================

TEST(swap_fork_child_exit_keeps_the_parents_slots) {
    // The parent writes 300 pages (the first ~60 go to slots), shrinks to 200
    // (freeing frames), and forks: the child's copies of the swapped-out entries
    // share the parent's slots. The child checks its 200 pages and exits.
    // If its exit freed the shared slots, the parent's next evictions would take
    // those slots and overwrite them: the parent then grows a new region of 150
    // pages to force evictions, and checks its first 200 pages.
    System s;
    Asm a;
    std::vector<size_t> fails;
    grow_heap(a, 300);
    write_pass(a);
    a.sbrk(-100 * kPage);
    a.li(s2, 200);
    a.syscall(SYS_FORK);
    const size_t to_child = a.branch(BEQ, a0, zero);
    a.li(a0, -1);                                   // parent: wait(-1, kBuf)
    a.li(a1, static_cast<int32_t>(kBuf));
    a.syscall(SYS_WAIT);
    a.emit({ADDI, s5, s1, 0, 0});                   // keep the old base
    grow_heap(a, 150);                              // the new region: s1, s2 = 150
    write_pass(a, 1000);
    a.emit({ADDI, s1, s5, 0, 0});
    a.li(s2, 200);
    check_pass(a, fails);
    a.li(t0, static_cast<int32_t>(kBuf));
    a.emit({LW, a0, t0, 0, 0});                     // the child's status
    a.exit_with_a0();
    a.patch(to_child, a.here());
    check_pass(a, fails);                           // child
    finish(a, fails);
    const uint32_t parent = s.spawn(a);
    CHECK(std::holds_alternative<kernel::AllDone>(s.k.run_all(kForever)));
    CHECK_EQ(s.status(parent), 0);
    CHECK_EQ(s.status(parent + 1), 0);
}

// =================== system calls touching swapped-out pages (D7) ===================

namespace {

// s1 = a heap of 300 pages, all written: the first ones are swapped out by
// the end (the pages after them pushed them out, and nothing touched them since).
void push_out_first_pages(Asm& a) {
    grow_heap(a, 300);
    write_pass(a);
}

// If the word at off(s1) isn't `value`, exit(code).
void expect_word(Asm& a, int32_t off, int32_t value, int32_t code) {
    a.emit({LW, t1, s1, 0, off});
    a.li(t2, value);
    const size_t ok = a.branch(BEQ, t1, t2);
    a.exit_with(code);
    a.patch(ok, a.here());
}

// If register r isn't `value`, exit(code).
void expect_reg(Asm& a, uint8_t r, int32_t value, int32_t code) {
    a.li(t2, value);
    const size_t ok = a.branch(BEQ, r, t2);
    a.exit_with(code);
    a.patch(ok, a.here());
}

// Opens path (fd 3) and reads one byte of it, then seeks back to 0, so every
// block the file needs is cached before RAM fills up: the block cache never
// evicts (C2), so a miss after that would be -ENOMEM, not what these tests test.
void open_and_cache(Asm& a, uint32_t path) {
    open(a, path);
    read(a, 3, kBuf, 1);
    lseek(a, 3, 0, kSet);
}

}  // namespace

TEST(swap_kaccess_setup_first_heap_pages_are_out) {
    // The premise of the tests below: after push_out_first_pages, heap pages 0
    // and 1 are swapped out.
    System s;
    Asm a;
    push_out_first_pages(a);
    a.busy_wait(1'000'000'000);
    a.exit_with(0);
    const uint32_t pid = s.spawn(a);
    CHECK(std::holds_alternative<kernel::StepLimit>(s.k.run_all(5'000'000)));
    const kernel::Process* p = s.k.process(pid);
    CHECK(p != nullptr);
    if (!p) return;
    for (uint32_t i : {0u, 1u}) {
        const auto e = p->page_table.lookup((kHeap >> mmu::kPageShift) + i);
        CHECK(e.has_value() && !e->valid && e->swap_slot.has_value());
    }
}

TEST(swap_kaccess_write_from_a_swapped_out_buffer) {
    // write prints page 0's word 0 (3), once: the re-run prints nothing twice.
    System s;
    Asm a;
    push_out_first_pages(a);
    a.li(a0, 1);
    a.emit({ADDI, a1, s1, 0, 0});
    a.li(a2, 4);
    a.syscall(SYS_WRITE);
    a.exit_with_a0();
    CHECK_EQ(run_one(s, a), 4);
    CHECK(s.out.str() == std::string("\x03\0\0\0", 4));
}

TEST(swap_kaccess_write_spanning_two_swapped_out_pages) {
    // The last 4 bytes of page 0 (never written: 0) and word 0 of page 1 (10).
    System s;
    Asm a;
    push_out_first_pages(a);
    a.li(a0, 1);
    a.emit({ADDI, a1, s1, 0, 0});
    a.li(t0, kPage - 4);
    a.emit({ADD, a1, a1, t0, 0});
    a.li(a2, 8);
    a.syscall(SYS_WRITE);
    a.exit_with_a0();
    CHECK_EQ(run_one(s, a), 8);
    CHECK(s.out.str() == std::string("\0\0\0\0\x0a\0\0\0", 8));
}

TEST(swap_kaccess_read_into_a_swapped_out_buffer) {
    // read "hi" into page 0 at byte 8: the page comes back first, so its
    // other words survive, and the offset moves once (a second read gets 0).
    System s;
    Strings str;
    const uint32_t path = str.add("/hello");
    Asm a;
    open_and_cache(a, path);                        // fd 3
    push_out_first_pages(a);
    a.li(a0, 3);
    a.emit({ADDI, a1, s1, 0, 8});
    a.li(a2, 2);
    a.syscall(SYS_READ);
    expect_reg(a, a0, 2, 1);
    expect_word(a, 8, 'h' | ('i' << 8), 2);
    expect_word(a, 0, 3, 3);
    expect_word(a, 2044, 4, 4);
    a.li(a0, 3);
    a.emit({ADDI, a1, s1, 0, 8});
    a.li(a2, 2);
    a.syscall(SYS_READ);
    expect_reg(a, a0, 0, 5);                        // end of file: read once only
    a.exit_with(0);
    const uint32_t pid = s.spawn(a, str.bytes);
    CHECK(std::holds_alternative<kernel::AllDone>(s.k.run_all(kForever)));
    CHECK_EQ(s.status(pid), 0);
}

TEST(swap_kaccess_read_into_an_untouched_page_evicts) {
    // RAM is full; the buffer is a heap page never touched. Before swapping
    // this was -EFAULT; now the kernel evicts to map it.
    System s;
    Strings str;
    const uint32_t path = str.add("/hello");
    Asm a;
    open_and_cache(a, path);
    grow_heap(a, 301);
    a.li(s2, 300);                                  // write pages 0..299 only
    write_pass(a);
    a.li(a0, 3);
    a.li(t0, 300 * kPage);
    a.emit({ADD, a1, s1, t0, 0});
    a.li(a2, 2);
    a.syscall(SYS_READ);
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a, str.bytes);
    CHECK(std::holds_alternative<kernel::AllDone>(s.k.run_all(kForever)));
    CHECK_EQ(s.status(pid), 2);
}

TEST(swap_kaccess_open_path_on_a_swapped_out_page) {
    // The path "/hello" is in heap page 0 (byte 100), which is then pushed out.
    // Opening it once first caches the blocks the walk needs.
    System s;
    Strings str;
    Asm a;
    open(a, str.add("/hello"));
    close(a, 3);
    grow_heap(a, 300);
    const char* path = "/hello";
    for (int i = 0; path[i]; i++) {
        a.li(t1, path[i]);
        a.emit({SB, 0, s1, t1, 100 + i});
    }
    write_pass(a);
    a.emit({ADDI, a0, s1, 0, 100});
    a.syscall(SYS_OPEN);
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a, str.bytes);
    CHECK(std::holds_alternative<kernel::AllDone>(s.k.run_all(kForever)));
    CHECK_EQ(s.status(pid), 3);
}

TEST(swap_kaccess_wait_status_into_a_swapped_out_page) {
    // The parent's status word is on page 0, swapped out (and copy-on-write
    // after fork). The child is collected exactly once, its status lands on
    // the page, and the page's other words survive.
    System s;
    Asm a;
    push_out_first_pages(a);
    a.syscall(SYS_FORK);
    const size_t to_child = a.branch(BEQ, a0, zero);
    a.emit({ADDI, s5, a0, 0, 0});                   // the child's pid
    a.li(a0, -1);
    a.emit({ADDI, a1, s1, 0, 8});
    a.syscall(SYS_WAIT);
    const size_t same = a.branch(BEQ, a0, s5);
    a.exit_with(1);
    a.patch(same, a.here());
    expect_word(a, 8, 7, 2);
    expect_word(a, 0, 3, 3);
    expect_word(a, 2044, 4, 4);
    a.li(a0, -1);
    a.li(a1, 0);
    a.syscall(SYS_WAIT);
    const size_t none_left = a.branch(BLT, a0, zero);
    a.exit_with(5);                                 // collected twice
    a.patch(none_left, a.here());
    a.exit_with(0);
    a.patch(to_child, a.here());
    a.exit_with(7);
    const uint32_t parent = s.spawn(a);
    CHECK(std::holds_alternative<kernel::AllDone>(s.k.run_all(kForever)));
    CHECK_EQ(s.status(parent), 0);
}

TEST(swap_kaccess_disk_write_from_a_swapped_out_buffer) {
    // The disk must get page 0's bytes, not a fresh zero page's.
    constexpr int32_t kSector = 8 * 1000;           // a free data block
    System s;
    Asm a;
    push_out_first_pages(a);
    a.li(a0, kSector);
    a.emit({ADDI, a1, s1, 0, 0});
    a.li(a2, 1);
    a.syscall(SYS_DISK_WRITE);
    a.exit_with_a0();
    CHECK_EQ(run_one(s, a), 0);
    const auto bytes = s.m.disk().host_read(kSector, 1);
    CHECK(bytes.has_value());
    if (bytes) CHECK_EQ(util::get32(*bytes, 0), 3u);
}
