#include "machine/machine.hpp"
#include "loader/elf.hpp"
#include "isa/isa.hpp"
namespace machine {

std::optional<std::string> Machine::load(std::span<const uint8_t> bytes) {
    std::variant<loader::Program, std::string> load_result = loader::load_elf(bytes, ram_, frame_allocator_, page_table_);
    if (std::holds_alternative<std::string>(load_result)) {
        return std::get<std::string>(load_result);
    }
    loader::Program program = std::get<loader::Program>(load_result);
    cpu_.set_page_table(&page_table_);
    cpu_.pc = program.entry;
    cpu_.set_reg(isa::reg::sp, loader::kStackTop);
    steps_ = 0;
    return std::nullopt;
}

std::optional<cpu::Trap> Machine::run(std::size_t max_step) {
    while (max_step > 0) {
        if (steps_ >= deadline_) {
            return cpu::Trap{cpu::Cause::timer_interrupt, cpu().pc, 0};
        }
        --max_step;
        ++steps_;
        std::optional<cpu::Trap> step_result = cpu_.step();
        if (step_result) {
            return step_result;
        }
    }
    return std::nullopt;
}

void Machine::set_timer(uint64_t deadline) {
    deadline_ = deadline;
}

void Machine::clear_timer() {
    deadline_ = kTimerOff;
}

}
