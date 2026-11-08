// System calls (docs/machine-spec.md, B3) and the memory helpers they use.
#include <algorithm>
#include <cassert>
#include <ranges>

#include "kernel/abi.hpp"
#include "kernel/kernel.hpp"
#include "loader/elf.hpp"
#include "util/variant.hpp"

namespace kernel {

std::variant<Exited, Killed, Continue, Switch, ThreadExited> Kernel::handle_syscall() {
    cpu().pc += 4;   // every call that resumes continues after the ecall
    uint32_t a7_value = cpu().reg(isa::reg::a7);
    switch (a7_value) {
        case abi::kSysExit :
            return sys_exit();
        case abi::kSysWrite :
            return util::convert_variant<Exited, Killed, Continue, Switch, ThreadExited>(sys_write());
        case abi::kSysSbrk :
            return sys_sbrk();
        case abi::kSysGetpid :
            return sys_getpid();
        case abi::kSysYield :
            return sys_yield();
        case abi::kSysFork : 
            return sys_fork();
        case abi::kSysWait :
            return util::convert_variant<Exited, Killed, Continue, Switch, ThreadExited>(sys_wait());
        case abi::kSysDiskRead :
            return util::convert_variant<Exited, Killed, Continue, Switch, ThreadExited>(sys_disk_read());
        case abi::kSysDiskWrite :
            return util::convert_variant<Exited, Killed, Continue, Switch, ThreadExited>(sys_disk_write());
        case abi::kSysOpen : 
            return util::convert_variant<Exited, Killed, Continue, Switch, ThreadExited>(sys_open());
        case abi::kSysClose :
            return sys_close();
        case abi::kSysLseek :
            return util::convert_variant<Exited, Killed, Continue, Switch, ThreadExited>(sys_lseek());
        case abi::kSysRead : 
            return util::convert_variant<Exited, Killed, Continue, Switch, ThreadExited>(sys_read());
        case abi::kSysExec :
            return util::convert_variant<Exited, Killed, Continue, Switch, ThreadExited>(sys_exec());
        case abi::kSysThreadCreate :
            return sys_thread_create();
        case abi::kSysGettid : 
            return sys_gettid();
        case abi::kSysThreadExit : 
            return sys_thread_exit();
        case abi::kSysThreadJoin : 
            return util::convert_variant<Exited, Killed, Continue, Switch, ThreadExited>(sys_thread_join());
        case abi::kSysPipe :
            return util::convert_variant<Exited, Killed, Continue, Switch, ThreadExited>(sys_pipe());
        case abi::kSysShmget :
            return sys_shmget();
        case abi::kSysShmrm : 
            return sys_shmrm();
        case abi::kSysShmat : 
            return sys_shmat();
        case abi::kSysShmdt : 
            return sys_shmdt();
        case abi::kSysSignal :
            return sys_signal();
        case abi::kSysKill : 
            return sys_kill();
        case abi::kSysSigreturn :
            return util::convert_variant<Exited, Killed, Continue, Switch, ThreadExited>(sys_sig_return());
        default :
            return sys_other();
    }
}

std::variant<Continue, Switch, Killed> Kernel::sys_sig_return() {
    uint32_t sp = cpu().reg(isa::reg::sp);
    cpu::Context context{};
    auto read_result = read_context(sp, context);
    if (std::holds_alternative<Killed>(read_result)) {
        return read_result;
    }
    if (std::holds_alternative<Switch>(read_result)) {
        return read_result;
    }
    cpu().set_context(context);
    return Continue{};
}

Continue Kernel::sys_kill() {
    uint32_t pid = cpu().reg(isa::reg::a0);
    uint32_t sig = cpu().reg(isa::reg::a1);
    if (sig != abi::kSigkill && sig != abi::kSigusr1 && sig != abi::kSigsegv && sig != abi::kSigusr2 && sig != abi::kSigpipe) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEinval));
        return Continue{};
    }
    auto it = process_table_.find(pid);
    if (it == process_table_.end()) { // no running process with that pid 
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEsrch));
        return Continue{};
    }
    Process& process = it->second;
    if (process.status) { // the process already ended
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEsrch));
        return Continue{};
    }
    cpu().set_reg(isa::reg::a0, 0);
    SignalAction signal_action = process.signal_actions[sig];
    if (signal_action.handler == abi::kSigIgnore) {
        return Continue{};
    }
    if (pid != this->process().pid && signal_action.handler == abi::kSigDefault) {
        end_all_threads(process, Killed{Signal{sig}});
        return Continue{};
    }
    process.pending_flags |= (1 << sig);
    for (uint32_t tid : process.threads) {
        Thread& thread = thread_table_[tid];
        if (thread.blocked != BlockReason::IO) {
            thread.blocked = BlockReason::None;
        }
    }
    return Continue{};
}

Continue Kernel::sys_signal() {
    uint32_t sig = cpu().reg(isa::reg::a0);
    uint32_t handler = cpu().reg(isa::reg::a1);
    uint32_t restorer = cpu().reg(isa::reg::a2);
    if (sig != abi::kSigusr1 && sig != abi::kSigsegv && sig != abi::kSigusr2 && sig != abi::kSigpipe) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEinval));
        return Continue{};
    }
    process().signal_actions[sig] = SignalAction{handler, restorer};
    cpu().set_reg(isa::reg::a0, 0);
    return Continue{};
}

Continue Kernel::sys_shmdt() {
    uint32_t start_addr = cpu().reg(isa::reg::a0);
    auto& attachments = process().attachments;
    auto found = attachments.find(start_addr);
    if (found == attachments.end()) { // invalid addr 
        cpu().set_reg(isa::reg::a0, -abi::kEinval);
        return Continue{};
    }
    uint32_t region_id = found->second;
    Region& region = region_table_[region_id];
    --region.attachment_count;
    for (uint32_t addr = start_addr; addr < start_addr + region.frames.size() * mmu::kPageSize; addr += mmu::kPageSize) {
        uint32_t vpn = addr >> mmu::kPageShift;
        unmap_page(vpn);
    }
    attachments.erase(start_addr);
    cpu().set_reg(isa::reg::a0, 0);
    return Continue{};
}

Continue Kernel::sys_shmat() {
    uint32_t region_id = cpu().reg(isa::reg::a0);
    auto found_region = region_table_.find(region_id);
    if (found_region == region_table_.end()) {
        cpu().set_reg(isa::reg::a0, -abi::kEinval);
        return Continue{};
    }
    Region& region = found_region->second;
    uint32_t size = region.frames.size() * mmu::kPageSize;
    uint32_t last = loader::kStackRegion;
    for (const auto& [start_addr, existing_region_id] : process().attachments | std::views::reverse) {
        uint32_t existing_size = region_table_[existing_region_id].frames.size() * mmu::kPageSize;
        uint32_t free_size = last - (start_addr + existing_size);
        if (free_size >= size) {
            ++region.attachment_count;
            process().attachments.emplace(last - size, region_id);
            cpu().set_reg(isa::reg::a0, last - size);
            return Continue{};
        }
        last = start_addr;
    }
    uint32_t free_size = last - process().brk;
    if (free_size >= size) {
        ++region.attachment_count;
        process().attachments.emplace(last - size, region_id);
        cpu().set_reg(isa::reg::a0, last - size);
        return Continue{};
    } else {
        cpu().set_reg(isa::reg::a0, -abi::kEnomem);
        return Continue{};
    }
}

Continue Kernel::sys_shmget() {
    int32_t size = static_cast<int32_t>(cpu().reg(isa::reg::a0));
    if (size <= 0) {
        cpu().set_reg(isa::reg::a0, -abi::kEinval);
        return Continue{};
    }
    if (region_table_.size() == kMaxSharedRegions) {
        cpu().set_reg(isa::reg::a0, -abi::kEnospc);
        return Continue{};
    }
    uint32_t page_count = (static_cast<uint64_t>(size) + mmu::kPageSize - 1) / mmu::kPageSize;
    Region region;
    region.frames.resize(page_count);
    uint32_t region_id = ++next_region_id;
    region_table_.emplace(region_id, region);
    assert(region_table_.size() <= kMaxSharedRegions);
    cpu().set_reg(isa::reg::a0, region_id);
    return Continue{};
}
Continue Kernel::sys_shmrm() {
    uint32_t id = cpu().reg(isa::reg::a0);
    auto it = region_table_.find(id);
    if (it == region_table_.end()) {
        cpu().set_reg(isa::reg::a0, -abi::kEinval);
        return Continue{};
    }
    Region& region = it->second;
    if (region.attachment_count > 0) {
        cpu().set_reg(isa::reg::a0, -abi::kEbusy);
        return Continue{};
    }
    for (auto frame : region.frames) {
        if (!frame) continue;
        frame_allocator().free(*frame);
    }
    region_table_.erase(id);
    cpu().set_reg(isa::reg::a0, 0);
    return Continue{};
}

std::variant<Continue, Switch> Kernel::sys_pipe() {
    uint32_t fds_addr = cpu().reg(isa::reg::a0);
    if (fds_addr % 4) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEfault));
        return Continue{};
    }
    uint32_t read_fd_addr;
    uint32_t write_fd_addr;
    for (uint32_t addr : {fds_addr, fds_addr + 4}) {
        auto access = try_access(addr, mmu::Access::store);
        if (std::holds_alternative<Fault>(access)) {
            cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEfault));
            return Continue{};
        }
        if (std::holds_alternative<Blocked>(access)) {
            thread().blocked = BlockReason::IO;
            return retry_call();
        }
        if (addr == fds_addr) {
            read_fd_addr = std::get<uint32_t>(access);
        } else {
            write_fd_addr = std::get<uint32_t>(access);
        }
    }
    auto maybe_free_fds = find_two_free_fds(process());
    if (!maybe_free_fds) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEmfile));
        return Continue{};
    }
    auto maybe_free_open_files = find_two_free_open_files();
    if (!maybe_free_open_files) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEnfile));
        return Continue{};
    }
    auto [read_fd, write_fd] = *maybe_free_fds;
    auto [read_open_file, write_open_file] = *maybe_free_open_files;
    std::shared_ptr<Pipe> pipe = std::make_shared<Pipe>();
    OpenFile read_file{.kind = PipeFile{pipe, PipeEnd::Read}, .refs = 1};
    OpenFile write_file{.kind = PipeFile{pipe, PipeEnd::Write}, .refs = 1};
    file_table_[read_open_file] = std::move(read_file);
    file_table_[write_open_file] = std::move(write_file);
    process().fd_table[read_fd] = read_open_file;
    process().fd_table[write_fd] = write_open_file;
    ram().write(read_fd_addr, 4, read_fd);
    ram().write(write_fd_addr, 4, write_fd);
    cpu().set_reg(isa::reg::a0, 0);
    return Continue{};
}

ThreadExited Kernel::sys_thread_exit() {
    int32_t status = static_cast<int32_t>(cpu().reg(isa::reg::a0));
    return ThreadExited{status};
}

std::variant<Continue, Switch> Kernel::sys_thread_join() {
    uint32_t tid = cpu().reg(isa::reg::a0);
    uint32_t status_addr = cpu().reg(isa::reg::a1);
    if (tid == executing_tid_) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEinval));
        return Continue{};
    }
    Process& process = this->process();
    auto it = process.ended_threads.find(tid);
    if (it != process.ended_threads.end()) {
        if (status_addr != 0) {
            if (status_addr % 4) {
                cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEfault));
                return Continue{};
            }
            auto access = try_access(status_addr, mmu::Access::store);
            if (std::holds_alternative<Blocked>(access)) {
                return retry_call();
            }
            if (std::holds_alternative<Fault>(access)) {
                cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEfault));
                return Continue{};
            }
            ram().write(std::get<uint32_t>(access), 4, static_cast<uint32_t>(it->second));
        }
        process.ended_threads.erase(it); // last: a re-run or a retry must still find it
        cpu().set_reg(isa::reg::a0, tid);
        return Continue{};
    }
    if (std::find(process.threads.begin(), process.threads.end(), tid) != process.threads.end()) {
        thread().blocked = BlockReason::Wait;
        cpu().pc -= 4;
        return Switch{};
    }
    cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEsrch));
    return Continue{};
}

Continue Kernel::sys_gettid() {
    cpu().set_reg(isa::reg::a0, executing_tid_);
    return Continue{};
}

Continue Kernel::sys_thread_create() {
    uint32_t pc = cpu().reg(isa::reg::a0);
    uint32_t sp = cpu().reg(isa::reg::a1);
    uint32_t a0 = cpu().reg(isa::reg::a2);
    uint32_t tid = ++next_pid_;
    cpu::Context context;
    context.pc = pc;
    context.regs[isa::reg::sp] = sp;
    context.regs[isa::reg::a0] = a0;
    Thread thread{.tid = tid, .pid = process().pid, .context = context};
    process().threads.push_back(tid);
    cpu().set_reg(isa::reg::a0, tid);
    thread_table_.emplace(tid, std::move(thread));
    return Continue{};
}

Exited Kernel::sys_exit() {
    int32_t a0_value = static_cast<int32_t>(cpu().reg(isa::reg::a0));
    return Exited{a0_value};
}

std::variant<Continue, Switch> Kernel::sys_write() {
    int32_t fd = static_cast<int32_t>(cpu().reg(isa::reg::a0));
    uint32_t buffer_addr = cpu().reg(isa::reg::a1);
    uint32_t bytes_count = cpu().reg(isa::reg::a2);
    if (fd != 1 && fd != 2) {
        OpenFile* file = open_file_of(static_cast<uint32_t>(fd));
        if (file) {
            if (auto* pipe_file = std::get_if<PipeFile>(&file->kind)) {
                return write_pipe(*pipe_file, buffer_addr, bytes_count);
            }
        }
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEbadf));
        return Continue{};
    }
    std::ostream& output = (fd == 1 ? out_ : err_);
    auto read_result = read_memory_on_ram(buffer_addr, bytes_count);
    if (std::holds_alternative<Blocked>(read_result)) {
        return retry_call();
    }
    if (std::holds_alternative<Fault>(read_result)) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEfault));
        return Continue{};
    }
    for (auto byte : std::get<std::vector<uint8_t>>(read_result)) {
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
    auto attachments = process().attachments;
    if (attachments.size() && attachments.begin()->first < new_break) {
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
    cpu().set_reg(isa::reg::a0, process().pid);
    return Continue{};
}

Switch Kernel::sys_yield() {
    cpu().set_reg(isa::reg::a0, 0);
    return Switch{};
}

Continue Kernel::sys_fork() {
    Process copied = process();
    for (const auto& [start_addr, region_id] : process().attachments) {
        Region& region = region_table_.at(region_id);
        ++region.attachment_count;
    }
    copied.threads.clear();
    copied.parent_pid = process().pid;
    cpu::Context context = cpu().context();
    context.regs[isa::reg::a0] = 0; 
    mmu::PageTable shared = share_table(page_table());
    copied.page_table = shared;
    for (auto& fd : copied.fd_table) {
        if (fd) share_open_file(*fd);
    }
    uint32_t child_pid = create_process(std::move(copied), context);
    cpu().set_reg(isa::reg::a0, child_pid);
    return Continue{};
}

std::variant<Continue, Switch> Kernel::sys_wait() {
    int32_t required_pid = static_cast<int32_t>(cpu().reg(isa::reg::a0));
    std::vector<uint32_t> running = get_running_children_process(process().pid, static_cast<uint32_t>(required_pid));
    std::vector<uint32_t> ended = get_ended_children_process(process().pid, static_cast<uint32_t>(required_pid));
    if (running.empty() && ended.empty()) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEchild));
        return Continue{};
    }
    if (ended.size()) {
        uint32_t pid = *std::min_element(ended.begin(), ended.end());
        auto it = ending_table_.find(pid);
        assert(it != ending_table_.end());
        Ending& end = it->second;
        int32_t end_status = 0;
        if (const auto* exited = std::get_if<Exited>(&end.status)) {
            end_status = exited->exit_status;
        } else if (const auto* trap = std::get_if<cpu::Trap>(&std::get<Killed>(end.status).reason)) {
            end_status = static_cast<int32_t>(trap->cause) + 128;           // a fault
        } else {
            end_status = static_cast<int32_t>(std::get<Signal>(std::get<Killed>(end.status).reason).number) + 256;
        }
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
        auto access = try_access(addr, mmu::Access::store);
        if (std::holds_alternative<Blocked>(access)) {
            return retry_call();
        }
        if (std::holds_alternative<Fault>(access)) {
            cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEfault));
            return Continue{};
        }
        end.collected = true;
        ram().write(std::get<uint32_t>(access), 4, end_status);
        cpu().set_reg(isa::reg::a0, pid);
        return Continue{};
    } 
    thread().blocked = kernel::BlockReason::Wait;
    cpu().pc -= 4;
    return Switch{};
}

Continue Kernel::sys_other() {
    cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEnosys));
    return Continue{};
}

}  // namespace kernel
