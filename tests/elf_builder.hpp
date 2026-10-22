// Builds small ELF32 RISC-V executables in memory, so tests need no files on disk.
// Layout: ELF header (52 bytes), then program headers (32 bytes each), then
// the segment contents.
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "isa/isa.hpp"

namespace elf_builder {

inline void put16(std::vector<uint8_t>& f, size_t at, uint16_t v) {
    f[at] = v & 0xff;
    f[at + 1] = v >> 8;
}

inline void put32(std::vector<uint8_t>& f, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) f[at + i] = (v >> (8 * i)) & 0xff;
}

inline constexpr uint32_t PT_LOAD = 1;
inline constexpr uint32_t PT_GNU_STACK = 0x6474e551;   // present in real clang/lld output
inline constexpr uint32_t PF_X = 1, PF_W = 2, PF_R = 4;  // p_flags bits

struct Segment {
    uint32_t vaddr;
    std::vector<uint8_t> data;   // the bytes stored in the file (p_filesz = data.size())
    uint32_t memsz = 0;          // 0 means "same as data.size()"
    uint32_t type = PT_LOAD;
    uint32_t flags = PF_R | PF_X;
};

// Field offsets in the ELF header and in a program header.
inline constexpr size_t kEhdrSize = 52, kPhdrSize = 32;
inline constexpr size_t E_TYPE = 16, E_MACHINE = 18, E_ENTRY = 24, E_PHOFF = 28, E_PHNUM = 44;
inline constexpr size_t P_OFFSET = 4, P_FILESZ = 16, P_MEMSZ = 20;

inline std::vector<uint8_t> make_elf(uint32_t entry, const std::vector<Segment>& segments) {
    size_t data_at = kEhdrSize + kPhdrSize * segments.size();
    size_t size = data_at;
    for (const Segment& s : segments) size += s.data.size();
    std::vector<uint8_t> f(size, 0);

    const uint8_t ident[] = {0x7f, 'E', 'L', 'F', 1 /*32-bit*/, 1 /*little-endian*/, 1 /*version*/};
    for (size_t i = 0; i < sizeof ident; i++) f[i] = ident[i];
    put16(f, E_TYPE, 2);          // ET_EXEC
    put16(f, E_MACHINE, 243);     // EM_RISCV
    put32(f, 20, 1);              // e_version
    put32(f, E_ENTRY, entry);
    put32(f, E_PHOFF, kEhdrSize);
    put16(f, 40, kEhdrSize);      // e_ehsize
    put16(f, 42, kPhdrSize);      // e_phentsize
    put16(f, E_PHNUM, static_cast<uint16_t>(segments.size()));

    for (size_t i = 0; i < segments.size(); i++) {
        const Segment& s = segments[i];
        const size_t ph = kEhdrSize + kPhdrSize * i;
        put32(f, ph + 0, s.type);
        put32(f, ph + P_OFFSET, static_cast<uint32_t>(data_at));
        put32(f, ph + 8, s.vaddr);   // p_vaddr
        put32(f, ph + 12, s.vaddr);  // p_paddr
        put32(f, ph + P_FILESZ, static_cast<uint32_t>(s.data.size()));
        put32(f, ph + P_MEMSZ, s.memsz ? s.memsz : static_cast<uint32_t>(s.data.size()));
        put32(f, ph + 24, s.flags);
        put32(f, ph + 28, 0x1000);   // p_align
        for (size_t j = 0; j < s.data.size(); j++) f[data_at + j] = s.data[j];
        data_at += s.data.size();
    }
    return f;
}

// Encodes instructions into little-endian bytes, ready to be a segment's data.
inline std::vector<uint8_t> code(std::initializer_list<isa::Instruction> program) {
    std::vector<uint8_t> bytes;
    for (const isa::Instruction& inst : program) {
        const uint32_t word = isa::encode(inst);
        for (int i = 0; i < 4; i++) bytes.push_back((word >> (8 * i)) & 0xff);
    }
    return bytes;
}

inline std::vector<uint8_t> code(const std::vector<isa::Instruction>& program) {
    std::vector<uint8_t> bytes;
    for (const isa::Instruction& inst : program) {
        const uint32_t word = isa::encode(inst);
        for (int i = 0; i < 4; i++) bytes.push_back((word >> (8 * i)) & 0xff);
    }
    return bytes;
}

// A program whose code starts at 0x10000, which is also its entry point.
inline std::vector<uint8_t> program_elf(std::initializer_list<isa::Instruction> program) {
    return make_elf(0x10000, {{0x10000, code(program)}});
}

}  // namespace elf_builder
