#include "mmu/frame_allocator.hpp"

namespace mmu {

// TODO: implement. tests/frame_allocator_test.cpp describes the expected behavior.

FrameAllocator::FrameAllocator() {
}

std::optional<uint32_t> FrameAllocator::allocate() { 
    if (free_count() == 0) {
        return std::nullopt;
    }
    uint32_t frame = 0;
    while (reading_count[frame]) ++frame;
    ++reading_count[frame];
    return frame;
}

void FrameAllocator::share(uint32_t frame) {
    ++reading_count[frame];
}

void FrameAllocator::free(uint32_t frame) {
    --reading_count[frame];
}

uint32_t FrameAllocator::free_count() const { 
    uint32_t count = 0;
    for (uint32_t i = 0; i < kNumFrames; ++i) {
        count += (reading_count[i] == 0);
    }
    return count;
}

uint32_t FrameAllocator::ref_count(uint32_t frame) const {
    return reading_count[frame];
}

}  // namespace mmu
