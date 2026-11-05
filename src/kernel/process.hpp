// One running program, as the kernel sees it.
#pragma once

#include <cstdint>

#include "cpu/cpu.hpp"
#include "mmu/page_table.hpp"

namespace kernel {

const uint32_t init = 0;
inline constexpr uint32_t kMaxFileDescriptors = 16;

struct Exited {
    int32_t exit_status = 0;    
};
struct Killed {
    cpu::Trap trap;
};

enum BlockReason {
    Wait,
    IO,
    None
};

struct Thread {
    uint32_t tid;
    uint32_t pid;
    cpu::Context context{};
    BlockReason blocked = BlockReason::None;
    std::optional<uint32_t> waiting_block;
    std::optional<uint32_t> waiting_page;
    bool must_end = false;
};

struct Process {
    std::array<std::optional<uint32_t>, kernel::kMaxFileDescriptors> fd_table{};
    uint32_t parent_pid = 0;
    uint32_t pid = 0;
    mmu::PageTable page_table;   // its own address space (spec A2, B1)
    uint32_t initial_break = 0;  // start of its heap; sbrk can't go below this
    uint32_t brk = 0;            // current end of its heap
    std::vector<uint32_t> threads;
    std::map<uint32_t, int32_t> ended_threads{};
    std::optional<std::variant<Exited, Killed>> status;
};

}  // namespace kernel
