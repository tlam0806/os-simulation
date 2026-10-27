// The kernel core: traps, the process table, and the scheduler.
// System calls are in syscalls.cpp.
#include <cassert>
#include <memory>

#include "kernel/kernel.hpp"
#include "loader/elf.hpp"
#include "util/variant.hpp"

namespace kernel {

uint32_t Kernel::create_process(Process&& process) {
    uint32_t pid = ++next_pid_;
    process.pid = pid;
    process_table_.emplace(process.pid, std::move(process));
    return pid;
}

std::variant<Killed, Continue> Kernel::handle_page_fault(const cpu::Trap& trap) {
    uint32_t addr = trap.tval;
    if (!in_region(addr)) {
        return Killed{trap};
    }
    // the address is valid, inside the region
    uint32_t vpn = mmu::get_vpn(addr);
    std::optional<mmu::Pte> found = page_table().lookup(vpn);

    if (!found) {
        // not mapped yet, should create a new one
        if (!map_page(vpn)) {
            return Killed{trap};
        } else {
            return Continue{};
        }
    }
    assert(found);
    if (trap.cause == cpu::Cause::store_page_fault && found->cow) { // copy-on-write
        auto new_pte = handle_cow(*found);
        if (!new_pte) {
            return Killed{trap};
        }
        page_table().map(vpn, *new_pte);
        return Continue{};
    }
    return Killed{trap};
}

std::variant<Exited, Killed, Continue, Switch> Kernel::handle_trap(const cpu::Trap& trap) {
    switch (trap.cause) {
        case cpu::Cause::ecall :
            return util::convert_variant<Exited, Killed, Continue, Switch>(handle_syscall());
        case cpu::Cause::timer_interrupt :
            return Switch{};
        case cpu::Cause::load_page_fault :
        case cpu::Cause::store_page_fault : 
            return util::convert_variant<Exited, Killed, Continue, Switch>(handle_page_fault(trap));
        default :
            return Killed{trap};   // any fault ends the process
    }
}

// ---- processes ----

mmu::PageTable Kernel::share_table(mmu::PageTable& other) {
    mmu::PageTable shared;
    std::vector<uint32_t> pages = other.mapped_pages();
    for (uint32_t vpn : pages) {
        std::optional<mmu::Pte> pte = other.lookup(vpn);
        assert(pte); // should be in the old page table
        uint32_t frame = pte->frame;
        frame_allocator().share(frame);
        if (pte->write) {
            pte->write = false;
            pte->cow = true;
            other.map(vpn, *pte);
        } 
        shared.map(vpn, *pte);
    }
    return shared;
}

std::vector<uint32_t> Kernel::get_running_children_process(uint32_t parent_pid, uint32_t required_pid) {
    const uint32_t any = static_cast<uint32_t>(-1);
    std::vector<uint32_t> returned;
    for (const auto& [children_pid, process] : process_table_) {
        if (process.parent_pid != parent_pid) continue;
        if (required_pid != any && required_pid != children_pid) continue;
        returned.push_back(children_pid);
    }
    return returned;
}

std::vector<uint32_t> Kernel::get_ended_children_process(uint32_t parent_pid, uint32_t required_pid) {
    std::vector<uint32_t> returned;
    const uint32_t any = static_cast<uint32_t>(-1);
    for (const auto& [children_pid, ending] : ending_table_) {
        if (ending.parent_pid != parent_pid) continue;
        if (required_pid != any && required_pid != children_pid) continue;
        if (ending.collected) continue;
        returned.push_back(children_pid);
    }
    return returned;
}

std::variant<uint32_t, std::string> Kernel::spawn(std::span<const uint8_t> elf) {
    Process process;
    std::variant<loader::Program, std::string> load_result = loader::load_elf(elf, ram(), frame_allocator(), process.page_table);
    if (auto* message = std::get_if<std::string>(&load_result)) {
        return *message;
    }
    loader::Program& program = std::get<loader::Program>(load_result);
    process.brk = process.initial_break = program.initial_break;
    process.context.pc = program.entry;
    process.context.regs[isa::reg::sp] = static_cast<uint32_t>(loader::kStackTop);
    return create_process(std::move(process));
}

const Process* Kernel::process(uint32_t pid) const {
    auto it = process_table_.find(pid);
    if (it == process_table_.end()) {
        return nullptr;
    }
    return std::addressof(it->second);
}

size_t Kernel::process_count() const {
    return process_table_.size();
}

Process& Kernel::find_next_process(uint32_t last_exec) {
    assert(process_count());
    while (true) {
        auto it = process_table_.upper_bound(last_exec);
        if (it == process_table_.end()) {
            it = process_table_.begin();
        }
        if (!it->second.blocked) {
            return it->second;
        }
        last_exec = it->first;
    }
}

void Kernel::end_process(Process& process, std::variant<Exited, Killed> status) {
    for (auto& [pid, other] : process_table_) {
        if (other.parent_pid == process.pid) {
            other.parent_pid = init;
        }
    }
    for (auto& [pid, ending] : ending_table_) {
        if (ending.parent_pid == process.pid) {
            ending.parent_pid = init;
        }
    }
    Ending ending{process.parent_pid, status, false};
    ending_table_.emplace(process.pid, ending);
    for (uint32_t vpn : process.page_table.mapped_pages()) {
        uint32_t frame = process.page_table.lookup(vpn)->frame;
        frame_allocator().free(frame);
    }
    // the parent is no longer blocked
    auto parent_it = process_table_.find(process.parent_pid);
    if (parent_it != process_table_.end()) {
        parent_it->second.blocked = false;
    }
    process_table_.erase(process.pid); // should be last, because process is deleted
}

std::optional<Ending> Kernel::ending(uint32_t pid) const {
    auto it = ending_table_.find(pid);
    if (it == ending_table_.end()) {
        return std::nullopt;
    } else {
        return it->second;
    }
}

// ---- scheduling ----

void Kernel::save_process(Process& process) {
    process.context = cpu().context();
}

void Kernel::load_process(Process& process) {
    executing_pid = process.pid;
    cpu().set_context(process.context);
    cpu().set_page_table(&process.page_table);
}

// Runs the next process for one turn; returns how many of max_steps are left.
uint64_t Kernel::run_helper(uint64_t max_steps) {
    Process& process = find_next_process(executing_pid);
    load_process(process);
    machine_.set_timer(machine_.steps() + kQuantum);
    // repeatedly run the process
    while (true) {
        // update taken steps to remaining steps
        uint64_t old_steps = machine_.steps();
        std::optional<cpu::Trap> run_result = machine_.run(max_steps);
        uint64_t taken_steps = machine_.steps() - old_steps;
        assert(taken_steps <= max_steps);
        max_steps -= taken_steps;
        // check the trap
        if (run_result) {
            const cpu::Trap& trap = *run_result;
            std::variant<Exited, Killed, Continue, Switch> handle_result = handle_trap(trap);
            if (std::holds_alternative<Switch>(handle_result)) {
                save_process(process);
                return max_steps;
            } else if (std::holds_alternative<Continue>(handle_result)) {
                continue;
            } else { // exited or killed
                end_process(process, util::convert_variant<Exited, Killed>(handle_result));
                return max_steps;
            }
        } else { // should be step limit
            save_process(process);
            return max_steps;
        }
    }
}

std::variant<AllDone, StepLimit> Kernel::run_all(uint64_t max_steps) {
    while (max_steps > 0 && process_count() > 0) {
        max_steps = run_helper(max_steps);
    }
    if (process_count() == 0) {
        return AllDone{};
    }
    assert(max_steps == 0);
    return StepLimit{};
}

}  // namespace kernel
