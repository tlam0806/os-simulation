# os-simulation

A RISC-V computer simulated in C++20, with a small Unix-like kernel that runs on it.

The simulator acts as the hardware: a 32-bit RISC-V CPU (RV32IM), an MMU, a timer and a hard disk. The kernel, also written in C++, runs real C programs compiled with clang. It gives each program its own virtual memory, switches between programs on timer interrupts, and lets them talk to each other through pipes, shared memory and signals.

Every run is deterministic. Time is counted in CPU steps, not wall-clock time, so every bug can be reproduced exactly.

```
$ ./build/simulation run ping.elf pong.elf
ping
pong
ping
pong
ping
pong
pid 1 (ping.elf) exited with status 0
pid 2 (pong.elf) exited with status 0
```

## Features

**Hardware**
- RV32IM CPU: 48 instructions, with precise traps (an instruction that traps has no effect, so the kernel can fix the cause and retry it)
- MMU with 4 KiB pages and valid, dirty and accessed bits
- Timer interrupt for preemptive scheduling
- Hard disk with realistic seek, rotation and transfer times, and a completion interrupt
- ELF loader for statically linked programs

**Kernel**
- Processes: `fork`, `exec`, `wait`, `exit`, `getpid`, `yield`
- Threads that share their process's memory, scheduled round robin with a time quantum
- Virtual memory: per-process page tables, demand paging for the stack and heap, `sbrk`, and copy-on-write `fork`
- Swapping: when RAM is full, pages are written to a swap area on disk, chosen with the CLOCK algorithm
- File system: inodes, directories and a block cache, with `open`, `read`, `lseek` and `close`
- Blocking system calls: a process waiting for the disk sleeps, and its call restarts safely when the data arrives
- IPC: pipes, shared memory, and signals with user-defined handlers

The full design, including every system call and its error cases, is in [docs/machine-spec.md](docs/machine-spec.md).

## Build and test

Requires CMake 3.20+ and a C++20 compiler (Apple clang works).

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Debug builds use AddressSanitizer and UndefinedBehaviorSanitizer by default, so memory bugs and undefined behavior make the tests fail. Turn them off with `cmake -B build -DOSSIM_SANITIZE=OFF`.

## Run a program

User programs are written in C and compiled for RISC-V. Apple's clang has no RISC-V backend, so use Homebrew LLVM (`brew install llvm lld`):

```sh
CC=/opt/homebrew/opt/llvm/bin/clang
FLAGS="--target=riscv32 -march=rv32im -mabi=ilp32 -O1 -ffreestanding -nostdlib -fuse-ld=lld"
$CC $FLAGS examples/ping.c -o build/ping.elf
$CC $FLAGS examples/pong.c -o build/pong.elf
./build/simulation run build/ping.elf build/pong.elf
```

Each program given to `run` becomes a process, and they all take turns on the CPU. There is no C library: programs make system calls directly with `ecall` (see [examples/ping.c](examples/ping.c)).

## Layout

```
src/isa/      instruction set: decode, encode, disassemble
src/cpu/      CPU, registers and traps
src/mmu/      page tables and the physical frame allocator
src/disk/     the simulated hard disk
src/fs/       on-disk file system layout
src/loader/   ELF loader
src/machine/  the whole machine: CPU, memory, timer, disk
src/kernel/   processes, scheduler, system calls, memory, swapping, file system, IPC
src/main.cpp  the simulation command
examples/     sample user programs in C
tests/        unit and integration tests (tests/test.hpp is the tiny test framework)
docs/         design documents
```
