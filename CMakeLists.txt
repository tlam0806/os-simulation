cmake_minimum_required(VERSION 3.20)
project(os_simulation VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # build/compile_commands.json, for editors

# Default to a Debug build: no optimization, full debug info.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(OSSIM_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" ON)

# Compiler settings shared by every target.
add_library(ossim_options INTERFACE)
target_compile_options(ossim_options INTERFACE -Wall -Wextra -Wpedantic)
if(OSSIM_SANITIZE)
  # no-sanitize-recover: undefined behavior aborts the program, so tests fail on it.
  set(SANITIZE_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  target_compile_options(ossim_options INTERFACE ${SANITIZE_FLAGS})
  target_link_options(ossim_options INTERFACE ${SANITIZE_FLAGS})
endif()

# The simulator as a library, so the executable and the tests share the same code.
add_library(ossim_core
  src/isa/isa.cpp
  src/isa/decode.cpp
  src/isa/encode.cpp
  src/isa/disasm.cpp
  src/cpu/cpu.cpp
  src/loader/elf.cpp
  src/machine/machine.cpp
  src/kernel/kernel.cpp
  src/kernel/syscalls.cpp
  src/mmu/page_table.cpp
  src/mmu/frame_allocator.cpp
)
target_include_directories(ossim_core PUBLIC src)
target_link_libraries(ossim_core PUBLIC ossim_options)

add_executable(simulation src/main.cpp)
target_link_libraries(simulation PRIVATE ossim_core)

# Tests: run with `ctest --test-dir build`.
enable_testing()
add_executable(ossim_tests
  tests/test_main.cpp
  tests/isa_test.cpp
  tests/mem_test.cpp
  tests/cpu_test.cpp
  tests/cpu_alu_imm_test.cpp
  tests/cpu_alu_reg_test.cpp
  tests/cpu_control_test.cpp
  tests/cpu_load_store_test.cpp
  tests/loader_test.cpp
  tests/machine_test.cpp
  tests/kernel_test.cpp
  tests/kernel_write_test.cpp
  tests/page_table_test.cpp
  tests/cpu_mmu_test.cpp
  tests/frame_allocator_test.cpp
  tests/kernel_sbrk_test.cpp
  tests/machine_timer_test.cpp
  tests/cpu_context_test.cpp
  tests/kernel_process_test.cpp
  tests/kernel_scheduler_test.cpp
  tests/kernel_getpid_yield_test.cpp
  tests/kernel_page_fault_test.cpp
  tests/kernel_lazy_heap_test.cpp
  tests/kernel_fork_test.cpp
)
target_link_libraries(ossim_tests PRIVATE ossim_core)
add_test(NAME ossim_tests COMMAND ossim_tests)
# A bug that loops forever should fail the test run, not hang it.
set_tests_properties(ossim_tests PROPERTIES TIMEOUT 60)
