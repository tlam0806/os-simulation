#include "disk/disk.hpp"
#include "mem/mem.hpp"
#include <cassert>
#include <cstdlib>
#include <algorithm>

namespace disk {

// TODO: implement. tests/disk_test.cpp describes the expected behavior.

Place place_of(uint32_t logical_sector_number) { 
    return Place{logical_sector_number / kSectorsPerTrack, logical_sector_number % kSectorsPerTrack};
}

Disk::Disk() {}


bool Disk::host_write(uint32_t sector, std::span<const uint8_t> bytes) { 
    if (bytes.size() == 0) {
        return true;
    }
    uint64_t last_sector = (uint64_t) sector + (uint64_t) ((uint32_t) bytes.size() - 1) / kSectorSize;
    if (last_sector >= static_cast<uint64_t>(kNumSectors)) {
        return false;
    }
    uint32_t bytes_written = 0;
    while (bytes_written < bytes.size()) {
        assert(sector < kNumSectors);
        uint32_t writing = std::min(kSectorSize, (uint32_t) bytes.size() - bytes_written);
        std::copy(bytes.begin() + bytes_written, bytes.begin() + bytes_written + writing, sectors_[sector].begin());
        bytes_written += writing;
        ++sector;
    }
    return true;
}

std::optional<std::vector<uint8_t>> Disk::host_read(uint32_t sector, uint32_t count) const { 
    uint64_t last_sector = (uint64_t) sector + (uint64_t) count - 1;
    if (count == 0) {
        return std::nullopt;
    }
    if (last_sector >= static_cast<uint64_t>(kNumSectors)) {
        return std::nullopt;
    }
    std::vector<uint8_t> returned;
    returned.reserve(count * kSectorSize);
    for (uint32_t s = sector; s < sector + count; ++s) {
        auto it = sectors_.find(s);
        if (it == sectors_.end()) {
            returned.insert(returned.end(), kSectorSize, 0);
        } else {
            returned.insert(returned.end(), it->second.begin(), it->second.end());
        }
    }
    return returned;
}

bool Disk::busy() const { 
    return busy_; 
}

uint32_t Disk::head_track() const { return head_track_; }

bool Disk::submit(const Request& request, uint64_t now) { 
    if (busy_) {
        return false;
    }
    if (request.count == 0) {
        return false;
    }
    uint64_t last_sector = static_cast<uint64_t>(request.first_sector) + static_cast<uint64_t>(request.count) - 1;
    if (last_sector >= static_cast<uint64_t>(kNumSectors)) {
        return false;
    }
    uint64_t last_addr = static_cast<uint64_t>(request.ram_addr) + static_cast<uint64_t>(request.count) * kSectorSize - 1;
    if (last_addr >= static_cast<uint64_t>(mem::Ram::kRamSize)) {
        return false;
    }
    busy_ = true;
    completion_time_ = now;
    Place start = place_of(request.first_sector);
    uint32_t distance = start.track > head_track_ ? start.track - head_track_ : head_track_ - start.track;
    if (distance) completion_time_ += kSeekSettle + kSeekPerTrack * distance;
    uint64_t left = request.count;
    uint64_t pos = start.sector;
    head_track_ = start.track;
    while (left > 0) {
        uint64_t here = std::min(left, static_cast<uint64_t>(kSectorsPerTrack) - pos);
        // wait for the first sector to come
        completion_time_ += (pos * kSectorTime + kRotationTime - completion_time_ % kRotationTime) % kRotationTime; 
        // wait for the rest of the sectors to come
        completion_time_ += here * kSectorTime;
        left -= here;
        if (left > 0) {
            ++head_track_; // move the head to the next track
            pos = 0;
            completion_time_ += kSeekSettle + kSeekPerTrack; // seek to the next track
        }
    }
    current_request_ = request;
    return true;
}

}  // namespace disk

namespace disk {

// TODO (step 2): implement. tests/disk_test.cpp, "timing" and "complete".

std::optional<uint64_t> Disk::completion_time() const { 
    if (!busy_) {
        return std::nullopt;
    }
    return completion_time_;
}

void Disk::complete(mem::Ram& ram) {
    for (uint32_t i = 0; i < current_request_.count; ++i) {
        uint32_t sector = current_request_.first_sector + i;
        uint32_t ram_addr = current_request_.ram_addr + i * kSectorSize;
        if (current_request_.op == Op::read) { // from disk to ram
            auto it = sectors_.find(sector);
            bool written = false;
            if (it != sectors_.end()) {
                written = ram.write_bytes(ram_addr, it->second);
            } else {
                std::vector<uint8_t> zeros(kSectorSize, 0);
                written = ram.write_bytes(ram_addr, zeros);
            }
            assert(written);
        } else { // from ram to disk
            std::optional<std::vector<uint8_t>> data = ram.read_bytes(ram_addr, kSectorSize);
            assert(data);
            std::copy(data->begin(), data->end(), sectors_[sector].begin());
        }
    }
    busy_ = false;
    head_track_ = place_of(current_request_.first_sector + current_request_.count - 1).track;
    completion_time_ = 0;
    current_request_ = {};
}

}  // namespace disk
