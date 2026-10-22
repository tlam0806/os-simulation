// The kernel core: traps, the process table, and the scheduler.
// System calls are in syscalls.cpp.
#include <cassert>
#include <memory>

#include "kernel/kernel.hpp"
#include "loader/elf.hpp"
#include "util/variant.hpp"

namespace kernel {

std::variant<Exited, Killed, Continue, Switch> Kernel::handle_trap(const cpu::Trap& trap) {
    switch (trap.cause) {
        case cpu::Cause::ecall :
            return util::convert_variant<Exited, Killed, Continue, Switch>(handle_syscall());
        case cpu::Cause::timer_interrupt :
            return Switch{};
        default :
            return Killed{trap};   // any fault ends the process
    }
}

// ---- processes ----

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
    uint32_t pid = ++next_pid_;
    process.pid = pid;
    process_table_.emplace(process.pid, std::move(process));
    return pid;
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
    auto it = process_table_.upper_bound(last_exec);
    if (it == process_table_.end()) {
        it = process_table_.begin();
    }
    assert(it != process_table_.end());
    return it->second;
}

void Kernel::end_process(Process& process, Ending ending) {
    ending_table_.emplace(process.pid, ending);
    for (uint32_t vpn : process.page_table.mapped_pages()) {
        uint32_t frame = process.page_table.lookup(vpn)->frame;
        frame_allocator().free(frame);
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
