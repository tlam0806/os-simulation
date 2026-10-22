// Loads ELF32 RISC-V executables into a fresh address space (docs/machine-spec.md, B1-B2).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <variant>

#include "mem/mem.hpp"
#include "mmu/frame_allocator.hpp"
#include "mmu/page_table.hpp"

namespace loader {

// Address-space layout (spec B1).
inline constexpr uint32_t kMinLoadAddress = 0x00010000;   // below: the null guard
inline constexpr uint32_t kStackRegion = 0x7ff00000;      // segments must end at or below this
inline constexpr uint32_t kStackTop = 0x80000000;         // initial sp; also the end of user space
inline constexpr uint32_t kInitialStackPages = 16;        // 64 KiB mapped below kStackTop at load

struct Program {
    uint32_t entry;           // where execution starts (e_entry)
    uint32_t initial_break;   // end of the highest segment, rounded up to a page: the heap starts here
};

// Builds the address space of `file` in `table`:
//   - every page touched by a PT_LOAD segment gets a frame from `frames`,
//     zero-filled, mapped with the segment's permissions (p_flags R/W/X; a page
//     shared by two segments gets both segments' permissions);
//   - the segment's file bytes are copied in; the rest stays zero (.bss);
//   - kInitialStackPages pages just below kStackTop are mapped R+W, zeroed.
// Other segment types are ignored. Pages are given frames in the order their
// segments appear in the file (low to high within a segment), then the stack.
//
// Returns an error message if the file is not a valid ELF32 little-endian
// RISC-V executable, a segment is malformed or outside
// [kMinLoadAddress, kStackRegion), or there aren't enough free frames.
// On error, `ram`, `frames` and `table` are left unchanged.
//
// It does not touch the CPU: the caller sets pc, sp and the CPU's page table.
std::variant<Program, std::string> load_elf(std::span<const uint8_t> file, mem::Ram& ram,
                                            mmu::FrameAllocator& frames, mmu::PageTable& table);

}  // namespace loader
