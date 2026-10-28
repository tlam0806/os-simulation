// The disk inside the machine (spec A3, A4, A6), disk step 3:
//   - run() completes a request once the clock reaches its completion time,
//     then raises the disk interrupt (cause 0x80000009) until acknowledged
//   - disk interrupt before timer interrupt
//   - wait_for_interrupt(): idle, the clock jumps to the next interrupt
#include <cstdint>
#include <optional>
#include <vector>

#include "cpu_helpers.hpp"   // prints Cause in failure messages
#include "disk/disk.hpp"
#include "elf_builder.hpp"
#include "machine/machine.hpp"
#include "test.hpp"

using namespace elf_builder;
using namespace isa::reg;
using enum isa::Op;
using disk::Op;

namespace {

constexpr uint32_t kEntry = 0x10000;
constexpr uint32_t kBuffer = 0xF0000;   // a RAM page the loader doesn't use

// a0 counts up forever: "loop: addi a0, a0, 1; jal zero, loop".
std::vector<uint8_t> counter_program() {
    return program_elf({{ADDI, a0, a0, 0, 1}, {JAL, zero, 0, 0, -4}});
}

bool is_cause(const std::optional<cpu::Trap>& t, cpu::Cause c) {
    return t.has_value() && t->cause == c;
}
bool is_disk(const std::optional<cpu::Trap>& t) { return is_cause(t, cpu::Cause::disk_interrupt); }
bool is_timer(const std::optional<cpu::Trap>& t) { return is_cause(t, cpu::Cause::timer_interrupt); }

// A machine running the counter, with a one-sector read of sector 100 into
// kBuffer submitted at clock 0: it completes at 808 (spec A6 timing).
struct Running {
    machine::Machine m;
    Running() {
        CHECK(!m.load(counter_program()));
        CHECK(m.disk().host_write(100, std::vector<uint8_t>(512, 0x77)));
        CHECK(m.disk().submit({Op::read, 100, 1, kBuffer}, m.steps()));
        CHECK_EQ(m.disk().completion_time().value_or(0), 808u);
    }
};

}  // namespace

// =================== run(): completion and the disk interrupt ===================

TEST(machine_disk_no_interrupt_before_completion) {
    Running r;
    CHECK(!r.m.run(500).has_value());          // only the step limit
    CHECK(r.m.disk().busy());
    CHECK_EQ(r.m.ram().read(kBuffer, 1).value_or(999), 0u);   // no data yet
}

TEST(machine_disk_interrupt_at_completion_time) {
    Running r;
    const auto t = r.m.run(100000);
    CHECK(is_disk(t));
    CHECK_EQ(r.m.steps(), 808u);               // exactly 808 instructions ran
    CHECK_EQ(r.m.cpu().reg(a0), 404u);         // half of them were the addi
    CHECK_EQ(t->pc, kEntry);                   // the instruction that did not run
    CHECK_EQ(t->tval, 0u);
}

TEST(machine_disk_request_is_completed_by_run) {
    Running r;
    CHECK(is_disk(r.m.run(100000)));
    CHECK(!r.m.disk().busy());
    CHECK_EQ(r.m.ram().read(kBuffer, 1).value_or(999), 0x77u);
    CHECK_EQ(r.m.ram().read(kBuffer + 511, 1).value_or(999), 0x77u);
}

TEST(machine_disk_completion_across_run_calls) {
    // The step limit splits the run; the interrupt still comes at 808.
    Running r;
    CHECK(!r.m.run(800).has_value());
    CHECK(is_disk(r.m.run(100)));
    CHECK_EQ(r.m.steps(), 808u);
}

TEST(machine_disk_interrupt_keeps_firing_until_acknowledged) {
    Running r;
    CHECK(is_disk(r.m.run(100000)));
    CHECK(is_disk(r.m.run(100000)));           // still pending, no instruction ran
    CHECK_EQ(r.m.steps(), 808u);
    r.m.acknowledge_disk_interrupt();
    CHECK(!r.m.run(100).has_value());          // gone: the program runs on
    CHECK_EQ(r.m.steps(), 908u);
}

TEST(machine_disk_interrupt_comes_before_timer) {
    Running r;
    r.m.set_timer(808);                        // both due at 808
    CHECK(is_disk(r.m.run(100000)));
    r.m.acknowledge_disk_interrupt();
    CHECK(is_timer(r.m.run(100000)));
    CHECK_EQ(r.m.steps(), 808u);
}

TEST(machine_disk_timer_first_when_it_is_earlier) {
    Running r;
    r.m.set_timer(300);
    CHECK(is_timer(r.m.run(100000)));
    CHECK_EQ(r.m.steps(), 300u);
    r.m.clear_timer();
    CHECK(is_disk(r.m.run(100000)));
    CHECK_EQ(r.m.steps(), 808u);
}

TEST(machine_disk_idle_disk_never_interrupts) {
    machine::Machine m;
    CHECK(!m.load(counter_program()));
    CHECK(!m.run(20000).has_value());
}

TEST(machine_disk_next_request_after_acknowledge) {
    // Second request submitted at 808 on the same track: sector 101 is passing,
    // sector 200 starts later in the same turn, at 1600; done at 1608.
    Running r;
    CHECK(is_disk(r.m.run(100000)));
    r.m.acknowledge_disk_interrupt();
    CHECK(r.m.disk().submit({Op::read, 200, 1, kBuffer}, r.m.steps()));
    CHECK(is_disk(r.m.run(100000)));
    CHECK_EQ(r.m.steps(), 1608u);
}

// =================== wait_for_interrupt(): idle ===================

TEST(machine_disk_wait_jumps_to_disk_completion) {
    Running r;
    const auto t = r.m.wait_for_interrupt();
    CHECK(is_disk(t));
    CHECK_EQ(r.m.steps(), 808u);               // the clock moved...
    CHECK_EQ(r.m.cpu().reg(a0), 0u);           // ...but no instruction ran
    CHECK_EQ(r.m.cpu().pc, kEntry);
    CHECK(!r.m.disk().busy());                 // and the request completed
    CHECK_EQ(r.m.ram().read(kBuffer, 1).value_or(999), 0x77u);
}

TEST(machine_disk_wait_jumps_to_earlier_timer) {
    Running r;
    r.m.set_timer(300);
    CHECK(is_timer(r.m.wait_for_interrupt()));
    CHECK_EQ(r.m.steps(), 300u);
    CHECK(r.m.disk().busy());                  // the disk isn't done yet
}

TEST(machine_disk_wait_disk_first_when_both_due) {
    Running r;
    r.m.set_timer(808);
    CHECK(is_disk(r.m.wait_for_interrupt()));
    CHECK_EQ(r.m.steps(), 808u);
}

TEST(machine_disk_wait_returns_at_once_if_already_due) {
    // The timer deadline has passed: no time passes.
    machine::Machine m;
    CHECK(!m.load(counter_program()));
    CHECK(!m.run(50).has_value());
    m.set_timer(20);
    CHECK(is_timer(m.wait_for_interrupt()));
    CHECK_EQ(m.steps(), 50u);
}

TEST(machine_disk_wait_returns_pending_disk_interrupt_at_once) {
    Running r;
    CHECK(is_disk(r.m.run(100000)));           // pending, not acknowledged
    CHECK(is_disk(r.m.wait_for_interrupt()));
    CHECK_EQ(r.m.steps(), 808u);
}

TEST(machine_disk_wait_with_nothing_pending_is_nullopt) {
    // Timer off, disk idle: nothing could ever fire.
    machine::Machine m;
    CHECK(!m.load(counter_program()));
    CHECK(!m.wait_for_interrupt().has_value());
    CHECK_EQ(m.steps(), 0u);
}
