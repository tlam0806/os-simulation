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
        auto interrupt = check_interrupts();
        if (interrupt) {
            return interrupt;
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

// TODO (disk step 3): implement. tests/machine_disk_test.cpp.

void Machine::acknowledge_disk_interrupt() {
    pending_disk_interrupt_ = false;
}

std::optional<cpu::Trap> Machine::wait_for_interrupt() { 
    auto interrupt = check_interrupts();
    if (interrupt) {
        return interrupt;
    }
    auto disk_completion = disk_.completion_time();
    auto next_interrupt = std::min(deadline_, disk_completion.value_or(UINT64_MAX));
    if (next_interrupt == UINT64_MAX) {
        return std::nullopt;
    }
    steps_ = next_interrupt;
    return check_interrupts();
}

std::optional<cpu::Trap> Machine::check_interrupts() {
    std::optional<uint64_t> disk_completion = disk_.completion_time();
    if (disk_completion && steps_ >= *disk_completion) {
        disk_.complete(ram_);
        pending_disk_interrupt_ = true;
    }
    if (pending_disk_interrupt_) {
        return cpu::Trap{cpu::Cause::disk_interrupt, cpu().pc, 0};
    }
    if (steps_ >= deadline_) {
        return cpu::Trap{cpu::Cause::timer_interrupt, cpu().pc, 0};
    }   
    return std::nullopt;
}

}
