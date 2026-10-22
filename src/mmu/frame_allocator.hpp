// Keeps track of which physical frames of RAM are in use.
#pragma once

#include <cstdint>
#include <optional>
#include<list>
#include <bitset>

#include "mem/mem.hpp"
#include "mmu/page_table.hpp"

namespace mmu {

inline constexpr uint32_t kNumFrames = mem::Ram::kRamSize / kPageSize;   // 256 (spec A2)

class FrameAllocator {
public:
    FrameAllocator();

    // Marks the lowest-numbered free frame as used and returns it,
    // or nullopt if every frame is in use. Does not touch RAM: the caller
    // zero-fills the frame if needed.
    std::optional<uint32_t> allocate();

    // Returns a frame to the free pool. The frame must currently be in use.
    void free(uint32_t frame);

    // How many frames are free right now.
    uint32_t free_count() const;

private:
    // TODO: your data members. All 256 frames start free.
    std::bitset<kNumFrames> free_frames;
};

}  // namespace mmu
