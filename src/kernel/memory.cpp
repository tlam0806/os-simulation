// User memory: page faults, copy-on-write, and checked access to user memory (spec A2, B1, B3).
#include <cassert>

#include "kernel/abi.hpp"
#include "kernel/kernel.hpp"
#include "loader/elf.hpp"
#include "util/variant.hpp"

namespace kernel {

std::variant<Killed, Continue, Switch> Kernel::handle_page_fault(const cpu::Trap& trap) {
    uint32_t addr = trap.tval;
    uint32_t vpn = mmu::get_vpn(addr);
    auto found = page_table().lookup(vpn);
    for (const PendingIO& io : pending_ios_) {
        if (const SwapIn* swap_in = std::get_if<SwapIn>(&io.owner)) {
            if (swap_in->vpn == vpn) {
                thread().waiting_page = vpn;
                thread().blocked = kernel::BlockReason::IO;
                return Switch{};
            }
        }
    }
    if (fault_needs_frame(found, trap) && frame_allocator().free_count() == 0) {
        auto evict_result = evict();
        if (evict_result == EvictResult::Failed) {
            return Killed{trap};
        } else if (evict_result == EvictResult::Blocked) {
            return Switch{};
        }
    }
    if (found && !found->valid) {
        assert(found->swap_slot);
        swap_in(vpn, *found);
        return Switch{};
    }
    if (!in_region(addr)) {
        return Killed{trap};
    }
    // the address is valid, inside the region
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
        if (pte->valid) {
            frame_allocator().share(frame);
        }
        if (pte->swap_slot) {
            ++slot_refs_[*pte->swap_slot];
        }
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

std::variant<uint32_t, Blocked, Fault> Kernel::try_access(uint32_t addr, mmu::Access access) {
    std::optional<uint32_t> physical_addr = mmu::translate(page_table(), addr, access);
    if (physical_addr) {
        return *physical_addr;
    }
    // translate failed, try to handle the page fault
    cpu::Trap trap{
        .cause = (access == mmu::Access::fetch ? cpu::Cause::instruction_page_fault :
                  (access == mmu::Access::load ? cpu::Cause::load_page_fault :
                  cpu::Cause::store_page_fault)),
        .pc = cpu().pc,
        .tval = addr
    };

    std::variant<Killed, Continue, Switch> result = handle_page_fault(trap);
    if (std::holds_alternative<Killed>(result)) {
        return Fault{};
    } else if (std::holds_alternative<Continue>(result)) {
        physical_addr = mmu::translate(page_table(), addr, access);
        assert(physical_addr);
        return *physical_addr;
    } else {
        assert(std::holds_alternative<Switch>(result));
        return Blocked{};
    }
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

void Kernel::swap_in(uint32_t vpn, const mmu::Pte& pte) {
    auto new_frame = frame_allocator().allocate();
    assert(new_frame);
    assert(pte.swap_slot); // the evicted page should have a copy in the swap slot
    uint32_t sector = (*pte.swap_slot + fs::kSwapStart) * fs::kSectorsPerBlock;
    enqueue(
        PendingIO{
            SwapIn{process().pid, vpn, *new_frame},
            disk::Request{disk::Op::read, sector, fs::kSectorsPerBlock, *new_frame << mmu::kPageShift}
        }
    );
    thread().waiting_page = vpn;
    thread().blocked = BlockReason::IO;
}

bool Kernel::map_page(uint32_t vpn) {
    std::optional<uint32_t> physical_frame = frame_allocator().allocate();
    if (!physical_frame) return false;
    mmu::Pte pte{.frame = *physical_frame, .read = true, .write = true};
    zero_frame(*physical_frame);
    page_table().map(vpn, pte);
    return true;
}

bool Kernel::unmap_page(uint32_t vpn) {
    auto pte = page_table().lookup(vpn);
    if (!pte) {
        return false;
    }
    drop_entry(*pte);
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

std::variant<std::vector<uint8_t>, Blocked, Fault> Kernel::read_memory_on_ram(uint32_t addr, uint32_t bytes_count) {
    std::vector<uint8_t> returned;
    while (bytes_count > 0) {
        auto access = try_access(addr, mmu::Access::load);
        if (std::holds_alternative<Blocked>(access)) {
            return Blocked{};
        }
        if (std::holds_alternative<Fault>(access)) {
            return Fault{};
        }
        std::optional<uint32_t> readed = ram().read(std::get<uint32_t>(access), 1);
        if (!readed) {
            return Fault{};
        }
        returned.push_back(static_cast<uint8_t>(*readed));
        ++addr;
        --bytes_count;
    }
    return returned;
}

std::variant<std::string, int32_t, Blocked> Kernel::read_user_string(uint32_t addr, uint32_t max_bytes) {
    std::string result;
    for (uint32_t i = 0; i < max_bytes; ++i) {
        uint64_t large_addr = static_cast<uint64_t>(addr) + static_cast<uint64_t>(i);
        if (large_addr > UINT32_MAX) {
            return -abi::kEfault;
        }
        auto access = try_access(addr + i, mmu::Access::load);
        if (std::holds_alternative<Blocked>(access)) {
            return Blocked{};
        }
        if (std::holds_alternative<Fault>(access)) {
            return -abi::kEfault;
        }
        std::optional<uint32_t> readed = ram().read(std::get<uint32_t>(access), 1);
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

std::optional<uint32_t> Kernel::alloc_slot() {
    for (uint32_t i = 0; i < fs::kSwapBlocks; ++i) {
        if (!slot_refs_[i]) {
            slot_refs_[i] = 1;
            return i;
        }
    }
    return std::nullopt;
}

bool Kernel::fault_needs_frame(const std::optional<mmu::Pte>& found, const cpu::Trap& trap) const {
    if (!found) {
        return true;
    }
    if (!found->valid) {
        return true;
    }
    if (trap.cause == cpu::Cause::store_page_fault && 
        found->cow && 
        frame_allocator().ref_count(found->frame) > 1
    ) {
        return true;
    }
    return false;
}

Kernel::EvictResult Kernel::evict() {
    std::optional<Victim> victim = choose_victim();
    if (!victim) {
        return EvictResult::Failed;
    }
    uint32_t frame = victim->frame;
    uint32_t pid = victim->pid;
    uint32_t vpn = victim->vpn;
    Process& owner = process_table_.find(pid)->second;
    std::optional<mmu::Pte> pte = owner.page_table.lookup(vpn);
    assert(pte);
    assert(pte->valid);
    bool no_write = pte->swap_slot.has_value() && !pte->dirty; // no need to write to disk if the page is already in the swap slot and no other process is using it
    if (no_write) {
        pte->valid = false;
        owner.page_table.map(vpn, *pte);
        frame_allocator().free(frame);
        return EvictResult::Freed;
    } else { // should write to disk
        std::optional<uint32_t> new_slot = alloc_slot();
        if (!new_slot) {
            return EvictResult::Failed;
        }
        if (pte->swap_slot) {
            --slot_refs_[*pte->swap_slot];
        }
        pte->swap_slot = new_slot;
        uint32_t sector = (*new_slot + fs::kSwapStart) * fs::kSectorsPerBlock;
        assert(sector < fs::kNumBlocks * fs::kSectorsPerBlock);
        enqueue(
            PendingIO{
                SwapOut{executing_tid_, frame}, 
                disk::Request{disk::Op::write, sector, fs::kSectorsPerBlock, frame << mmu::kPageShift}
            }
        );
        pte->valid = false;
        owner.page_table.map(vpn, *pte);
        thread().blocked = BlockReason::IO;
        return EvictResult::Blocked;
    }
}

void Kernel::drop_entry(const mmu::Pte& pte) {
    if (pte.swap_slot) {
        --slot_refs_[*pte.swap_slot];
    }
    if (pte.valid) {
        frame_allocator().free(pte.frame);
    }
}

}  // namespace kernel
