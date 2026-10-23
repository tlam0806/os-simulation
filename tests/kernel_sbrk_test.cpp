// The sbrk system call (spec B1, B3): a7 = 3, a0 = increment in bytes.
// Returns the old break (where new memory starts), or -ENOMEM / -EINVAL.
//
// Expected behavior:
//   - The break starts at the loader's initial_break (the page after the program).
//   - Growing only moves the break. Each heap page (from initial_break up to the
//     page holding the last heap byte) is mapped the first time the program
//     touches it: read+write, not executable, zero-filled. See also
//     kernel_lazy_heap_test.cpp.
//   - Shrinking unmaps the pages the heap no longer covers and frees their frames.
//   - The break can't go below initial_break (-EINVAL) or past the stack
//     region at 0x7ff00000 (-ENOMEM). On those errors nothing changes.
//   - Otherwise sbrk always succeeds, even beyond RAM (overcommit): running out
//     of frames shows up later, as a page fault that kills the process.
#include <cstdint>
#include <variant>
#include <vector>

#include "asm.hpp"
#include "cpu_helpers.hpp"
#include "kernel_helpers.hpp"
#include "elf_builder.hpp"
#include "kernel/kernel.hpp"
#include "machine/machine.hpp"
#include <sstream>
#include "test.hpp"

using namespace isa::reg;
using enum isa::Op;

namespace {

constexpr int32_t kEnomem = 12, kEinval = 22;   // not ENOMEM/EINVAL: those are macros
constexpr uint32_t kInitialBreak = 0x11000;     // code at 0x10000 is under one page

Result run(machine::Machine& m, const Asm& a) {
    std::ostringstream out, err;
    return run_process(m, elf_builder::make_elf(0x10000, {{0x10000, elf_builder::code(a.code)}}), 100000, out, err);
}

// The program checks a register itself: if reg != value, it exits with status 1.
// (After a process ends, its registers are gone, so the test can't look at them.)
void expect_reg(Asm& a, uint8_t reg, int32_t value) {
    a.li(t2, value);
    const size_t ok = a.branch(BEQ, reg, t2);
    a.exit_with(1);
    a.patch(ok, a.here());
}

int32_t exit_status(const Result& r) {
    const auto* e = std::get_if<kernel::Exited>(&r);
    CHECK(e != nullptr);
    return e ? e->exit_status : -12345;
}

cpu::Trap killed_by(const Result& r) {
    const auto* k = std::get_if<kernel::Killed>(&r);
    CHECK(k != nullptr);
    return k ? k->trap : cpu::Trap{};
}

}  // namespace

// ---- the break ----

TEST(sbrk_zero_returns_initial_break) {
    machine::Machine m;
    Asm a;
    a.sbrk(0);
    a.exit_with_a0();
    CHECK_EQ(exit_status(run(m, a)), kInitialBreak);
}

TEST(sbrk_returns_old_break_and_moves_it) {
    machine::Machine m;
    Asm a;
    a.sbrk(100);
    a.emit({ADDI, s0, a0, 0, 0});    // s0 = old break
    a.sbrk(0);                       // a0 = new break
    a.emit({SUB, a0, a0, s0, 0});    // how far it moved
    a.exit_with_a0();
    CHECK_EQ(exit_status(run(m, a)), 100);
}

TEST(sbrk_first_call_returns_initial_break) {
    machine::Machine m;
    Asm a;
    a.sbrk(100);
    a.exit_with_a0();
    CHECK_EQ(exit_status(run(m, a)), kInitialBreak);
}

// ---- the new memory ----

TEST(sbrk_memory_is_usable) {
    // Grow by two pages; write the first and last word, read them back.
    machine::Machine m;
    Asm a;
    a.sbrk(8192);
    a.emit({ADDI, s0, a0, 0, 0});
    a.li(t0, 42);
    a.emit({SW, 0, s0, t0, 0});
    a.li(t1, 8188);
    a.emit({ADD, t1, s0, t1, 0});
    a.li(t0, 7);
    a.emit({SW, 0, t1, t0, 0});
    a.emit({LW, a0, s0, 0, 0});
    a.emit({LW, t2, t1, 0, 0});
    a.emit({ADD, a0, a0, t2, 0});
    a.exit_with_a0();
    CHECK_EQ(exit_status(run(m, a)), 49);
}

TEST(sbrk_memory_is_zero_filled) {
    // Fill RAM with garbage first: frames handed out later must be cleared.
    machine::Machine m;
    for (uint32_t addr = 0; addr < mem::Ram::kRamSize; addr += 4) m.ram().write(addr, 4, 0xffffffff);
    Asm a;
    a.sbrk(4096);
    a.li(t0, 4092);                  // offsets above 2047 don't fit in lw
    a.emit({ADD, t0, a0, t0, 0});
    a.emit({LW, a0, t0, 0, 0});      // last word of the new page
    a.exit_with_a0();
    CHECK_EQ(exit_status(run(m, a)), 0);
}

TEST(sbrk_memory_past_the_break_is_not_mapped) {
    // Growing by 16 bytes maps the whole page, but not the next one.
    machine::Machine m;
    Asm a;
    a.sbrk(16);
    a.emit({LW, t0, a0, 0, 2044});   // same page: fine
    a.li(t1, 4096);
    a.emit({ADD, t1, a0, t1, 0});
    a.emit({LW, t0, t1, 0, 0});      // next page: page fault
    a.exit_with_a0();
    const cpu::Trap t = killed_by(run(m, a));
    CHECK_EQ(t.cause, cpu::Cause::load_page_fault);
    CHECK_EQ(t.tval, kInitialBreak + 4096);
}

TEST(sbrk_memory_is_not_executable) {
    // Heap is data: jumping into it must fault, even though it is mapped.
    machine::Machine m;
    Asm a;
    a.sbrk(4096);
    a.emit({JALR, zero, a0, 0, 0});
    const cpu::Trap t = killed_by(run(m, a));
    CHECK_EQ(t.cause, cpu::Cause::instruction_page_fault);
    CHECK_EQ(t.tval, kInitialBreak);
}

// ---- shrinking ----

TEST(sbrk_shrink_returns_old_break_and_unmaps) {
    machine::Machine m;
    Asm a;
    a.sbrk(8192);
    a.emit({ADDI, s0, a0, 0, 0});    // s0 = start of the heap
    a.emit({SW, 0, s0, zero, 0});    // touch both pages, so they get mapped
    a.li(t0, 4096);
    a.emit({ADD, t0, s0, t0, 0});
    a.emit({SW, 0, t0, zero, 0});
    a.sbrk(-8192);                   // a0 = old break = s0 + 8192
    a.emit({SUB, s1, a0, s0, 0});    // s1 = 8192
    expect_reg(a, s1, 8192);
    a.emit({LW, t0, s0, 0, 0});      // heap is gone: page fault
    a.exit_with_a0();
    const cpu::Trap t = killed_by(run(m, a));
    CHECK_EQ(t.cause, cpu::Cause::load_page_fault);
    CHECK_EQ(t.tval, kInitialBreak);
}

TEST(sbrk_partial_shrink_keeps_page_still_in_use) {
    // Break 0x11000 -> +6000 = 0x12770 (pages 0x11, 0x12) -> -1000 = 0x12388.
    // Page 0x12 still holds heap bytes, so it stays mapped.
    machine::Machine m;
    Asm a;
    a.sbrk(6000);
    a.sbrk(-1000);
    a.li(t0, 0x12380);
    a.li(t1, 5);
    a.emit({SW, 0, t0, t1, 0});
    a.emit({LW, a0, t0, 0, 0});
    a.exit_with_a0();
    CHECK_EQ(exit_status(run(m, a)), 5);
}

TEST(sbrk_shrink_frees_frames) {
    // Grow by 64 KiB, touch every page, shrink again: 100 times. That's 1600
    // pages in total, but RAM has only 256 frames: this only works if
    // shrinking gives frames back.
    machine::Machine m;
    Asm a;
    a.li(s1, 100);
    const size_t loop = a.here();
    a.sbrk(65536);
    const size_t fail1 = a.branch(BLT, a0, zero);
    a.emit({ADDI, t0, a0, 0, 0});    // touch all 16 new pages
    a.li(t1, 4096);
    a.li(t3, 16);
    const size_t touch = a.here();
    a.emit({SW, 0, t0, zero, 0});
    a.emit({ADD, t0, t0, t1, 0});
    a.emit({ADDI, t3, t3, 0, -1});
    a.patch(a.branch(BNE, t3, zero), touch);
    a.sbrk(-65536);
    const size_t fail2 = a.branch(BLT, a0, zero);
    a.emit({ADDI, s1, s1, 0, -1});
    a.patch(a.branch(BNE, s1, zero), loop);
    a.li(a0, 0);
    a.exit_with_a0();
    const size_t fail = a.here();
    a.patch(fail1, fail);
    a.patch(fail2, fail);
    a.exit_with_a0();   // exits with the negative error code
    CHECK_EQ(exit_status(run(m, a)), 0);
}

// ---- errors ----

TEST(sbrk_more_than_ram_succeeds) {
    // 112 MiB: far more than 1 MiB of RAM, but nothing is mapped yet, so it works.
    machine::Machine m;
    Asm a;
    a.sbrk(0x7000000);
    a.emit({ADDI, s0, a0, 0, 0});
    expect_reg(a, s0, kInitialBreak);           // the old break: success
    a.sbrk(0);
    a.emit({ADDI, s1, a0, 0, 0});
    expect_reg(a, s1, kInitialBreak + 0x7000000);
    a.li(t1, 5);
    a.emit({SW, 0, s0, t1, 0});                 // the first page works when touched
    a.emit({LW, a0, s0, 0, 0});
    a.exit_with_a0();
    CHECK_EQ(exit_status(run(m, a)), 5);
}

TEST(sbrk_touching_more_than_ram_kills) {
    // The heap is bigger than RAM; touching page after page eventually finds no
    // free frame. That access is a page fault the kernel can't fix: the process
    // is killed, and every frame it had comes back.
    machine::Machine m;
    Asm a;
    a.sbrk(0x100000);                // 256 pages
    a.emit({ADDI, t0, a0, 0, 0});
    a.li(t1, 4096);
    const size_t loop = a.here();
    a.emit({SW, 0, t0, zero, 0});
    a.emit({ADD, t0, t0, t1, 0});
    a.patch(a.branch(JAL, zero, zero), loop);   // until killed
    const cpu::Trap t = killed_by(run(m, a));
    CHECK_EQ(t.cause, cpu::Cause::store_page_fault);
    CHECK(t.tval >= kInitialBreak && t.tval < kInitialBreak + 0x100000);   // inside the heap
    CHECK_EQ(m.frame_allocator().free_count(), mmu::kNumFrames);
}

TEST(sbrk_into_stack_region_returns_enomem) {
    // 0x7ff00000 - 0x11000 + 1: one byte too many, even if RAM were huge.
    machine::Machine m;
    Asm a;
    a.sbrk(0x7ff00000 - static_cast<int32_t>(kInitialBreak) + 1);
    a.exit_with_a0();
    CHECK_EQ(exit_status(run(m, a)), -kEnomem);
}

TEST(sbrk_below_initial_break_returns_einval) {
    machine::Machine m;
    Asm a;
    a.sbrk(100);
    a.sbrk(-200);                    // would end 100 bytes below the initial break
    a.emit({ADDI, s0, a0, 0, 0});
    a.sbrk(0);
    a.emit({ADDI, s1, a0, 0, 0});    // unchanged: initial break + 100
    expect_reg(a, s1, kInitialBreak + 100);
    a.emit({ADDI, a0, s0, 0, 0});
    a.exit_with_a0();
    CHECK_EQ(exit_status(run(m, a)), -kEinval);
}

TEST(sbrk_reserved_heap_uses_no_frames) {
    // Process 1 reserves 200 pages and never touches them; process 2 then uses
    // 30 pages. With eager mapping, process 1 would hold 200 of the 222 free
    // frames and process 2's sbrk would fail. With lazy mapping both are fine.
    machine::Machine m;
    std::ostringstream out, err;
    kernel::Kernel k(m, out, err);
    Asm one;
    one.sbrk(200 * 4096);
    one.syscall(SYS_YIELD);          // let process 2 run while the heap is reserved
    one.exit_with_a0();              // a0 = 0 from yield
    Asm two;
    two.sbrk(30 * 4096);
    expect_reg(two, a0, kInitialBreak);
    two.emit({ADDI, t0, a0, 0, 0});
    two.li(t1, 4096);
    two.li(t3, 30);
    const size_t loop = two.here();
    two.emit({SW, 0, t0, zero, 0});
    two.emit({ADD, t0, t0, t1, 0});
    two.emit({ADDI, t3, t3, 0, -1});
    two.patch(two.branch(BNE, t3, zero), loop);
    two.exit_with(0);
    for (const Asm* a : {&one, &two})
        CHECK(std::holds_alternative<uint32_t>(k.spawn(elf_builder::make_elf(0x10000, {{0x10000, elf_builder::code(a->code)}}))));
    CHECK(std::holds_alternative<kernel::AllDone>(k.run_all(1'000'000)));
    for (uint32_t pid : {1u, 2u}) {
        const auto e = k.ending(pid);
        const auto* exited = e ? std::get_if<kernel::Exited>(&e->reason) : nullptr;
        CHECK(exited != nullptr);
        if (exited) CHECK_EQ(exited->exit_status, 0);
    }
}

TEST(sbrk_whole_new_page_is_zero_filled) {
    // The program can read the whole page, not just up to the break, so the
    // part past the break must be zero too (spec B1: new pages are zero-filled).
    machine::Machine m;
    for (uint32_t addr = 0; addr < mem::Ram::kRamSize; addr += 4) m.ram().write(addr, 4, 0xffffffff);
    Asm a;
    a.sbrk(16);
    a.emit({LW, a0, a0, 0, 2044});   // past the break, same page
    a.exit_with_a0();
    CHECK_EQ(exit_status(run(m, a)), 0);
}
