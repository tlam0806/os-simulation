#pragma once
#include "mem/mem.hpp"
#include "cpu/cpu.hpp"
#include <span>

namespace machine {

class Machine {
public :
    std::optional<std::string> load(std::span<const uint8_t> bytes);

    std::optional<cpu::Trap> run(std::size_t max_step);

    const cpu::Cpu& cpu() const {
        return cpu_;
    }

    cpu::Cpu& cpu() {
        return cpu_;
    }

    uint64_t steps() const {
        return steps_;
    }

private :
    mem::Ram ram_;
    cpu::Cpu cpu_{ram_};
    uint64_t steps_{};
};

}