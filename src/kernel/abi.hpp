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

// Error codes, using Linux's values. A failed call returns -code in a0.
// (Not EBADF etc.: those names are macros in <errno.h>.)
inline constexpr int32_t kEbadf = 9;    // bad file descriptor
inline constexpr int32_t kEnomem = 12;  // out of memory
inline constexpr int32_t kEfault = 14;  // bad pointer passed to the kernel
inline constexpr int32_t kEinval = 22;  // invalid argument
inline constexpr int32_t kEnosys = 38;  // unknown system call number

}  // namespace kernel::abi
