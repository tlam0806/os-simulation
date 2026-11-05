// The disk driver: the FCFS request queue and the disk interrupt (spec B3, C2).
#include <cassert>

#include "kernel/abi.hpp"
#include "kernel/kernel.hpp"
#include "loader/elf.hpp"
#include "util/variant.hpp"

namespace kernel {

std::variant<Continue, Switch> Kernel::sys_disk_read() {
    return disk_io(disk::Op::read);
}

std::variant<Continue, Switch> Kernel::sys_disk_write() {
    return disk_io(disk::Op::write);
}

std::variant<Continue, Switch> Kernel::disk_io(disk::Op op) {
    uint32_t sector = cpu().reg(isa::reg::a0);
    uint32_t buffer_addr = cpu().reg(isa::reg::a1);
    uint32_t count = cpu().reg(isa::reg::a2);
    if (count == 0) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEinval));
        return Continue{};
    }
    if (static_cast<uint64_t>(sector) + static_cast<uint64_t>(count) > static_cast<uint64_t>(disk::kNumSectors)) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEinval));
        return Continue{};
    }
    uint64_t last = static_cast<uint64_t>(buffer_addr) + static_cast<uint64_t>(count) * disk::kSectorSize - 1;
    if (last > UINT32_MAX) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEinval));
        return Continue{};
    }
    if (mmu::get_vpn(buffer_addr) != mmu::get_vpn(static_cast<uint32_t>(last))) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEinval));
        return Continue{};
    }
    mmu::Access access = (op == disk::Op::read ? mmu::Access::store : mmu::Access::load);
    auto physical_addr = try_access(buffer_addr, access);
    if (std::holds_alternative<Blocked>(physical_addr)) {
        return retry_call();
    }
    if (std::holds_alternative<Fault>(physical_addr)) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEfault));
        return Continue{};
    }
    cpu().set_reg(isa::reg::a0, 0);
    enqueue(PendingIO{executing_tid_, disk::Request{op, sector, count, std::get<uint32_t>(physical_addr)}});
    thread().blocked = kernel::BlockReason::IO;
    return Switch{};
}

void Kernel::enqueue(PendingIO io) {
    pending_ios_.push_back(io);
    if (!disk().busy()) {
        start_next_request();
    }
}

void Kernel::start_next_request() {
    assert(!disk().busy());
    const PendingIO& next = pending_ios_.front();
    bool submitted = disk().submit(next.request, machine_.steps());
    assert(submitted);
}

Continue Kernel::handle_disk_interrupt() {
    assert(pending_ios_.size());
    const PendingIO& next = pending_ios_.front();
    machine_.acknowledge_disk_interrupt();
    if (const uint32_t* tid = std::get_if<uint32_t>(&next.owner)) {
        Thread& thread = thread_table_.find(*tid)->second;
        assert(thread.blocked == kernel::BlockReason::IO);
        thread.blocked = kernel::BlockReason::None;
        thread.context.regs[isa::reg::a0] = 0; // return value for disk_read/disk_write
    } else if (const auto* swap_out = std::get_if<SwapOut>(&next.owner)) {
        frame_allocator().free(swap_out->frame);
        auto thread_it = thread_table_.find(swap_out->tid);
        if (thread_it != thread_table_.end()) {
            Thread& thread = thread_it->second;
            assert(thread.blocked == kernel::BlockReason::IO);
            thread.blocked = kernel::BlockReason::None;
        }
    } else if (const auto* swap_in = std::get_if<SwapIn>(&next.owner)) {
        Process& process = process_table_.find(swap_in->pid)->second;
        std::optional<mmu::Pte> pte = process.page_table.lookup(swap_in->vpn);
        assert(pte);
        assert(!pte->valid);
        pte->valid = true;
        pte->accessed = false;
        pte->dirty = false;
        pte->frame = swap_in->frame;
        process.page_table.map(swap_in->vpn, *pte);
        for (uint32_t tid : process.threads) {
            Thread& thread = thread_table_.find(tid)->second;
            if (thread.waiting_page && thread.waiting_page.value() == swap_in->vpn) {
                assert(thread.blocked == kernel::BlockReason::IO);
                thread.blocked = kernel::BlockReason::None;
                thread.waiting_page.reset();
            }
        }
    }  else if (const auto* cache_fill = std::get_if<CacheFill>(&next.owner)) {
        // TODO (step 2): std::get<CacheFill>(next.owner).block is now in the cache.
        uint32_t b = cache_fill->block;
        cached_blocks_[b].ready = true;
        for (auto& [tid, thread] : thread_table_) {
            if (thread.waiting_block && thread.waiting_block.value() == b) {
                assert(thread.blocked == kernel::BlockReason::IO);
                thread.blocked = kernel::BlockReason::None;
                thread.waiting_block.reset();
            }
        }
    } else {
        assert(false && "Unknown PendingIO owner type");
    }
    assert(disk().busy() == false);
    pending_ios_.pop_front();
    if (pending_ios_.size()) {
        start_next_request();
    }
    return Continue{};
}

}  // namespace kernel
