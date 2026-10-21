#include "kernel/kernel.hpp"
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

std::variant<Exited, Continue> Kernel::handle_syscall() {
    cpu::Cpu& cpu = machine_.cpu();
    uint32_t a7_value = cpu.reg(static_cast<uint8_t>(isa::reg::a7));
    if (a7_value == 1) {
        int32_t a0_value = static_cast<int32_t>(cpu.reg(static_cast<uint8_t>(isa::reg::a0)));
        return Exited{a0_value};
    } else {
        constexpr int32_t kENOSYS = 38;
        cpu.set_reg(static_cast<uint8_t>(isa::reg::a0), static_cast<uint32_t>(-kENOSYS));
        cpu.pc += 4;
        return Continue{};
    }
}

}