// System calls (docs/machine-spec.md, B3) and the memory helpers they use.
#include <cassert>

#include "kernel/abi.hpp"
#include "kernel/kernel.hpp"
#include "loader/elf.hpp"
#include "util/variant.hpp"

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
        case abi::kSysWait :
            return util::convert_variant<Exited, Continue, Switch>(sys_wait());
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
    int32_t required_pid = static_cast<int32_t>(cpu().reg(isa::reg::a0));
    // cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEchild));
    std::vector<uint32_t> running = get_running_children_process(executing_pid, static_cast<uint32_t>(required_pid));
    std::vector<uint32_t> ended = get_ended_children_process(executing_pid, static_cast<uint32_t>(required_pid));
    if (running.empty() && ended.empty()) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEchild));
        return Continue{};
    }
    if (ended.size()) {
        uint32_t pid = *std::min_element(ended.begin(), ended.end());
        auto it = ending_table_.find(pid);
        assert(it != ending_table_.end());
        Ending& end = it->second;
        int32_t end_status = std::holds_alternative<Exited>(end.reason) 
            ? std::get<Exited>(end.reason).exit_status 
            : static_cast<int32_t>(std::get<Killed>(end.reason).trap.cause) + 128;
        // cpu().set_reg(isa::reg::a1, end_status);
        uint32_t addr = cpu().reg(isa::reg::a1);
        if (addr == 0) {
            end.collected = true;
            cpu().set_reg(isa::reg::a0, pid);
            return Continue{};    
        }
        if (addr % 4) {
            cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEfault));
            return Continue{};
        }
        auto physical_addr = try_access(page_table(), addr, mmu::Access::store);
        if (!physical_addr) {
            cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEfault));
            return Continue{};
        }
        end.collected = true;
        ram().write(*physical_addr, 4, end_status);
        cpu().set_reg(isa::reg::a0, pid);
        return Continue{};
    } 
    cpu().pc -= 4;
    return Switch{};
}

Continue Kernel::sys_exec() {

}

Continue Kernel::sys_other() {
    cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEnosys));
    return Continue{};
}

// ---- memory helpers ----

std::optional<uint32_t> Kernel::try_access(mmu::PageTable& table, uint32_t addr, mmu::Access access) {
    std::optional<uint32_t> physical_addr = mmu::translate(table, addr, access);
    if (physical_addr) {
        return *physical_addr;
    }
    if (!in_region(addr)) {
        return std::nullopt;
    }
    bool created = map_page(mmu::get_vpn(addr));
    if (!created) {
        return std::nullopt;
    }
    physical_addr = mmu::translate(table, addr, mmu::Access::load);
    assert(physical_addr);
    return *physical_addr;
}

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

bool Kernel::write_memory_on_ram(uint32_t addr, std::span<uint8_t> buffer) {
    for (auto byte : buffer) {
        std::optional<uint32_t> physical_addr = try_access(page_table(), addr, mmu::Access::store);
        if (!physical_addr) {
            return false;
        }
        bool wrote = ram().write(*physical_addr, 1, byte);
        assert(wrote);
        ++addr;
    }
    return true;
}

std::optional<std::vector<uint8_t>> Kernel::read_memory_on_ram(uint32_t addr, uint32_t bytes_count) {
    std::vector<uint8_t> returned;
    while (bytes_count > 0) {
        std::optional<uint32_t> physical_addr = try_access(page_table(), addr, mmu::Access::load);
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
