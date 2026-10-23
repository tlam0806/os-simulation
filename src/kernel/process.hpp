// One running program, as the kernel sees it.
#pragma once

#include <cstdint>

#include "cpu/cpu.hpp"
#include "mmu/page_table.hpp"

namespace kernel {

const uint32_t init = 0;

struct Process {
    uint32_t pid = 0;
    uint32_t parent_pid = 0;
    cpu::Context context{};        // registers and pc, saved while it isn't running
    mmu::PageTable page_table;   // its own address space (spec A2, B1)
    uint32_t initial_break = 0;  // start of its heap; sbrk can't go below this
    uint32_t brk = 0;            // current end of its heap
};

}  // namespace kernel
