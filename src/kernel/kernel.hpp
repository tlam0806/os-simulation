#pragma once
#include <array>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <map>
#include <optional>
#include <queue>
#include "kernel/process.hpp"
#include "machine/machine.hpp"
#include "fs/layout.hpp"
namespace kernel {
struct StepLimit {
};
// A syscall finished: resume the same process right after the ecall.
struct Continue {
};
// The process's turn is over (timer interrupt): switch to the next process.
struct Switch {
};

struct ThreadExited {
    int32_t status = 0;
};

struct Blocked {
};

struct Fault {
};


// How a process ended.
// using Ending = std::variant<Exited, Killed>;

struct Ending {
    uint32_t parent_pid = 0;
    std::variant<Exited, Killed> status;
    bool collected = false;
};

// run_all() stopped because every process has ended.
struct AllDone {
};

struct CachedBlock {
    uint32_t frame;
    bool ready;
};

struct Region {
    std::vector<std::optional<uint32_t> > frames;
    uint32_t attachment_count = 0;
};

// A page inside one of a process's attachments.
struct AttachedPage {
    Region* region;
    uint32_t index;   // the page's index in the region
};

// Instructions a process may run before the timer switches to the next one (spec A4).
inline constexpr uint64_t kQuantum = 1000;
inline constexpr uint32_t kMaxOpenFiles = 64;
inline constexpr uint32_t kMaxPathBytes = 256;   // a path for open, with its zero byte
inline constexpr uint32_t kMaxSharedRegions = 8;

class Kernel {
public :
    // A disk read into the block cache (spec C2), not on behalf of one process.
    struct CacheFill {
        uint32_t block;
    };

    // A cache lookup that missed: block must be fetched before the call can go on.
    struct Missing {
        uint32_t block;
    };

    struct SwapOut {
        uint32_t tid; 
        uint32_t frame; 
    };

    struct SwapIn {
        uint32_t pid;
        uint32_t vpn;
        uint32_t frame;
    };

    struct Victim {
        uint32_t frame;
        uint32_t pid;
        uint32_t vpn;
    };

    struct FrameOwner {
        uint32_t pid;
        uint32_t vpn;
    };

    // A request in the disk queue, and whom its completion is for:
    //   uint32_t   a user's disk_read / disk_write: wake this pid, its a0 = 0
    //   SwapOut    a swap-out: wake the process, its a0 = 0
    //   SwapIn     a swap-in: wake the process, its a0 = 0
    //   CacheFill  the cache: the block is ready, wake everyone waiting for it
    struct PendingIO {
        std::variant<uint32_t, CacheFill, SwapOut, SwapIn> owner;
        disk::Request request;
    };

    enum class PipeEnd { Read, Write };

    struct InodeFile {
        uint32_t inode{};
        uint32_t offset{};
    };

    // One end of a pipe. Both ends share the Pipe, which is freed with the last of them.
    struct PipeFile {
        std::shared_ptr<Pipe> pipe;
        PipeEnd end = PipeEnd::Read;
    };

    struct OpenFile {
        std::variant<InodeFile, PipeFile> kind;
        uint32_t refs{};
    };


    enum class EvictResult {
        Freed,
        Blocked, 
        Failed
    };

    Kernel(machine::Machine& machine, std::ostream& out, std::ostream& err) : 
        machine_{machine},
        out_{out},
        err_{err}
    {}    

    // Creates a new process from an ELF file (spec B2): its own page table,
    // built by the loader with frames from the machine's frame allocator, and
    // a context with pc = entry, sp = loader::kStackTop, all other registers 0.
    // Its break starts at the loader's initial_break.
    // Returns the new pid (1, 2, 3, ... in creation order), or the loader's
    // error message. A failed spawn creates no process and uses no pid.
    // It does not run the process or touch the CPU.
    std::variant<uint32_t, std::string> spawn(std::span<const uint8_t> elf);

    // The process with this pid, or nullptr if there is none.
    const Process* process(uint32_t pid) const;

    // The thread with this tid, or nullptr if there is none.
    const Thread* thread(uint32_t tid) const;

    // How many processes exist.
    size_t process_count() const;

    // Runs all processes in the table, taking turns (round robin, in pid order),
    // until every process has ended (AllDone) or the clock has advanced by
    // max_steps in this call (StepLimit). That counts instructions of all
    // processes and idle time (all blocked: the machine waits for an interrupt).
    //   - Each turn lasts at most kQuantum instructions: the machine timer
    //     interrupts the process, the kernel saves its context and moves on to
    //     the next pid (wrapping around to the lowest).
    //   - A process ends when it calls exit or a trap kills it. The kernel
    //     records how it ended (see ending()), frees all its frames, removes it
    //     from the table, and continues with the next process.
    //   - A blocked process (Process::blocked) is skipped: a wait with nothing to
    //     collect yet, or a disk_read / disk_write whose request hasn't
    //     completed. The end of a child it waits for, or the disk interrupt for
    //     its request, makes it runnable again.
    //   - Disk requests are queued and given to the disk one at a time, first
    //     come first served (spec B3, disk_read / disk_write).
    //   - System calls act on the process that made them: its memory and its break.
    //   - After StepLimit, calling run_all() again continues where it stopped.
    std::variant<AllDone, StepLimit> run_all(uint64_t max_steps);

    // How the process with this pid ended, or nullopt if it hasn't ended
    // (still in the table) or never existed.
    std::optional<Ending> ending(uint32_t pid) const;
    
private : 
    uint32_t executing_tid_{};
    uint32_t next_pid_{};
    uint32_t next_region_id{};
    std::map<uint32_t, Process> process_table_{};
    std::map<uint32_t, Thread> thread_table_{};
    std::map<uint32_t, Ending> ending_table_{};
    machine::Machine& machine_;
    std::ostream& out_;
    std::ostream& err_;
    std::deque<PendingIO> pending_ios_;
    std::array<std::optional<OpenFile>, kMaxOpenFiles> file_table_{};
    std::map<uint32_t, Region> region_table_{};
    std::unordered_map<uint32_t, CachedBlock> cached_blocks_{};
    std::array<uint32_t, fs::kSwapBlocks> slot_refs_{};
    uint32_t clock_hand_{};


    std::variant<Continue, Switch> read_inode_file(InodeFile& file, uint32_t addr, uint32_t count);

    void wake_pipe_waiters(Pipe& pipe);
    std::variant<Continue, Switch> read_pipe(PipeFile& file, uint32_t addr, uint32_t count);
    std::variant<Continue, Switch> write_pipe(PipeFile& file, uint32_t addr, uint32_t count);


    std::variant<Continue, Switch> disk_io(disk::Op op);
    
    std::variant<Exited, Killed, Continue, Switch, ThreadExited> handle_trap(const cpu::Trap& trap);
    
    std::variant<Exited, Continue, Switch, ThreadExited> handle_syscall();

    std::variant<std::vector<uint8_t>, Blocked, Fault> read_memory_on_ram(uint32_t addr, uint32_t bytes_count); 

    Continue sys_shmdt();
    Continue sys_shmat();
    Continue sys_shmget();
    Continue sys_shmrm();
    
    std::variant<Continue, Switch> sys_pipe();

    ThreadExited sys_thread_exit();

    std::variant<Continue, Switch> sys_thread_join();

    Continue sys_gettid();

    Continue sys_thread_create();

    std::variant<Continue, Switch> sys_exec();

    std::variant<Continue, Switch> sys_read();

    std::variant<Continue, Switch> sys_lseek();

    Continue sys_close();

    std::variant<Continue, Switch> sys_open();

    Exited sys_exit(); 

    std::variant<Continue, Switch> sys_write(); 

    Continue sys_sbrk(); 

    Continue sys_fork();

    std::variant<Continue, Switch> sys_wait();

    Continue sys_other(); 

    Continue sys_getpid();

    std::variant<Continue, Switch> sys_disk_read();

    std::variant<Continue, Switch> sys_disk_write();

    std::variant<Killed, Continue, Switch> handle_page_fault(const cpu::Trap& trap);

    Continue handle_disk_interrupt();

    Switch sys_yield();

    mem::Ram& ram() {
        return machine_.ram();
    }

    cpu::Cpu& cpu() {
        return machine_.cpu();
    }

    const cpu::Cpu& cpu() const {
        return machine_.cpu();
    }

    Thread& thread() {
        return thread_table_.find(executing_tid_)->second;
    }

    const Thread& thread() const {
        return thread_table_.find(executing_tid_)->second;
    }

    Process& process() {  // context is not up to date when the process is running, needs to load again
        const Thread& current_thread = thread();
        return process_table_.find(current_thread.pid)->second;
    }

    const Process& process() const {  // context is not up to date when the process is running, needs to load again
        const Thread& current_thread = thread();
        return process_table_.find(current_thread.pid)->second;
    }

    uint32_t current_break() {
        return process().brk;
    }

    uint32_t initial_break() {
        return process().initial_break;
    }

    mmu::PageTable& page_table() {
        return process().page_table;
    }

    const mmu::PageTable& page_table() const {
        return process().page_table;
    }

    disk::Disk& disk() {
        return machine_.disk();
    }

    // Adds io to the FCFS disk queue, and starts it at once if the disk is idle.
    void enqueue(PendingIO io);

    void start_next_request();

    mmu::PageTable share_table(mmu::PageTable& other);

    void copy_frame(uint32_t old_frame, uint32_t new_frame);

    mmu::FrameAllocator& frame_allocator() {
        return machine_.frame_allocator();
    }

    const mmu::FrameAllocator& frame_allocator() const {
        return machine_.frame_allocator();
    }

    bool in_region(uint64_t addr);
    // The attachment page of the current process that holds addr, if there is one.
    std::optional<AttachedPage> find_attached_page(uint32_t addr);

    bool map_page(uint32_t vpn);

    bool unmap_page(uint32_t vpn);

    void zero_frame(uint32_t frame);

    std::variant<std::string, int32_t, Blocked> read_user_string(uint32_t addr, uint32_t max_bytes);

    // The index of the first empty open-file slot, or nullopt. Doesn't take it.
    std::optional<uint32_t> find_free_open_file() const;

    std::optional<std::array<uint32_t, 2>> find_two_free_fds(const Process& process) const;

    std::optional<std::array<uint32_t, 2>> find_two_free_open_files() const;

    // The lowest empty fd from 3 up, or nullopt. Doesn't take it.
    std::optional<uint32_t> find_free_fd(const Process& process) const;

    std::variant<uint32_t, Blocked, Fault> try_access(uint32_t addr, mmu::Access access);

    Thread* find_next_thread(uint32_t last_exec);

    void end_all_threads(Process& process, std::variant<Exited, Killed> status);

    void end_thread(uint32_t tid);

    void end_process(uint32_t pid);

    void save_thread(Thread& thread);
    
    void load_thread(Thread& thread);

    uint64_t run_helper(uint64_t max_step);

    uint32_t create_process(Process&& process, const cpu::Context& context);

    std::optional<mmu::Pte> handle_cow(mmu::Pte pte);

    std::vector<uint32_t> get_running_children_process(uint32_t parent_pid, uint32_t pid);
    std::vector<uint32_t> get_ended_children_process(uint32_t parent_pid, uint32_t pid);

    Switch block_on(uint32_t b);

    // A user page the call needs is on its way (D7): re-run the ECALL once it arrives.
    // evict / swap_in have already blocked the process.
    Switch retry_call();
    // Block b is needed but not cached: fetch it and block the caller (pc back on
    // its ECALL), or -ENOMEM in a0 if no frame is free for it.
    std::variant<Continue, Switch> wait_for(uint32_t b);
    // The open-file entry the caller's fd points to, or nullptr if fd isn't an
    // open file (out of range, 0..2, or an empty slot).
    OpenFile* open_file_of(uint32_t fd);
    std::optional<fs::Block> cache_get(uint32_t b);
    bool cache_fetch(uint32_t b);

    // Inode i, decoded from its inode-table block in the cache, or Missing{that block}.
    std::variant<fs::Inode, Missing> inode_of(uint32_t i);

    // The disk block number of file block k of inode (direct blocks only, k < 12).
    // Doesn't read the block itself.
    std::variant<uint32_t, Missing> block_of(const fs::Inode& inode, uint32_t k);

    // The path walk through the cache (spec C3, open). path is absolute.
    //   uint32_t  the inode the path names
    //   Missing   a block the walk needs is not cached; fetch it and re-run the call
    //   int32_t   -EINVAL, -ENOTDIR or -ENOENT
    std::variant<uint32_t, Missing, int32_t> walk(std::string_view path);

    // Reads the path at user address addr and walks it: the same three results as
    // walk, plus -EFAULT / -ENAMETOOLONG from reading the path, or Blocked while a page
    // of the path is swapped in.
    std::variant<uint32_t, Missing, int32_t, Blocked> resolve_user_path(uint32_t addr);

    // One more fd points to open-file entry `entry` (fork).
    void share_open_file(uint32_t entry);
    // One fd less points to `entry`; the entry is freed when none is left.
    void release_open_file(uint32_t entry);

    std::variant<std::vector<uint8_t>, Missing> file_bytes(const fs::Inode& inode);

    void free_address_space(mmu::PageTable& table);

    std::optional<Victim> choose_victim();

    std::optional<uint32_t> alloc_slot();

    bool fault_needs_frame(const std::optional<mmu::Pte>& found, const cpu::Trap& trap) const;

    EvictResult evict();

    void swap_in(uint32_t vpn, const mmu::Pte& pte);

    void drop_entry(const mmu::Pte&);
};


}