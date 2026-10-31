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
    if (!physical_addr) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEfault));
        return Continue{};
    }
    cpu().set_reg(isa::reg::a0, 0);
    enqueue(PendingIO{executing_pid, disk::Request{op, sector, count, *physical_addr}});
    process().blocked = kernel::BlockReason::IO;
    return Switch{};
}

void Kernel::enqueue(PendingIO io) {
    pending_ios_.push(io);
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
    if (pending_ios_.empty()) {
        return Continue{};
    }
    const PendingIO& next = pending_ios_.front();
    machine_.acknowledge_disk_interrupt();
    if (const uint32_t* pid = std::get_if<uint32_t>(&next.owner)) {
        kernel::Process& process = process_table_.find(*pid)->second;
        assert(process.blocked == kernel::BlockReason::IO);
        process.blocked = kernel::BlockReason::None;
        process.context.regs[isa::reg::a0] = 0; // return value for disk_read/disk_write
    } else {
        // TODO (step 2): std::get<CacheFill>(next.owner).block is now in the cache.
        CacheFill fill = std::get<CacheFill>(next.owner);
        uint32_t b = fill.block;
        cached_blocks_[b].ready = true;
        for (auto& [pid, process] : process_table_) {
            if (process.waiting_block && process.waiting_block.value() == b) {
                assert(process.blocked == kernel::BlockReason::IO);
                process.blocked = kernel::BlockReason::None;
                process.waiting_block.reset();
            }
        }
    }
    assert(disk().busy() == false);
    pending_ios_.pop();
    if (pending_ios_.size()) {
        start_next_request();
    }
    return Continue{};
}

}  // namespace kernel
