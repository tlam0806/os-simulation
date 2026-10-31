// User memory: page faults, copy-on-write, and checked access to user memory (spec A2, B1, B3).
#include <cassert>

#include "kernel/abi.hpp"
#include "kernel/kernel.hpp"
#include "loader/elf.hpp"
#include "util/variant.hpp"

namespace kernel {

std::variant<Killed, Continue> Kernel::handle_page_fault(const cpu::Trap& trap) {
    uint32_t addr = trap.tval;
    if (!in_region(addr)) {
        return Killed{trap};
    }
    // the address is valid, inside the region
    uint32_t vpn = mmu::get_vpn(addr);
    std::optional<mmu::Pte> found = page_table().lookup(vpn);

    if (!found) {
        // not mapped yet, should create a new one
        if (!map_page(vpn)) {
            return Killed{trap};
        } else {
            return Continue{};
        }
    }
    assert(found);
    if (trap.cause == cpu::Cause::store_page_fault && found->cow) { // copy-on-write
        auto new_pte = handle_cow(*found);
        if (!new_pte) {
            return Killed{trap};
        }
        page_table().map(vpn, *new_pte);
        return Continue{};
    }
    return Killed{trap};
}

mmu::PageTable Kernel::share_table(mmu::PageTable& other) {
    mmu::PageTable shared;
    std::vector<uint32_t> pages = other.mapped_pages();
    for (uint32_t vpn : pages) {
        std::optional<mmu::Pte> pte = other.lookup(vpn);
        assert(pte); // should be in the old page table
        uint32_t frame = pte->frame;
        frame_allocator().share(frame);
        if (pte->write) {
            pte->write = false;
            pte->cow = true;
            other.map(vpn, *pte);
        } 
        shared.map(vpn, *pte);
    }
    return shared;
}

std::optional<mmu::Pte> Kernel::handle_cow(mmu::Pte pte) {
    assert(pte.cow == true);
    assert(pte.write == false);
    if (frame_allocator().ref_count(pte.frame) > 1) {
        std::optional<uint32_t> new_frame = frame_allocator().allocate();
        if (!new_frame) {
            return std::nullopt;
        }
        copy_frame(pte.frame, *new_frame);
        frame_allocator().free(pte.frame);
        pte.frame = *new_frame;
    }   
    pte.cow = false;
    pte.write = true;
    return pte;
}

std::optional<uint32_t> Kernel::try_access(uint32_t addr, mmu::Access access) {
    std::optional<uint32_t> physical_addr = mmu::translate(page_table(), addr, access);
    if (physical_addr) {
        return *physical_addr;
    }
    if (!in_region(addr)) {
        return std::nullopt;
    }
    uint32_t vpn = mmu::get_vpn(addr);
    std::optional<mmu::Pte> found = page_table().lookup(vpn);
    if (found && found->cow && access == mmu::Access::store) { // copy-on-write
        auto new_pte = handle_cow(*found);
        if (!new_pte) {
            return std::nullopt;
        }
        page_table().map(vpn, *new_pte);
        physical_addr = mmu::translate(page_table(), addr, access);
        assert(physical_addr);
        return *physical_addr;
    }
    bool created = map_page(vpn);
    if (!created) {
        return std::nullopt;
    }
    physical_addr = mmu::translate(page_table(), addr, access);
    assert(physical_addr);
    return *physical_addr;
}

bool Kernel::in_region(uint64_t large_addr) {
    if (large_addr > UINT32_MAX) {
        return false;
    }
    uint32_t addr = static_cast<uint32_t>(large_addr);
    if (loader::kStackRegion <= addr && addr < loader::kStackTop) {
        return true;
    }
    uint32_t vpn = mmu::get_vpn(addr);
    uint32_t first_vpn = mmu::get_vpn(initial_break());
    uint32_t last_vpn = mmu::get_vpn(current_break() - 1);
    if (first_vpn <= vpn && vpn <= last_vpn) {
        return true;
    }
    return false;
}

bool Kernel::map_page(uint32_t vpn) {
    std::optional<uint32_t> physical_frame = frame_allocator().allocate();
    if (!physical_frame) return false;
    mmu::Pte pte{*physical_frame, true, true, false};
    zero_frame(*physical_frame);
    page_table().map(vpn, pte);
    return true;
}

bool Kernel::unmap_page(uint32_t vpn) {
    auto pte = page_table().lookup(vpn);
    if (!pte) {
        return false;
    }
    frame_allocator().free(pte->frame);
    page_table().unmap(vpn);
    return true;
}

void Kernel::copy_frame(uint32_t from_frame, uint32_t to_frame) {
    bool copied = ram().copy(from_frame * mmu::kPageSize, to_frame * mmu::kPageSize, mmu::kPageSize);
    assert(copied);
}

void Kernel::zero_frame(uint32_t frame) {
    for (uint32_t addr = frame * mmu::kPageSize; addr < frame * mmu::kPageSize + mmu::kPageSize; addr += 4) {
        bool writable = ram().write(addr, 4, 0);
        assert(writable);
    }
}

std::optional<std::vector<uint8_t>> Kernel::read_memory_on_ram(uint32_t addr, uint32_t bytes_count) {
    std::vector<uint8_t> returned;
    while (bytes_count > 0) {
        std::optional<uint32_t> physical_addr = try_access(addr, mmu::Access::load);
        if (!physical_addr) {
            return std::nullopt;
        }
        std::optional<uint32_t> readed = ram().read(*physical_addr, 1);
        if (!readed) {
            return std::nullopt;
        }
        returned.push_back(static_cast<uint8_t>(*readed));
        ++addr;
        --bytes_count;
    }
    return returned;
}

bool Kernel::write_memory_on_ram(uint32_t addr, std::span<uint8_t> buffer) {
    for (auto byte : buffer) {
        std::optional<uint32_t> physical_addr = try_access(addr, mmu::Access::store);
        if (!physical_addr) {
            return false;
        }
        bool wrote = ram().write(*physical_addr, 1, byte);
        assert(wrote);
        ++addr;
    }
    return true;
}

std::variant<std::string, int32_t> Kernel::read_user_string(uint32_t addr, uint32_t max_bytes) {
    std::string result;
    for (uint32_t i = 0; i < max_bytes; ++i) {
        uint64_t large_addr = static_cast<uint64_t>(addr) + static_cast<uint64_t>(i);
        if (large_addr > UINT32_MAX) {
            return -abi::kEfault;
        }
        std::optional<uint32_t> physical_addr = try_access(addr + i, mmu::Access::load);
        if (!physical_addr) {
            return -abi::kEfault;
        }

        std::optional<uint32_t> readed = ram().read(*physical_addr, 1);
        if (!readed) {
            return -abi::kEfault;
        }
        char c = static_cast<char>(*readed);
        if (c == '\0') {
            return result;
        }
        result.push_back(c);
    }   
    return -abi::kEnametoolong;
}

}  // namespace kernel
