#pragma once
#include <array>
#include <cstdint>
#include <cassert>
#include <variant>

#include "mem/mem.hpp"
#include "cpu/trap.hpp"
#include "isa/isa.hpp"

namespace cpu {

class Cpu {
public : 
    uint32_t pc{};  

    explicit Cpu(mem::Ram& ram) : 
        ram(ram)
    {}

    uint32_t reg(uint8_t register_index) const noexcept {
        assert(register_index < 32);
        return registers[register_index];
    }

    void set_reg(uint8_t register_index, uint32_t value) noexcept {
        assert(register_index < 32);
        if (register_index == 0) {
            return;
        }
        registers[register_index] = value;
    }

    std::optional<Trap> step() noexcept;

private : 
    std::array<uint32_t, 32> registers{};
    mem::Ram& ram;

    std::variant<uint32_t, Trap> fetch() noexcept;
};

}