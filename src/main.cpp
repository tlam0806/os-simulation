// simulation: for now a small disassembler. Usage: simulation 0xff010113 0x00112623 ...
// It will become `simulation run program.elf` once the run loop exists.
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <cassert>
#include <iostream>
#include <array>

#include "isa/isa.hpp"
#include "machine/machine.hpp"
#include "kernel/kernel.hpp"

std::optional<std::vector<uint8_t> > read_file(const char* file) {
    int fd = open(file, O_RDONLY);
    if (fd == -1) {
        return std::nullopt;
    }
    std::array<uint8_t, 4096> buffer;
    std::vector<uint8_t> returned;
    while (true) {
        int count = read(fd, buffer.begin(), 4096);
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
    assert(0); // should never go here 
}

void log_and_exit(const std::string& message) {
    std::cerr << message << "\n";
    exit(-1);
}

int run(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "invalid number of arguments\n", argv[0]);
        return 1;
    }
    std::optional<std::vector<uint8_t>> readed = read_file(argv[2]);
    if (!readed) {
        log_and_exit("could not read");
    }
    machine::Machine machine;
    kernel::Kernel kernel(machine, std::cout, std::cerr);
    std::optional<std::string> load_instructions_error = machine.load(*readed);
    if (load_instructions_error) {
        log_and_exit(*load_instructions_error);
    }
    using kernel::Exited, kernel::Killed, kernel::StepLimit;
    std::variant<Exited, Killed, StepLimit> run_result = kernel.run(1e8);

    if (const auto* exited = std::get_if<Exited>(&run_result)) {
        std::cerr << "program exited with status " << exited->exit_status << "\n";
        return exited->exit_status;
    }
    if (const auto* killed = std::get_if<Killed>(&run_result)) {
        std::cerr << "program killed by a trap:\n" << killed->trap;
        return 1;
    }
    std::cerr << "program did not finish within " << machine.steps() << " instructions\n";
    return 1;
}

int main(int argc, char** argv) {
    if (argc >= 2 && static_cast<std::string_view>(argv[1]) == "run") {
        return run(argc, argv);
    }
    
}
