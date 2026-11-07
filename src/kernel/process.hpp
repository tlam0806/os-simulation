// One running program, as the kernel sees it.
#pragma once

#include <cstdint>

#include "cpu/cpu.hpp"
#include "mmu/page_table.hpp"

namespace kernel {

const uint32_t init = 0;
inline constexpr uint32_t kMaxFileDescriptors = 16;
inline constexpr uint32_t kPipeSize = 4096;        // bytes, one page


struct Exited {
    int32_t exit_status = 0;    
};
struct Killed {
    cpu::Trap trap;
};

enum class BlockReason {
    Wait,
    IO,
    Pipe,   // waiting to read from or write to a pipe (spec F1)
    None
};

struct Pipe {
    std::array<uint8_t, kPipeSize> buffer{};
    uint32_t head = 0; // next byte to read
    uint32_t count = 0; // how many bytes are in the buffer
    bool read_open = true;  // the read end is open
    bool write_open = true; // the write end is open
};

struct Thread {
    uint32_t tid;
    uint32_t pid;
    cpu::Context context{};
    BlockReason blocked = BlockReason::None;
    std::optional<uint32_t> waiting_block;
    std::optional<uint32_t> waiting_page;
    bool must_end = false;
    Pipe* waiting_pipe = nullptr; // the pipe the process is waiting on (spec F1)
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
    std::map<uint32_t, uint32_t> attachments; // starting address -> region id 
};

}  // namespace kernel
