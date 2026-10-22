#include "mmu/page_table.hpp"

namespace mmu {

// TODO: implement. tests/page_table_test.cpp describes the expected behavior.

void PageTable::map(uint32_t vpn, Pte pte) {
    entries_[vpn] = pte;
}

void PageTable::unmap(uint32_t vpn) {
    entries_.erase(vpn);
}

std::optional<Pte> PageTable::lookup(uint32_t vpn) const { 
    auto it = entries_.find(vpn);
    if (it == entries_.end()) {
        return std::nullopt;
    } else {
        return it->second;
    }
}

uint32_t get_vpn(uint32_t vaddr) {
    return vaddr >> kPageShift;
}

std::optional<uint32_t> translate(const PageTable& table, uint32_t vaddr, Access access) {
    uint32_t vpn = vaddr >> kPageShift;
    std::optional<Pte> lookup_result = table.lookup(vpn);
    if (!lookup_result) {
        return std::nullopt;
    }
    Pte pte = *lookup_result;
    if (access == Access::fetch && pte.exec == false) {
        return std::nullopt;
    }
    if (access == Access::load && pte.read == false) {
        return std::nullopt;
    }
    if (access == Access::store && pte.write == false) {
        return std::nullopt;
    }
    uint32_t frame = pte.frame;
    uint32_t offset = vaddr & 0xFFF;
    return (frame << kPageShift) | offset;
}

}  // namespace mmu
