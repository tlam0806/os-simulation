// A tiny assembler for test programs: instruction lists with labels patched later.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "isa/isa.hpp"

// System call numbers (spec B3).
inline constexpr int32_t SYS_EXIT = 1, SYS_WRITE = 2, SYS_SBRK = 3, SYS_GETPID = 4, SYS_YIELD = 5,
                         SYS_FORK = 6, SYS_EXEC = 7, SYS_WAIT = 8,
                         SYS_DISK_READ = 9, SYS_DISK_WRITE = 10;

struct Asm {
    using enum isa::Op;
    std::vector<isa::Instruction> code;

    size_t here() const { return code.size(); }
    void emit(isa::Instruction i) { code.push_back(i); }

    // rd = value, for any 32-bit value (lui + addi, like the "li" pseudo-instruction).
    void li(uint8_t rd, int32_t value) {
        const int32_t hi = static_cast<int32_t>((static_cast<uint32_t>(value) + 0x800) & 0xfffff000);
        const int32_t lo = static_cast<int32_t>(static_cast<uint32_t>(value) - static_cast<uint32_t>(hi));
        if (hi == 0) { emit({ADDI, rd, isa::reg::zero, 0, lo}); return; }
        emit({LUI, rd, 0, 0, hi});
        if (lo != 0) emit({ADDI, rd, rd, 0, lo});
    }

    // ecall with a7 = number; arguments must already be in a0..a2.
    void syscall(int32_t number) { li(isa::reg::a7, number); emit({ECALL}); }

    void sbrk(int32_t increment) { li(isa::reg::a0, increment); syscall(SYS_SBRK); }
    void exit_with_a0() { syscall(SYS_EXIT); }
    void exit_with(int32_t status) { li(isa::reg::a0, status); syscall(SYS_EXIT); }

    // write(1, address, length)
    void write_stdout(uint32_t address, int32_t length) {
        li(isa::reg::a0, 1);
        li(isa::reg::a1, static_cast<int32_t>(address));
        li(isa::reg::a2, length);
        syscall(SYS_WRITE);
    }

    // Spins for about 2 * iterations instructions (uses t0).
    void busy_wait(int32_t iterations) {
        li(isa::reg::t0, iterations);
        const size_t loop = here();
        emit({ADDI, isa::reg::t0, isa::reg::t0, 0, -1});
        patch(branch(BNE, isa::reg::t0, isa::reg::zero), loop);
    }

    // Emits a branch whose target is filled in by patch() once it's known.
    size_t branch(isa::Op op, uint8_t rs1, uint8_t rs2) { emit({op, 0, rs1, rs2, 0}); return here() - 1; }
    void patch(size_t at, size_t target) {
        code[at].imm = static_cast<int32_t>((static_cast<int64_t>(target) - static_cast<int64_t>(at)) * 4);
    }
};
