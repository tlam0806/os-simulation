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

// ---- reference counts (copy-on-write fork, spec B3) ----

TEST(frames_free_frame_has_no_references) {
    mmu::FrameAllocator f;
    CHECK_EQ(f.ref_count(0), 0);
    CHECK_EQ(f.ref_count(mmu::kNumFrames - 1), 0);
}

TEST(frames_allocated_frame_has_one_reference) {
    mmu::FrameAllocator f;
    const uint32_t a = f.allocate().value();
    CHECK_EQ(f.ref_count(a), 1);
}

TEST(frames_share_adds_a_reference) {
    mmu::FrameAllocator f;
    const uint32_t a = f.allocate().value();
    f.share(a);
    CHECK_EQ(f.ref_count(a), 2);
    f.share(a);
    CHECK_EQ(f.ref_count(a), 3);
    CHECK_EQ(f.free_count(), 255);               // sharing uses no new frame
}

TEST(frames_free_of_shared_frame_keeps_it_in_use) {
    mmu::FrameAllocator f;
    const uint32_t a = f.allocate().value();     // frame 0
    f.share(a);
    f.free(a);
    CHECK_EQ(f.ref_count(a), 1);
    CHECK_EQ(f.free_count(), 255);
    CHECK_EQ(f.allocate().value(), 1);           // frame 0 is not handed out again
}

TEST(frames_last_free_returns_frame_to_pool) {
    mmu::FrameAllocator f;
    const uint32_t a = f.allocate().value();     // frame 0
    f.share(a);
    f.free(a);
    f.free(a);
    CHECK_EQ(f.ref_count(a), 0);
    CHECK_EQ(f.free_count(), 256);
    CHECK_EQ(f.allocate().value(), 0);           // reused
}

TEST(frames_reallocated_frame_starts_at_one_reference) {
    mmu::FrameAllocator f;
    const uint32_t a = f.allocate().value();
    f.share(a);
    f.free(a);
    f.free(a);
    const uint32_t b = f.allocate().value();
    CHECK_EQ(b, a);
    CHECK_EQ(f.ref_count(b), 1);                 // not left over from before
}

TEST(frames_counts_are_per_frame) {
    mmu::FrameAllocator f;
    const uint32_t a = f.allocate().value();
    const uint32_t b = f.allocate().value();
    f.share(a);
    f.share(a);
    CHECK_EQ(f.ref_count(a), 3);
    CHECK_EQ(f.ref_count(b), 1);
    f.free(b);
    CHECK_EQ(f.ref_count(a), 3);
    CHECK_EQ(f.ref_count(b), 0);
}
