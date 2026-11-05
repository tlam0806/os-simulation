// The kernel core: traps, the process table, and the scheduler.
// System calls are in syscalls.cpp.
#include <cassert>
#include <memory>

#include "kernel/kernel.hpp"
#include "loader/elf.hpp"
#include "util/variant.hpp"

namespace kernel {

uint32_t Kernel::create_process(Process&& process, const cpu::Context& context) {
    uint32_t pid = ++next_pid_;
    process.pid = pid;
    Thread thread{.tid = pid, .pid = pid, .context = context};
    process.threads.push_back(thread.tid);
    thread_table_.emplace(pid, std::move(thread));
    process_table_.emplace(pid, std::move(process));
    return pid;
}

std::variant<Exited, Killed, Continue, Switch, ThreadExited> Kernel::handle_trap(const cpu::Trap& trap) {
    switch (trap.cause) {
        case cpu::Cause::ecall :
            return util::convert_variant<Exited, Killed, Continue, Switch, ThreadExited>(handle_syscall());
        case cpu::Cause::disk_interrupt :
            return handle_disk_interrupt();        
        case cpu::Cause::timer_interrupt :
            return Switch{};
        case cpu::Cause::load_page_fault :
        case cpu::Cause::store_page_fault : 
        case cpu::Cause::instruction_page_fault :
            return util::convert_variant<Exited, Killed, Continue, Switch, ThreadExited>(handle_page_fault(trap));
        default :
            return Killed{trap};   // any fault ends the process
    }
}

// ---- processes ----

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
    cpu::Context context{};
    context.pc = program.entry;
    context.regs[isa::reg::sp] = static_cast<uint32_t>(loader::kStackTop);
    return create_process(std::move(process), context);
}

const Process* Kernel::process(uint32_t pid) const {
    auto it = process_table_.find(pid);
    if (it == process_table_.end()) {
        return nullptr;
    }
    return std::addressof(it->second);
}

const Thread* Kernel::thread(uint32_t tid) const {
    auto it = thread_table_.find(tid);
    if (it == thread_table_.end()) {
        return nullptr;
    }
    return std::addressof(it->second);
}

size_t Kernel::process_count() const {
    return process_table_.size();
}

Thread* Kernel::find_next_thread(uint32_t last_exec) {
    assert(process_count());
    uint32_t cur_pid = last_exec;
    uint32_t thread_count = thread_table_.size();
    while (thread_count--) {
        auto it = thread_table_.upper_bound(cur_pid);
        if (it == thread_table_.end()) {
            it = thread_table_.begin();
        }
        if (it->second.blocked == BlockReason::None) {
            return std::addressof(it->second);
        }
        cur_pid = it->first;
    } 
    // no thread is runnable
    return nullptr;
}

void Kernel::end_all_threads(Process& process, std::variant<Exited, Killed> status) {
    process.status = status;
    const std::vector<uint32_t> threads = process.threads;
    for (uint32_t tid : threads) {
        assert(thread_table_.find(tid) != thread_table_.end());
        auto thread_it = thread_table_.find(tid);
        assert(thread_it != thread_table_.end());
        Thread& thread = thread_it->second;
        if (thread.blocked == BlockReason::IO) {
            thread.must_end = true;
        } else {
            end_thread(tid);
        }
    }
}

void Kernel::end_thread(uint32_t tid) {
    assert(thread_table_.find(tid) != thread_table_.end());
    auto thread_it = thread_table_.find(tid);
    assert(thread_it != thread_table_.end());
    Thread& thread = thread_it->second;
    Process& process = process_table_.find(thread.pid)->second;
    uint32_t pid = process.pid;
    std::erase(process.threads, tid);
    thread_table_.erase(tid);
    if (process.threads.empty()) {
        end_process(pid);
    } else {
        // There are still threads in the process, so we don't end it yet
        for (const uint32_t tid : process.threads) {
            auto thread_it = thread_table_.find(tid);
            assert(thread_it != thread_table_.end());
            Thread& thread = thread_it->second;
            if (thread.blocked == BlockReason::Wait) {
                thread.blocked = BlockReason::None;
            }
        }
    }
}

void Kernel::end_process(uint32_t pid) {
    auto it = process_table_.find(pid);
    assert(it != process_table_.end());
    Process& process = it->second;
    for (uint32_t tid : process.threads) {
        thread_table_.erase(tid);
    }
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
    Ending ending{process.parent_pid, *process.status, false};
    ending_table_.emplace(process.pid, ending);
    free_address_space(process.page_table);
    for (auto& fd : process.fd_table) {
        if (fd) {
            release_open_file(*fd);
            fd.reset();
        }
    }
    // the parent is no longer blocked
    auto parent_it = process_table_.find(process.parent_pid);
    if (parent_it != process_table_.end()) {
        const Process& parent = parent_it->second;
        for (const uint32_t tid : parent.threads) {
            auto thread_it = thread_table_.find(tid);
            assert(thread_it != thread_table_.end());
            Thread& thread = thread_it->second;
            if (thread.blocked == BlockReason::Wait) {
                thread.blocked = BlockReason::None;
                thread.waiting_block.reset();
            }
        }
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

void Kernel::save_thread(Thread& thread) {
    thread.context = cpu().context();
}

void Kernel::load_thread(Thread& thread) {
    executing_tid_ = thread.tid;
    cpu().set_context(thread.context);
    cpu().set_page_table(&process().page_table);
}

// Runs the next thread for one turn; returns how many of max_steps are left.
uint64_t Kernel::run_helper(uint64_t max_steps) {
    Thread* maybe_thread = find_next_thread(executing_tid_);
    if (!maybe_thread) { // every thread is blocked: wait for an interrupt
        uint64_t old_steps = machine_.steps();
        machine_.set_timer(machine_.steps() + max_steps);
        auto trap = machine_.wait_for_interrupt();
        uint64_t taken_steps = machine_.steps() - old_steps;
        assert(taken_steps <= max_steps);
        max_steps -= taken_steps;
        if (!trap) { // no interrupt is ever due
            return max_steps;
        }
        if (trap->cause == cpu::Cause::disk_interrupt) {
            handle_disk_interrupt();
        }
        return max_steps;
    }
    Thread& thread = *maybe_thread;
    if (thread.must_end) {
        end_thread(thread.tid);
        return max_steps;
    }
    load_thread(thread);
    machine_.set_timer(machine_.steps() + kQuantum);
    // repeatedly run the thread
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
            std::variant<Exited, Killed, Continue, Switch, ThreadExited> handle_result = handle_trap(trap);
            if (std::holds_alternative<Switch>(handle_result)) {
                save_thread(thread);
                return max_steps;
            } else if (std::holds_alternative<Continue>(handle_result)) {
                continue;
            } else if (auto* exited = std::get_if<Exited>(&handle_result)) {
                // end_process(process(), *exited);
                end_all_threads(process(), *exited);
                return max_steps;
            } else if (auto* thread_exited = std::get_if<ThreadExited>(&handle_result)) {
                uint32_t tid = thread.tid;
                process().status = Exited{thread_exited->status};
                process().ended_threads.emplace(tid, thread_exited->status);
                end_thread(tid);
                return max_steps;
            } else { // killed
                auto* killed = std::get_if<Killed>(&handle_result);
                end_all_threads(process(), *killed);
                return max_steps;   
            }
        } else { // should be step limit
            save_thread(thread);
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
