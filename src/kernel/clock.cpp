// CLOCK page replacement (spec D3).
// TODO: a stub so the project builds. tests/clock_test.cpp describes it.
#include "kernel/clock.hpp"

namespace kernel {

std::optional<ClockChoice> clock_choose(std::span<const ClockFrame> frames, uint32_t hand) {
    (void)frames;
    (void)hand;
    bool has_candidate = false;
    for (const auto& frame : frames) {
        if (frame.candidate) {
            has_candidate = true;
            break;
        }
    }
    if (!has_candidate) {
        return std::nullopt;
    }
    std::vector<uint32_t> cleared;
    std::vector<bool> copy_accessed;
    for (const auto& frame : frames) {
        copy_accessed.push_back(frame.accessed);
    }
    while (true) {
        const ClockFrame& frame = frames[hand];
        uint32_t next_hand = (hand + 1) % frames.size();
        if (!frame.candidate) {
            hand = next_hand;
            continue;
        }
        if (copy_accessed[hand]) {
            cleared.push_back(hand);
            copy_accessed[hand] = false;
            hand = next_hand;
            continue;
        }
        return ClockChoice{.victim = hand, .hand = next_hand, .cleared = std::move(cleared)};
    }
}

}  // namespace kernel
