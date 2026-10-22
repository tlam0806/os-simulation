#include "mmu/frame_allocator.hpp"
#include "test.hpp"

TEST(frames_all_free_at_start) {
    mmu::FrameAllocator f;
    CHECK_EQ(mmu::kNumFrames, 256);
    CHECK_EQ(f.free_count(), 256);
}

TEST(frames_allocated_lowest_first) {
    mmu::FrameAllocator f;
    CHECK_EQ(f.allocate().value(), 0);
    CHECK_EQ(f.allocate().value(), 1);
    CHECK_EQ(f.allocate().value(), 2);
    CHECK_EQ(f.free_count(), 253);
}

TEST(frames_run_out) {
    mmu::FrameAllocator f;
    for (uint32_t i = 0; i < mmu::kNumFrames; i++) CHECK(f.allocate().has_value());
    CHECK_EQ(f.free_count(), 0);
    CHECK(!f.allocate().has_value());
}

TEST(frames_freed_frame_is_reused) {
    mmu::FrameAllocator f;
    for (int i = 0; i < 5; i++) f.allocate();   // frames 0..4
    f.free(2);
    CHECK_EQ(f.free_count(), 252);
    CHECK_EQ(f.allocate().value(), 2);           // the lowest free frame again
    CHECK_EQ(f.allocate().value(), 5);
}
