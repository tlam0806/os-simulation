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

std::variant<Exited, Killed, StepLimit> Kernel::run(uint64_t max_steps) {
    uint64_t starting_step = machine_.steps();
    uint64_t maximum_step = starting_step + max_steps;
    while (true) {
        uint64_t current_step = machine_.steps();
        std::optional<cpu::Trap> run_result = machine_.run(maximum_step - current_step);
        if (run_result) {
            cpu::Trap trap = *run_result;
            std::variant<Exited, Killed, Continue> handle_result = handle_trap(trap);
            if (std::holds_alternative<Continue>(handle_result)) {
                machine_.cpu().pc += 4;
                continue;
            }
            return convert_variant<Exited, Killed, StepLimit>(handle_result);
        } else {
            return StepLimit{};
        }
    }
    return StepLimit{};
}

std::variant<Exited, Killed, Continue> Kernel::handle_trap(const cpu::Trap& trap) {
    if (trap.cause == cpu::Cause::ecall) {
        return convert_variant<Exited, Killed, Continue>(handle_syscall());
    } else {
        return Killed{trap};
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
    mmu::PageTable& page_table = machine_.page_table();
    while (bytes_count > 0) {
        std::optional<uint32_t> physical_addr = mmu::translate(page_table, addr, mmu::Access::load);
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
        machine_.set_break(next_break);
    }
    while (current_break() > new_break) {
        uint32_t next_break = current_break() - 1;
        uint32_t cur_vpn = mmu::get_vpn(next_break);
        uint32_t next_vpn = mmu::get_vpn(next_break - 1);
        if (cur_vpn != next_vpn) {
            unmap_page(cur_vpn);
        }
        machine_.set_break(next_break);
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

}