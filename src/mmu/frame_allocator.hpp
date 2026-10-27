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

    // Drops one reference to a frame that is in use. The frame goes back to
    // the free pool only when its last reference is dropped.
    void free(uint32_t frame);

    // Adds one reference to a frame that is in use (one more mapping points to
    // it, e.g. a page shared by fork). A freshly allocated frame has 1.
    void share(uint32_t frame);

    // How many references a frame has: 0 if it is free.
    uint32_t ref_count(uint32_t frame) const;

    // How many frames are free right now.
    uint32_t free_count() const;

private:
    // TODO: your data members. All 256 frames start free.
    std::array<uint32_t, kNumFrames> reading_count{}; // zero at the beginning
};

}  // namespace mmu
