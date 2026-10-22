// The CPU translates every fetch, load and store through its page table (spec A2, A3).
// In these tests virtual and physical addresses are deliberately different, so a
// test fails if an access skips translation.
#include <cstdint>

#include "cpu_helpers.hpp"
#include "mmu/page_table.hpp"
#include "test.hpp"

using namespace isa::reg;
using enum isa::Op;
using mmu::Pte;

namespace {

constexpr Pte RX(uint32_t frame) { return {frame, true, false, true}; }
constexpr Pte RW(uint32_t frame) { return {frame, true, true, false}; }
constexpr Pte R(uint32_t frame) { return {frame, true, false, false}; }
constexpr Pte X(uint32_t frame) { return {frame, false, false, true}; }

// Virtual layout used by most tests:
//   code: virtual page 0x10 (0x10000) -> frame 5 (physical 0x5000), R+X
//   data: virtual page 0x20 (0x20000) -> frame 6 (physical 0x6000), varies per test
constexpr uint32_t kCodeVirt = 0x10000, kCodePhys = 0x5000;
constexpr uint32_t kDataVirt = 0x20000, kDataPhys = 0x6000;

// Places one instruction at the start of the code page and points pc at it.
void put_code(Machine& m, mmu::PageTable& t, isa::Instruction inst) {
    t.map(kCodeVirt >> mmu::kPageShift, RX(kCodePhys >> mmu::kPageShift));
    m.ram.write(kCodePhys, 4, isa::encode(inst));
    m.cpu.pc = kCodeVirt;
    m.cpu.set_page_table(&t);
}

cpu::Trap expect_trap(Machine& m) {
    const auto trap = m.cpu.step();
    CHECK(trap.has_value());
    return trap.value_or(cpu::Trap{});
}

}  // namespace

// ---- fetch ----

TEST(mmu_fetch_is_translated) {
    Machine m;
    mmu::PageTable t;
    put_code(m, t, {ADDI, a0, zero, 0, 7});
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.cpu.reg(a0), 7);
    CHECK_EQ(m.cpu.pc, kCodeVirt + 4);   // pc stays virtual
}

TEST(mmu_fetch_from_unmapped_page_faults) {
    Machine m;
    mmu::PageTable t;
    m.cpu.set_page_table(&t);
    m.cpu.pc = kCodeVirt;
    const cpu::Trap trap = expect_trap(m);
    CHECK_EQ(trap.cause, cpu::Cause::instruction_page_fault);
    CHECK_EQ(trap.pc, kCodeVirt);
    CHECK_EQ(trap.tval, kCodeVirt);   // tval is the virtual address
    CHECK_EQ(m.cpu.pc, kCodeVirt);
}

TEST(mmu_fetch_from_non_executable_page_faults) {
    Machine m;
    mmu::PageTable t;
    t.map(kCodeVirt >> mmu::kPageShift, RW(kCodePhys >> mmu::kPageShift));   // data, not code
    m.ram.write(kCodePhys, 4, isa::encode({ADDI, a0, zero, 0, 7}));
    m.cpu.set_page_table(&t);
    m.cpu.pc = kCodeVirt;
    const cpu::Trap trap = expect_trap(m);
    CHECK_EQ(trap.cause, cpu::Cause::instruction_page_fault);
    CHECK_EQ(m.cpu.reg(a0), 0);
}

TEST(mmu_code_continues_across_pages_in_different_frames) {
    // Virtual pages 0x10 and 0x11 are next to each other; their frames (5 and 9) are not.
    Machine m;
    mmu::PageTable t;
    t.map(0x10, RX(5));
    t.map(0x11, RX(9));
    m.ram.write(0x5ffc, 4, isa::encode({ADDI, a0, zero, 0, 1}));   // virtual 0x10ffc
    m.ram.write(0x9000, 4, isa::encode({ADDI, a0, a0, 0, 1}));     // virtual 0x11000
    m.cpu.set_page_table(&t);
    m.cpu.pc = 0x10ffc;
    CHECK(!m.cpu.step().has_value());
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.cpu.reg(a0), 2);
    CHECK_EQ(m.cpu.pc, 0x11004);
}

// ---- load ----

TEST(mmu_load_is_translated) {
    Machine m;
    mmu::PageTable t;
    put_code(m, t, {LW, a0, a1, 0, 0x10});
    t.map(kDataVirt >> mmu::kPageShift, R(kDataPhys >> mmu::kPageShift));
    m.ram.write(kDataPhys + 0x10, 4, 0xcafef00d);
    m.ram.write(kDataVirt + 0x10, 4, 0xbad);   // what an untranslated load would see
    m.cpu.set_reg(a1, kDataVirt);
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.cpu.reg(a0), 0xcafef00d);
}

TEST(mmu_load_from_unmapped_page_faults) {
    Machine m;
    mmu::PageTable t;
    put_code(m, t, {LW, a0, a1, 0, 8});
    m.cpu.set_reg(a1, kDataVirt);
    m.cpu.set_reg(a0, 0x99);
    const cpu::Trap trap = expect_trap(m);
    CHECK_EQ(trap.cause, cpu::Cause::load_page_fault);
    CHECK_EQ(trap.pc, kCodeVirt);
    CHECK_EQ(trap.tval, kDataVirt + 8);
    CHECK_EQ(m.cpu.reg(a0), 0x99);   // rd not written
    CHECK_EQ(m.cpu.pc, kCodeVirt);
}

TEST(mmu_load_from_execute_only_page_faults) {
    Machine m;
    mmu::PageTable t;
    put_code(m, t, {LW, a0, a1, 0, 0});
    t.map(kDataVirt >> mmu::kPageShift, X(kDataPhys >> mmu::kPageShift));
    m.cpu.set_reg(a1, kDataVirt);
    CHECK_EQ(expect_trap(m).cause, cpu::Cause::load_page_fault);
}

TEST(mmu_byte_and_half_loads_are_translated) {
    Machine m;
    mmu::PageTable t;
    put_code(m, t, {LBU, a0, a1, 0, 3});
    t.map(kDataVirt >> mmu::kPageShift, R(kDataPhys >> mmu::kPageShift));
    m.ram.write(kDataPhys, 4, 0x80112233);
    m.cpu.set_reg(a1, kDataVirt);
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.cpu.reg(a0), 0x80);
}

// ---- store ----

TEST(mmu_store_is_translated) {
    Machine m;
    mmu::PageTable t;
    put_code(m, t, {SW, 0, a1, a2, 4});
    t.map(kDataVirt >> mmu::kPageShift, RW(kDataPhys >> mmu::kPageShift));
    m.cpu.set_reg(a1, kDataVirt);
    m.cpu.set_reg(a2, 0x12345678);
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.ram.read(kDataPhys + 4, 4).value(), 0x12345678);
    CHECK_EQ(m.ram.read(kDataVirt + 4, 4).value(), 0);   // the virtual address itself untouched
}

TEST(mmu_store_to_read_only_page_faults) {
    Machine m;
    mmu::PageTable t;
    put_code(m, t, {SW, 0, a1, a2, 0});
    t.map(kDataVirt >> mmu::kPageShift, R(kDataPhys >> mmu::kPageShift));
    m.cpu.set_reg(a1, kDataVirt);
    m.cpu.set_reg(a2, 0xffffffff);
    const cpu::Trap trap = expect_trap(m);
    CHECK_EQ(trap.cause, cpu::Cause::store_page_fault);
    CHECK_EQ(trap.tval, kDataVirt);
    CHECK_EQ(m.ram.read(kDataPhys, 4).value(), 0);   // memory unchanged
}

TEST(mmu_store_to_code_page_faults) {
    // Code is R+X: a program can't overwrite its own instructions.
    Machine m;
    mmu::PageTable t;
    put_code(m, t, {SW, 0, a1, a2, 0});
    m.cpu.set_reg(a1, kCodeVirt);
    m.cpu.set_reg(a2, 0);
    CHECK_EQ(expect_trap(m).cause, cpu::Cause::store_page_fault);
    CHECK_EQ(m.ram.read(kCodePhys, 4).value(), isa::encode({SW, 0, a1, a2, 0}));
}

TEST(mmu_store_to_unmapped_page_faults) {
    Machine m;
    mmu::PageTable t;
    put_code(m, t, {SH, 0, a1, a2, 2});
    m.cpu.set_reg(a1, kDataVirt);
    const cpu::Trap trap = expect_trap(m);
    CHECK_EQ(trap.cause, cpu::Cause::store_page_fault);
    CHECK_EQ(trap.tval, kDataVirt + 2);
}

// ---- order of checks, and edge cases ----

TEST(mmu_misaligned_is_reported_before_page_fault) {
    // RISC-V checks alignment first: a misaligned access to an unmapped page is
    // "misaligned", not "page fault".
    Machine m;
    mmu::PageTable t;
    put_code(m, t, {LW, a0, a1, 0, 2});
    m.cpu.set_reg(a1, kDataVirt);
    CHECK_EQ(expect_trap(m).cause, cpu::Cause::load_address_misaligned);
}

TEST(mmu_frame_outside_ram_faults) {
    // A mapping to a frame that doesn't exist (RAM has 256 frames) can't be used.
    Machine m;
    mmu::PageTable t;
    put_code(m, t, {LW, a0, a1, 0, 0});
    t.map(kDataVirt >> mmu::kPageShift, R(300));
    m.cpu.set_reg(a1, kDataVirt);
    const cpu::Trap trap = expect_trap(m);
    CHECK_EQ(trap.cause, cpu::Cause::load_page_fault);
    CHECK_EQ(trap.tval, kDataVirt);
}

TEST(mmu_switching_tables_switches_address_space) {
    // The same virtual address reads different data in two address spaces,
    // like two processes after a context switch.
    Machine m;
    mmu::PageTable a, b;
    for (mmu::PageTable* t : {&a, &b}) t->map(kCodeVirt >> mmu::kPageShift, RX(5));
    a.map(kDataVirt >> mmu::kPageShift, R(6));
    b.map(kDataVirt >> mmu::kPageShift, R(7));
    m.ram.write(0x5000, 4, isa::encode({LW, a0, a1, 0, 0}));
    m.ram.write(0x6000, 4, 111);
    m.ram.write(0x7000, 4, 222);
    m.cpu.set_reg(a1, kDataVirt);

    m.cpu.set_page_table(&a);
    m.cpu.pc = kCodeVirt;
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.cpu.reg(a0), 111);

    m.cpu.set_page_table(&b);
    m.cpu.pc = kCodeVirt;
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.cpu.reg(a0), 222);
}

TEST(mmu_null_table_means_physical_addresses) {
    // Turning translation off again restores milestone 1-2 behavior.
    Machine m;
    mmu::PageTable t;
    m.cpu.set_page_table(&t);
    m.cpu.set_page_table(nullptr);
    m.load(0x5000, {{ADDI, a0, zero, 0, 9}});
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.cpu.reg(a0), 9);
}
