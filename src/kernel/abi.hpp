// The interface between user programs and the kernel (docs/machine-spec.md, B3):
// system call numbers (in a7) and error codes (returned negated in a0).
#pragma once

#include <cstdint>

namespace kernel::abi {

// System call numbers.
inline constexpr uint32_t kSysExit = 1;
inline constexpr uint32_t kSysWrite = 2;
inline constexpr uint32_t kSysSbrk = 3;
inline constexpr uint32_t kSysGetpid = 4;
inline constexpr uint32_t kSysYield = 5;
inline constexpr uint32_t kSysFork = 6;
inline constexpr uint32_t kSysExec = 7;
inline constexpr uint32_t kSysWait = 8;
inline constexpr uint32_t kSysDiskRead = 9;
inline constexpr uint32_t kSysDiskWrite = 10;
inline constexpr uint32_t kSysOpen = 11;
inline constexpr uint32_t kSysClose = 12;
inline constexpr uint32_t kSysRead = 13;
inline constexpr uint32_t kSysLseek = 14;
inline constexpr uint32_t kSysThreadCreate = 15;
inline constexpr uint32_t kSysThreadExit = 16;
inline constexpr uint32_t kSysThreadJoin = 17;
inline constexpr uint32_t kSysGettid = 18;
inline constexpr uint32_t kSysPipe = 19;
inline constexpr uint32_t kSysShmget = 20;
inline constexpr uint32_t kSysShmat = 21;
inline constexpr uint32_t kSysShmdt = 22;
inline constexpr uint32_t kSysShmrm = 23;
inline constexpr uint32_t kSysSignal = 24;
inline constexpr uint32_t kSysKill = 25;
inline constexpr uint32_t kSysSigreturn = 26;

// Signal numbers (spec F3), Linux's values.
inline constexpr uint32_t kSigkill = 9;
inline constexpr uint32_t kSigusr1 = 10;
inline constexpr uint32_t kSigsegv = 11;
inline constexpr uint32_t kSigusr2 = 12;
inline constexpr uint32_t kSigpipe = 13;

// signal()'s handler values that aren't addresses.
inline constexpr uint32_t kSigDefault = 0;
inline constexpr uint32_t kSigIgnore = 1;

// Error codes, using Linux's values. A failed call returns -code in a0.
// (Not EBADF etc.: those names are macros in <errno.h>.)
inline constexpr int32_t kEnoent = 2;   // no such file or directory
inline constexpr int32_t kEsrch = 3;    // no such thread or process
inline constexpr int32_t kEnoexec = 8;  // not an executable file
inline constexpr int32_t kEbadf = 9;    // bad file descriptor
inline constexpr int32_t kEchild = 10;  // no child to wait for
inline constexpr int32_t kEnomem = 12;  // out of memory
inline constexpr int32_t kEfault = 14;  // bad pointer passed to the kernel
inline constexpr int32_t kEbusy = 16;   // the shared memory region is still attached
inline constexpr int32_t kEnotdir = 20; // not a directory
inline constexpr int32_t kEinval = 22;  // invalid argument
inline constexpr int32_t kEnfile = 23;  // the open-file table is full
inline constexpr int32_t kEmfile = 24;  // the process's fd table is full
inline constexpr int32_t kEnospc = 28;  // the shared memory table is full
inline constexpr int32_t kEspipe = 29;  // can't seek on a pipe
inline constexpr int32_t kEpipe = 32;   // writing to a pipe nobody can read
inline constexpr int32_t kEnametoolong = 36;  // path too long
inline constexpr int32_t kEnosys = 38;  // unknown system call number

}  // namespace kernel::abi
