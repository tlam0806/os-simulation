#pragma once
#include <span>
#include <string>
#include <variant>
#include <map>
#include <optional>
#include "kernel/process.hpp"
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
// A syscall finished: resume the same process right after the ecall.
struct Continue {
};
// The process's turn is over (timer interrupt): switch to the next process.
struct Switch {
};

// How a process ended.
using Ending = std::variant<Exited, Killed>;

// run_all() stopped because every process has ended.
struct AllDone {
};

// Instructions a process may run before the timer switches to the next one (spec A4).
inline constexpr uint64_t kQuantum = 1000;

class Kernel {
public :
    Kernel(machine::Machine& machine, std::ostream& out, std::ostream& err) : 
        machine_{machine},
        out_{out},
        err_{err}
    {}
    

    // Creates a new process from an ELF file (spec B2): its own page table,
    // built by the loader with frames from the machine's frame allocator, and
    // a context with pc = entry, sp = loader::kStackTop, all other registers 0.
    // Its break starts at the loader's initial_break.
    // Returns the new pid (1, 2, 3, ... in creation order), or the loader's
    // error message. A failed spawn creates no process and uses no pid.
    // It does not run the process or touch the CPU.
    std::variant<uint32_t, std::string> spawn(std::span<const uint8_t> elf);

    // The process with this pid, or nullptr if there is none.
    const Process* process(uint32_t pid) const;

    // How many processes exist.
    size_t process_count() const;

    // Runs all processes in the table, taking turns (round robin, in pid order),
    // until every process has ended (AllDone) or max_steps instructions have
    // run in this call, counted across all processes (StepLimit).
    //   - Each turn lasts at most kQuantum instructions: the machine timer
    //     interrupts the process, the kernel saves its context and moves on to
    //     the next pid (wrapping around to the lowest).
    //   - A process ends when it calls exit or a trap kills it. The kernel
    //     records how it ended (see ending()), frees all its frames, removes it
    //     from the table, and continues with the next process.
    //   - System calls act on the process that made them: its memory and its break.
    //   - After StepLimit, calling run_all() again continues where it stopped.
    std::variant<AllDone, StepLimit> run_all(uint64_t max_steps);

    // How the process with this pid ended, or nullopt if it hasn't ended
    // (still in the table) or never existed.
    std::optional<Ending> ending(uint32_t pid) const;
    
private : 
    uint32_t executing_pid{};
    uint32_t next_pid_{};
    std::map<uint32_t, Process> process_table_{};
    std::map<uint32_t, Ending> ending_table_{};
    machine::Machine& machine_;
    std::ostream& out_;
    std::ostream& err_;
    
    std::variant<Exited, Killed, Continue, Switch> handle_trap(const cpu::Trap& trap);
    
    std::variant<Exited, Continue, Switch> handle_syscall();

    std::optional<std::vector<uint8_t>> read_memory_on_ram(uint32_t addr, uint32_t bytes_count) const; 

    Exited sys_exit(); // 1 

    Continue sys_write(); // 2

    Continue sys_sbrk(); // 3

    Continue sys_other(); // other

    Continue sys_getpid();

    Switch sys_yield();

    mem::Ram& ram() {
        return machine_.ram();
    }

    cpu::Cpu& cpu() {
        return machine_.cpu();
    }

    Process& process() {
        return process_table_.find(executing_pid)->second;
    }

    uint32_t current_break() {
        Process& process = process_table_.find(executing_pid)->second;
        return process.brk;
    }

    uint32_t initial_break() {
        Process& process = process_table_.find(executing_pid)->second;
        return process.initial_break;
    }

    mmu::PageTable& page_table() {
        Process& process = process_table_.find(executing_pid)->second;
        return process.page_table;
    }

    const mmu::PageTable& page_table() const {
        const Process& process = process_table_.find(executing_pid)->second;
        return process.page_table;
    }

    mmu::FrameAllocator& frame_allocator() {
        return machine_.frame_allocator();
    }

    void map_page(uint32_t vpn);

    void unmap_page(uint32_t vpn);

    void zero_frame(uint32_t frame);

    Process& find_next_process(uint32_t last_exec);

    void end_process(Process& process, Ending ending);

    void save_process(Process& process);
    
    void load_process(Process& process);

    uint64_t run_helper(uint64_t max_step);
};


}