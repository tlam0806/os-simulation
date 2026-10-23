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
        case abi::kSysFork : 
            return sys_fork();
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
    uint32_t old_break = current_break();
    int64_t new_break = old_break + n;
    if (new_break < initial_break()) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEinval));
        return Continue{};
    }
    if (new_break > loader::kStackRegion) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEnomem));
        return Continue{};
    }
    cpu().set_reg(isa::reg::a0, old_break);
    process().brk = new_break;
    if (n >= 0) {
        return Continue{};
    }
    // shrink the heap
    uint32_t old_vpn = mmu::get_vpn(old_break - 1);
    uint32_t new_vpn = mmu::get_vpn(new_break - 1);
    for (uint32_t vpn = new_vpn + 1; vpn <= old_vpn; ++vpn) { // iterate through every pages through unmap 
        unmap_page(vpn);
    }
    assert(current_break() == new_break);
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

std::optional<mmu::PageTable> Kernel::inherit_table(const mmu::PageTable& other) {
    mmu::PageTable inherited;
    std::vector<uint32_t> pages = other.mapped_pages();
    if (pages.size() > frame_allocator().free_count()) {
        return std::nullopt;
    }
    for (uint32_t vpn : pages) {
        std::optional<uint32_t> new_frame = frame_allocator().allocate();
        assert(new_frame);
        std::optional<mmu::Pte> pte = other.lookup(vpn);
        assert(pte); // should be in the old page table
        copy_frame(pte->frame, *new_frame);
        mmu::Pte new_pte = *pte;
        new_pte.frame = *new_frame;
        inherited.map(vpn, new_pte);
    }
    return inherited;
}

Continue Kernel::sys_fork() {
    Process copied = process();
    copied.parent_pid = executing_pid;
    copied.context = cpu().context();  // update context
    copied.context.regs[isa::reg::a0] = 0; 
    std::optional<mmu::PageTable> inherited = inherit_table(page_table());
    if (!inherited) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEnomem));
        return Continue{};
    }
    copied.page_table = *inherited;
    uint32_t child_pid = create_process(std::move(copied));
    cpu().set_reg(isa::reg::a0, child_pid);
    return Continue{};
}

std::variant<Continue, Switch> Kernel::sys_wait() {

}

Continue Kernel::sys_exec() {

}

Continue Kernel::sys_other() {
    cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEnosys));
    return Continue{};
}

// ---- memory helpers ----

void Kernel::copy_frame(uint32_t from_frame, uint32_t to_frame) {
    bool copied = ram().copy(from_frame * mmu::kPageSize, to_frame * mmu::kPageSize, mmu::kPageSize);
    assert(copied);
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

void Kernel::zero_frame(uint32_t frame) {
    for (uint32_t addr = frame * mmu::kPageSize; addr < frame * mmu::kPageSize + mmu::kPageSize; addr += 4) {
        bool writable = ram().write(addr, 4, 0);
        assert(writable);
    }
}

std::optional<std::vector<uint8_t>> Kernel::read_memory_on_ram(uint32_t addr, uint32_t bytes_count) {
    std::vector<uint8_t> returned;
    mem::Ram& ram = machine_.ram();
    while (bytes_count > 0) {
        std::optional<uint32_t> physical_addr = mmu::translate(page_table(), addr, mmu::Access::load);
        if (!physical_addr) {
            if (!in_region(addr)) {
                return std::nullopt;
            }
            bool created = map_page(mmu::get_vpn(addr));
            if (!created) return std::nullopt;
            physical_addr = mmu::translate(page_table(), addr, mmu::Access::load);
            assert(physical_addr);
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

}  // namespace kernel
