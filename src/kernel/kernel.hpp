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
// using Ending = std::variant<Exited, Killed>;

struct Ending {
    uint32_t parent_pid = 0;
    std::variant<Exited, Killed> reason;
    bool collected = false;
};

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

    std::optional<std::vector<uint8_t>> read_memory_on_ram(uint32_t addr, uint32_t bytes_count); 

    // return false when theres no frame left on ram or the address is invalid
    bool write_memory_on_ram(uint32_t addr, std::span<uint8_t> buffer);  

    Exited sys_exit(); 

    Continue sys_write(); 

    Continue sys_sbrk(); 

    Continue sys_fork();

    std::variant<Continue, Switch> sys_wait();

    Continue sys_exec();

    Continue sys_other(); 

    Continue sys_getpid();

    std::variant<Killed, Continue> handle_page_fault(const cpu::Trap& trap);

    Switch sys_yield();

    mem::Ram& ram() {
        return machine_.ram();
    }

    cpu::Cpu& cpu() {
        return machine_.cpu();
    }

    Process& process() {  // context is not up to date when the process is running, needs to load again
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

    std::optional<mmu::PageTable> inherit_table(const mmu::PageTable& other); // null if theres not enough frame to allocate

    void copy_frame(uint32_t old_frame, uint32_t new_frame);

    const mmu::PageTable& page_table() const {
        const Process& process = process_table_.find(executing_pid)->second;
        return process.page_table;
    }

    mmu::FrameAllocator& frame_allocator() {
        return machine_.frame_allocator();
    }

    bool in_region(uint64_t addr);

    bool map_page(uint32_t vpn);

    bool unmap_page(uint32_t vpn);

    void zero_frame(uint32_t frame);

    std::optional<uint32_t> try_access(mmu::PageTable& table, uint32_t addr, mmu::Access access);

    Process& find_next_process(uint32_t last_exec);

    void end_process(Process& process, std::variant<Exited, Killed> status);

    void save_process(Process& process);
    
    void load_process(Process& process);

    uint64_t run_helper(uint64_t max_step);
    
    uint32_t create_process(Process&& process);

    std::vector<uint32_t> get_running_children_process(uint32_t parent_pid, uint32_t pid);
    std::vector<uint32_t> get_ended_children_process(uint32_t parent_pid, uint32_t pid);
};


}