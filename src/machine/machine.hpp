#pragma once
#include "mem/mem.hpp"
#include "cpu/cpu.hpp"
#include "mmu/frame_allocator.hpp"
#include <span>

namespace machine {

class Machine {
public :

    std::optional<std::string> load(std::span<const uint8_t> bytes);

    std::optional<cpu::Trap> run(std::size_t max_step);

    // Timer (spec A4). The clock is steps(). Before each instruction, if
    // steps() >= deadline, run() returns a Cause::timer_interrupt trap instead
    // of running it (pc = that instruction, tval = 0). Taking the interrupt
    // does not advance the clock, and it keeps firing until the deadline is
    // moved or the timer is cleared. The timer is off until set.
    void set_timer(uint64_t deadline);
    void clear_timer();

    const cpu::Cpu& cpu() const {
        return cpu_;
    }

    cpu::Cpu& cpu() {
        return cpu_;
    }

    mem::Ram& ram() {
        return ram_;
    }

    mmu::FrameAllocator& frame_allocator() {
        return frame_allocator_;
    }

    mmu::PageTable& page_table() {
        return page_table_;
    }

    uint64_t steps() const {
        return steps_;
    }

private :
    static constexpr uint64_t kTimerOff = UINT64_MAX;
    mmu::FrameAllocator frame_allocator_{};
    mmu::PageTable page_table_{};
    mem::Ram ram_{};
    cpu::Cpu cpu_{ram_};
    uint64_t steps_{};
    uint64_t deadline_ = kTimerOff;
};

}