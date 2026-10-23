// Lazy heap (spec B1, B3): sbrk maps nothing; a heap page is mapped on its
// first access, exactly like a stack page. System calls that read user memory
// (write) map untouched heap and stack pages too, instead of failing.
//
// Expected behavior, on a load or store page fault (in addition to the stack):
//   - The page lies in the heap: from initial_break up to the page holding the
//     last heap byte (brk - 1). If it isn't mapped yet and a frame is free:
//     map a zero-filled R+W page and run the same instruction again.
//   - Otherwise the process is killed, as before.
// In write(): a buffer byte in an untouched heap or stack page gets its page
// mapped, then is read normally. If no frame is free for it: -EFAULT.
#include <cstdint>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "asm.hpp"
#include "cpu_helpers.hpp"
#include "elf_builder.hpp"
#include "kernel/kernel.hpp"
#include "loader/elf.hpp"
#include "machine/machine.hpp"
#include "test.hpp"

using namespace isa::reg;
using enum isa::Op;

namespace {

constexpr uint32_t kInitialBreak = 0x11000;   // code at 0x10000 is under one page
constexpr uint32_t kHeapVpn = kInitialBreak >> mmu::kPageShift;
constexpr int32_t kEfault = 14;

struct System {
    machine::Machine m;
    std::ostringstream out, err;
    kernel::Kernel k{m, out, err};

    uint32_t spawn(const Asm& a) {
        const auto r = k.spawn(elf_builder::make_elf(0x10000, {{0x10000, elf_builder::code(a.code)}}));
        CHECK(std::holds_alternative<uint32_t>(r));
        return std::holds_alternative<uint32_t>(r) ? std::get<uint32_t>(r) : 0;
    }

    void run() { CHECK(std::holds_alternative<kernel::AllDone>(k.run_all(1'000'000))); }

    int32_t status(uint32_t pid) {
        const auto e = k.ending(pid);
        const auto* exited = e ? std::get_if<kernel::Exited>(&*e) : nullptr;
        CHECK(exited != nullptr);
        return exited ? exited->exit_status : -12345;
    }
};

// write(1, buffer in a1, 4) and exit with its return value.
void write4_from_a1_and_exit(Asm& a) {
    a.li(a0, 1);
    a.li(a2, 4);
    a.syscall(SYS_WRITE);
    a.exit_with_a0();
}

}  // namespace

// ---- page faults in the heap ----

TEST(lazy_heap_maps_only_touched_pages) {
    // Reserve 10 pages, touch only the 4th, then spin so the process is still
    // alive when the test looks at its page table.
    System s;
    Asm a;
    a.sbrk(10 * 4096);
    a.li(t0, static_cast<int32_t>(kInitialBreak + 3 * 4096));
    a.emit({SW, 0, t0, zero, 0});
    a.emit({JAL, zero, 0, 0, 0});    // loop forever
    const uint32_t pid = s.spawn(a);
    CHECK(std::holds_alternative<kernel::StepLimit>(s.k.run_all(10000)));
    const kernel::Process* p = s.k.process(pid);
    CHECK(p != nullptr);
    if (!p) return;
    for (uint32_t i = 0; i < 10; i++) {
        const bool mapped = p->page_table.lookup(kHeapVpn + i).has_value();
        CHECK_EQ(mapped, i == 3);
    }
    const auto pte = p->page_table.lookup(kHeapVpn + 3);
    if (pte) CHECK(pte->read && pte->write && !pte->exec);
}

TEST(lazy_heap_fault_retries_the_same_instruction_once) {
    System s;
    Asm a;
    a.sbrk(4096);
    a.li(t1, 6);
    a.emit({SW, 0, a0, t1, 0});      // faults once, then succeeds
    a.emit({LW, a0, a0, 0, 0});
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(pid), 6);
    CHECK_EQ(s.m.steps(), a.code.size() + 1);
}

TEST(lazy_heap_shrink_frees_only_touched_pages) {
    // Pages 0 and 5 of 10 were touched; shrinking everything must free those
    // two frames and not trip over the 8 pages that were never mapped.
    System s;
    const uint32_t before = s.m.frame_allocator().free_count();
    Asm a;
    a.sbrk(10 * 4096);
    a.li(t0, static_cast<int32_t>(kInitialBreak));
    a.emit({SW, 0, t0, zero, 0});
    a.li(t0, static_cast<int32_t>(kInitialBreak + 5 * 4096));
    a.emit({SW, 0, t0, zero, 0});
    a.sbrk(-10 * 4096);
    a.exit_with(0);
    const uint32_t pid = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(pid), 0);
    CHECK_EQ(s.m.frame_allocator().free_count(), before);
}

TEST(lazy_heap_page_is_fresh_after_shrink_and_regrow) {
    // Write 7, give the page back, grow again: the new page must read 0, not 7.
    System s;
    Asm a;
    a.sbrk(4096);
    a.emit({ADDI, s0, a0, 0, 0});
    a.li(t1, 7);
    a.emit({SW, 0, s0, t1, 0});
    a.sbrk(-4096);
    a.sbrk(4096);
    a.emit({LW, a0, s0, 0, 0});
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(pid), 0);
}

TEST(lazy_heap_past_the_break_page_still_segfaults) {
    // Only pages up to the one holding the last heap byte are valid.
    System s;
    Asm a;
    a.sbrk(4096);                    // heap = exactly page 0x11; break = 0x12000
    a.li(t0, static_cast<int32_t>(kInitialBreak + 4096));
    a.emit({LW, a0, t0, 0, 0});
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a);
    s.run();
    const auto e = s.k.ending(pid);
    const auto* killed = e ? std::get_if<kernel::Killed>(&*e) : nullptr;
    CHECK(killed != nullptr);
    if (killed) {
        CHECK_EQ(killed->trap.cause, cpu::Cause::load_page_fault);
        CHECK_EQ(killed->trap.tval, kInitialBreak + 4096);
    }
}

// ---- system calls reading untouched pages ----

TEST(lazy_write_from_untouched_heap) {
    // The buffer is valid heap memory that was never touched: write maps it
    // (zeros) instead of returning -EFAULT.
    System s;
    Asm a;
    a.sbrk(4096);
    a.emit({ADDI, a1, a0, 0, 0});
    write4_from_a1_and_exit(a);
    const uint32_t pid = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(pid), 4);
    CHECK_EQ(s.out.str(), std::string(4, '\0'));
}

TEST(lazy_write_from_untouched_stack) {
    // 100 pages below the initial stack: valid, never touched.
    System s;
    Asm a;
    a.li(a1, static_cast<int32_t>(loader::kStackTop - 116 * 4096));
    write4_from_a1_and_exit(a);
    const uint32_t pid = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(pid), 4);
    CHECK_EQ(s.out.str(), std::string(4, '\0'));
}

TEST(lazy_write_past_the_break_is_still_efault) {
    System s;
    Asm a;
    a.sbrk(16);
    a.li(a1, static_cast<int32_t>(kInitialBreak + 4096));   // the page after the heap
    write4_from_a1_and_exit(a);
    const uint32_t pid = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(pid), -kEfault);
    CHECK_EQ(s.out.str(), std::string(""));
}

TEST(lazy_write_without_free_frame_is_efault) {
    // 15 processes x 17 frames = 255 of 256 frames: one is left. Process 1
    // touches one heap page (taking it), then writes from a second, untouched
    // heap page: no frame for it, so -EFAULT (and nothing printed, no crash).
    System s;
    Asm one;
    one.sbrk(2 * 4096);
    one.emit({ADDI, s0, a0, 0, 0});
    one.emit({SW, 0, s0, zero, 0});           // takes the last free frame
    one.li(t0, 4096);
    one.emit({ADD, a1, s0, t0, 0});           // the second, untouched page
    write4_from_a1_and_exit(one);
    const uint32_t pid = s.spawn(one);
    Asm idle;
    idle.exit_with(0);
    for (int i = 0; i < 14; i++) s.spawn(idle);
    CHECK_EQ(s.m.frame_allocator().free_count(), 1);
    s.run();
    CHECK_EQ(s.status(pid), -kEfault);
    CHECK_EQ(s.out.str(), std::string(""));
}
