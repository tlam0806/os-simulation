// disk_read (a7 = 9) and disk_write (a7 = 10), spec B3: raw disk access
// through the kernel's disk driver.
//
//   rawdisk_args_    -EINVAL / -EFAULT at once, without blocking
//   rawdisk_data_    the bytes really move, into and out of user memory
//   rawdisk_block_   the caller blocks; others run; idle when all are blocked;
//                    the ECALL is not run again
//   rawdisk_queue_   one request at a time, first come first served
#include <cstdint>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "asm.hpp"
#include "cpu_helpers.hpp"
#include "disk/disk.hpp"
#include "elf_builder.hpp"
#include "kernel/kernel.hpp"
#include "machine/machine.hpp"
#include "test.hpp"

using namespace isa::reg;
using enum isa::Op;

namespace {

constexpr uint32_t kData = 0x20000;        // read-only bytes: letters to print
constexpr uint32_t kBuf = 0x80000000 - 512;   // last 512 bytes of the top stack page
constexpr int32_t kEfault = 14, kEinval = 22;

std::vector<uint8_t> elf(const Asm& a, std::vector<uint8_t> data = {}) {
    using namespace elf_builder;
    std::vector<Segment> segments = {{0x10000, code(a.code)}};
    if (!data.empty()) segments.push_back({kData, std::move(data), 0, PT_LOAD, PF_R});
    return make_elf(0x10000, segments);
}

struct System {
    machine::Machine m;
    std::ostringstream out, err;
    kernel::Kernel k{m, out, err};

    uint32_t spawn(const Asm& a, std::vector<uint8_t> data = {}) {
        const auto r = k.spawn(elf(a, std::move(data)));
        CHECK(std::holds_alternative<uint32_t>(r));
        return std::holds_alternative<uint32_t>(r) ? std::get<uint32_t>(r) : 0;
    }

    void run() { CHECK(std::holds_alternative<kernel::AllDone>(k.run_all(10'000'000))); }

    int32_t status(uint32_t pid) {
        const auto e = k.ending(pid);
        CHECK(e.has_value());
        const auto* exited = e ? std::get_if<kernel::Exited>(&e->reason) : nullptr;
        CHECK(exited != nullptr);
        return exited ? exited->exit_status : -12345;
    }
};

// a0 = sector, a1 = buffer, a2 = count; ecall. Returns the index of the ecall.
size_t disk_call(Asm& a, int32_t number, uint32_t sector, uint32_t buffer, uint32_t count) {
    a.li(a0, static_cast<int32_t>(sector));
    a.li(a1, static_cast<int32_t>(buffer));
    a.li(a2, static_cast<int32_t>(count));
    a.syscall(number);
    return a.here() - 1;
}

// 512 bytes whose 32-bit words are base, base + 1, base + 2, ...
std::vector<uint8_t> words_from(uint32_t base) {
    std::vector<uint8_t> v(512);
    for (uint32_t i = 0; i < 128; i++)
        for (uint32_t b = 0; b < 4; b++) v[4 * i + b] = static_cast<uint8_t>((base + i) >> (8 * b));
    return v;
}

}  // namespace

// =================== arguments: fail at once, no blocking ===================

TEST(rawdisk_args_count_zero_is_einval) {
    System s;
    Asm a;
    disk_call(a, SYS_DISK_READ, 0, kBuf, 0);
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(pid), -kEinval);
}

TEST(rawdisk_args_past_end_of_disk_is_einval) {
    System s;
    Asm a;
    disk_call(a, SYS_DISK_READ, disk::kNumSectors, kBuf, 1);
    a.emit({ADDI, s1, a0, 0, 0});
    disk_call(a, SYS_DISK_WRITE, disk::kNumSectors - 1, kBuf - 512, 2);
    a.emit({ADD, a0, a0, s1, 0});         // -22 + -22
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(pid), -2 * kEinval);
}

TEST(rawdisk_args_buffer_across_pages_is_einval) {
    // 1024 bytes starting 512 bytes before a page boundary: two pages.
    System s;
    Asm a;
    disk_call(a, SYS_DISK_READ, 0, 0x80000000 - 4096 - 512, 2);
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(pid), -kEinval);
}

TEST(rawdisk_args_whole_page_buffer_is_fine) {
    // Exactly one page, 8 sectors: allowed.
    System s;
    Asm a;
    disk_call(a, SYS_DISK_READ, 0, 0x80000000 - 4096, 8);
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(pid), 0);
}

TEST(rawdisk_args_read_into_code_page_is_efault) {
    // disk_read writes the buffer: the code page is read-only.
    System s;
    Asm a;
    disk_call(a, SYS_DISK_READ, 0, 0x10000, 1);
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(pid), -kEfault);
}

TEST(rawdisk_args_write_from_code_page_is_fine) {
    // disk_write only reads the buffer: a read-only page is enough.
    System s;
    Asm a;
    disk_call(a, SYS_DISK_WRITE, 3, 0x10000, 1);
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(pid), 0);
    const auto sector = s.m.disk().host_read(3, 1);
    CHECK(sector.has_value());
    if (sector) CHECK_EQ((*sector)[0], 0x13u);   // low byte of the first instruction (lui/addi a0)
}

TEST(rawdisk_args_kernel_address_is_efault) {
    System s;
    Asm a;
    disk_call(a, SYS_DISK_WRITE, 0, 0x90000000, 1);
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(pid), -kEfault);
}

TEST(rawdisk_args_errors_do_not_block) {
    // A failed call returns at once: the clock never reaches the disk's
    // completion times (thousands of µs).
    System s;
    Asm a;
    disk_call(a, SYS_DISK_READ, 0, kBuf, 0);
    disk_call(a, SYS_DISK_READ, 0, 0x10000, 1);
    a.exit_with(0);
    s.spawn(a);
    s.run();
    CHECK(s.m.steps() < 100);
    CHECK(!s.m.disk().busy());
}

// =================== data ===================

TEST(rawdisk_data_read_one_sector) {
    System s;
    CHECK(s.m.disk().host_write(5, words_from(1000)));
    Asm a;
    disk_call(a, SYS_DISK_READ, 5, kBuf, 1);
    a.emit({ADDI, s1, a0, 0, 0});         // 0
    a.li(t0, static_cast<int32_t>(kBuf));
    a.emit({LW, t1, t0, 0, 0});           // first word: 1000
    a.emit({LW, t2, t0, 0, 508});         // last word: 1127
    a.emit({ADD, a0, t1, t2, 0});
    a.emit({ADD, a0, a0, s1, 0});
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(pid), 1000 + 1127);
}

TEST(rawdisk_data_read_a_whole_page) {
    // 8 sectors into a fresh heap page (mapped by the kernel first).
    System s;
    for (uint32_t i = 0; i < 8; i++) CHECK(s.m.disk().host_write(40 + i, words_from(i * 128)));
    Asm a;
    a.sbrk(4096);
    a.emit({ADDI, s0, a0, 0, 0});         // heap page, untouched
    a.li(a0, 40);
    a.emit({ADDI, a1, s0, 0, 0});
    a.li(a2, 8);
    a.syscall(SYS_DISK_READ);
    a.emit({LW, t1, s0, 0, 2044});        // word 511: 511
    a.li(t0, 2048);
    a.emit({ADD, t0, s0, t0, 0});
    a.emit({LW, t2, t0, 0, 0});           // word 512: 512
    a.li(t0, 4092);
    a.emit({ADD, t0, s0, t0, 0});
    a.emit({LW, t3, t0, 0, 0});           // word 1023: 1023
    a.emit({ADD, a0, t1, t2, 0});
    a.emit({ADD, a0, a0, t3, 0});
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(pid), 511 + 512 + 1023);
}

TEST(rawdisk_data_write_one_sector) {
    System s;
    Asm a;
    a.li(t0, static_cast<int32_t>(kBuf));
    a.li(t1, 0x11223344);
    a.emit({SW, 0, t0, t1, 0});
    a.li(t1, 0x55667788);
    a.emit({SW, 0, t0, t1, 508});
    disk_call(a, SYS_DISK_WRITE, 9, kBuf, 1);
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(pid), 0);
    const auto sector = s.m.disk().host_read(9, 1);
    CHECK(sector.has_value());
    if (!sector) return;
    CHECK_EQ((*sector)[0], 0x44u);
    CHECK_EQ((*sector)[3], 0x11u);
    CHECK_EQ((*sector)[508], 0x88u);
    CHECK_EQ((*sector)[511], 0x55u);
    CHECK(s.m.disk().host_read(10, 1) == std::vector<uint8_t>(512, 0));   // only one sector
}

TEST(rawdisk_data_write_then_read_back) {
    System s;
    Asm a;
    a.li(t0, static_cast<int32_t>(kBuf));
    a.li(t1, 4242);
    a.emit({SW, 0, t0, t1, 0});
    disk_call(a, SYS_DISK_WRITE, 77, kBuf, 1);
    a.li(t0, static_cast<int32_t>(kBuf));
    a.emit({SW, 0, t0, zero, 0});         // wipe the buffer
    disk_call(a, SYS_DISK_READ, 77, kBuf, 1);
    a.li(t0, static_cast<int32_t>(kBuf));
    a.emit({LW, a0, t0, 0, 0});
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(pid), 4242);
}

TEST(rawdisk_data_read_breaks_copy_on_write) {
    // The parent stores 5 in the buffer, then forks. The child reads a sector
    // into the shared (cow) page: only the child's copy changes.
    System s;
    CHECK(s.m.disk().host_write(3, words_from(900)));
    Asm a;
    a.li(t0, static_cast<int32_t>(kBuf));
    a.li(t1, 5);
    a.emit({SW, 0, t0, t1, 0});
    a.syscall(SYS_FORK);
    const size_t to_child = a.branch(BEQ, a0, zero);
    a.li(a0, -1);
    a.li(a1, 0);
    a.syscall(SYS_WAIT);                  // parent: wait for the child
    a.li(t0, static_cast<int32_t>(kBuf));
    a.emit({LW, a0, t0, 0, 0});           // still 5
    a.exit_with_a0();
    a.patch(to_child, a.here());
    disk_call(a, SYS_DISK_READ, 3, kBuf, 1);
    a.li(t0, static_cast<int32_t>(kBuf));
    a.emit({LW, a0, t0, 0, 0});           // 900
    a.exit_with_a0();
    const uint32_t parent = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(parent), 5);
    CHECK_EQ(s.status(2), 900);
    CHECK_EQ(s.m.frame_allocator().free_count(), mmu::kNumFrames);
}

// =================== blocking and idle ===================

TEST(rawdisk_block_caller_is_blocked_during_io) {
    // pid 1 reads sector 1000 (track 0: starts at 8000 µs); pid 2 loops.
    System s;
    Asm reader;
    disk_call(reader, SYS_DISK_READ, 1000, kBuf, 1);
    reader.exit_with(0);
    Asm looper;
    looper.emit({JAL, zero, 0, 0, 0});
    s.spawn(reader);
    s.spawn(looper);
    CHECK(std::holds_alternative<kernel::StepLimit>(s.k.run_all(3000)));
    const kernel::Process* p = s.k.process(1);
    CHECK(p != nullptr);
    if (p) CHECK(p->blocked == kernel::BlockReason::IO);
    CHECK(s.m.disk().busy());
    CHECK(std::holds_alternative<kernel::StepLimit>(s.k.run_all(10000)));
    CHECK(s.k.ending(1).has_value());     // woken and finished by now
}

TEST(rawdisk_block_others_run_during_io) {
    // pid 1 reads, then prints A. pid 2 prints B at once. B comes first.
    System s;
    Asm reader;
    disk_call(reader, SYS_DISK_READ, 100, kBuf, 1);
    reader.write_stdout(kData, 1);
    reader.exit_with(0);
    Asm other;
    other.write_stdout(kData + 1, 1);
    other.exit_with(0);
    s.spawn(reader, {'A', 'B'});
    s.spawn(other, {'A', 'B'});
    s.run();
    CHECK_EQ(s.out.str(), std::string("BA"));
}

TEST(rawdisk_block_idle_until_completion_exactly) {
    // One process: it reads sector 100, so the clock idles to the disk's
    // completion (808) and runs the rest of the program from there. The
    // total is exact: no instruction runs while idle, and the ECALL isn't
    // run again.
    System s;
    Asm a;
    const size_t ecall = disk_call(a, SYS_DISK_READ, 100, kBuf, 1);
    a.exit_with_a0();
    const uint64_t after = a.here() - (ecall + 1);   // instructions after the ecall
    CHECK(ecall + 1 < 800);                          // submitted before sector 100 comes round
    const uint32_t pid = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(pid), 0);
    CHECK_EQ(s.m.steps(), 808 + after);
}

TEST(rawdisk_block_parent_waits_child_reads) {
    // Parent blocked in wait, child blocked on the disk: everyone is blocked,
    // the kernel idles, and both finish.
    System s;
    CHECK(s.m.disk().host_write(12, words_from(321)));
    Asm a;
    a.syscall(SYS_FORK);
    const size_t to_child = a.branch(BEQ, a0, zero);
    a.li(a0, -1);
    a.li(t0, static_cast<int32_t>(kBuf - 512));
    a.emit({ADDI, a1, t0, 0, 0});
    a.syscall(SYS_WAIT);
    a.li(t0, static_cast<int32_t>(kBuf - 512));
    a.emit({LW, a0, t0, 0, 0});           // the child's status: 321
    a.exit_with_a0();
    a.patch(to_child, a.here());
    disk_call(a, SYS_DISK_READ, 12, kBuf, 1);
    a.li(t0, static_cast<int32_t>(kBuf));
    a.emit({LW, a0, t0, 0, 0});
    a.exit_with_a0();
    const uint32_t parent = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(parent), 321);
}

TEST(rawdisk_block_child_exit_does_not_wake_disk_reader) {
    // The parent forks, then blocks on a read of sector 1000 (done ~8000 µs).
    // The child exits at once. Its end must not wake the parent: the parent
    // is waiting for the disk, not for a child. Woken early, it would see the
    // buffer before the data arrived.
    System s;
    CHECK(s.m.disk().host_write(1000, words_from(555)));
    Asm a;
    a.syscall(SYS_FORK);
    const size_t to_child = a.branch(BEQ, a0, zero);
    disk_call(a, SYS_DISK_READ, 1000, kBuf, 1);
    a.li(t0, static_cast<int32_t>(kBuf));
    a.emit({LW, t1, t0, 0, 0});
    a.emit({ADD, a0, a0, t1, 0});         // disk_read's 0 + the first word
    a.exit_with_a0();
    a.patch(to_child, a.here());
    a.exit_with(0);
    const uint32_t parent = s.spawn(a);
    s.run();
    CHECK_EQ(s.status(parent), 555);
}

TEST(rawdisk_block_step_limit_while_idle) {
    // One process blocked on sector 1000 (done ~8000 µs); run_all(500) ends
    // while the kernel idles. The interrupt that ends the idle is the timer,
    // not the disk: the request is still in flight and the caller still blocked.
    System s;
    CHECK(s.m.disk().host_write(1000, words_from(42)));
    Asm a;
    disk_call(a, SYS_DISK_READ, 1000, kBuf, 1);
    a.li(t0, static_cast<int32_t>(kBuf));
    a.emit({LW, a0, t0, 0, 0});
    a.exit_with_a0();
    const uint32_t pid = s.spawn(a);
    CHECK(std::holds_alternative<kernel::StepLimit>(s.k.run_all(500)));
    CHECK_EQ(s.m.steps(), 500u);          // the clock advanced by exactly 500
    const kernel::Process* p = s.k.process(pid);
    CHECK(p != nullptr);
    if (p) CHECK(p->blocked == kernel::BlockReason::IO);
    CHECK(s.m.disk().busy());
    s.run();                              // continues where it stopped
    CHECK_EQ(s.status(pid), 42);
}

// =================== the queue: one at a time, first come first served ===================

TEST(rawdisk_queue_requests_are_served_in_arrival_order) {
    // pid 1 reads track 0, sector 1000 (busy until ~8000 µs). While it runs,
    // pid 2 asks for track 120 and pid 3 for track 1. First come first served:
    // 2 before 3, although 3 is much closer. Each prints its digit when done.
    System s;
    std::vector<uint8_t> digits = {'1', '2', '3'};
    const uint32_t sectors[] = {1000, 120 * 1024, 1 * 1024};
    for (uint32_t i = 0; i < 3; i++) {
        Asm a;
        disk_call(a, SYS_DISK_READ, sectors[i], kBuf, 1);
        a.write_stdout(kData + i, 1);
        a.exit_with(0);
        s.spawn(a, digits);
    }
    s.run();
    CHECK_EQ(s.out.str(), std::string("123"));
}

TEST(rawdisk_queue_one_request_at_a_time) {
    // Two readers of the same far sector: the second request waits for the
    // first, so finishing takes at least two seeks' worth of time.
    System s;
    for (int i = 0; i < 2; i++) {
        Asm a;
        disk_call(a, SYS_DISK_READ, 127 * 1024, kBuf, 1);
        a.exit_with(0);
        s.spawn(a);
    }
    s.run();
    CHECK(s.m.steps() > 7350);            // the first alone: seek 7350 + wait + 8
    CHECK(!s.m.disk().busy());
}

TEST(rawdisk_queue_no_interrupt_leaks_to_processes) {
    // Disk interrupts are the kernel's business: no process is ever killed by one.
    System s;
    for (int i = 0; i < 3; i++) {
        Asm a;
        disk_call(a, SYS_DISK_READ, static_cast<uint32_t>(i * 5000), kBuf, 1);
        a.busy_wait(3000);
        a.exit_with(7);
        s.spawn(a);
    }
    s.run();
    for (uint32_t pid = 1; pid <= 3; pid++) CHECK_EQ(s.status(pid), 7);
}
