#include "mmu/frame_allocator.hpp"

namespace mmu {

// TODO: implement. tests/frame_allocator_test.cpp describes the expected behavior.

FrameAllocator::FrameAllocator() {
    free_frames.set();
}

std::optional<uint32_t> FrameAllocator::allocate() { 
    if (free_count() == 0) {
        return std::nullopt;
    }
    uint32_t frame = 0;
    while (!free_frames[frame]) ++frame;
    free_frames[frame] = 0;
    return frame;
}

void FrameAllocator::free(uint32_t frame) {
    free_frames[frame] = 1;
}

uint32_t FrameAllocator::free_count() const { 
    return free_frames.count();
}

}  // namespace mmu
