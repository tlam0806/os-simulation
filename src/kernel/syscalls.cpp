// System calls (docs/machine-spec.md, B3) and the memory helpers they use.
#include <cassert>

#include "kernel/abi.hpp"
#include "kernel/kernel.hpp"
#include "loader/elf.hpp"

namespace kernel {

std::variant<Exited, Continue, Switch> Kernel::handle_syscall() {
    cpu().pc += 4;   // every call that resumes continues after the ecall
    uint32_t a7_value = cpu().reg(isa::reg::a7);
    switch (a7_value) {
        case abi::kSysExit :
            return sys_exit();
        case abi::kSysWrite :
            return sys_write();
        case abi::kSysSbrk :
            return sys_sbrk();
        case abi::kSysGetpid :
            return sys_getpid();
        case abi::kSysYield :
            return sys_yield();
        default :
            return sys_other();
    }
}

Exited Kernel::sys_exit() {
    int32_t a0_value = static_cast<int32_t>(cpu().reg(isa::reg::a0));
    return Exited{a0_value};
}

Continue Kernel::sys_write() {
    int32_t fd = static_cast<int32_t>(cpu().reg(isa::reg::a0));
    uint32_t buffer_addr = cpu().reg(isa::reg::a1);
    uint32_t bytes_count = cpu().reg(isa::reg::a2);
    if (fd != 1 && fd != 2) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEbadf));
        return Continue{};
    }
    std::ostream& output = (fd == 1 ? out_ : err_);
    std::optional<std::vector<uint8_t>> read_result = read_memory_on_ram(buffer_addr, bytes_count);
    if (!read_result) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEfault));
        return Continue{};
    }
    for (auto byte : *read_result) {
        output << byte;
    }
    cpu().set_reg(isa::reg::a0, bytes_count);
    return Continue{};
}

Continue Kernel::sys_sbrk() {
    int32_t n = static_cast<int32_t>(cpu().reg(isa::reg::a0));
    int64_t new_break = current_break() + n;
    uint32_t old_break = current_break();
    if (new_break < initial_break()) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEinval));
        return Continue{};
    }
    if (new_break > loader::kStackRegion) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEnomem));
        return Continue{};
    }
    if (n > 0 && mmu::get_vpn(new_break - 1) - mmu::get_vpn(current_break() - 1) > frame_allocator().free_count()) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEnomem));
        return Continue{};
    }
    while (current_break() < new_break) {
        uint32_t next_break = current_break() + 1;
        uint32_t cur_vpn = mmu::get_vpn(current_break() - 1);
        uint32_t next_vpn = mmu::get_vpn(current_break());
        if (cur_vpn != next_vpn) {
            map_page(next_vpn);
        }
        process().brk = next_break;
    }
    while (current_break() > new_break) {
        uint32_t next_break = current_break() - 1;
        uint32_t cur_vpn = mmu::get_vpn(next_break);
        uint32_t next_vpn = mmu::get_vpn(next_break - 1);
        if (cur_vpn != next_vpn) {
            unmap_page(cur_vpn);
        }
        process().brk = next_break;
    }
    assert(current_break() == new_break);
    cpu().set_reg(isa::reg::a0, old_break);
    return Continue{};
}

Continue Kernel::sys_getpid() {
    cpu().set_reg(isa::reg::a0, executing_pid);
    return Continue{};
}

Switch Kernel::sys_yield() {
    cpu().set_reg(isa::reg::a0, 0);
    return Switch{};
}

Continue Kernel::sys_other() {
    cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEnosys));
    return Continue{};
}

// ---- memory helpers ----

void Kernel::zero_frame(uint32_t frame) {
    for (uint32_t addr = frame * mmu::kPageSize; addr < frame * mmu::kPageSize + mmu::kPageSize; addr += 4) {
        bool writable = ram().write(addr, 4, 0);
        assert(writable);
    }
}

std::optional<std::vector<uint8_t>> Kernel::read_memory_on_ram(uint32_t addr, uint32_t bytes_count) const {
    std::vector<uint8_t> returned;
    mem::Ram& ram = machine_.ram();
    while (bytes_count > 0) {
        std::optional<uint32_t> physical_addr = mmu::translate(page_table(), addr, mmu::Access::load);
        if (!physical_addr) {
            return std::nullopt;
        }
        std::optional<uint32_t> readed = ram.read(*physical_addr, 1);
        if (!readed) {
            return std::nullopt;
        }
        returned.push_back(static_cast<uint8_t>(*readed));
        ++addr;
        --bytes_count;
    }
    return returned;
}

void Kernel::map_page(uint32_t vpn) {
    std::optional<uint32_t> physical_frame = frame_allocator().allocate();
    assert(physical_frame);
    mmu::Pte pte{*physical_frame, true, true, false};
    zero_frame(*physical_frame);
    page_table().map(vpn, pte);
}

void Kernel::unmap_page(uint32_t vpn) {
    auto pte = page_table().lookup(vpn);
    assert(pte);
    frame_allocator().free(pte->frame);
    page_table().unmap(vpn);
}

}  // namespace kernel
