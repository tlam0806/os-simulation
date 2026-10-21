#pragma once
#include <variant>
#include "machine/machine.hpp"
namespace kernel {

struct Exited {
    int32_t exit_status = 0;    
};

struct Killed {
    cpu::Trap trap;
};

struct StepLimit {
};

struct Continue {
};

class Kernel {
public :
    Kernel(machine::Machine& machine) : 
        machine_{machine}
    {}

    std::variant<Exited, Killed, StepLimit> run(uint64_t max_steps);
    
private : 
    machine::Machine& machine_;

    std::variant<Exited, Killed, Continue> handle_trap(const cpu::Trap& trap);

    std::variant<Exited, Continue> handle_syscall();
};

}