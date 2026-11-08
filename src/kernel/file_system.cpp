// The file system in the kernel: the block cache, paths and the file system calls (spec C2, C3).
#include <cassert>

#include "kernel/abi.hpp"
#include "kernel/kernel.hpp"
#include "kernel/clock.hpp"
#include "loader/elf.hpp"
#include "util/bytes.hpp"
#include "util/variant.hpp"

namespace kernel {

std::variant<Continue, Switch> Kernel::sys_open() {
    uint32_t addr = cpu().reg(isa::reg::a0);
    std::variant<uint32_t, Missing, int32_t, Blocked> result = resolve_user_path(addr);
    if (std::holds_alternative<Blocked>(result)) {
        return retry_call();
    }
    if (auto error = std::get_if<int32_t>(&result)) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(*error));
        return Continue{};
    }
    if (auto missing = std::get_if<Missing>(&result)) {
        return wait_for(missing->block);
    }
    auto inode_index = std::get<uint32_t>(result);
    auto fd = find_free_fd(process());
    if (!fd) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEmfile));
        return Continue{};
    }
    auto open_file = find_free_open_file();
    if (!open_file) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEnfile));
        return Continue{};
    }

    file_table_[*open_file] = OpenFile{.kind = InodeFile{.inode = inode_index, .offset = 0}, .refs = 1};
    process().fd_table[*fd] = *open_file;
    cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(*fd));
    return Continue{};
}

Continue Kernel::sys_close() {
    uint32_t fd = cpu().reg(isa::reg::a0);
    if (!open_file_of(fd)) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEbadf));
        return Continue{};
    }
    uint32_t entry = *process().fd_table[fd];
    process().fd_table[fd].reset();
    release_open_file(entry);
    cpu().set_reg(isa::reg::a0, 0);
    return Continue{};
}

std::variant<Continue, Switch> Kernel::sys_lseek() {
    uint32_t fd = cpu().reg(isa::reg::a0);
    int32_t tmp_offset = static_cast<int32_t>(cpu().reg(isa::reg::a1));
    int64_t offset = static_cast<int64_t>(tmp_offset);
    int32_t whence = static_cast<int32_t>(cpu().reg(isa::reg::a2));

    OpenFile* file = open_file_of(fd);
    if (!file) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEbadf));
        return Continue{};
    }
    OpenFile& open_file = *file;
    if (std::holds_alternative<PipeFile>(open_file.kind)) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEspipe));
        return Continue{};
    }
    assert(std::holds_alternative<InodeFile>(open_file.kind));
    InodeFile& inode_file = std::get<InodeFile>(open_file.kind);

    int64_t new_offset;
    switch (whence) {
        case 0: // SEEK_SET
            new_offset = offset;
            break;
        case 1: // SEEK_CUR
            new_offset = inode_file.offset + offset;
            break;
        case 2: { // SEEK_END 
            auto inode_variant = inode_of(inode_file.inode);
            if (auto missing = std::get_if<Missing>(&inode_variant)) {
                return wait_for(missing->block);
            }
            const fs::Inode& inode = std::get<fs::Inode>(inode_variant);
            new_offset = inode.size + offset;
            break;
        } 
        default:
            cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEinval));
            return Continue{};
    }

    if (new_offset < 0) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEinval));
        return Continue{};
    }
    if (new_offset > 0x7FFFFFFF) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEinval));
        return Continue{};
    }

    inode_file.offset = new_offset;
    cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(new_offset));
    return Continue{};
}

std::variant<Continue, Switch> Kernel::read_inode_file(InodeFile& inode_file, uint32_t addr, uint32_t count) {
    auto node = inode_of(inode_file.inode);
    if (auto missing = std::get_if<Missing>(&node)) {
        return wait_for(missing->block);
    }
    const fs::Inode& inode = std::get<fs::Inode>(node);
    if (inode_file.offset >= inode.size) {
        cpu().set_reg(isa::reg::a0, 0);
        return Continue{};
    }
    uint32_t bytes_to_read = std::min({count, inode.size - inode_file.offset, fs::kBlockSize - (inode_file.offset % fs::kBlockSize)});
    uint32_t block_index = inode_file.offset / fs::kBlockSize;
    auto block_variant = block_of(inode, block_index);
    if (auto missing = std::get_if<Missing>(&block_variant)) {
        return wait_for(missing->block);
    }
    uint32_t block_num = std::get<uint32_t>(block_variant);
    auto block = cache_get(block_num);
    if (!block) {
        return wait_for(block_num);
    }
    for (uint32_t i = 0; i < bytes_to_read; ++i) {
        auto access = try_access(addr + i, mmu::Access::store);
        if (std::holds_alternative<Blocked>(access)) {
            return retry_call();
        }
        if (std::holds_alternative<Fault>(access)) {
            cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEfault));
            return Continue{};
        }
    }
    for (uint32_t i = 0; i < bytes_to_read; ++i) {
        auto access = try_access(addr + i, mmu::Access::store);
        assert(std::holds_alternative<uint32_t>(access));
        bool wrote = ram().write(std::get<uint32_t>(access), 1, (*block)[inode_file.offset % fs::kBlockSize + i]);
        assert(wrote);
    }
    cpu().set_reg(isa::reg::a0, bytes_to_read);
    inode_file.offset += bytes_to_read;
    return Continue{};
}

void Kernel::wake_pipe_waiters(Pipe& pipe) {
    for (auto& [tid, thread] : thread_table_) {
        if (thread.blocked == BlockReason::Pipe && thread.waiting_pipe == &pipe) {
            thread.blocked = BlockReason::None;
            thread.waiting_pipe = nullptr;
        }
    }
}

std::variant<Continue, Switch> Kernel::read_pipe(PipeFile& file, uint32_t addr, uint32_t count) {
    if (file.end != PipeEnd::Read) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEbadf));
        return Continue{};
    }
    if (count == 0) {
        cpu().set_reg(isa::reg::a0, 0);
        return Continue{};
    }
    if (file.pipe->count == 0 && file.pipe->write_open) {
        thread().blocked = BlockReason::Pipe;
        thread().waiting_pipe = file.pipe.get();
        return retry_call();
    }
    uint32_t bytes_read = std::min(count, file.pipe->count);
    for (uint32_t i = 0; i < bytes_read; ++i) {
        auto access = try_access(addr + i, mmu::Access::store);
        if (std::holds_alternative<Blocked>(access)) {
            return retry_call();
        }
        if (std::holds_alternative<Fault>(access)) {
            cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEfault));
            return Continue{};
        }
    }
    for (uint32_t i = 0; i < bytes_read; ++i) {
        auto access = try_access(addr + i, mmu::Access::store);
        assert(std::holds_alternative<uint32_t>(access));
        bool wrote = ram().write(std::get<uint32_t>(access), 1, file.pipe->buffer[file.pipe->head]);
        assert(wrote);
        file.pipe->head = (file.pipe->head + 1) % kPipeSize;
    }
    cpu().set_reg(isa::reg::a0, bytes_read);
    file.pipe->count -= bytes_read;
    wake_pipe_waiters(*file.pipe);
    return Continue{};
}

std::variant<Continue, Switch> Kernel::write_pipe(PipeFile& file, uint32_t addr, uint32_t count) {
    if (file.end != PipeEnd::Write) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEbadf));
        return Continue{};
    }
    if (!file.pipe->read_open) {
        SignalAction action = process().signal_actions[abi::kSigpipe];
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEpipe));
        if (action.handler != abi::kSigIgnore) {
            process().pending_flags |= (1 << abi::kSigpipe);
            return Continue{};
        }
        return Continue{};
    }
    if (count == 0) {
        cpu().set_reg(isa::reg::a0, 0);
        return Continue{};
    }
    if (file.pipe->count == kPipeSize) {
        thread().blocked = BlockReason::Pipe;
        thread().waiting_pipe = file.pipe.get();
        return retry_call();
    }
    uint32_t bytes_written = std::min(count, kPipeSize - file.pipe->count);
    // Checks every user byte before the buffer changes, so -EFAULT adds nothing.
    auto bytes = read_memory_on_ram(addr, bytes_written);
    if (std::holds_alternative<Blocked>(bytes)) {
        return retry_call();
    }
    if (std::holds_alternative<Fault>(bytes)) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEfault));
        return Continue{};
    }
    for (uint8_t byte : std::get<std::vector<uint8_t>>(bytes)) {
        file.pipe->buffer[(file.pipe->head + file.pipe->count) % kPipeSize] = byte;
        ++file.pipe->count;
    }
    cpu().set_reg(isa::reg::a0, bytes_written);
    wake_pipe_waiters(*file.pipe);
    return Continue{};
}

std::variant<Continue, Switch> Kernel::sys_read() {
    uint32_t fd = cpu().reg(isa::reg::a0);
    uint32_t buffer_addr = cpu().reg(isa::reg::a1);
    uint32_t bytes_count = cpu().reg(isa::reg::a2);
    OpenFile* file = open_file_of(fd);
    if (!file) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEbadf));
        return Continue{};
    }
    if (auto* inode_file = std::get_if<InodeFile>(&file->kind)) {
        return read_inode_file(*inode_file, buffer_addr, bytes_count);
    } else if (auto* pipe_file = std::get_if<PipeFile>(&file->kind)) {
        return read_pipe(*pipe_file, buffer_addr, bytes_count);
    } else {
        assert(false && "Unknown file type");
    }
    
}

std::variant<Continue, Switch> Kernel::sys_exec() {
    auto found = resolve_user_path(cpu().reg(isa::reg::a0));
    if (std::holds_alternative<Blocked>(found)) {
        return retry_call();
    }
    if (auto error = std::get_if<int32_t>(&found)) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(*error));
        return Continue{};
    }
    if (auto missing = std::get_if<Missing>(&found)) {
        return wait_for(missing->block);
    }
    auto inode_index = std::get<uint32_t>(found);
    auto found_inode = inode_of(inode_index);
    if (auto missing = std::get_if<Missing>(&found_inode)) {
        return wait_for(missing->block);
    }
    const fs::Inode& inode = std::get<fs::Inode>(found_inode);
    if (inode.type == fs::InodeType::directory) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEnoexec));
        return Continue{};
    }
    auto gathered_bytes = file_bytes(inode);
    if (auto missing = std::get_if<Missing>(&gathered_bytes)) {
        return wait_for(missing->block);
    }
    mmu::PageTable fresh;
    const std::vector<uint8_t>& elf_bytes = std::get<std::vector<uint8_t>>(gathered_bytes);
    auto loaded = loader::load_elf(elf_bytes, ram(), frame_allocator(), fresh);
    if (std::holds_alternative<std::string>(loaded)) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEnoexec));
        return Continue{};
    }
    bool blocking_thread = false;
    const std::vector<uint32_t> threads = process().threads;
    uint32_t calling_tid = thread().tid;
    for (uint32_t tid : threads) {
        if (tid == calling_tid) continue; // the calling thread will be replaced by the new program
        Thread& thread = thread_table_.at(tid);
        if (thread.blocked == BlockReason::IO) {
            blocking_thread = true;
            thread.must_end = true;
        } else {
            end_thread(tid);
        }
    }
    if (blocking_thread) {
        free_address_space(fresh);
        thread().blocked = BlockReason::Wait;
        cpu().pc -= 4;
        return Switch{};
    }
    free_address_space(process().page_table);
    page_table() = std::move(fresh);
    const auto& program = std::get<loader::Program>(loaded);
    for (SignalAction& singal_action : process().signal_actions) {
        if (singal_action.handler != abi::kSigDefault && singal_action.handler != abi::kSigIgnore) {
            singal_action = {};
        }
    }
    process().brk = process().initial_break = program.initial_break;
    for (const auto& [start_addr, region_id] : process().attachments) {
        Region& region = region_table_.at(region_id);
        --region.attachment_count;
    }
    process().attachments.clear();
    thread().context = {};
    thread().context.pc = program.entry;
    thread().context.regs[isa::reg::sp] = static_cast<uint32_t>(loader::kStackTop);
    cpu().set_context(thread().context);
    return Continue{};
}

void Kernel::share_open_file(uint32_t entry) {
    assert(file_table_[entry]);
    ++file_table_[entry]->refs;
}

void Kernel::release_open_file(uint32_t entry) {
    assert(file_table_[entry]);
    OpenFile& open_file = *file_table_[entry];
    if (--open_file.refs == 0) {
        if (auto pipe_file = std::get_if<PipeFile>(&open_file.kind)) {
            if (pipe_file->end == PipeEnd::Read) {
                pipe_file->pipe->read_open = false;
            } else {
                pipe_file->pipe->write_open = false;
            }
            wake_pipe_waiters(*pipe_file->pipe);
        }
        file_table_[entry].reset();
    }
}

std::optional<std::array<uint32_t, 2>> Kernel::find_two_free_fds(const Process& process) const {
    std::array<uint32_t, 2> found{};
    uint32_t count = 0;
    for (uint32_t fd = 3; fd < kMaxFileDescriptors; ++fd) {
        if (!process.fd_table[fd]) {
            found[count++] = fd;
            if (count == 2) {
                return found;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::array<uint32_t, 2>> Kernel::find_two_free_open_files() const {
    std::array<uint32_t, 2> found{};
    uint32_t count = 0;
    for (uint32_t i = 0; i < kMaxOpenFiles; ++i) {
        if (!file_table_[i]) {
            found[count++] = i;
            if (count == 2) {
                return found;
            }
        }
    }
    return std::nullopt;
}

std::optional<uint32_t> Kernel::find_free_open_file() const {
    for (uint32_t i = 0; i < kMaxOpenFiles; ++i) {
        if (!file_table_[i]) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<uint32_t> Kernel::find_free_fd(const Process& process) const {
    for (uint32_t fd = 3; fd < kMaxFileDescriptors; ++fd) {
        if (!process.fd_table[fd]) {
            return fd;
        }
    }
    return std::nullopt;
}

std::optional<fs::Block> Kernel::cache_get(uint32_t b) { 
    auto it = cached_blocks_.find(b);
    if (it != cached_blocks_.end() && it->second.ready) {
        uint32_t frame = it->second.frame;
        auto data = ram().read_bytes(frame << mmu::kPageShift, mmu::kPageSize);
        if (data) {
            fs::Block block;
            std::copy(data->begin(), data->end(), block.begin());
            return block;
        }
    }
    return std::nullopt;
}

Kernel::OpenFile* Kernel::open_file_of(uint32_t fd) {
    if (fd < 3 || fd >= kMaxFileDescriptors || !process().fd_table[fd]) {
        return nullptr;
    }
    const uint32_t entry = *process().fd_table[fd];
    assert(file_table_[entry]);
    return &*file_table_[entry];
}

std::variant<Continue, Switch> Kernel::wait_for(uint32_t b) {
    if (!cache_fetch(b)) {
        cpu().set_reg(isa::reg::a0, static_cast<uint32_t>(-abi::kEnomem));
        return Continue{};
    }
    return block_on(b);
}

Switch Kernel::block_on(uint32_t b) {
    thread().blocked = BlockReason::IO;
    thread().waiting_block = b;
    cpu().pc -= 4; // Rewind the program counter to retry the syscall later
    return Switch{};
}

Switch Kernel::retry_call() {
    assert(thread().blocked != BlockReason::None);
    cpu().pc -= 4;
    return Switch{};
}

bool Kernel::cache_fetch(uint32_t b) { 
    assert(b < fs::kNumBlocks);
    auto it = cached_blocks_.find(b);
    if (it != cached_blocks_.end()) {
        return true;
    }
    auto frame = frame_allocator().allocate();
    if (!frame) {
        return false;
    }
    cached_blocks_.emplace(b, CachedBlock{.frame = *frame, .ready = false});
    CacheFill fill{.block = b};
    enqueue(PendingIO{fill, disk::Request{
        .op = disk::Op::read, .first_sector = b * fs::kSectorsPerBlock, 
        .count = fs::kSectorsPerBlock, 
        .ram_addr = *frame << mmu::kPageShift}});
    return true;
}

std::variant<uint32_t, Kernel::Missing, int32_t> Kernel::walk(std::string_view path) {
    if (path.empty() || path[0] != '/') {
        return static_cast<int32_t>(-abi::kEinval);
    }
    size_t pos = 1; // Skip the leading '/'
    uint32_t inode_index = fs::kRootInode;
    while (pos < path.size()) {
        size_t next_slash = path.find('/', pos);
        if (next_slash == std::string_view::npos) {
            next_slash = path.size();
        }
        if (next_slash == pos) {
            ++pos;
            continue;
        }
        std::string_view component = path.substr(pos, next_slash - pos);
        if (!fs::valid_name(component)) {
            return static_cast<int32_t>(-abi::kEinval);
        }
        auto cur_node = inode_of(inode_index);
        if (auto missing = std::get_if<Missing>(&cur_node)) {
            return *missing;
        }
        const fs::Inode& dir = std::get<fs::Inode>(cur_node);
        if (dir.type != fs::InodeType::directory) {
            return static_cast<int32_t>(-abi::kEnotdir);
        }
        std::optional<uint32_t> next;
        for (uint32_t k = 0; !next && k * fs::kBlockSize < dir.size; ++k) {
            auto block_num = block_of(dir, k);
            if (auto missing = std::get_if<Missing>(&block_num)) {
                return *missing;
            }
            auto block = cache_get(std::get<uint32_t>(block_num));
            if (!block) {
                return Missing{.block = std::get<uint32_t>(block_num)};
            }
            next = fs::find_in_dir_block(*block, dir.size - k * fs::kBlockSize, component);
        }
        if (!next) {
            return static_cast<int32_t>(-abi::kEnoent);
        }
        inode_index = *next;
        pos = next_slash + 1;
    }
    return inode_index;
}

std::variant<fs::Inode, Kernel::Missing> Kernel::inode_of(uint32_t i) {
    const auto [block_num, offset] = fs::inode_location(i);
    auto block = cache_get(block_num);
    if (!block) {
        return Missing{.block = block_num};
    }
    return fs::decode_inode(std::span<const uint8_t, fs::kInodeSize>(block->data() + offset, fs::kInodeSize));
}

std::variant<uint32_t, Kernel::Missing> Kernel::block_of(const fs::Inode& inode, uint32_t k) {
    assert(k < fs::kMaxFileBlocks);
    if (k < fs::kNumDirect) {
        return inode.direct[k];
    }
    auto indirect_block = cache_get(inode.indirect);
    if (!indirect_block) {
        return Missing{.block = inode.indirect};
    }
    return util::get32(*indirect_block, (k - fs::kNumDirect) * 4);
}

std::variant<uint32_t, Kernel::Missing, int32_t, Blocked> Kernel::resolve_user_path(uint32_t addr) {
    std::variant<std::string, int32_t, Blocked> path = read_user_string(addr, kMaxPathBytes);
    if (std::holds_alternative<Blocked>(path)) {
        return Blocked{};
    }
    if (auto error = std::get_if<int32_t>(&path)) {
        return *error;
    }
    return util::convert_variant<uint32_t, Missing, int32_t, Blocked>(walk(std::get<std::string>(path)));
}

std::variant<std::vector<uint8_t>, Kernel::Missing> Kernel::file_bytes(const fs::Inode& inode) {
    std::vector<uint8_t> bytes;
    bytes.reserve(inode.size);
    for (uint32_t k = 0; k * fs::kBlockSize < inode.size; ++k) {
        auto block_num_variant = block_of(inode, k);
        if (auto missing = std::get_if<Missing>(&block_num_variant)) {
            return *missing;
        }
        uint32_t block_num = std::get<uint32_t>(block_num_variant);
        auto block = cache_get(block_num);
        if (!block) {
            return Missing{.block = block_num};
        }
        uint32_t bytes_to_copy = std::min(fs::kBlockSize, inode.size - k * fs::kBlockSize);
        bytes.insert(bytes.end(), block->begin(), block->begin() + bytes_to_copy);
    }
    return bytes;
}

void Kernel::free_address_space(mmu::PageTable& table) {
    for (uint32_t vpn : table.mapped_pages()) {
        auto pte = table.lookup(vpn);
        assert(pte);
        drop_entry(*pte);
    }
}

std::optional<Kernel::Victim> Kernel::choose_victim() {
    std::vector<ClockFrame> candidates(mmu::kNumFrames);
    std::unordered_map<uint32_t, FrameOwner> frame_owners; 
    std::array<bool, mmu::kNumFrames> busy_frame{false}; // a request is only for 1 frame
    // std::deque<PendingIO> pending_ios_;
    for (PendingIO& pending : pending_ios_) {
        disk::Request& request = pending.request;
        uint32_t frame = request.ram_addr >> mmu::kPageShift;
        busy_frame[frame] = true;
    }
    for (auto &[pid, process] : process_table_) {
        for (uint32_t vpn : process.page_table.mapped_pages()) {
            auto pte = process.page_table.lookup(vpn);
            assert(pte);
            if (pte->valid && frame_allocator().ref_count(pte->frame) == 1 && !busy_frame[pte->frame]) {
                candidates[pte->frame] = ClockFrame{.candidate = true, .accessed = pte->accessed};
                frame_owners[pte->frame] = FrameOwner{.pid = pid, .vpn = vpn};
            }
        }
    }
    auto clock_choice = clock_choose(candidates, clock_hand_);
    if (!clock_choice) {
        return std::nullopt;
    }
    clock_hand_ = clock_choice->hand;
    for (auto frame : clock_choice->cleared) {
        for (auto &[pid, process] : process_table_) {
            for (uint32_t vpn : process.page_table.mapped_pages()) {
                auto pte = process.page_table.lookup(vpn);
                assert(pte);
                if (pte->valid && pte->frame == frame) {
                    pte->accessed = false;
                    process.page_table.map(vpn, *pte);
                }
            }
        }
    }
    return Victim{.frame = clock_choice->victim, .pid = frame_owners[clock_choice->victim].pid, .vpn = frame_owners[clock_choice->victim].vpn}; // TODO: fill in pid and vpn
}

}  // namespace kernel
