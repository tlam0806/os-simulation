#include "machine/machine.hpp"
#include "loader/elf.hpp"
#include "isa/isa.hpp"
namespace machine {

std::optional<std::string> Machine::load(std::span<const uint8_t> bytes) {
    std::variant<loader::Program, std::string> load_result = loader::load_elf(bytes, ram_);
    if (std::holds_alternative<std::string>(load_result)) {
        return std::get<std::string>(load_result);
    }
    loader::Program program = std::get<loader::Program>(load_result);
    cpu_.pc = program.entry;
    cpu_.set_reg(static_cast<uint8_t>(isa::reg::sp), mem::Ram::kRamSize);
    steps_ = 0;
    return std::nullopt;
}

std::optional<cpu::Trap> Machine::run(std::size_t max_step) {

    while (max_step--) {
        ++steps_;
        std::optional<cpu::Trap> step_result = cpu_.step();
        if (step_result) {
            return step_result;
        }
    }
    return std::nullopt;
}

}