// Runs a single program as a process, for tests that care about one program.
#pragma once

#include <cstdint>
#include <ostream>
#include <variant>
#include <vector>

#include "kernel/kernel.hpp"
#include "machine/machine.hpp"
#include "test.hpp"

// How a single program run ended.
using Result = std::variant<kernel::Exited, kernel::Killed, kernel::StepLimit>;

// Spawns `elf` as the only process and runs it with run_all().
inline Result run_process(machine::Machine& m, const std::vector<uint8_t>& elf, uint64_t max_steps,
                          std::ostream& out, std::ostream& err) {
    kernel::Kernel k(m, out, err);
    const auto spawned = k.spawn(elf);
    CHECK(std::holds_alternative<uint32_t>(spawned));
    if (!std::holds_alternative<uint32_t>(spawned)) return kernel::StepLimit{};
    const uint32_t pid = std::get<uint32_t>(spawned);

    if (std::holds_alternative<kernel::StepLimit>(k.run_all(max_steps))) return kernel::StepLimit{};
    const auto ending = k.ending(pid);
    CHECK(ending.has_value());
    if (!ending) return kernel::StepLimit{};
    if (const auto* e = std::get_if<kernel::Exited>(&*ending)) return *e;
    return std::get<kernel::Killed>(*ending);
}
