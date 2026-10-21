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

void run(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "invalid number of arguments\n", argv[0]);
        return;
    }
    std::optional<std::vector<uint8_t>> readed = read_file(argv[2]);
    if (!readed) {
        log_and_exit("could not read");
    }
    machine::Machine machine;
    std::optional<std::string> load_instructions_error = machine.load(*readed);
    if (load_instructions_error) {
        log_and_exit(*load_instructions_error);
    }
    std::optional<cpu::Trap> trapped = machine.run(1e8);
    if (trapped) {
        std::cerr << *trapped;
        return;
    }
    std::cerr << machine.cpu().reg(static_cast<uint8_t>(isa::reg::a0));
}

int main(int argc, char** argv) {
    if (argc >= 2 && static_cast<std::string_view>(argv[1]) == "run") {
        run(argc, argv);
    }
}
