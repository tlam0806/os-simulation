#include "kernel/kernel.hpp"
#include "loader/elf.hpp"
#include <cstdlib>
#include <cassert>

namespace kernel {

template<typename... T>
constexpr bool contains = false;

template<typename T, typename U, typename... Rest>
constexpr bool contains<T, U, Rest...> = std::is_same_v<T, U> || contains<T, Rest...>;


template<std::size_t index, typename... ResultTypes, typename... ArgTypes>
static inline std::variant<ResultTypes...> convert_helper(const std::variant<ArgTypes...>& var) {
    if constexpr (index == sizeof...(ResultTypes)) {
        std::abort();
    } else {
        using type = std::variant_alternative_t<index, std::variant<ResultTypes...>>;
        if constexpr (contains<type, ArgTypes...>) {
            auto* tmp = std::get_if<type>(&var);
            if (tmp) {
                return *tmp;
            } 
        }
        return convert_helper<index + 1, ResultTypes...>(var);
    }
}

template<typename... ResultTypes, typename... ArgTypes>
static inline std::variant<ResultTypes...> convert_variant(const std::variant<ArgTypes...>& var) {
    return convert_helper<0, ResultTypes...>(var);
}

std::variant<Exited, Killed, Continue, Switch> Kernel::handle_trap(const cpu::Trap& trap) {
    switch (trap.cause) {
        case cpu::Cause::ecall :
            return convert_variant<Exited, Killed, Continue, Switch>(handle_syscall());
        case cpu::Cause::timer_interrupt :
            return Switch{};
        default :
            return Killed{trap};   // any fault ends the process
    }
}

void Kernel::zero_frame(uint32_t frame) {
    for (uint32_t addr = frame * mmu::kPageSize; addr < frame * mmu::kPageSize + mmu::kPageSize; addr += 4) {
        bool writable = ram().write(addr, 4, 0);
        assert(writable);
    }
}

std::optional<std::vector<uint8_t>> Kernel::read_memory_on_ram(uint32_t addr, uint32_t bytes_count) const {
    std::vector<uint8_t> returned;
    mem::Ram& ram = machine_.ram();
    while (bytes_count > 0) {
        std::optional<uint32_t> physical_addr = mmu::translate(page_table(), addr, mmu::Access::load);
        if (!physical_addr) {
            return std::nullopt;
        }
        std::optional<uint32_t> readed = ram.read(*physical_addr, 1);
        if (!readed) {
            return std::nullopt;
        }
        returned.push_back(static_cast<uint8_t>(*readed));
        ++addr;
        --bytes_count;
    }
    return returned;
}

Exited Kernel::sys_exit() {
    int32_t a0_value = static_cast<int32_t>(cpu().reg(isa::reg::a0));
    return Exited{a0_value};
}

Continue Kernel::sys_write() {
    constexpr int32_t kEFAULT = 14;
    constexpr int32_t kEBADF = 9;
    int32_t fd = static_cast<int32_t>(cpu().reg(isa::reg::a0));
    uint32_t buffer_addr = cpu().reg(isa::reg::a1);
    uint32_t bytes_count = cpu().reg(isa::reg::a2);
    if (fd != 1 && fd != 2) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-kEBADF));
        return Continue{};
    }
    std::ostream& output = (fd == 1 ? out_ : err_);
    std::optional<std::vector<uint8_t>> read_result = read_memory_on_ram(buffer_addr, bytes_count);
    if (!read_result) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-kEFAULT));
        return Continue{};
    } 
    for (auto byte : *read_result) {
        output << byte;
    }
    cpu().set_reg(isa::reg::a0, bytes_count);
    return Continue{};  
}

Continue Kernel::sys_sbrk() {
    int32_t n = static_cast<int32_t>(cpu().reg(isa::reg::a0));
    int64_t new_break = current_break() + n;
    uint32_t old_break = current_break();
    const int32_t kEINVAL = 22;
    const int32_t kENOMEM = 12;
    if (new_break < initial_break()) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-kEINVAL));
        return Continue{};
    }
    if (new_break > loader::kStackRegion) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-kENOMEM));
        return Continue{};
    }
    if (n > 0 && mmu::get_vpn(new_break - 1) - mmu::get_vpn(current_break() - 1) > frame_allocator().free_count()) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-kENOMEM));
        return Continue{};
    }
    while (current_break() < new_break) {
        uint32_t next_break = current_break() + 1;
        uint32_t cur_vpn = mmu::get_vpn(current_break() - 1);
        uint32_t next_vpn = mmu::get_vpn(current_break());
        if (cur_vpn != next_vpn) {
            map_page(next_vpn);
        }
        process().brk = next_break;
    }
    while (current_break() > new_break) {
        uint32_t next_break = current_break() - 1;
        uint32_t cur_vpn = mmu::get_vpn(next_break);
        uint32_t next_vpn = mmu::get_vpn(next_break - 1);
        if (cur_vpn != next_vpn) {
            unmap_page(cur_vpn);
        }
        process().brk = next_break;
    }
    assert(current_break() == new_break);
    cpu().set_reg(isa::reg::a0, old_break);
    return Continue{};
}

Continue Kernel::sys_other() {
    constexpr int32_t kENOSYS = 38;
    cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-kENOSYS));
    return Continue{};
}

void Kernel::map_page(uint32_t vpn) {
    std::optional<uint32_t> physical_frame = frame_allocator().allocate();
    assert(physical_frame);
    mmu::Pte pte{*physical_frame, true, true, false}; 
    zero_frame(*physical_frame);
    page_table().map(vpn, pte);
}

void Kernel::unmap_page(uint32_t vpn) {
    auto pte = page_table().lookup(vpn);
    assert(pte);
    frame_allocator().free(pte->frame);
    page_table().unmap(vpn);
}

std::variant<Exited, Continue> Kernel::handle_syscall() {
    uint32_t a7_value = cpu().reg(isa::reg::a7);
    switch (a7_value) {
        case 1 : 
            return sys_exit();
        case 2 : 
            return sys_write();
        case 3 : 
            return sys_sbrk();
        default : 
            return sys_other();
    }   
}

// TODO: implement. tests/kernel_process_test.cpp describes the expected behavior.
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
    uint32_t pid = ++next_pid_;;
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

Process& Kernel::find_next_process(uint32_t last_exec) { // should not 
    assert(process_count());
    auto it = process_table_.upper_bound(last_exec);
    if (it == process_table_.end()) {
        it = process_table_.begin();
    }
    assert(it != process_table_.end());
    return it->second;
}

size_t Kernel::process_count() const { 
    return process_table_.size();
}

void Kernel::end_process(Process& process, Ending ending) {
    ending_table_.emplace(process.pid, ending);
    for (uint32_t vpn : process.page_table.mapped_pages()) {
        uint32_t frame = process.page_table.lookup(vpn)->frame;
        frame_allocator().free(frame);
    }
    process_table_.erase(process.pid); // should be last, because process is deleted
}

void Kernel::save_process(Process& process) {
    process.context = cpu().context();
}
    
void Kernel::load_process(Process& process) {
    executing_pid = process.pid;
    cpu().set_context(process.context);
    cpu().set_page_table(&process.page_table);
}

uint64_t Kernel::run_helper(uint64_t max_steps) {
    Process& process = find_next_process(executing_pid);
    load_process(process);
    machine_.set_timer(machine_.steps() + kQuantum);
    // repatedly run the process
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
                // return run_all(max_steps);
            } else if (std::holds_alternative<Continue>(handle_result)) { 
                machine_.cpu().pc += 4;
                continue;
            } else { // exited or killed
                end_process(process, convert_variant<Exited, Killed>(handle_result));
                return max_steps;
            }
        } else { // should be step limit
            save_process(process);
            return max_steps;
        }
    }
}

// TODO: implement. tests/kernel_scheduler_test.cpp describes the expected behavior.
std::variant<AllDone, StepLimit> Kernel::run_all(uint64_t max_steps) {  // handle one process, then recursively call run_all again
    while (max_steps > 0 && process_count() > 0) {
        max_steps = run_helper(max_steps);
    }
    if (process_count() == 0) {
        return AllDone{};
    }
    assert(max_steps == 0);
    return StepLimit{};
}

std::optional<Ending> Kernel::ending(uint32_t pid) const { 
    auto it = ending_table_.find(pid);
    if (it == ending_table_.end()) {
        return std::nullopt; 
    } else {
        return it->second;
    }
}

}
