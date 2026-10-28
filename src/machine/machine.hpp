#pragma once
#include "mem/mem.hpp"
#include "cpu/cpu.hpp"
#include "mmu/frame_allocator.hpp"
#include "disk/disk.hpp"
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

    // Disk (spec A6). The kernel submits requests with disk().submit(r, steps()).
    // Before each instruction, run() checks the disk:
    //   - busy and steps() >= its completion_time(): the machine calls
    //     disk().complete(ram()) and the disk interrupt becomes pending.
    //   - a pending disk interrupt is returned as a Cause::disk_interrupt trap
    //     (pc = the instruction that did not run, tval = 0), before the timer
    //     is checked. Like the timer, it does not advance the clock and keeps
    //     firing until acknowledged.
    disk::Disk& disk() {
        return disk_;
    }
    void acknowledge_disk_interrupt();

    // Idle (spec A4, like RISC-V's WFI): runs no instruction. If an interrupt
    // is already due, returns it at once. Otherwise moves the clock forward to
    // the earliest of the timer deadline and the disk's completion time, and
    // returns that interrupt (the disk first if both are due then; a completed
    // disk request is completed exactly as in run()). With nothing that could
    // ever fire, returns nullopt and leaves the clock alone.
    std::optional<cpu::Trap> wait_for_interrupt();

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
    disk::Disk disk_{};
    bool pending_disk_interrupt_ = false;

    std::optional<cpu::Trap> check_interrupts();
    // TODO: whatever you need to remember a pending disk interrupt.
};

}