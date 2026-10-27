# Machine Specification

> **Status:** Draft v0.1 · 2026-10-21

This document defines the simulated machine: the "hardware" that user programs run on, and the rules the kernel can rely on. The kernel itself is ordinary C++ code inside the simulator. Only user programs run on the simulated CPU.

- **Part A** describes the hardware: CPU, memory, traps, timer, disk.
- **Part B** describes the conventions between user programs and the kernel: address space layout, program loading, system calls.

Instruction behavior follows the [RISC-V Unprivileged ISA Specification](https://github.com/riscv/riscv-isa-manual/releases). This document only records the choices RISC-V leaves open and the places where we simplify.

## Design principles

1. **Kernel in C++, programs on the CPU.** We do not implement the RISC-V privileged architecture (privilege modes, CSRs, hardware page-table walks). When a trap happens, the simulator hands control to C++ kernel code.
2. **Deterministic.** The same program with the same input always runs the same way. Time is counted in CPU steps, not wall-clock time, so every bug is reproducible.
3. **Precise traps.** An instruction that traps has no effect, so the kernel can fix the cause and run it again.
4. **Real toolchain.** User programs are written in C and compiled with standard clang (see Appendix).
5. **Real units, one scale.** One step is **1 µs** of simulated time: the CPU runs at 1 MIPS. Every size and time in this document is stated in real units under that single rule, so it can be checked against real hardware. Format sizes (512-byte sector, 4 KiB page) are not scaled. Capacities keep real ratios: the disk is 64 × RAM.

---

# Part A — Hardware

## A1. CPU

| Property | Value |
|---|---|
| ISA | RV32IM (RV32I base + M multiply/divide extension) |
| Registers | `x0`–`x31`, 32 bits each. `x0` always reads 0; writes to it are ignored. |
| Program counter | `pc`, 32 bits |
| Byte order | Little-endian, for both instructions and data |
| Instruction size | 32 bits, always 4-byte aligned (no compressed instructions) |
| Execution model | One instruction per step. Single core, no pipeline, no caches. |

### Instruction set (48 instructions)

| Group | Instructions |
|---|---|
| Upper immediate | `LUI`, `AUIPC` |
| Jumps | `JAL`, `JALR` |
| Branches | `BEQ`, `BNE`, `BLT`, `BGE`, `BLTU`, `BGEU` |
| Loads | `LB`, `LH`, `LW`, `LBU`, `LHU` |
| Stores | `SB`, `SH`, `SW` |
| ALU, immediate | `ADDI`, `SLTI`, `SLTIU`, `XORI`, `ORI`, `ANDI`, `SLLI`, `SRLI`, `SRAI` |
| ALU, register | `ADD`, `SUB`, `SLL`, `SLT`, `SLTU`, `XOR`, `SRL`, `SRA`, `OR`, `AND` |
| Multiply / divide | `MUL`, `MULH`, `MULHSU`, `MULHU`, `DIV`, `DIVU`, `REM`, `REMU` |
| System | `FENCE`, `ECALL`, `EBREAK` |

### Our choices where RISC-V leaves them open

- **`FENCE`** is a no-op, since there is one core and no caches.
- **`ECALL`** raises an *environment call* trap. This is how programs make system calls (B3).
- **`EBREAK`** raises a *breakpoint* trap, reserved for a future debugger.
- **Misaligned data access traps.** `LW`/`SW` need 4-byte-aligned addresses; `LH`/`LHU`/`SH` need 2-byte-aligned addresses. An aligned access never crosses a page boundary, so each instruction touches at most one data page. This is what makes precise traps simple (A3).
- **Misaligned jump targets trap.** A taken branch or jump to an address that is not 4-byte aligned raises *instruction address misaligned*. The trap is reported on the branch or jump itself, not at the target.
- **Everything else is illegal.** Any encoding not in the table raises *illegal instruction*. This includes compressed, atomic, floating-point, `FENCE.I` and CSR instructions, and the all-zero word `0x00000000`. As a result, a program that runs into zero-filled memory traps instead of silently continuing.

### Rules that are easy to get wrong in C++

These come from the RISC-V spec but are common sources of simulator bugs:

- **Division never traps.** Dividing by zero: `DIV` and `DIVU` return all ones (`0xFFFFFFFF`), and `REM`/`REMU` return the dividend. Signed overflow, `DIV(-2³¹, -1)`, returns `-2³¹`, and the matching `REM` returns 0. In C++, `x / 0` and `INT32_MIN / -1` are undefined behavior, so check for both cases before dividing.
- **Shift amounts** use only the low 5 bits of `rs2`. For `SLLI`/`SRLI`/`SRAI`, an immediate with bit 5 set is illegal on RV32.
- **`JALR`** clears bit 0 of the computed target. When `rd == rs1`, compute the target from the old `rs1` *before* writing `rd`.
- **Sign extension:** all immediates are sign-extended. `LB`/`LH` sign-extend the loaded value; `LBU`/`LHU` zero-extend it.

## A2. Memory and MMU

| Property | Value |
|---|---|
| Physical RAM | 1 MiB (`0x00000`–`0xFFFFF`), byte-addressable, all zeros at power-on |
| Page / frame size | 4 KiB (4096 bytes) |
| Physical frames | 256 |
| Virtual address | 32 bits: virtual page number = bits 31–12, offset = bits 11–0 |

**Every CPU memory access goes through the MMU**: instruction fetch, load and store. The CPU never touches physical memory directly. The MMU translates addresses using the **current address space**, which the kernel selects (like RISC-V's `satp` register). Switching address spaces is part of switching processes.

Each page mapping holds a frame number and these permission bits:

| Bit | Meaning | Required for |
|---|---|---|
| V | Valid | Any access |
| R | Readable | Loads |
| W | Writable | Stores |
| X | Executable | Instruction fetch |

An access to an unmapped page, or one its permissions don't allow, raises the matching page fault (fetch, load or store; see A3).

**Page tables live in host memory, not in simulated RAM** (decided in Milestone 3). Each address space is a C++ table from virtual page number to mapping, owned by the kernel. Simulated RAM holds only program code and data, so all 256 frames are available to programs. Real RISC-V (Sv32) stores page tables in RAM and walks them in hardware; we skip that, because our kernel is C++ and doesn't need to.

Translation of an address `vaddr` for an access (fetch, load or store):

1. Look up the virtual page number `vaddr >> 12`. No entry → page fault.
2. Check the permission bit for the access: X for fetch, R for load, W for store. Not set → page fault.
3. The physical address is `frame * 4096 + (vaddr & 0xFFF)`.

## A3. Traps

A **trap** stops the CPU and hands control to the kernel. An *exception* is caused by the current instruction. An *interrupt* comes from outside it: the timer (A4) or the disk (A6).

Cause codes are the same as RISC-V's, so RISC-V documentation stays useful as a reference.

| Cause | Name | Raised when | `tval` |
|---|---|---|---|
| 0 | Instruction address misaligned | Taken branch or jump to a target that isn't 4-byte aligned | Target address |
| 2 | Illegal instruction | Encoding not listed in A1 | The instruction word |
| 3 | Breakpoint | `EBREAK` | 0 |
| 4 | Load address misaligned | Misaligned `LH`, `LHU` or `LW` | Address |
| 6 | Store address misaligned | Misaligned `SH` or `SW` | Address |
| 8 | Environment call | `ECALL` | 0 |
| 12 | Instruction page fault | Fetch from a page that is unmapped or not X | Address |
| 13 | Load page fault | Load from a page that is unmapped or not R | Address |
| 15 | Store page fault | Store to a page that is unmapped or not W | Address |
| Interrupt 5 | Timer interrupt | Clock has reached the timer deadline (A4) | 0 |
| Interrupt 9 | Disk interrupt | The disk has finished a request (A6) | 0 |

If both interrupts are pending, the disk interrupt is raised first (as in RISC-V, external interrupts come before the timer).

On a trap, the CPU gives the kernel a **trap record**:

- `cause`: a code from the table, plus a flag saying whether it is an interrupt
- `pc`: the address of the instruction that trapped. For an interrupt, the instruction that was about to run.
- `tval`: extra information, as listed in the table

**Traps are precise.** A trapping instruction changes nothing: no register, no memory, and `pc` stays the same. This is guaranteed because an instruction makes at most one data access, and an aligned access never crosses a page boundary. The CPU can therefore check the fetch and the data access before it changes any state.

**After a trap, the kernel decides how to resume:**

| The kernel wants to… | Resume at |
|---|---|
| Retry the instruction (e.g. after mapping a missing page) | `pc` |
| Continue after a system call | `pc + 4`. As in RISC-V, the trap's `pc` points at the `ECALL` itself. |
| Run a different process | That process's saved `pc` |
| Kill the process | Nothing to resume |

Kernel code is C++ and always runs to completion. It is never interrupted, so the machine has no interrupt-enable flag.

## A4. Clock and timer

- The machine keeps a 64-bit **clock** that goes up by 1 for every instruction the CPU attempts, including one that ends in an exception. Kernel code takes zero simulated time.
- The kernel sets a 64-bit **timer deadline** (like RISC-V's `mtimecmp`). Before each instruction, if `clock >= deadline`, the CPU raises a timer interrupt instead of running the instruction. Taking the interrupt does not advance the clock.
- The interrupt keeps firing until the kernel moves the deadline forward, for example `deadline = clock + quantum`. The starting quantum is 1000 (1 ms).
- **Idle.** When no process can run (all are blocked, e.g. on the disk), the kernel tells the machine to **wait for an interrupt** (like RISC-V's `WFI`). The clock jumps straight to the next pending interrupt, timer or disk, and that interrupt is raised. Idle time counts as clock time but runs no instruction. With no interrupt pending, waiting would never end: that is a kernel bug.

## A5. Machine state summary

This is everything the hardware holds. A process's CPU context is the first row; it is what the kernel saves and restores on a context switch.

| State | Size |
|---|---|
| `x0`–`x31`, `pc` | 33 × 32 bits |
| Physical RAM | 1 MiB |
| Current address space | Set by the kernel |
| Clock, timer deadline | 64 bits each |
| Disk contents | 64 MiB (A6) |
| Disk head track, current request | A6 |

## A6. Disk

A single magnetic hard disk, modelled on CS2106 L11: a 1-D array of sectors to the OS, with real seek, rotation and transfer times underneath.

| Property | Value |
|---|---|
| Sector size | 512 bytes. The smallest unit the disk reads or writes. |
| Geometry | 128 tracks × 1024 sectors per track, one surface, one head |
| Capacity | 131072 sectors = 64 MiB (64 × RAM) |
| Rotation | One turn every 8192 µs (~7300 RPM). A sector passes under the head in 8 µs. |
| Seek | 0 on the same track; otherwise 1000 µs + 50 µs per track crossed (1.05–7.35 ms) |
| Transfer | 8 µs per sector, i.e. the time it takes to pass under the head (64 MB/s) |
| Contents at power-on | All zeros |
| Head at power-on | Track 0 |

### Addressing

The kernel names sectors by **logical sector number** `s`, from 0 to 131071. The disk maps it to a place on the surface:

- track `s / 1024`, sector `s % 1024` within that track.

So consecutive sectors lie one after another along a track, then continue on the next track.

### Rotation

The platter turns with the clock and never stops. At clock time `t`, sector `(t / 8) % 1024` of the current track is starting to pass under the head. The disk can start transferring sector `k` only when its start reaches the head.

### Requests

The disk does one request at a time; keeping a queue and choosing the order is the kernel's job (disk scheduling, L11). A request is:

- **operation:** read or write
- **first sector** `s` and **count** `n ≥ 1`
- **RAM address:** the physical address of `n × 512` bytes in RAM. The disk copies data between itself and RAM directly (*DMA*); the CPU is not involved.

The kernel **submits** a request when the disk is idle. The disk refuses a request, and does nothing, if it is busy, if `n` is 0, if the sectors run past the end of the disk, or if the RAM range runs past the end of RAM. Refusing is not an interrupt; the kernel sees it at once.

### Timing

A request submitted at clock time `t₀` with the head on track `H` takes:

1. **Seek** to the track of sector `s`: 0 if it is `H`, else `1000 + 50 × |track − H|` µs.
2. **Rotational latency:** wait until the start of sector `s` reaches the head (0 to 8184 µs).
3. **Transfer:** 8 µs per sector, while the sectors pass under the head.
4. If the request runs past the end of a track, the head moves to the next track (a 1-track seek, 1050 µs), waits for sector 0 of that track, and goes on transferring. Repeat until all `n` sectors are done.

The request **completes** at the end of the last transfer. At that moment the data moves (read: disk → RAM; write: RAM → disk), the head stays on the last track used, the disk becomes idle, and a **disk interrupt** is raised (A3). The interrupt keeps firing until the kernel acknowledges it.

While the request is in progress, the kernel must not touch the RAM range: what it would see there is not defined.

Example, matching the lecture's formula *seek + rotational latency + transfer*: the head is on track 0 at `t₀ = 0`, and the kernel reads 8 sectors (one page) starting at sector `s = 2·1024 + 100` (track 2, sector 100). Seek: 1000 + 50 × 2 = 1100 µs, so the head arrives at `t = 1100`. Sector `(1100 / 8) % 1024 = 137` is passing, so it waits for sector 100 to come round again: it starts at `t = 8192 + 800 = 8992`. Transfer: 8 × 8 = 64 µs. The request completes at `t = 9056`.

### Host access

Outside the simulated machine, the simulator (and tests) can read and write the disk contents directly, with no timing and no interrupt: for example, to fill the disk with an image before the machine runs. Programs and the kernel never use this.

---

# Part B — Software conventions

## B1. Process address space

```
0xFFFFFFFF ┌────────────────────────────┐
           │ kernel-reserved            │  never mapped
0x80000000 ├────────────────────────────┤  ← initial sp
           │ stack ↓                    │  up to 1 MiB
0x7FF00000 ├────────────────────────────┤
           │                            │
           │ unmapped                   │
           │                            │
           ├────────────────────────────┤  ← current break
           │ heap ↑                     │  grows with sbrk
           ├────────────────────────────┤  ← initial break
           │ data + bss           RW    │
           ├────────────────────────────┤
           │ code + rodata        RX/R  │
0x00010000 ├────────────────────────────┤
           │ null guard                 │  never mapped
0x00000000 └────────────────────────────┘
                  (not to scale)
```

- **User addresses are below `0x80000000`.** The upper half is reserved, so any access to it faults.
- **Null guard:** `0x00000000`–`0x0000FFFF` is never mapped, so a null-pointer access faults.
- **Stack:** the initial `sp` is `0x80000000`, which meets the RISC-V ABI's 16-byte alignment rule. The stack region is `0x7FF00000`–`0x7FFFFFFF`. The loader maps the top 16 pages (64 KiB) read+write. The rest of the region is mapped **on demand**: a load or store page fault at an unmapped stack address makes the kernel map a zero-filled read+write page and retry the instruction. A fault anywhere else, a permission fault (except a store to a copy-on-write page, see `fork` in B3), or a fault when no frame is free kills the process (a *segmentation fault*).
- **Heap:** the initial break is the end of the highest loaded segment, rounded up to 4 KiB. It moves with `sbrk` and cannot grow past `0x7FF00000`. Heap pages are mapped **lazily**: `sbrk` only moves the break, and a page from the initial break up to the page holding the last heap byte is mapped (zero-filled, read+write) the first time it is accessed, like the stack. If no frame is free at that moment, the process is killed: a successful `sbrk` does not guarantee memory (*overcommit*).
- **New pages are always zero-filled**, so a process can never see another process's old data.

## B2. Program format and loading

User programs are **ELF32 executables** for RISC-V: little-endian, `EM_RISCV` (243), `ET_EXEC`, statically linked.

The loader:

1. Checks the ELF header: class, byte order, machine and type.
2. For each `PT_LOAD` segment:
   - requires it to lie within `[0x00010000, 0x7FF00000)`
   - gives each of its pages a free frame, zero-filled, mapped with permissions taken from `p_flags` (R/W/X)
   - copies `p_filesz` bytes from the file and zero-fills the rest up to `p_memsz` (this is `.bss`)

   If two segments share a page, that page gets the union of their permissions.
3. Sets the initial CPU state: `pc = e_entry`, `sp` (`x2`) `= 0x80000000`, all other registers 0.

`argc`/`argv` are not defined yet (Milestone 5, with `exec`).

## B3. System calls

The register convention is the same as Linux on RISC-V:

| Register | Purpose |
|---|---|
| `a7` (`x17`) | System call number |
| `a0`–`a5` (`x10`–`x15`) | Arguments |
| `a0` | Return value. A negative value is an error code (`-errno`). |

The program executes `ECALL`. The kernel handles the call and resumes the program at `pc + 4` (except a blocked `wait`, see below). The kernel changes no registers other than `a0`.

The call numbers are our own, not Linux's, because we don't aim to run Linux binaries.

| # | Name | Arguments | Returns | Milestone |
|---|---|---|---|---|
| 1 | `exit` | `a0` = status | Does not return | 2 |
| 2 | `write` | `a0` = fd, `a1` = buffer address, `a2` = length | Bytes written | 2 |
| 3 | `sbrk` | `a0` = bytes to grow (may be negative) | Old break | 3 |
| 4 | `getpid` | — | Process ID | 4 |
| 5 | `yield` | — | 0 | 4 |
| 6 | `fork` | — | In the parent: the child's pid. In the child: 0 | 5 |
| 7 | `exec` | To be defined | | 5 |
| 8 | `wait` | `a0` = pid to wait for (−1: any child), `a1` = address to store the status (0: don't store) | The pid of the child that ended | 5 |

For now, `write` supports only fd 1 (stdout) and fd 2 (stderr).

`sbrk` returns `-EINVAL` if the new break would be below the initial break, and `-ENOMEM` if it would be above `0x7FF00000`. Otherwise it always succeeds: it maps nothing (see B1). Shrinking unmaps the heap pages the heap no longer covers; each of their frames is freed once no other mapping uses it (see `fork`).

**`fork`** creates a new process, the *child*, as a copy of the caller, the *parent*:

- **Memory (copy-on-write):** the child maps every page the parent has mapped, at the same virtual address and to the **same frame**. A read-only page (code, read-only data) is simply shared. A writable page becomes read-only in *both* processes and is marked *copy-on-write*. After `fork`, parent and child still never see each other's writes:
  - A store to a copy-on-write page raises a store page fault. If other mappings still use the frame, the kernel copies it into a new frame, maps that read+write for the writer only, and retries the store. If the writer's mapping is the last one, the kernel just makes the page read+write again, with no copy.
  - If no frame is free for the copy, the writer is killed, like a fault on an untouched heap page (B1).
  - A kernel write into user memory (for example `wait`'s status) breaks copy-on-write the same way before it writes. If no frame is free, the call returns `-EFAULT`.
- **Frame reference counts:** each frame in use counts the mappings that point to it. Unmapping a page (`exit`, a kill, a shrinking `sbrk`) decrements the count, and the frame is freed only when the count reaches 0.
- **Registers and `pc`:** the same in both, so both continue after the `ecall`. Only `a0` differs: the child's pid in the parent, 0 in the child.
- **Break:** the child starts with the parent's `initial_break` and break, including heap pages that were never touched (they stay valid and unmapped).
- **pid:** the next unused pid. The child's *parent* is the caller.
- `fork` allocates no frames, so it never fails for lack of memory: running out shows up later, as a kill on the copy-on-write fault (*overcommit*, like the heap).

**`wait`** lets a parent collect a child that has ended:

- It waits for a child of the caller that has ended: the one with pid `a0`, or any child if `a0` is −1. If such a child has already ended, it returns at once. Otherwise the caller is **blocked**: the scheduler skips it, so it runs no instructions, until a matching child ends (exits or is killed). That makes the caller runnable again. While blocked, its `pc` stays on the `ECALL`, so when it is next scheduled the `wait` runs again and collects the child. A blocked `wait` changed nothing, so running it again is safe. Deadlock is impossible: a blocked process always has a live child, and following children down always reaches one that isn't blocked.
- It returns the child's pid. If `a1` isn't 0, it stores the child's status there as a 32-bit word: the exit status if the child called `exit`, or 128 + the trap cause if it was killed. Then the child is forgotten: a second `wait` can't collect it again.
- If the caller has no child matching `a0` (ended or still running), it returns `-ECHILD` at once.
- If `a1` isn't 0 and the status can't be stored there (the address isn't 4-byte aligned, like `SW`, or isn't writable by the caller), it returns `-EFAULT` and the child is **not** collected. Like `write`, the kernel maps an untouched heap or stack page first.
- A child whose parent has ended has no parent; nobody can wait for it.

**Error codes** use Linux's values so they are familiar:

| Name | Value | Meaning |
|---|---|---|
| `EBADF` | 9 | Bad file descriptor |
| `ECHILD` | 10 | No child to wait for |
| `ENOMEM` | 12 | Out of memory |
| `EFAULT` | 14 | Bad pointer passed to the kernel |
| `EINVAL` | 22 | Invalid argument |
| `ENOSYS` | 38 | Unknown system call number |

**Rules:**

- An unknown system call number returns `-ENOSYS`. The process is not killed.
- The kernel checks every pointer a program passes in. If a buffer covers an address the process may not access, the call returns `-EFAULT`. If it covers a valid page that isn't mapped yet (a heap or stack page the program hasn't touched), the kernel maps the page and continues, exactly as a page fault would. If no frame is free for it, the call returns `-EFAULT`. The `ECALL` is never re-executed after it has had side effects, so they can't happen twice. The only re-executed call is a blocked `wait`, which had none.

---

## Milestone notes

**Milestones 1–2 (before the MMU, no longer how programs run):** addresses are physical (virtual = physical), and permission bits are not checked. Programs still load at `0x00010000`, but the initial `sp` is the top of RAM, `0x00100000`, because address `0x80000000` doesn't exist yet. An access outside RAM raises the matching page fault.

**Milestone 1 (no kernel yet):** any trap stops the simulation and prints the trap record. `ECALL` stops the simulation and prints `a0`, which gives test programs a way to report a result.

## Not in scope (for now)

- Multiple CPUs, caches, TLB
- Devices other than the one disk (no second disk, no keyboard or screen device)
- Floating-point, atomic, compressed and CSR instructions
- Hardware support for misaligned access

## Open questions

| Question | Decide by |
|---|---|
| Layout of `argc`/`argv` on the initial stack | Milestone 5 |
| How `exec` names the program to run (there is no file system yet) | Milestone 5 |
| Reading input (`read`, stdin) | Later |

## Appendix: Toolchain

Apple's clang has no RISC-V backend, so use Homebrew LLVM (`brew install llvm lld`):

```sh
/opt/homebrew/opt/llvm/bin/clang --target=riscv32 -march=rv32im -mabi=ilp32 \
    -O1 -ffreestanding -nostdlib -c prog.c -o prog.o
```

`-march=rv32im` is required. Without it, clang's default target includes compressed and atomic instructions, which this CPU rejects as illegal. The link command (via `ld.lld`) will be added in Milestone 1.
