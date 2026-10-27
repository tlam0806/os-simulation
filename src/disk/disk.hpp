// The magnetic hard disk (docs/machine-spec.md, A6).
// To the kernel: a 1-D array of 512-byte sectors, one request at a time,
// data moved to/from RAM by DMA. Underneath: tracks, a rotating platter and
// a moving head, which decide how long a request takes.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
#include <unordered_map>

#include "mem/mem.hpp"

namespace disk {

inline constexpr uint32_t kSectorSize = 512;                       // bytes
inline constexpr uint32_t kTracks = 128;
inline constexpr uint32_t kSectorsPerTrack = 1024;
inline constexpr uint32_t kNumSectors = kTracks * kSectorsPerTrack; // 131072 = 64 MiB

// Times in clock steps (1 step = 1 µs).
inline constexpr uint64_t kSectorTime = 8;                              // one sector passes the head
inline constexpr uint64_t kRotationTime = kSectorTime * kSectorsPerTrack; // 8192: one full turn
inline constexpr uint64_t kSeekSettle = 1000;                           // any seek to another track
inline constexpr uint64_t kSeekPerTrack = 50;                           // plus this per track crossed

// Where a logical sector lies on the surface.
struct Place {
    uint32_t track = 0;
    uint32_t sector = 0;   // within the track, 0..kSectorsPerTrack-1

    bool operator==(const Place&) const = default;
};

// Logical sector s -> track s / 1024, sector s % 1024. s must be < kNumSectors.
Place place_of(uint32_t s);

enum class Op { read, write };

struct Request {
    Op op = Op::read;
    uint32_t first_sector = 0;
    uint32_t count = 0;       // sectors, >= 1
    uint32_t ram_addr = 0;    // physical RAM address of count * kSectorSize bytes
};

class Disk {
public:
    using Sector = std::array<uint8_t, kSectorSize>;
    // All sectors zero, head on track 0, idle.
    Disk();

    // ---- host access: no timing, no interrupt (spec A6, "Host access") ----

    // Copies `bytes` onto the disk starting at the first byte of sector `s`.
    // The size need not be a multiple of kSectorSize. Returns false, and
    // writes nothing, if the bytes would run past the end of the disk.
    bool host_write(uint32_t s, std::span<const uint8_t> bytes);

    // The contents of `count` sectors starting at sector `s`, or nullopt if
    // they would run past the end of the disk.
    std::optional<std::vector<uint8_t>> host_read(uint32_t s, uint32_t count) const;

    // ---- the device, as the kernel sees it ----

    // True from an accepted submit() until the request completes.
    bool busy() const;

    // The track the head is on.
    uint32_t head_track() const;

    // Starts `r` at clock time `now`. Returns false and changes nothing if the
    // disk is busy, count is 0, the sectors run past the end of the disk, or
    // the RAM range runs past the end of RAM.
    bool submit(const Request& r, uint64_t now);

    // The clock time at which the current request completes (spec A6,
    // "Timing": seek + rotational latency + transfer, track by track), or
    // nullopt when idle. Known as soon as the request is submitted.
    std::optional<uint64_t> completion_time() const;

    // Finishes the current request. Called once the clock has reached
    // completion_time(); the disk does not watch the clock itself.
    //   - moves the data: read = disk -> RAM, write = RAM -> disk
    //   - leaves the head on the last track the request used
    //   - becomes idle
    // Must only be called while busy.
    void complete(mem::Ram& ram);

private:
    // TODO: your data members. Hint: 64 MiB is a lot to allocate for every
    // Disk a test creates; most tests touch only a few sectors.
    std::unordered_map<uint32_t, Sector> sectors_;
    Request current_request_;
    uint64_t completion_time_ = 0;
    uint32_t head_track_ = 0;
    bool busy_ = false;
};

}  // namespace disk
