// Page faults (spec B1): the stack grows on demand; every other fault is a
// segmentation fault that kills the process.
//
// Expected behavior, on a load or store page fault:
//   - The address is in the stack region [0x7ff00000, 0x80000000) and its page
//     is not mapped: map a zero-filled R+W page and run the same instruction
//     again (pc unchanged). The program never notices.
//   - Anything else kills the process with that trap: an address outside the
//     stack region, a page that is mapped but doesn't allow the access, or no
//     free frame left.
// Instruction page faults are never fixed: the stack is not executable.
#include <cstdint>
#include <sstream>
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

constexpr uint32_t kStackTop = loader::kStackTop;                                     // 0x80000000
constexpr uint32_t kMappedBottom = kStackTop - loader::kInitialStackPages * mmu::kPageSize;   // 0x7fff0000
constexpr uint32_t kRegionBottom = loader::kStackRegion;                              // 0x7ff00000

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

    cpu::Trap killed_by(uint32_t pid) {
        const auto e = k.ending(pid);
        const auto* killed = e ? std::get_if<kernel::Killed>(&*e) : nullptr;
        CHECK(killed != nullptr);
        return killed ? killed->trap : cpu::Trap{};
    }
};

// Stores `value` at `address`, loads it back, exits with it.
Asm store_then_load(uint32_t address, int32_t value) {
    Asm a;
    a.li(t0, static_cast<int32_t>(address));
    a.li(t1, value);
    a.emit({SW, 0, t0, t1, 0});
    a.emit({LW, a0, t0, 0, 0});
    a.exit_with_a0();
    return a;
}

// Loads from `address` and exits with the value.
Asm load_from(uint32_t address) {
    Asm a;
    a.li(t0, static_cast<int32_t>(address));
    a.emit({LW, a0, t0, 0, 0});
    a.exit_with_a0();
    return a;
}

}  // namespace

// ---- the stack grows on demand ----

TEST(stack_grows_just_below_initial_stack) {
    System s;
    const uint32_t pid = s.spawn(store_then_load(kMappedBottom - 4, 42));
    s.run();
    CHECK_EQ(s.status(pid), 42);
}

TEST(stack_grows_far_down) {
    // Touch every page from the initial stack down 100 pages: like deep recursion.
    System s;
    Asm a;
    a.li(t0, static_cast<int32_t>(kMappedBottom - 4));
    a.li(s1, 100);
    const size_t loop = a.here();
    a.emit({SW, 0, t0, s1, 0});
    a.li(t1, static_cast<int32_t>(mmu::kPageSize));
    a.emit({SUB, t0, t0, t1, 0});
    a.emit({ADDI, s1, s1, 0, -1});
    a.patch(a.branch(BNE, s1, zero), loop);
    a.li(t0, static_cast<int32_t>(kMappedBottom - 4));
    a.emit({LW, a0, t0, 0, 0});        // the first value stored: 100
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(pid), 100);
}

TEST(stack_grows_down_to_region_bottom) {
    // The lowest stack address is valid: the region starts at 0x7ff00000.
    System s;
    const uint32_t pid = s.spawn(store_then_load(kRegionBottom, 7));
    s.run();
    CHECK_EQ(s.status(pid), 7);
}

TEST(stack_new_page_is_zero_filled) {
    System s;
    for (uint32_t addr = 0; addr < mem::Ram::kRamSize; addr += 4) s.m.ram().write(addr, 4, 0xffffffff);
    const uint32_t pid = s.spawn(load_from(kMappedBottom - 100 * mmu::kPageSize));
    s.run();
    CHECK_EQ(s.status(pid), 0);
}

TEST(stack_fault_retries_the_same_instruction_once) {
    // The faulting store runs twice (fault, then retry); everything else once.
    // A retry that skipped the store, or ran it at the wrong pc, would change this.
    System s;
    const Asm a = store_then_load(kMappedBottom - 4, 5);
    const uint32_t pid = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(pid), 5);
    CHECK_EQ(s.m.steps(), a.code.size() + 1);
}

TEST(stack_growth_frames_are_freed_at_exit) {
    System s;
    const uint32_t before = s.m.frame_allocator().free_count();
    s.spawn(store_then_load(kMappedBottom - 50 * mmu::kPageSize, 1));
    s.run();
    CHECK_EQ(s.m.frame_allocator().free_count(), before);
}

TEST(stack_grows_independently_per_process) {
    System s;
    const uint32_t a = s.spawn(store_then_load(kMappedBottom - 4, 11));
    const uint32_t b = s.spawn(store_then_load(kMappedBottom - 4, 22));
    s.run();
    CHECK_EQ(s.status(a), 11);
    CHECK_EQ(s.status(b), 22);
}

// ---- everything else is a segmentation fault ----

TEST(segfault_below_stack_region) {
    System s;
    const uint32_t pid = s.spawn(load_from(kRegionBottom - 4));
    s.run();
    const cpu::Trap t = s.killed_by(pid);
    CHECK_EQ(t.cause, cpu::Cause::load_page_fault);
    CHECK_EQ(t.tval, kRegionBottom - 4);
}

TEST(segfault_null_pointer) {
    System s;
    const uint32_t pid = s.spawn(load_from(0));
    s.run();
    const cpu::Trap t = s.killed_by(pid);
    CHECK_EQ(t.cause, cpu::Cause::load_page_fault);
    CHECK_EQ(t.tval, 0);
}

TEST(segfault_above_user_space) {
    System s;
    const uint32_t pid = s.spawn(store_then_load(kStackTop, 1));   // 0x80000000: kernel-reserved
    s.run();
    const cpu::Trap t = s.killed_by(pid);
    CHECK_EQ(t.cause, cpu::Cause::store_page_fault);
    CHECK_EQ(t.tval, kStackTop);
}

TEST(segfault_store_to_code) {
    // The page is mapped, the access isn't allowed: not something to fix.
    System s;
    const uint32_t pid = s.spawn(store_then_load(0x10000, 1));
    s.run();
    const cpu::Trap t = s.killed_by(pid);
    CHECK_EQ(t.cause, cpu::Cause::store_page_fault);
    CHECK_EQ(t.tval, 0x10000);
}

TEST(segfault_executing_the_stack) {
    // Jumping into a stack address faults on fetch; the stack is never executable,
    // whether the page is already mapped or not.
    for (uint32_t target : {kStackTop - 16, kMappedBottom - 16}) {
        System s;
        Asm a;
        a.li(t0, static_cast<int32_t>(target));
        a.emit({JALR, zero, t0, 0, 0});
        const uint32_t pid = s.spawn(a);
        s.run();
        const cpu::Trap t = s.killed_by(pid);
        CHECK_EQ(t.cause, cpu::Cause::instruction_page_fault);
        CHECK_EQ(t.tval, target);
    }
}

TEST(segfault_when_out_of_memory) {
    // Grow the stack one page at a time until RAM runs out: the process is
    // killed at the page that can't be mapped, and all its frames come back.
    System s;
    const uint32_t before = s.m.frame_allocator().free_count();
    Asm a;
    a.li(t0, static_cast<int32_t>(kMappedBottom - 4));
    a.li(t1, static_cast<int32_t>(mmu::kPageSize));
    const size_t loop = a.here();
    a.emit({SW, 0, t0, zero, 0});
    a.emit({SUB, t0, t0, t1, 0});
    a.patch(a.branch(JAL, zero, zero), loop);   // "jal zero, loop": forever, until killed
    const uint32_t pid = s.spawn(a);
    s.run();
    const cpu::Trap t = s.killed_by(pid);
    CHECK_EQ(t.cause, cpu::Cause::store_page_fault);
    CHECK_EQ(s.m.frame_allocator().free_count(), before);
}

TEST(segfault_does_not_stop_other_processes) {
    System s;
    const uint32_t bad = s.spawn(load_from(0));
    const uint32_t good = s.spawn(store_then_load(kMappedBottom - 4, 9));
    s.run();
    CHECK_EQ(s.killed_by(bad).cause, cpu::Cause::load_page_fault);
    CHECK_EQ(s.status(good), 9);
}
