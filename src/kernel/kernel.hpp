#pragma once
#include <variant>
#include "machine/machine.hpp"
namespace kernel {

struct Exited {
    int32_t exit_status = 0;    
};
struct Killed {
    cpu::Trap trap;
};
struct StepLimit {
};
struct Continue {
};

class Kernel {
public :
    Kernel(machine::Machine& machine, std::ostream& out, std::ostream& err) : 
        machine_{machine},
        out_{out},
        err_{err}
    {}

    
    std::variant<Exited, Killed, StepLimit> run(uint64_t max_steps);
    
private : 
    machine::Machine& machine_;
    std::ostream& out_;
    std::ostream& err_;
    
    std::variant<Exited, Killed, Continue> handle_trap(const cpu::Trap& trap);
    
    std::variant<Exited, Continue> handle_syscall();

    std::optional<std::vector<uint8_t>> read_memory_on_ram(uint32_t addr, uint32_t bytes_count) const; 

    Exited sys_exit(); // 1 

    Continue sys_write(); // 2

    Continue sys_sbrk(); // 3

    Continue sys_other(); // other

    mem::Ram& ram() {
        return machine_.ram();
    }

    cpu::Cpu& cpu() {
        return machine_.cpu();
    }

    uint32_t current_break() {
        return machine_.current_break();
    }

    uint32_t initial_break() {
        return machine_.initial_break();
    }

    mmu::PageTable& page_table() {
        return machine_.page_table();
    }

    mmu::FrameAllocator& frame_allocator() {
        return machine_.frame_allocator();
    }

    void map_page(uint32_t vpn);

    void unmap_page(uint32_t vpn);

    void zero_frame(uint32_t frame);
};


}