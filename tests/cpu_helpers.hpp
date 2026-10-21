// Shared helpers for the CPU tests.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <ostream>

#include "cpu/cpu.hpp"
#include "isa/isa.hpp"

namespace cpu {
// Let CHECK_EQ print a Cause when a check fails.
inline std::ostream& operator<<(std::ostream& os, Cause c) { return os << static_cast<uint32_t>(c); }
}  // namespace cpu

// A CPU with its own RAM.
struct Machine {
    mem::Ram ram;
    cpu::Cpu cpu{ram};

    // Encodes `program` into RAM starting at `addr`, and points pc at it.
    void load(uint32_t addr, std::initializer_list<isa::Instruction> program) {
        cpu.pc = addr;
        for (const isa::Instruction& inst : program) {
            ram.write(addr, 4, isa::encode(inst));
            addr += 4;
        }
    }
};
