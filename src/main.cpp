// simulation: runs RISC-V programs on the simulated machine.
//
//   simulation run <program.elf>...
//
// Every program becomes a process; they all run together, taking turns.
// Exit status: with one program, that program's exit status (1 if it was
// killed or didn't finish). With several, 0 if every program exited with
// status 0, otherwise 1.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kernel/kernel.hpp"
#include "machine/machine.hpp"

namespace {

constexpr uint64_t kMaxSteps = 100'000'000;

std::optional<std::vector<uint8_t>> read_file(const char* file) {
    int fd = open(file, O_RDONLY);
    if (fd == -1) {
        return std::nullopt;
    }
    std::array<uint8_t, 4096> buffer;
    std::vector<uint8_t> returned;
    while (true) {
        ssize_t count = read(fd, buffer.data(), buffer.size());
        if (count == -1) {
            close(fd);
            return std::nullopt;
        }
        if (count == 0) {
            close(fd);
            return returned;
        }
        returned.insert(returned.end(), buffer.begin(), buffer.begin() + count);
    }
}

int usage(const char* program) {
    std::fprintf(stderr, "usage: %s run <program.elf>...\n", program);
    return 1;
}

struct Spawned {
    uint32_t pid;
    std::string file;
};

int run(int argc, char** argv) {
    if (argc < 3) return usage(argv[0]);

    machine::Machine machine;
    kernel::Kernel kernel(machine, std::cout, std::cerr);

    // Load every program first; if any can't be loaded, run nothing.
    std::vector<Spawned> spawned;
    for (int i = 2; i < argc; i++) {
        const std::optional<std::vector<uint8_t>> bytes = read_file(argv[i]);
        if (!bytes) {
            std::cerr << argv[i] << ": cannot read file\n";
            return 1;
        }
        const auto result = kernel.spawn(*bytes);
        if (const auto* error = std::get_if<std::string>(&result)) {
            std::cerr << argv[i] << ": " << *error << "\n";
            return 1;
        }
        spawned.push_back({std::get<uint32_t>(result), argv[i]});
    }

    const auto run_result = kernel.run_all(kMaxSteps);

    // Report how each program ended.
    bool all_ok = true;
    int single_status = 1;
    for (const Spawned& s : spawned) {
        std::cerr << "pid " << s.pid << " (" << s.file << ") ";
        const std::optional<kernel::Ending> ending = kernel.ending(s.pid);
        if (!ending) {
            std::cerr << "did not finish within " << kMaxSteps << " instructions\n";
            all_ok = false;
        } else if (const auto* exited = std::get_if<kernel::Exited>(&*ending)) {
            std::cerr << "exited with status " << exited->exit_status << "\n";
            single_status = exited->exit_status;
            all_ok = all_ok && exited->exit_status == 0;
        } else {
            std::cerr << "was killed by a trap:\n" << std::get<kernel::Killed>(*ending).trap;
            all_ok = false;
        }
    }
    assert(std::holds_alternative<kernel::AllDone>(run_result) || !all_ok);

    if (spawned.size() == 1) return single_status;
    return all_ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string_view(argv[1]) == "run") return run(argc, argv);
    return usage(argv[0]);
}
