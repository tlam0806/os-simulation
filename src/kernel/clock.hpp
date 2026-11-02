// CLOCK page replacement (spec D3), as a function of the frames' states. The
// kernel builds the frames' states from its page tables, calls clock_choose,
// then applies the result: clears the A bits it lists, evicts the victim.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace kernel {

// One frame as CLOCK sees it.
struct ClockFrame {
    bool candidate = false;   // may be evicted (spec D3): one private user page, no disk transfer
    bool accessed = false;    // that page's A bit
};

struct ClockChoice {
    uint32_t victim;                 // the frame to evict
    uint32_t hand;                   // where the hand is afterwards: just past the victim
    std::vector<uint32_t> cleared;   // the frames whose A bit was cleared, in the order the hand passed them
};

// Starting at frame `hand` (< frames.size()), moves the hand one frame at a time,
// wrapping around: skips non-candidates; clears a candidate's A bit and skips it
// if set; stops at the first candidate whose A bit is clear: the victim.
// nullopt if no frame is a candidate (and then nothing is cleared).
std::optional<ClockChoice> clock_choose(std::span<const ClockFrame> frames, uint32_t hand);

}  // namespace kernel
