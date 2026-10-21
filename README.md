# os-simulation
A simulated CPU, MMU, and kernel running inside a C++ program.

The machine is described in [docs/machine-spec.md](docs/machine-spec.md).

## Build and test

Requires CMake 3.20+ and a C++20 compiler (Apple clang works).

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Debug builds use AddressSanitizer and UndefinedBehaviorSanitizer by default, so memory bugs and undefined behavior make the tests fail. Turn them off with `cmake -B build -DOSSIM_SANITIZE=OFF`.

To disassemble instruction words:

```sh
./build/simulation 0xff010113 0x00112623
```

## Layout

```
src/isa/      instruction set: decode, encode, disassemble
src/main.cpp  the simulation command
tests/        unit tests (tests/test.hpp is the tiny test framework)
docs/         design documents
reference/    the original AI-generated example, kept for comparison
```
