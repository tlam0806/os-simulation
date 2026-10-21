// Loads and stores: LB LH LW LBU LHU SB SH SW.
#include "cpu_helpers.hpp"
#include "test.hpp"

using namespace isa::reg;
using enum isa::Op;

namespace {

constexpr uint32_t kBase = 0x1000;   // data address used by most tests

// Runs one load "op a0, offset(a1)" with a1 = kBase; returns a0.
uint32_t load(isa::Op op, int32_t offset, Machine& m) {
    m.load(0, {{op, a0, a1, 0, offset}});
    m.cpu.set_reg(a1, kBase);
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.cpu.pc, 4);
    return m.cpu.reg(a0);
}

}  // namespace

TEST(sw_then_lw) {
    Machine m;
    m.load(0, {{SW, 0, a1, a2, 8}, {LW, a0, a1, 0, 8}});
    m.cpu.set_reg(a1, kBase);
    m.cpu.set_reg(a2, 0x12345678);
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.ram.read(kBase + 8, 4).value(), 0x12345678);
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.cpu.reg(a0), 0x12345678);
    CHECK_EQ(m.cpu.pc, 8);
}

TEST(load_negative_offset) {
    Machine m;
    m.ram.write(kBase - 8, 4, 0xcafef00d);
    CHECK_EQ(load(LW, -8, m), 0xcafef00d);
}

TEST(lb_sign_extends_lbu_does_not) {
    Machine m;
    m.ram.write(kBase, 1, 0x80);
    m.ram.write(kBase + 1, 1, 0x7f);
    CHECK_EQ(load(LB, 0, m), 0xffffff80);
    CHECK_EQ(load(LBU, 0, m), 0x80);
    CHECK_EQ(load(LB, 1, m), 0x7f);
}

TEST(lh_sign_extends_lhu_does_not) {
    Machine m;
    m.ram.write(kBase, 2, 0x8001);
    m.ram.write(kBase + 2, 2, 0x7001);
    CHECK_EQ(load(LH, 0, m), 0xffff8001);
    CHECK_EQ(load(LHU, 0, m), 0x8001);
    CHECK_EQ(load(LH, 2, m), 0x7001);
}

TEST(sb_sh_write_only_their_bytes) {
    Machine m;
    m.ram.write(kBase, 4, 0xaabbccdd);
    m.load(0, {{SB, 0, a1, a2, 1}, {SH, 0, a1, a2, 2}});
    m.cpu.set_reg(a1, kBase);
    m.cpu.set_reg(a2, 0x55667744);
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.ram.read(kBase, 4).value(), 0xaabb44dd);   // byte 1 = 0x44
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.ram.read(kBase, 4).value(), 0x774444dd);   // bytes 2..3 = 0x7744
}

TEST(lb_sb_any_address_is_aligned) {
    Machine m;
    m.load(0, {{SB, 0, a1, a2, 3}, {LBU, a0, a1, 0, 3}});
    m.cpu.set_reg(a1, kBase);
    m.cpu.set_reg(a2, 0x5a);
    CHECK(!m.cpu.step().has_value());
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.cpu.reg(a0), 0x5a);
}

TEST(misaligned_load_traps_without_side_effects) {
    for (auto [op, offset] : {std::pair{LW, 2}, {LW, 1}, {LH, 1}, {LHU, 3}}) {
        Machine m;
        m.load(0, {{op, a0, a1, 0, offset}});
        m.cpu.set_reg(a1, kBase);
        m.cpu.set_reg(a0, 0x99);
        const auto trap = m.cpu.step();
        CHECK(trap.has_value());
        CHECK_EQ(trap->cause, cpu::Cause::load_address_misaligned);
        CHECK_EQ(trap->pc, 0);
        CHECK_EQ(trap->tval, kBase + offset);   // tval = the bad address
        CHECK_EQ(m.cpu.reg(a0), 0x99);          // rd not written
        CHECK_EQ(m.cpu.pc, 0);
    }
}

TEST(misaligned_store_traps_without_side_effects) {
    for (auto [op, offset] : {std::pair{SW, 2}, {SW, 3}, {SH, 1}}) {
        Machine m;
        m.load(0, {{op, 0, a1, a2, offset}});
        m.cpu.set_reg(a1, kBase);
        m.cpu.set_reg(a2, 0xffffffff);
        const auto trap = m.cpu.step();
        CHECK(trap.has_value());
        CHECK_EQ(trap->cause, cpu::Cause::store_address_misaligned);
        CHECK_EQ(trap->tval, kBase + offset);
        CHECK_EQ(m.ram.read(kBase, 4).value(), 0);   // memory unchanged
        CHECK_EQ(m.cpu.pc, 0);
    }
}

TEST(load_outside_ram_is_page_fault) {
    for (uint32_t addr : {static_cast<uint32_t>(mem::Ram::kRamSize), 0xfffffffcu}) {
        Machine m;
        m.load(0, {{LW, a0, a1, 0, 0}});
        m.cpu.set_reg(a1, addr);
        m.cpu.set_reg(a0, 0x99);
        const auto trap = m.cpu.step();
        CHECK(trap.has_value());
        CHECK_EQ(trap->cause, cpu::Cause::load_page_fault);
        CHECK_EQ(trap->tval, addr);
        CHECK_EQ(m.cpu.reg(a0), 0x99);
        CHECK_EQ(m.cpu.pc, 0);
    }
}

TEST(store_outside_ram_is_page_fault) {
    Machine m;
    m.load(0, {{SW, 0, a1, a2, 0}});
    m.cpu.set_reg(a1, mem::Ram::kRamSize);
    const auto trap = m.cpu.step();
    CHECK(trap.has_value());
    CHECK_EQ(trap->cause, cpu::Cause::store_page_fault);
    CHECK_EQ(trap->tval, mem::Ram::kRamSize);
    CHECK_EQ(m.cpu.pc, 0);
}

TEST(address_wraps_around_32_bits) {
    // 0 + (-4) = 0xfffffffc, which is outside RAM.
    Machine m;
    m.load(0, {{LW, a0, zero, 0, -4}});
    const auto trap = m.cpu.step();
    CHECK(trap.has_value());
    CHECK_EQ(trap->cause, cpu::Cause::load_page_fault);
    CHECK_EQ(trap->tval, 0xfffffffc);
}

TEST(load_into_x0_is_discarded) {
    Machine m;
    m.ram.write(kBase, 4, 0x12345678);
    m.load(0, {{LW, zero, a1, 0, 0}});
    m.cpu.set_reg(a1, kBase);
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.cpu.reg(zero), 0);
}

TEST(load_rd_same_as_base) {
    Machine m;
    m.ram.write(kBase, 4, 0x12345678);
    m.load(0, {{LW, a1, a1, 0, 0}});
    m.cpu.set_reg(a1, kBase);
    CHECK(!m.cpu.step().has_value());
    CHECK_EQ(m.cpu.reg(a1), 0x12345678);
}
