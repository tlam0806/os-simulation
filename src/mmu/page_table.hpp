// Page tables and address translation (docs/machine-spec.md, A2).
// A PageTable is one address space: it maps virtual pages to physical frames.
#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>
#include <map>

namespace mmu {

inline constexpr uint32_t kPageSize = 4096;
inline constexpr uint32_t kPageShift = 12;   // vaddr >> 12 = virtual page number

// What kind of memory access is being translated.
enum class Access { fetch, load, store };

// One mapping. A page that isn't in the table is not valid (the spec's V bit).
struct Pte {
    uint32_t frame = 0;   // physical frame number: physical address = frame * kPageSize
    bool read = false;    // R: loads allowed
    bool write = false;   // W: stores allowed
    bool exec = false;    // X: instruction fetches allowed

    bool operator==(const Pte&) const = default;
};

class PageTable {
public:
    // Maps virtual page `vpn` to `pte`, replacing any existing mapping.
    void map(uint32_t vpn, Pte pte);

    // Removes the mapping for `vpn`. Does nothing if there isn't one.
    void unmap(uint32_t vpn);

    // The mapping for `vpn`, or nullopt if the page is not mapped.
    std::optional<Pte> lookup(uint32_t vpn) const;

    // Every mapped virtual page number, in increasing order.
    std::vector<uint32_t> mapped_pages() const;

private:
    std::map<uint32_t, Pte> entries_;
};

// Translates `vaddr` for one kind of access.
// Returns the physical address, or nullopt (a page fault) if the page is not
// mapped or its permission bit for this access is not set.
std::optional<uint32_t> translate(const PageTable& table, uint32_t vaddr, Access access);

uint32_t get_vpn(uint32_t vaddr);

}  // namespace mmu
