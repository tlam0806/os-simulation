#pragma once
#include <array>
#include <cstdint>
#include <cassert>
#include <variant>

#include "mem/mem.hpp"
#include "cpu/trap.hpp"
#include "isa/isa.hpp"
#include "mmu/page_table.hpp"
#include "cpu/context.hpp"

namespace cpu {

// Everything a program needs to continue later exactly where it stopped:
// its registers and pc (spec A5). The kernel saves one per process.


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

    // Save / restore all registers and pc at once, for context switches.
    // set_context requires regs[0] == 0 (asserted). The page table is not part of
    // the context; the kernel switches it separately with set_page_table.
    Context context() const noexcept;
    void set_context(const Context& context) noexcept;

    // Selects the address space that translates every fetch, load and store
    // (like RISC-V's satp register; spec A2). The kernel switches it when it
    // switches processes. nullptr turns translation off: addresses are then
    // physical and permissions are not checked (milestones 1-2).
    // The Cpu does not own the table; it must outlive its use here.
    void set_page_table(const mmu::PageTable* table) noexcept;

private : 
    std::array<uint32_t, 32> registers{};
    mem::Ram& ram;
    const mmu::PageTable* page_table = nullptr;

    std::variant<uint32_t, Trap> fetch() noexcept;

    std::optional<uint32_t> to_physical(uint32_t vaddr, mmu::Access access) const noexcept;
};

}