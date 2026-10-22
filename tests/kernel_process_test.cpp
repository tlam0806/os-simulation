// Process table: Kernel::spawn creates processes, each with its own address space.
#include <cstdint>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "elf_builder.hpp"
#include "kernel/kernel.hpp"
#include "loader/elf.hpp"
#include "machine/machine.hpp"
#include "test.hpp"

using namespace elf_builder;
using namespace isa::reg;
using enum isa::Op;

namespace {

// A machine and a kernel, ready to spawn processes.
struct System {
    machine::Machine m;
    std::ostringstream out, err;
    kernel::Kernel k{m, out, err};

    uint32_t spawn_ok(const std::vector<uint8_t>& elf) {
        const auto r = k.spawn(elf);
        CHECK(std::holds_alternative<uint32_t>(r));
        return std::holds_alternative<uint32_t>(r) ? std::get<uint32_t>(r) : 0;
    }

    // Reads a word of a process's memory through its own page table.
    uint32_t read(uint32_t pid, uint32_t vaddr) {
        const kernel::Process* p = k.process(pid);
        CHECK(p != nullptr);
        if (!p) return 0xdeadbeef;
        const auto pte = p->page_table.lookup(vaddr >> mmu::kPageShift);
        CHECK(pte.has_value());
        if (!pte) return 0xdeadbeef;
        return m.ram().read(pte->frame * mmu::kPageSize + (vaddr & (mmu::kPageSize - 1)), 4).value();
    }
};

// A program at 0x10000 whose first word is `marker` (an addi with that immediate).
std::vector<uint8_t> marked_program(int32_t marker) {
    return program_elf({{ADDI, a0, zero, 0, marker}, {ECALL}});
}

constexpr uint32_t kFramesPerProgram = 1 + loader::kInitialStackPages;   // 1 code page + stack

}  // namespace

TEST(process_table_starts_empty) {
    System s;
    CHECK_EQ(s.k.process_count(), 0);
    CHECK(s.k.process(1) == nullptr);
}

TEST(spawn_returns_pids_in_order) {
    System s;
    CHECK_EQ(s.spawn_ok(marked_program(1)), 1);
    CHECK_EQ(s.spawn_ok(marked_program(2)), 2);
    CHECK_EQ(s.spawn_ok(marked_program(3)), 3);
    CHECK_EQ(s.k.process_count(), 3);
}

TEST(spawn_sets_up_context) {
    System s;
    const uint32_t pid = s.spawn_ok(make_elf(0x10008, {{0x10000, code({{ECALL}, {ECALL}, {ECALL}})}}));
    const kernel::Process* p = s.k.process(pid);
    CHECK(p != nullptr);
    if (!p) return;
    CHECK_EQ(p->pid, pid);
    CHECK_EQ(p->context.pc, 0x10008);                 // the ELF entry point
    CHECK_EQ(p->context.regs[sp], loader::kStackTop);
    for (uint8_t r = 0; r < 32; r++)
        if (r != sp) CHECK_EQ(p->context.regs[r], 0);
}

TEST(spawn_sets_break) {
    System s;
    const uint32_t pid = s.spawn_ok(make_elf(0x10000, {
        {0x10000, code({{ECALL}})},
        {0x12000, {1}, /*memsz=*/0x345},   // ends at 0x12345
    }));
    const kernel::Process* p = s.k.process(pid);
    CHECK(p != nullptr);
    if (!p) return;
    CHECK_EQ(p->initial_break, 0x13000);
    CHECK_EQ(p->brk, 0x13000);
}

TEST(spawned_processes_have_separate_address_spaces) {
    // Same virtual address 0x10000 in both, but different frames and contents.
    System s;
    const uint32_t a = s.spawn_ok(marked_program(111));
    const uint32_t b = s.spawn_ok(marked_program(222));
    CHECK_EQ(s.read(a, 0x10000), isa::encode({ADDI, a0, zero, 0, 111}));
    CHECK_EQ(s.read(b, 0x10000), isa::encode({ADDI, a0, zero, 0, 222}));
    const kernel::Process* pa = s.k.process(a);
    const kernel::Process* pb = s.k.process(b);
    CHECK(pa != nullptr && pb != nullptr);
    if (!pa || !pb) return;
    const auto fa = pa->page_table.lookup(0x10);
    const auto fb = pb->page_table.lookup(0x10);
    CHECK(fa.has_value() && fb.has_value());
    if (fa && fb) CHECK(fa->frame != fb->frame);
}

TEST(spawn_uses_frames_from_the_machine) {
    System s;
    const uint32_t before = s.m.frame_allocator().free_count();
    s.spawn_ok(marked_program(1));
    s.spawn_ok(marked_program(2));
    CHECK_EQ(s.m.frame_allocator().free_count(), before - 2 * kFramesPerProgram);
}

TEST(spawn_does_not_run_anything) {
    System s;
    s.spawn_ok(marked_program(1));
    CHECK_EQ(s.m.steps(), 0);
}

TEST(spawn_bad_file_returns_error_and_uses_no_pid) {
    System s;
    const auto r = s.k.spawn(std::vector<uint8_t>{1, 2, 3});
    CHECK(std::holds_alternative<std::string>(r));
    if (const auto* msg = std::get_if<std::string>(&r)) CHECK(!msg->empty());
    CHECK_EQ(s.k.process_count(), 0);
    CHECK_EQ(s.spawn_ok(marked_program(1)), 1);   // the failed spawn didn't take pid 1
}

TEST(spawn_fails_when_ram_is_full) {
    // 256 frames / 17 per program = 15 programs fit; the 16th doesn't.
    System s;
    const uint32_t fit = mmu::kNumFrames / kFramesPerProgram;
    for (uint32_t i = 1; i <= fit; i++) CHECK_EQ(s.spawn_ok(marked_program(static_cast<int32_t>(i))), i);
    const uint32_t free_before = s.m.frame_allocator().free_count();
    const auto r = s.k.spawn(marked_program(99));
    CHECK(std::holds_alternative<std::string>(r));
    CHECK_EQ(s.k.process_count(), fit);
    CHECK_EQ(s.m.frame_allocator().free_count(), free_before);   // nothing leaked
    CHECK_EQ(s.read(fit, 0x10000), isa::encode({ADDI, a0, zero, 0, static_cast<int32_t>(fit)}));   // others intact
}

TEST(process_lookup_unknown_pid) {
    System s;
    s.spawn_ok(marked_program(1));
    CHECK(s.k.process(0) == nullptr);
    CHECK(s.k.process(2) == nullptr);
    CHECK(s.k.process(1) != nullptr);
}

TEST(process_pointer_stays_valid_after_more_spawns) {
    // Pointers from process() are used while more processes are created later.
    System s;
    const uint32_t first = s.spawn_ok(marked_program(1));
    const kernel::Process* p = s.k.process(first);
    for (int i = 0; i < 10; i++) s.spawn_ok(marked_program(2));
    CHECK(p != nullptr);
    CHECK(p == s.k.process(first));
    if (p) CHECK_EQ(p->pid, first);
}
