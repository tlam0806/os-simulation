// The disk (docs/machine-spec.md, A6), step 1: geometry, addressing, host
// access and request validation. No timing yet.
//
//   disk_geometry_   sizes and the sector -> (track, sector) mapping
//   disk_host_       host_read / host_write: direct access, no timing
//   disk_submit_     which requests the disk accepts or refuses
#include <cstdint>
#include <vector>

#include "disk/disk.hpp"
#include "mem/mem.hpp"
#include "test.hpp"

using disk::Disk;
using disk::Op;
using disk::Place;
using disk::Request;

namespace {

// n bytes: 1, 2, 3, ... (wrapping), so misplaced bytes are easy to spot.
std::vector<uint8_t> pattern(size_t n, uint8_t start = 1) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; i++) v[i] = static_cast<uint8_t>(start + i);
    return v;
}

}  // namespace

// =================== geometry and addressing ===================

TEST(disk_geometry_sizes) {
    CHECK_EQ(disk::kSectorSize, 512u);
    CHECK_EQ(disk::kNumSectors, 131072u);
    CHECK_EQ(static_cast<uint64_t>(disk::kNumSectors) * disk::kSectorSize,
             64ull * mem::Ram::kRamSize);                   // 64 MiB = 64 x RAM
    CHECK_EQ(disk::kRotationTime, 8192u);
}

TEST(disk_geometry_first_sectors_are_on_track_0) {
    CHECK(disk::place_of(0) == (Place{0, 0}));
    CHECK(disk::place_of(1) == (Place{0, 1}));
    CHECK(disk::place_of(1023) == (Place{0, 1023}));
}

TEST(disk_geometry_next_track_after_1024_sectors) {
    CHECK(disk::place_of(1024) == (Place{1, 0}));
    CHECK(disk::place_of(1025) == (Place{1, 1}));
    CHECK(disk::place_of(2 * 1024 + 100) == (Place{2, 100}));   // the spec's example
}

TEST(disk_geometry_last_sector) {
    CHECK(disk::place_of(disk::kNumSectors - 1) == (Place{127, 1023}));
}

// =================== host access ===================

TEST(disk_host_starts_all_zero) {
    Disk d;
    for (uint32_t s : {0u, 1u, 5000u, disk::kNumSectors - 1}) {
        const auto data = d.host_read(s, 1);
        CHECK(data.has_value());
        if (data) CHECK(*data == std::vector<uint8_t>(512, 0));
    }
}

TEST(disk_host_write_then_read_one_sector) {
    Disk d;
    const auto bytes = pattern(512);
    CHECK(d.host_write(7, bytes));
    CHECK(d.host_read(7, 1) == bytes);
    CHECK(d.host_read(6, 1) == std::vector<uint8_t>(512, 0));   // neighbours untouched
    CHECK(d.host_read(8, 1) == std::vector<uint8_t>(512, 0));
}

TEST(disk_host_read_returns_count_sectors) {
    Disk d;
    const auto bytes = pattern(3 * 512);
    CHECK(d.host_write(100, bytes));
    const auto data = d.host_read(100, 3);
    CHECK(data.has_value());
    if (data) CHECK_EQ(data->size(), 3u * 512);
    CHECK(data == bytes);
    CHECK(d.host_read(101, 1) == std::vector<uint8_t>(bytes.begin() + 512, bytes.begin() + 1024));
}

TEST(disk_host_write_partial_sector) {
    // 10 bytes: the rest of the sector keeps its old contents.
    Disk d;
    CHECK(d.host_write(3, std::vector<uint8_t>(512, 0xAA)));
    CHECK(d.host_write(3, pattern(10)));
    const auto data = d.host_read(3, 1);
    CHECK(data.has_value());
    if (!data) return;
    CHECK((std::vector<uint8_t>(data->begin(), data->begin() + 10)) == pattern(10));
    CHECK_EQ((*data)[10], 0xAA);
    CHECK_EQ((*data)[511], 0xAA);
}

TEST(disk_host_write_across_tracks) {
    // Sectors 1023 and 1024 are on different tracks but consecutive to the host.
    Disk d;
    const auto bytes = pattern(1024, 50);
    CHECK(d.host_write(1023, bytes));
    CHECK(d.host_read(1023, 2) == bytes);
}

TEST(disk_host_last_sector) {
    Disk d;
    const auto bytes = pattern(512, 9);
    CHECK(d.host_write(disk::kNumSectors - 1, bytes));
    CHECK(d.host_read(disk::kNumSectors - 1, 1) == bytes);
}

TEST(disk_host_write_past_end_is_refused) {
    Disk d;
    CHECK(!d.host_write(disk::kNumSectors - 1, pattern(513)));   // one byte too many
    CHECK(d.host_read(disk::kNumSectors - 1, 1) == std::vector<uint8_t>(512, 0));   // nothing written
    CHECK(!d.host_write(disk::kNumSectors, pattern(1)));
}

TEST(disk_host_read_past_end_is_nullopt) {
    Disk d;
    CHECK(!d.host_read(disk::kNumSectors, 1).has_value());
    CHECK(!d.host_read(disk::kNumSectors - 1, 2).has_value());
    CHECK(!d.host_read(0xFFFFFFFFu, 2).has_value());            // no wraparound
}

// =================== submit: accept or refuse ===================

TEST(disk_submit_starts_idle_on_track_0) {
    Disk d;
    CHECK(!d.busy());
    CHECK_EQ(d.head_track(), 0u);
}

TEST(disk_submit_valid_request_makes_it_busy) {
    Disk d;
    CHECK(d.submit({Op::read, 0, 1, 0}, 0));
    CHECK(d.busy());
}

TEST(disk_submit_while_busy_is_refused) {
    Disk d;
    CHECK(d.submit({Op::read, 0, 1, 0}, 0));
    CHECK(!d.submit({Op::write, 10, 1, 4096}, 5));
    CHECK(d.busy());
}

TEST(disk_submit_count_zero_is_refused) {
    Disk d;
    CHECK(!d.submit({Op::read, 0, 0, 0}, 0));
    CHECK(!d.busy());
}

TEST(disk_submit_past_end_of_disk_is_refused) {
    Disk d;
    CHECK(d.submit({Op::read, disk::kNumSectors - 2, 2, 0}, 0));   // exactly fits
    Disk e;
    CHECK(!e.submit({Op::read, disk::kNumSectors - 2, 3, 0}, 0));
    CHECK(!e.submit({Op::read, disk::kNumSectors, 1, 0}, 0));
    CHECK(!e.submit({Op::read, 0xFFFFFFFFu, 2, 0}, 0));         // no wraparound
    CHECK(!e.busy());
}

TEST(disk_submit_past_end_of_ram_is_refused) {
    const uint32_t ram = static_cast<uint32_t>(mem::Ram::kRamSize);
    Disk d;
    CHECK(d.submit({Op::write, 0, 8, ram - 8 * 512}, 0));       // the last page: fits
    Disk e;
    CHECK(!e.submit({Op::write, 0, 8, ram - 8 * 512 + 1}, 0));  // one byte over
    CHECK(!e.submit({Op::read, 0, 1, ram}, 0));
    CHECK(!e.submit({Op::read, 0, 1, 0xFFFFFF00u}, 0));         // no wraparound
    CHECK(!e.busy());
}

TEST(disk_submit_large_request_is_accepted) {
    // A request may span tracks; only the disk's and RAM's ends limit it.
    Disk d;
    CHECK(d.submit({Op::read, 1000, 2048, 0}, 0));               // 1 MiB, over 3 tracks
}

TEST(disk_submit_does_not_move_data_yet) {
    // Data moves when the request completes (step 2), not at submit.
    Disk d;
    CHECK(d.host_write(0, pattern(512)));
    CHECK(d.submit({Op::write, 0, 1, 0}, 0));                   // RAM is not even involved here
    CHECK(d.host_read(0, 1) == pattern(512));
}

// =================== step 2: timing (spec A6, "Timing") ===================
// completion = now + seek + rotational latency + transfer, track by track.
// Rotation: sector k of any track starts under the head at times t with
// t % 8192 == 8 * k. Seek to another track: 1000 + 50 per track crossed.

namespace {

// Submits r at `now`, checks it was accepted, returns its completion time.
uint64_t done_at(Disk& d, const Request& r, uint64_t now) {
    CHECK(d.submit(r, now));
    const auto t = d.completion_time();
    CHECK(t.has_value());
    return t.value_or(0);
}

// Moves the head to `track` with a 1-sector read, completed at once.
void put_head_on(Disk& d, mem::Ram& ram, uint32_t track) {
    done_at(d, {Op::read, track * disk::kSectorsPerTrack, 1, 0}, 0);
    d.complete(ram);
    CHECK_EQ(d.head_track(), track);
}

}  // namespace

TEST(disk_timing_idle_has_no_completion_time) {
    Disk d;
    CHECK(!d.completion_time().has_value());
}

TEST(disk_timing_sector_already_under_head) {
    // Head on track 0, sector 0 starts at t = 0: no seek, no wait, 8 µs transfer.
    Disk d;
    CHECK_EQ(done_at(d, {Op::read, 0, 1, 0}, 0), 8u);
}

TEST(disk_timing_wait_for_sector_to_come_round) {
    // Sector 100 starts at t = 800.
    Disk d;
    CHECK_EQ(done_at(d, {Op::read, 100, 1, 0}, 0), 808u);
}

TEST(disk_timing_just_missed_waits_a_full_turn) {
    // At t = 1, sector 0 has just started passing: wait for the next turn.
    Disk d;
    CHECK_EQ(done_at(d, {Op::read, 0, 1, 0}, 1), 8192u + 8);
}

TEST(disk_timing_rotation_follows_the_clock) {
    // Many turns later, the platter is wherever the clock says.
    Disk d;
    const uint64_t now = 1000 * 8192 + 80;            // sector 10 starting
    CHECK_EQ(done_at(d, {Op::read, 10, 1, 0}, now), now + 8);
}

TEST(disk_timing_transfer_is_8_us_per_sector) {
    Disk d;
    CHECK_EQ(done_at(d, {Op::read, 0, 8, 0}, 0), 64u);   // one page: 8 sectors
}

TEST(disk_timing_spec_example) {
    // Spec A6: head on track 0, t0 = 0, read 8 sectors from track 2, sector 100.
    // Seek 1100 -> t = 1100; sector 100 next starts at 8192 + 800; + 64.
    Disk d;
    CHECK_EQ(done_at(d, {Op::read, 2 * 1024 + 100, 8, 0}, 0), 9056u);
}

TEST(disk_timing_seek_cost_by_distance) {
    // Target sector 0 of each track, so the wait is easy to see:
    // arrival = 1000 + 50 * tracks; then wait for the next multiple of 8192.
    Disk d;
    CHECK_EQ(done_at(d, {Op::read, 1 * 1024, 1, 0}, 0), 8192u + 8);       // arrives 1050
    Disk e;
    CHECK_EQ(done_at(e, {Op::read, 127 * 1024, 1, 0}, 0), 8192u + 8);     // arrives 7350
    Disk f;
    CHECK_EQ(done_at(f, {Op::read, 127 * 1024, 1, 0}, 900), 2 * 8192u + 8);  // arrives 8250: missed
}

TEST(disk_timing_seek_back_towards_track_0) {
    // Head on track 5; at t = 10000 read track 2 sector 0.
    // Seek 1000 + 150 -> 11150; sector 0 next at 16384; + 8.
    Disk d;
    mem::Ram ram;
    put_head_on(d, ram, 5);
    CHECK_EQ(done_at(d, {Op::read, 2 * 1024, 1, 0}, 10000), 16392u);
}

TEST(disk_timing_same_track_needs_no_seek) {
    // Head already on track 5: only rotation and transfer.
    Disk d;
    mem::Ram ram;
    put_head_on(d, ram, 5);
    CHECK_EQ(done_at(d, {Op::read, 5 * 1024 + 3, 1, 0}, 8192 * 4), 8192u * 4 + 24 + 8);
}

TEST(disk_timing_request_across_a_track_boundary) {
    // Sectors 1022..1025: two on track 0, two on track 1.
    // 1022 starts at 8176; two transfers end at 8192. 1-track seek: 1050 -> 9242.
    // Sector 0 of track 1 next starts at 16384; two transfers end at 16400.
    Disk d;
    CHECK_EQ(done_at(d, {Op::read, 1022, 4, 0}, 0), 16400u);
}

TEST(disk_timing_completion_time_does_not_change_while_busy) {
    Disk d;
    const uint64_t t = done_at(d, {Op::read, 100, 1, 0}, 0);
    CHECK(!d.submit({Op::read, 0, 1, 0}, 50));          // refused
    CHECK_EQ(d.completion_time().value_or(0), t);
}

// =================== step 2: complete ===================

TEST(disk_complete_read_moves_disk_to_ram) {
    Disk d;
    mem::Ram ram;
    const auto bytes = pattern(1024);
    CHECK(d.host_write(40, bytes));
    done_at(d, {Op::read, 40, 2, 0x3000}, 0);
    d.complete(ram);
    for (uint32_t i = 0; i < 1024; i++) CHECK_EQ(ram.read(0x3000 + i, 1).value_or(999), bytes[i]);
    CHECK_EQ(ram.read(0x3000 + 1024, 1).value_or(999), 0u);       // nothing past the end
}

TEST(disk_complete_write_moves_ram_to_disk) {
    Disk d;
    mem::Ram ram;
    const auto bytes = pattern(512, 7);
    for (uint32_t i = 0; i < 512; i++) ram.write(0x5000 + i, 1, bytes[i]);
    done_at(d, {Op::write, 9, 1, 0x5000}, 0);
    d.complete(ram);
    CHECK(d.host_read(9, 1) == bytes);
    CHECK(d.host_read(10, 1) == std::vector<uint8_t>(512, 0));
}

TEST(disk_complete_write_uses_ram_at_completion) {
    // Data moves when the request completes: a RAM change before that counts.
    Disk d;
    mem::Ram ram;
    done_at(d, {Op::write, 0, 1, 0}, 0);
    ram.write(0, 1, 0x5A);
    d.complete(ram);
    CHECK_EQ(d.host_read(0, 1).value()[0], 0x5A);
}

TEST(disk_complete_makes_it_idle) {
    Disk d;
    mem::Ram ram;
    done_at(d, {Op::read, 0, 1, 0}, 0);
    d.complete(ram);
    CHECK(!d.busy());
    CHECK(!d.completion_time().has_value());
    CHECK(d.submit({Op::read, 1, 1, 0}, 100));            // accepts the next one
}

TEST(disk_complete_head_stays_on_last_track) {
    Disk d;
    mem::Ram ram;
    done_at(d, {Op::read, 1022, 4, 0}, 0);                // ends on track 1
    d.complete(ram);
    CHECK_EQ(d.head_track(), 1u);
    done_at(d, {Op::read, 3 * 1024 + 7, 1, 0}, 20000);
    d.complete(ram);
    CHECK_EQ(d.head_track(), 3u);
}
