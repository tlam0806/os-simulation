// Loads ELF32 RISC-V executables into RAM (docs/machine-spec.md, B2).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <variant>

#include "mem/mem.hpp"

namespace loader {

// Programs must not use addresses below this: page 0.. is the null guard (spec B1).
inline constexpr uint32_t kMinLoadAddress = 0x00010000;

struct Program {
    uint32_t entry;   // where execution starts (e_entry)
};

// Copies every PT_LOAD segment of `file` into RAM and zero-fills the rest of
// each segment (.bss). Other segment types are ignored.
//
// Returns an error message if the file is not a valid ELF32 little-endian
// RISC-V executable, or if a segment is malformed or doesn't fit in
// [kMinLoadAddress, end of RAM). On error, RAM is left unchanged.
//
// It does not touch the CPU: the caller sets pc to `entry` and sets sp.
std::variant<Program, std::string> load_elf(std::span<const uint8_t> file, mem::Ram& ram);

}  // namespace loader
