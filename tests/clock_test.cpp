// CLOCK, the second-chance page replacement algorithm (spec D3, L9 "Second-Chance"),
// as a function of the frames' states: kernel::clock_choose.
//
//   clock_   one choice: skipping, clearing A bits, wrapping, no candidate
//   clock_lecture_   the L9 example: 3 frames, 12 references, 6 page faults
#include <cstdint>
#include <vector>

#include "kernel/clock.hpp"
#include "test.hpp"

using kernel::ClockFrame;

namespace {

constexpr ClockFrame used(bool accessed) { return {true, accessed}; }   // a candidate
constexpr ClockFrame kPinned = {false, false};                         // not a candidate
constexpr ClockFrame kPinnedAccessed = {false, true};

using Frames = std::vector<ClockFrame>;
using List = std::vector<uint32_t>;

}  // namespace

// =================== one choice ===================

TEST(clock_victim_is_at_the_hand) {
    // The frame under the hand has A clear: it goes, nothing is cleared.
    const Frames f = {used(true), used(false), used(true)};
    const auto c = kernel::clock_choose(f, 1);
    CHECK(c.has_value());
    if (!c) return;
    CHECK_EQ(c->victim, 1u);
    CHECK_EQ(c->hand, 2u);
    CHECK(c->cleared.empty());
}

TEST(clock_second_chance_clears_and_skips) {
    // Frames 0 and 1 were accessed: their bits are cleared, frame 2 goes.
    const Frames f = {used(true), used(true), used(false), used(false)};
    const auto c = kernel::clock_choose(f, 0);
    CHECK(c.has_value());
    if (!c) return;
    CHECK_EQ(c->victim, 2u);
    CHECK_EQ(c->hand, 3u);
    CHECK(c->cleared == (List{0, 1}));
}

TEST(clock_hand_wraps_around) {
    // Starting at the last frame: it is skipped, the hand wraps to 0.
    const Frames f = {used(false), used(false), used(true)};
    const auto c = kernel::clock_choose(f, 2);
    CHECK(c.has_value());
    if (!c) return;
    CHECK_EQ(c->victim, 0u);
    CHECK_EQ(c->hand, 1u);
    CHECK(c->cleared == (List{2}));
}

TEST(clock_hand_after_the_last_frame_is_0) {
    const Frames f = {used(true), used(false)};
    const auto c = kernel::clock_choose(f, 1);
    CHECK(c.has_value());
    if (!c) return;
    CHECK_EQ(c->victim, 1u);
    CHECK_EQ(c->hand, 0u);
}

TEST(clock_all_accessed_degenerates_to_fifo) {
    // Every bit set: the hand clears them all in one turn and takes the frame
    // it started at (L9: "degenerates into FIFO").
    const Frames f = {used(true), used(true), used(true), used(true)};
    const auto c = kernel::clock_choose(f, 1);
    CHECK(c.has_value());
    if (!c) return;
    CHECK_EQ(c->victim, 1u);
    CHECK_EQ(c->hand, 2u);
    CHECK(c->cleared == (List{1, 2, 3, 0}));
}

TEST(clock_non_candidates_are_skipped_untouched) {
    // Pinned frames (shared, cache, under a disk transfer) are passed over, and
    // their A bits are not the hand's business: not cleared.
    const Frames f = {kPinnedAccessed, kPinned, used(true), kPinnedAccessed, used(false)};
    const auto c = kernel::clock_choose(f, 0);
    CHECK(c.has_value());
    if (!c) return;
    CHECK_EQ(c->victim, 4u);
    CHECK_EQ(c->hand, 0u);
    CHECK(c->cleared == (List{2}));
}

TEST(clock_one_candidate_with_a_set) {
    // A full turn clears its bit; the second turn takes it.
    const Frames f = {kPinned, used(true), kPinned};
    const auto c = kernel::clock_choose(f, 2);
    CHECK(c.has_value());
    if (!c) return;
    CHECK_EQ(c->victim, 1u);
    CHECK_EQ(c->hand, 2u);
    CHECK(c->cleared == (List{1}));
}

TEST(clock_no_candidate) {
    const Frames f = {kPinned, kPinnedAccessed, kPinned};
    CHECK(!kernel::clock_choose(f, 1).has_value());
}

TEST(clock_256_frames) {
    // The kernel's size: RAM has 256 frames. Only frame 200 has A clear.
    Frames f(256, used(true));
    f[200] = used(false);
    const auto c = kernel::clock_choose(f, 10);
    CHECK(c.has_value());
    if (!c) return;
    CHECK_EQ(c->victim, 200u);
    CHECK_EQ(c->hand, 201u);
    CHECK_EQ(c->cleared.size(), 190u);
    CHECK_EQ(c->cleared.front(), 10u);
    CHECK_EQ(c->cleared.back(), 199u);
}

// =================== the lecture's example ===================

TEST(clock_lecture_example_six_page_faults) {
    // L9 "Example: CLOCK": frames A, B, C; references 2 3 2 1 5 2 4 5 3 2 5 2.
    // As in the lecture, a newly loaded page starts with its reference bit 0,
    // and a hit sets it to 1. (In our kernel the retried access sets A at
    // once instead, see D5.)
    std::vector<int> page(3, -1);              // which page each frame holds
    Frames f(3, used(false));
    uint32_t hand = 0;
    int faults = 0;
    for (int ref : {2, 3, 2, 1, 5, 2, 4, 5, 3, 2, 5, 2}) {
        bool hit = false;
        for (uint32_t i = 0; i < 3; i++)
            if (page[i] == ref) { f[i].accessed = true; hit = true; }
        if (hit) continue;
        faults++;
        uint32_t into = 3;
        for (uint32_t i = 0; i < 3 && into == 3; i++)
            if (page[i] == -1) into = i;    // a free frame: no replacement yet
        if (into == 3) {
            const auto c = kernel::clock_choose(f, hand);
            CHECK(c.has_value());
            if (!c) return;
            for (uint32_t i : c->cleared) f[i].accessed = false;
            into = c->victim;
            hand = c->hand;
        }
        page[into] = ref;
        f[into].accessed = false;
    }
    CHECK_EQ(faults, 6);
    // The last row of the lecture's table: A = 2, B = 5, C = 3.
    CHECK(page == (std::vector<int>{2, 5, 3}));
}
