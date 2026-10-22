#include "loader/elf.hpp"

#include <unordered_map>
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
constexpr uint32_t kFlagX = 1, kFlagW = 2, kFlagR = 4;   // p_flags bits

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
    uint32_t flags;    // p_flags: R/W/X
};

// A page to map, with the union of the permissions of the segments on it.
struct PagePlan {
    uint32_t vpn;
    mmu::Pte perms;   // frame is filled in when the page is given one
};

// Physical address of a virtual address in a page that is known to be mapped.
uint32_t physical(const mmu::PageTable& table, uint32_t vaddr) {
    const mmu::Pte pte = table.lookup(vaddr >> mmu::kPageShift).value();
    return pte.frame * mmu::kPageSize + (vaddr & (mmu::kPageSize - 1));
}

}  // namespace

std::variant<Program, std::string> load_elf(std::span<const uint8_t> file, mem::Ram& ram,
                                            mmu::FrameAllocator& frames, mmu::PageTable& table) {
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

    // Pass 1: check every segment and plan its pages. Nothing changes until all are valid.
    std::vector<Segment> segments;
    std::vector<PagePlan> pages;
    std::unordered_map<uint32_t, size_t> page_index;   // vpn -> position in `pages`
    uint64_t highest_end = kMinLoadAddress;
    for (size_t i = 0; i < phnum; i++) {
        const size_t ph = phoff + i * kPhdrSize;
        if (read32(file, ph) != kPtLoad) continue;
        const Segment s{read32(file, ph + 8), read32(file, ph + 4), read32(file, ph + 16),
                        read32(file, ph + 20), read32(file, ph + 24)};

        if (s.filesz > s.memsz) return std::string("segment is larger in the file than in memory");
        if (uint64_t{s.offset} + s.filesz > file.size())
            return std::string("segment data is outside the file");
        if (s.vaddr < kMinLoadAddress)
            return std::string("segment is below the lowest program address (null guard)");
        const uint64_t end = uint64_t{s.vaddr} + s.memsz;
        if (end > kStackRegion) return std::string("segment overlaps the stack region");
        if (s.memsz == 0) continue;
        segments.push_back(s);
        if (end > highest_end) highest_end = end;

        for (uint32_t vpn = s.vaddr >> mmu::kPageShift;
             vpn <= static_cast<uint32_t>((end - 1) >> mmu::kPageShift); vpn++) {
            auto [it, is_new] = page_index.try_emplace(vpn, pages.size());
            if (is_new) pages.push_back({vpn, {}});
            mmu::Pte& perms = pages[it->second].perms;
            perms.read |= (s.flags & kFlagR) != 0;
            perms.write |= (s.flags & kFlagW) != 0;
            perms.exec |= (s.flags & kFlagX) != 0;
        }
    }
    for (uint32_t i = kInitialStackPages; i > 0; i--)
        pages.push_back({(kStackTop >> mmu::kPageShift) - i, {0, true, true, false}});

    if (pages.size() > frames.free_count())
        return std::string("not enough free memory to load the program");

    // Pass 2: give each page a zeroed frame and map it. Can't fail: frames were counted.
    for (PagePlan& p : pages) {
        p.perms.frame = frames.allocate().value();
        const uint32_t base = p.perms.frame * mmu::kPageSize;
        for (uint32_t i = 0; i < mmu::kPageSize; i += 4) ram.write(base + i, 4, 0);
        table.map(p.vpn, p.perms);
    }

    // Pass 3: copy the file bytes. The rest of each segment is already zero (.bss).
    for (const Segment& s : segments)
        for (uint32_t i = 0; i < s.filesz; i++)
            ram.write(physical(table, s.vaddr + i), 1, file[s.offset + i]);

    const uint32_t initial_break =
        static_cast<uint32_t>((highest_end + mmu::kPageSize - 1) & ~uint64_t{mmu::kPageSize - 1});
    return Program{entry, initial_break};
}

}  // namespace loader
