#include "loader/elf.hpp"

#include <vector>

namespace loader {
namespace {

// ELF32 constants, from the ELF specification.
constexpr size_t kEhdrSize = 52;
constexpr size_t kPhdrSize = 32;
constexpr uint8_t kElfClass32 = 1;
constexpr uint8_t kElfDataLittle = 1;
constexpr uint16_t kTypeExec = 2;
constexpr uint16_t kMachineRiscv = 243;
constexpr uint32_t kPtLoad = 1;

// Little-endian reads. Callers check bounds first.
uint16_t read16(std::span<const uint8_t> f, size_t at) {
    return static_cast<uint16_t>(f[at] | f[at + 1] << 8);
}

uint32_t read32(std::span<const uint8_t> f, size_t at) {
    return uint32_t{f[at]} | uint32_t{f[at + 1]} << 8 | uint32_t{f[at + 2]} << 16 |
           uint32_t{f[at + 3]} << 24;
}

struct Segment {
    uint32_t vaddr;
    uint32_t offset;   // where its bytes start in the file
    uint32_t filesz;   // bytes copied from the file
    uint32_t memsz;    // bytes in memory; the rest after filesz is zero-filled
};

}  // namespace

std::variant<Program, std::string> load_elf(std::span<const uint8_t> file, mem::Ram& ram) {
    // Sizes and offsets are added in 64 bits, so a sum can't wrap around 32 bits.
    if (file.size() < kEhdrSize) return std::string("file too small for an ELF header");
    if (file[0] != 0x7f || file[1] != 'E' || file[2] != 'L' || file[3] != 'F')
        return std::string("not an ELF file");
    if (file[4] != kElfClass32) return std::string("not a 32-bit ELF file");
    if (file[5] != kElfDataLittle) return std::string("not a little-endian ELF file");
    if (read16(file, 16) != kTypeExec) return std::string("not an executable (ET_EXEC)");
    if (read16(file, 18) != kMachineRiscv) return std::string("not a RISC-V program");

    const uint32_t entry = read32(file, 24);
    const uint64_t phoff = read32(file, 28);
    const uint16_t phentsize = read16(file, 42);
    const uint16_t phnum = read16(file, 44);
    if (phnum > 0 && phentsize != kPhdrSize) return std::string("unexpected program header size");
    if (phoff + uint64_t{phnum} * kPhdrSize > file.size())
        return std::string("program headers are outside the file");

    // Pass 1: check every segment. Nothing is written until all of them are valid.
    std::vector<Segment> segments;
    for (size_t i = 0; i < phnum; i++) {
        const size_t ph = phoff + i * kPhdrSize;
        if (read32(file, ph) != kPtLoad) continue;
        const Segment s{read32(file, ph + 8), read32(file, ph + 4),
                        read32(file, ph + 16), read32(file, ph + 20)};

        if (s.filesz > s.memsz) return std::string("segment is larger in the file than in memory");
        if (uint64_t{s.offset} + s.filesz > file.size())
            return std::string("segment data is outside the file");
        if (s.vaddr < kMinLoadAddress)
            return std::string("segment is below the lowest program address (null guard)");
        if (uint64_t{s.vaddr} + s.memsz > mem::Ram::kRamSize)
            return std::string("segment does not fit in RAM");
        segments.push_back(s);
    }

    // Pass 2: copy file bytes, then zero-fill up to memsz (.bss).
    // These writes can't fail: pass 1 checked that every segment fits in RAM.
    for (const Segment& s : segments) {
        for (uint32_t i = 0; i < s.memsz; i++) {
            const uint8_t byte = i < s.filesz ? file[s.offset + i] : 0;
            ram.write(s.vaddr + i, 1, byte);
        }
    }
    return Program{entry};
}

}  // namespace loader
