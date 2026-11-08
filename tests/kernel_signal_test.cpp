// Signals (spec F3), in build stages. Each stage's filter runs only its tests:
//
//   sig1_   signal and kill with default and ignore: no handler runs yet
//   sig2_   handlers: the signal frame, redirecting, sigreturn
//   sig3_   a handler wakes a blocked thread, and its call runs again
//   sig4_   signals from the kernel: SIGSEGV on a fault, SIGPIPE on a pipe
//
// A program with handlers starts with a jump over its restorer (begin) and its
// handlers, which are emitted next, then patched to the main code.
#include <string>
#include <variant>
#include <vector>

#include "fs_system.hpp"

using namespace fs_system;

namespace {

constexpr int32_t kEsrch = 3, kEpipe = 32;
constexpr int32_t kSigkill = 9, kSigusr1 = 10, kSigsegv = 11, kSigusr2 = 12, kSigpipe = 13;
constexpr int32_t kDefault = 0, kIgnore = 1;
constexpr uint32_t kStatus = kBuf + 16;         // wait's status
constexpr uint32_t kVar = kBuf + 32;            // a stack word that handlers write
constexpr uint32_t kByte = kBuf + 48;           // a byte to send through a pipe
constexpr uint32_t kFds = kBuf + 64;            // pipe's two fds
constexpr uint32_t kIn = kBuf + 128;            // read's buffer
constexpr int32_t kRead = 3, kWrite = 4;        // the first pipe's fds
constexpr int32_t kRead2 = 5, kWrite2 = 6;      // the second pipe's fds
constexpr uint64_t kShort = 2'000'000;
constexpr uint64_t kDisk = 100'000'000;

constexpr int32_t by_signal(int32_t sig) { return 256 + sig; }

void expect(Asm& a, uint8_t r, int32_t value, int32_t code) {
    a.li(t6, value);
    const size_t ok = a.branch(BEQ, r, t6);
    a.exit_with(code);
    a.patch(ok, a.here());
}

uint32_t here_addr(const Asm& a) { return 0x10000 + static_cast<uint32_t>(a.here()) * 4; }

// Jumps over the restorer and whatever comes next (the handlers): patch the
// returned index to the main code. `restorer` is the restorer's address.
size_t begin(Asm& a, uint32_t& restorer) {
    const size_t skip = a.branch(JAL, zero, zero);
    restorer = here_addr(a);
    a.syscall(SYS_SIGRETURN);
    return skip;
}

void ret(Asm& a) { a.emit({JALR, zero, ra, 0, 0}); }

void signal(Asm& a, int32_t sig, uint32_t handler, uint32_t restorer = 0) {
    a.li(a0, sig);
    a.li(a1, static_cast<int32_t>(handler));
    a.li(a2, static_cast<int32_t>(restorer));
    a.syscall(SYS_SIGNAL);
}

void kill(Asm& a, int32_t pid, int32_t sig) {
    a.li(a0, pid);
    a.li(a1, sig);
    a.syscall(SYS_KILL);
}

// kill(the pid in register r, sig)
void kill_reg(Asm& a, uint8_t r, int32_t sig) {
    a.emit({ADDI, a0, r, 0, 0});
    a.li(a1, sig);
    a.syscall(SYS_KILL);
}

void kill_self(Asm& a, int32_t sig) {
    a.syscall(SYS_GETPID);
    a.li(a1, sig);
    a.syscall(SYS_KILL);
}

// In the parent, s4 = the child's pid.
void fork_to_parent(Asm& a, size_t& to_parent) {
    a.syscall(SYS_FORK);
    to_parent = a.branch(BNE, a0, zero);
}

void at_parent(Asm& a, size_t to_parent) {
    a.patch(to_parent, a.here());
    a.emit({ADDI, s4, a0, 0, 0});
}

// wait(any, kStatus); a0 = the child's status.
void wait_status(Asm& a) {
    a.li(a0, -1);
    a.li(a1, static_cast<int32_t>(kStatus));
    a.syscall(SYS_WAIT);
    a.li(t6, static_cast<int32_t>(kStatus));
    a.emit({LW, a0, t6, 0, 0});
}

void forever(Asm& a) {
    const size_t loop = a.here();
    a.patch(a.branch(JAL, zero, zero), loop);
}

void make_pipe(Asm& a) {
    a.li(a0, static_cast<int32_t>(kFds));
    a.syscall(SYS_PIPE);
}

void read1(Asm& a, int32_t fd) {
    a.li(a0, fd);
    a.li(a1, static_cast<int32_t>(kIn));
    a.li(a2, 1);
    a.syscall(SYS_READ);
}

// write(fd, kByte, 1), after storing 'x' at kByte.
void write1(Asm& a, int32_t fd) {
    a.li(t0, 'x');
    a.li(t1, static_cast<int32_t>(kByte));
    a.emit({SB, 0, t1, t0, 0});
    a.li(a0, fd);
    a.emit({ADDI, a1, t1, 0, 0});
    a.li(a2, 1);
    a.syscall(SYS_WRITE);
}

void load_var(Asm& a, uint8_t rd) {
    a.li(t5, static_cast<int32_t>(kVar));
    a.emit({LW, rd, t5, 0, 0});
}

void store_var(Asm& a, uint8_t rs) {
    a.li(t5, static_cast<int32_t>(kVar));
    a.emit({SW, 0, t5, rs, 0});
}

// kVar = kVar * 10 + digit (uses t0, t1)
void append_digit(Asm& a, int32_t digit) {
    load_var(a, t0);
    a.li(t1, 10);
    a.emit({MUL, t0, t0, t1, 0});
    a.emit({ADDI, t0, t0, 0, digit});
    store_var(a, t0);
}

// A handler that stores its a0 (the signal) in kVar and returns.
uint32_t record_handler(Asm& a) {
    const uint32_t h = here_addr(a);
    store_var(a, a0);
    ret(a);
    return h;
}

int32_t run_one(System& s, const Asm& a, const Strings& str = {}, uint64_t limit = kShort) {
    const uint32_t pid = s.spawn(a, str.bytes);
    CHECK(std::holds_alternative<kernel::AllDone>(s.k.run_all(limit)));
    return s.status(pid);
}

int32_t run_one(const Asm& a) {
    System s;
    return run_one(s, a);
}

}  // namespace

// =================== Stage 1: signal and kill, default and ignore. Filter: sig1_ ===================

TEST(sig1_kill_ends_a_process_by_default) {
    Asm a;
    size_t to_parent;
    fork_to_parent(a, to_parent);
    forever(a);
    at_parent(a, to_parent);
    kill_reg(a, s4, kSigusr1);
    expect(a, a0, 0, 1);
    wait_status(a);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), by_signal(kSigusr1));
}

TEST(sig1_the_status_tells_which_signal) {
    Asm a;
    size_t to_parent;
    fork_to_parent(a, to_parent);
    forever(a);
    at_parent(a, to_parent);
    kill_reg(a, s4, kSigusr2);
    wait_status(a);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), by_signal(kSigusr2));
}

TEST(sig1_an_ignored_signal_is_dropped) {
    // Set before the fork: the child keeps it.
    Asm a;
    signal(a, kSigusr2, kIgnore);
    expect(a, a0, 0, 1);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    a.busy_wait(3000);
    a.exit_with(5);
    at_parent(a, to_parent);
    kill_reg(a, s4, kSigusr2);
    expect(a, a0, 0, 2);
    wait_status(a);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 5);
}

TEST(sig1_sigkill_ends_a_process_that_ignores_other_signals) {
    Asm a;
    signal(a, kSigusr1, kIgnore);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    forever(a);
    at_parent(a, to_parent);
    kill_reg(a, s4, kSigusr1);              // ignored: the child keeps running
    a.syscall(SYS_YIELD);
    kill_reg(a, s4, kSigkill);
    expect(a, a0, 0, 1);
    wait_status(a);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), by_signal(kSigkill));
}

TEST(sig1_a_process_can_kill_itself) {
    Asm a;
    size_t to_parent;
    fork_to_parent(a, to_parent);
    kill_self(a, kSigusr1);
    a.exit_with(1);                         // never reached
    at_parent(a, to_parent);
    wait_status(a);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), by_signal(kSigusr1));
}

TEST(sig1_signal_rejects_unknown_signals_and_sigkill) {
    Asm a;
    signal(a, 0, kIgnore);
    expect(a, a0, -kEinval, 1);
    signal(a, 14, kIgnore);
    expect(a, a0, -kEinval, 2);
    signal(a, -1, kIgnore);
    expect(a, a0, -kEinval, 3);
    signal(a, kSigkill, kIgnore);
    expect(a, a0, -kEinval, 4);
    signal(a, kSigkill, kDefault);
    expect(a, a0, -kEinval, 5);
    signal(a, kSigusr1, kIgnore);
    expect(a, a0, 0, 6);
    signal(a, kSigusr1, kDefault);
    expect(a, a0, 0, 7);
    a.exit_with(9);
    CHECK_EQ(run_one(a), 9);
}

TEST(sig1_kill_rejects_unknown_signals_and_pids) {
    Asm a;
    a.syscall(SYS_GETPID);
    a.emit({ADDI, s3, a0, 0, 0});
    kill_reg(a, s3, 0);
    expect(a, a0, -kEinval, 1);
    kill_reg(a, s3, 14);
    expect(a, a0, -kEinval, 2);
    kill(a, 99, 0);                         // the signal is checked first
    expect(a, a0, -kEinval, 3);
    kill(a, 99, kSigusr1);
    expect(a, a0, -kEsrch, 4);
    kill(a, 0, kSigusr1);
    expect(a, a0, -kEsrch, 5);
    a.exit_with(9);
    CHECK_EQ(run_one(a), 9);
}

TEST(sig1_a_process_that_has_ended_cant_be_killed) {
    Asm a;
    size_t to_parent;
    fork_to_parent(a, to_parent);
    a.exit_with(4);
    at_parent(a, to_parent);
    a.busy_wait(3000);                      // the child has exited, and isn't collected yet
    kill_reg(a, s4, kSigusr1);
    expect(a, a0, -kEsrch, 1);
    wait_status(a);
    expect(a, a0, 4, 2);
    kill_reg(a, s4, kSigusr1);              // collected
    expect(a, a0, -kEsrch, 3);
    a.exit_with(9);
    CHECK_EQ(run_one(a), 9);
}

TEST(sig1_kill_ends_a_process_blocked_on_a_pipe) {
    Asm a;
    make_pipe(a);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    read1(a, kRead);                        // empty, and the write end stays open
    a.exit_with(1);
    at_parent(a, to_parent);
    a.syscall(SYS_YIELD);                   // the child runs and blocks
    kill_reg(a, s4, kSigusr1);
    wait_status(a);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), by_signal(kSigusr1));
}

TEST(sig1_kill_ends_a_process_blocked_in_wait) {
    // The child waits for a grandchild that runs for a while.
    Asm a;
    size_t to_parent;
    fork_to_parent(a, to_parent);
    size_t to_child;
    fork_to_parent(a, to_child);
    a.busy_wait(20000);                     // the grandchild
    a.exit_with(0);
    a.patch(to_child, a.here());
    wait_status(a);                         // the child
    a.exit_with(1);
    at_parent(a, to_parent);
    a.syscall(SYS_YIELD);
    kill_reg(a, s4, kSigusr1);
    a.li(a0, -1);
    a.emit({ADDI, a0, s4, 0, 0});
    a.li(a1, static_cast<int32_t>(kStatus));
    a.syscall(SYS_WAIT);
    a.li(t6, static_cast<int32_t>(kStatus));
    a.emit({LW, a0, t6, 0, 0});
    a.exit_with_a0();
    CHECK_EQ(run_one(a), by_signal(kSigusr1));
}

TEST(sig1_kill_ends_every_thread) {
    Asm a;
    const size_t to_main = a.branch(JAL, zero, zero);
    const uint32_t body = here_addr(a);
    forever(a);
    a.patch(to_main, a.here());
    size_t to_parent;
    fork_to_parent(a, to_parent);
    a.li(a0, static_cast<int32_t>(body));
    a.li(a1, 0);
    a.li(a2, 0);
    a.syscall(SYS_THREAD_CREATE);
    forever(a);
    at_parent(a, to_parent);
    a.syscall(SYS_YIELD);
    kill_reg(a, s4, kSigusr1);
    wait_status(a);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), by_signal(kSigusr1));
}

// =================== Stage 2: handlers. Filter: sig2_ ===================

TEST(sig2_a_handler_runs_with_the_signal_number) {
    Asm a;
    uint32_t r;
    const size_t skip = begin(a, r);
    const uint32_t h = record_handler(a);
    a.patch(skip, a.here());
    signal(a, kSigusr1, h, r);
    expect(a, a0, 0, 1);
    kill_self(a, kSigusr1);                 // the handler runs as kill returns
    load_var(a, a0);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), kSigusr1);
}

TEST(sig2_the_interrupted_code_keeps_every_register) {
    Asm a;
    uint32_t r;
    const size_t skip = begin(a, r);
    const uint32_t h = here_addr(a);
    for (uint8_t reg : {gp, tp, t0, t1, t2, s0, s1, a1, a2, a3, a4, a5, a6, a7, s2, s3, s4, s5, s6, s7,
                        s8, s9, s10, s11, t3, t4, t5, t6})
        a.li(reg, 0);
    ret(a);
    a.patch(skip, a.here());
    signal(a, kSigusr1, h, r);
    const std::vector<uint8_t> regs = {gp, tp, s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11, t3, t4};
    for (size_t i = 0; i < regs.size(); ++i) a.li(regs[i], 100 + static_cast<int32_t>(i));
    kill_self(a, kSigusr1);
    expect(a, a0, 0, 1);                    // kill's result
    expect(a, a1, kSigusr1, 2);             // still the argument
    for (size_t i = 0; i < regs.size(); ++i) expect(a, regs[i], 100 + static_cast<int32_t>(i), 10 + static_cast<int32_t>(i));
    a.exit_with(7);
    CHECK_EQ(run_one(a), 7);
}

TEST(sig2_the_frame_goes_below_sp_rounded_down_to_16) {
    constexpr int32_t kSp = 0x7FFFF009;
    constexpr int32_t kFrame = 0x7FFFF000 - 128;
    Asm a;
    uint32_t r;
    const size_t skip = begin(a, r);
    const uint32_t h = here_addr(a);
    store_var(a, sp);
    ret(a);
    a.patch(skip, a.here());
    signal(a, kSigusr1, h, r);
    a.li(sp, kSp);
    kill_self(a, kSigusr1);
    expect(a, sp, kSp, 1);                  // restored
    load_var(a, t0);
    expect(a, t0, kFrame, 2);
    a.exit_with(7);
    CHECK_EQ(run_one(a), 7);
}

TEST(sig2_the_frame_holds_pc_and_registers) {
    // The handler copies the saved pc and changes the saved a0 (word 10).
    Asm a;
    uint32_t r;
    const size_t skip = begin(a, r);
    const uint32_t h = here_addr(a);
    a.emit({LW, t0, sp, 0, 0});
    store_var(a, t0);
    a.li(t0, 77);
    a.emit({SW, 0, sp, t0, 10 * 4});
    ret(a);
    a.patch(skip, a.here());
    signal(a, kSigusr1, h, r);
    kill_self(a, kSigusr1);
    const uint32_t after = here_addr(a) ;   // the instruction after kill's ECALL
    expect(a, a0, 77, 1);
    load_var(a, t0);
    expect(a, t0, static_cast<int32_t>(after), 2);
    a.exit_with(7);
    CHECK_EQ(run_one(a), 7);
}

TEST(sig2_a_handler_can_exit) {
    Asm a;
    uint32_t r;
    const size_t skip = begin(a, r);
    const uint32_t h = here_addr(a);
    a.exit_with(42);
    a.patch(skip, a.here());
    signal(a, kSigusr2, h, r);
    kill_self(a, kSigusr2);
    a.exit_with(1);
    CHECK_EQ(run_one(a), 42);
}

TEST(sig2_a_signal_from_another_process_runs_the_handler) {
    // The child spins until its handler has run.
    Asm a;
    uint32_t r;
    const size_t skip = begin(a, r);
    const uint32_t h = record_handler(a);
    a.patch(skip, a.here());
    signal(a, kSigusr1, h, r);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    const size_t loop = a.here();
    load_var(a, t0);
    a.patch(a.branch(BEQ, t0, zero), loop);
    a.emit({ADDI, a0, t0, 0, 3});
    a.exit_with_a0();
    at_parent(a, to_parent);
    kill_reg(a, s4, kSigusr1);
    expect(a, a0, 0, 1);
    wait_status(a);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), kSigusr1 + 3);
}

TEST(sig2_a_signal_sent_twice_before_it_runs_runs_once) {
    Asm a;
    uint32_t r;
    const size_t skip = begin(a, r);
    const uint32_t h = here_addr(a);
    append_digit(a, 1);
    ret(a);
    a.patch(skip, a.here());
    signal(a, kSigusr1, h, r);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    a.busy_wait(3000);
    load_var(a, a0);
    a.exit_with_a0();
    at_parent(a, to_parent);
    kill_reg(a, s4, kSigusr1);
    kill_reg(a, s4, kSigusr1);
    wait_status(a);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 1);
}

TEST(sig2_two_pending_signals_run_lowest_first) {
    // The child writes kVar before the signals come, so the handlers don't fault on
    // copy-on-write: a fault is a resume too, and would deliver the second signal in
    // the middle of the first handler.
    Asm a;
    uint32_t r;
    const size_t skip = begin(a, r);
    const uint32_t h1 = here_addr(a);
    append_digit(a, 1);
    ret(a);
    const uint32_t h2 = here_addr(a);
    append_digit(a, 2);
    ret(a);
    a.patch(skip, a.here());
    signal(a, kSigusr1, h1, r);
    signal(a, kSigusr2, h2, r);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    store_var(a, zero);
    a.busy_wait(3000);
    load_var(a, a0);
    a.exit_with_a0();
    at_parent(a, to_parent);
    a.syscall(SYS_YIELD);                   // the child writes kVar and spins
    kill_reg(a, s4, kSigusr2);
    kill_reg(a, s4, kSigusr1);
    wait_status(a);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 12);
}

TEST(sig2_a_handler_can_be_interrupted) {
    // USR1's handler sends USR2, whose handler runs inside it. Each sigreturn
    // removes one frame, and both interrupted contexts come back.
    Asm a;
    uint32_t r;
    const size_t skip = begin(a, r);
    const uint32_t h1 = here_addr(a);
    a.li(s3, 9);
    kill_self(a, kSigusr2);
    expect(a, s3, 9, 3);
    append_digit(a, 1);
    ret(a);
    const uint32_t h2 = here_addr(a);
    a.li(s3, 0);
    append_digit(a, 2);
    ret(a);
    a.patch(skip, a.here());
    signal(a, kSigusr1, h1, r);
    signal(a, kSigusr2, h2, r);
    a.li(s2, 55);
    kill_self(a, kSigusr1);
    expect(a, s2, 55, 4);
    load_var(a, a0);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 21);
}

TEST(sig2_default_again_ends_the_process) {
    Asm a;
    uint32_t r;
    const size_t skip = begin(a, r);
    const uint32_t h = record_handler(a);
    a.patch(skip, a.here());
    size_t to_parent;
    fork_to_parent(a, to_parent);
    signal(a, kSigusr1, h, r);
    signal(a, kSigusr1, kDefault);
    kill_self(a, kSigusr1);
    a.exit_with(1);
    at_parent(a, to_parent);
    wait_status(a);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), by_signal(kSigusr1));
}

TEST(sig2_ignore_after_a_handler_drops_the_signal) {
    Asm a;
    uint32_t r;
    const size_t skip = begin(a, r);
    const uint32_t h = record_handler(a);
    a.patch(skip, a.here());
    signal(a, kSigusr1, h, r);
    signal(a, kSigusr1, kIgnore);
    kill_self(a, kSigusr1);
    load_var(a, a0);
    a.emit({ADDI, a0, a0, 0, 7});
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 7);
}

TEST(sig2_threads_share_handlers) {
    // A second thread signals its own process. It runs next, so it runs the handler.
    Asm a;
    uint32_t r;
    const size_t skip = begin(a, r);
    const uint32_t h = record_handler(a);
    const uint32_t body = here_addr(a);
    kill_self(a, kSigusr2);
    a.li(a0, 0);
    a.syscall(SYS_THREAD_EXIT);
    a.patch(skip, a.here());
    signal(a, kSigusr2, h, r);
    a.li(a0, static_cast<int32_t>(body));
    a.li(a1, 0x7FFF8000);                   // its stack, for the frame
    a.li(a2, 0);
    a.syscall(SYS_THREAD_CREATE);
    a.li(a1, 0);
    a.syscall(SYS_THREAD_JOIN);
    load_var(a, a0);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), kSigusr2);
}

TEST(sig2_exec_resets_handlers_and_keeps_ignored_signals) {
    // The new program sends itself USR2 (still ignored), then USR1 (default now).
    System s;
    Asm next;
    kill_self(next, kSigusr2);
    kill_self(next, kSigusr1);
    next.exit_with(1);
    CHECK(std::holds_alternative<uint32_t>(fs::create_file(s.m.disk(), fs::kRootInode, "next", elf(next, {}))));
    Strings str;
    const uint32_t path = str.add("/next");
    Asm a;
    uint32_t r;
    const size_t skip = begin(a, r);
    const uint32_t h = record_handler(a);
    a.patch(skip, a.here());
    size_t to_parent;
    fork_to_parent(a, to_parent);
    signal(a, kSigusr1, h, r);
    signal(a, kSigusr2, kIgnore);
    a.li(a0, static_cast<int32_t>(path));
    a.syscall(SYS_EXEC);
    a.exit_with(2);
    at_parent(a, to_parent);
    wait_status(a);
    a.exit_with_a0();
    CHECK_EQ(run_one(s, a, str, kDisk), by_signal(kSigusr1));
}

TEST(sig2_a_frame_that_cant_be_written_ends_the_process) {
    Asm a;
    uint32_t r;
    const size_t skip = begin(a, r);
    const uint32_t h = record_handler(a);
    a.patch(skip, a.here());
    size_t to_parent;
    fork_to_parent(a, to_parent);
    signal(a, kSigusr1, h, r);
    a.li(sp, 0x1000);                       // the frame would be at 0xF80: not the process's
    kill_self(a, kSigusr1);
    a.exit_with(1);
    at_parent(a, to_parent);
    wait_status(a);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), by_signal(kSigsegv));
}

TEST(sig2_sigreturn_with_no_frame_ends_the_process) {
    Asm a;
    size_t to_parent;
    fork_to_parent(a, to_parent);
    a.li(sp, 0x1000);
    a.syscall(SYS_SIGRETURN);
    a.exit_with(1);
    at_parent(a, to_parent);
    wait_status(a);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), by_signal(kSigsegv));
}

// =================== Stage 3: blocked threads. Filter: sig3_ ===================

TEST(sig3_a_handler_wakes_a_blocked_reader_and_the_read_runs_again) {
    // Only the child's handler writes to the pipe. If the signal didn't wake the
    // child, nobody would, and the test would never finish.
    Asm a;
    uint32_t r;
    const size_t skip = begin(a, r);
    const uint32_t h = here_addr(a);
    a.li(t0, 1);
    store_var(a, t0);
    write1(a, kWrite);
    ret(a);
    a.patch(skip, a.here());
    signal(a, kSigusr1, h, r);
    make_pipe(a);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    read1(a, kRead);                        // blocks, then runs again after the handler
    a.emit({ADDI, s3, a0, 0, 0});
    load_var(a, t0);
    a.li(t1, 10);
    a.emit({MUL, t0, t0, t1, 0});
    a.emit({ADD, a0, t0, s3, 0});
    a.exit_with_a0();
    at_parent(a, to_parent);
    a.syscall(SYS_YIELD);                   // the child runs and blocks
    kill_reg(a, s4, kSigusr1);
    wait_status(a);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 11);
}

TEST(sig3_after_the_handler_a_read_can_block_again) {
    // The child's handler tells the parent through a second pipe. Only then does
    // the parent write, so the child's read blocks once more in between.
    Asm a;
    uint32_t r;
    const size_t skip = begin(a, r);
    const uint32_t h = here_addr(a);
    a.li(t0, 1);
    store_var(a, t0);
    write1(a, kWrite2);
    ret(a);
    a.patch(skip, a.here());
    signal(a, kSigusr1, h, r);
    make_pipe(a);
    make_pipe(a);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    read1(a, kRead);
    a.emit({ADDI, s3, a0, 0, 0});
    load_var(a, t0);
    a.li(t1, 10);
    a.emit({MUL, t0, t0, t1, 0});
    a.emit({ADD, a0, t0, s3, 0});
    a.exit_with_a0();
    at_parent(a, to_parent);
    a.syscall(SYS_YIELD);
    kill_reg(a, s4, kSigusr1);
    read1(a, kRead2);                       // until the handler has run
    expect(a, a0, 1, 1);
    a.syscall(SYS_YIELD);                   // the child's read blocks again
    write1(a, kWrite);
    wait_status(a);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 11);
}

TEST(sig3_a_handler_wakes_a_blocked_wait_and_the_wait_runs_again) {
    // The grandchild reads from the pipe, and only the child's handler writes to it.
    // The child's wait runs again after the handler and collects the grandchild.
    Asm a;
    uint32_t r;
    const size_t skip = begin(a, r);
    const uint32_t h = here_addr(a);
    a.li(t0, 1);
    store_var(a, t0);
    write1(a, kWrite);
    ret(a);
    a.patch(skip, a.here());
    signal(a, kSigusr1, h, r);
    make_pipe(a);
    size_t to_parent;
    fork_to_parent(a, to_parent);
    size_t to_child;
    fork_to_parent(a, to_child);
    read1(a, kRead);                        // the grandchild
    a.emit({ADDI, a0, a0, 0, 3});
    a.exit_with_a0();
    a.patch(to_child, a.here());
    wait_status(a);                         // the child
    a.emit({ADDI, s3, a0, 0, 0});
    load_var(a, t0);
    a.li(t1, 10);
    a.emit({MUL, t0, t0, t1, 0});
    a.emit({ADD, a0, t0, s3, 0});
    a.exit_with_a0();
    at_parent(a, to_parent);
    a.syscall(SYS_YIELD);
    a.syscall(SYS_YIELD);
    kill_reg(a, s4, kSigusr1);
    wait_status(a);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 14);
}

// =================== Stage 4: signals from the kernel. Filter: sig4_ ===================

TEST(sig4_a_segv_handler_runs_on_a_bad_access) {
    // L4's example: the handler reports the signal and exits.
    Asm a;
    uint32_t r;
    const size_t skip = begin(a, r);
    const uint32_t h = here_addr(a);
    a.exit_with_a0();
    a.patch(skip, a.here());
    signal(a, kSigsegv, h, r);
    a.emit({LW, t0, zero, 0, 0});           // the null guard
    a.exit_with(1);
    CHECK_EQ(run_one(a), kSigsegv);
}

TEST(sig4_the_saved_pc_is_the_faulting_instruction) {
    Asm a;
    uint32_t r;
    const size_t skip = begin(a, r);
    const uint32_t h = here_addr(a);
    a.emit({LW, a0, sp, 0, 0});
    a.exit_with_a0();
    a.patch(skip, a.here());
    signal(a, kSigsegv, h, r);
    const uint32_t fault = here_addr(a);
    a.emit({LW, t0, zero, 0, 0});
    a.exit_with(1);
    CHECK_EQ(run_one(a), static_cast<int32_t>(fault));
}

TEST(sig4_a_handler_can_skip_the_faulting_instruction) {
    Asm a;
    uint32_t r;
    const size_t skip = begin(a, r);
    const uint32_t h = here_addr(a);
    a.emit({LW, t0, sp, 0, 0});
    a.emit({ADDI, t0, t0, 0, 4});
    a.emit({SW, 0, sp, t0, 0});
    ret(a);
    a.patch(skip, a.here());
    signal(a, kSigsegv, h, r);
    a.li(t1, 5);
    a.emit({LW, t1, zero, 0, 0});           // skipped: t1 keeps 5
    a.emit({ADDI, a0, t1, 0, 2});
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 7);
}

TEST(sig4_a_fault_kills_as_before_even_if_segv_is_ignored) {
    Asm a;
    size_t to_parent;
    fork_to_parent(a, to_parent);
    signal(a, kSigsegv, kIgnore);
    a.emit({LW, t0, zero, 0, 0});
    a.exit_with(1);
    at_parent(a, to_parent);
    wait_status(a);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), 128 + 13);         // a load page fault
}

TEST(sig4_a_fault_whose_frame_cant_be_written_ends_the_process) {
    Asm a;
    uint32_t r;
    const size_t skip = begin(a, r);
    const uint32_t h = here_addr(a);
    a.exit_with_a0();
    a.patch(skip, a.here());
    size_t to_parent;
    fork_to_parent(a, to_parent);
    signal(a, kSigsegv, h, r);
    a.li(sp, 0x1000);
    a.emit({LW, t0, zero, 0, 0});
    a.exit_with(1);
    at_parent(a, to_parent);
    wait_status(a);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), by_signal(kSigsegv));
}

TEST(sig4_sigpipe_ends_a_writer_by_default) {
    Asm a;
    size_t to_parent;
    fork_to_parent(a, to_parent);
    make_pipe(a);
    a.li(a0, kRead);
    a.syscall(SYS_CLOSE);
    write1(a, kWrite);
    a.exit_with(1);
    at_parent(a, to_parent);
    wait_status(a);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), by_signal(kSigpipe));
}

TEST(sig4_an_ignored_sigpipe_leaves_epipe) {
    Asm a;
    signal(a, kSigpipe, kIgnore);
    make_pipe(a);
    a.li(a0, kRead);
    a.syscall(SYS_CLOSE);
    write1(a, kWrite);
    expect(a, a0, -kEpipe, 1);
    a.exit_with(7);
    CHECK_EQ(run_one(a), 7);
}

TEST(sig4_a_sigpipe_handler_runs_then_write_returns_epipe) {
    Asm a;
    uint32_t r;
    const size_t skip = begin(a, r);
    const uint32_t h = record_handler(a);
    a.patch(skip, a.here());
    signal(a, kSigpipe, h, r);
    make_pipe(a);
    a.li(a0, kRead);
    a.syscall(SYS_CLOSE);
    write1(a, kWrite);
    expect(a, a0, -kEpipe, 1);
    load_var(a, a0);
    a.exit_with_a0();
    CHECK_EQ(run_one(a), kSigpipe);
}
