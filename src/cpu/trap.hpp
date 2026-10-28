#pragma once
#include <cstdint>
#include <iostream>
namespace cpu {

// Values match the RISC-V cause codes (docs/machine-spec.md, A3).
// The top bit marks an interrupt; without it, the cause is an exception
// raised by the current instruction.
enum class Cause : uint32_t {
    instruction_address_misaligned = 0,
    illegal_instruction            = 2,
    breakpoint                     = 3,
    load_address_misaligned        = 4,
    store_address_misaligned       = 6,
    ecall                          = 8,
    instruction_page_fault         = 12,
    load_page_fault                = 13,
    store_page_fault               = 15,

    timer_interrupt                = 0x80000005,
    disk_interrupt                 = 0x80000009,
};

struct Trap {
    Cause cause = Cause::illegal_instruction;
    uint32_t pc = 0;
    uint32_t tval = 0;
};

inline std::ostream& operator<<(std::ostream& os, const Trap& trap) {
    return os 
        << "cause = " << static_cast<uint32_t>(trap.cause) << "\n"
        << "pc = " << trap.pc << "\n"
        << "tval= " << trap.tval << "\n";
}

}