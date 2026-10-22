#pragma once
#include <array>
#include <cstdint>

namespace cpu {

struct Context {
    std::array<uint32_t, 32> regs{};   // x0..x31; regs[0] is always 0
    uint32_t pc = 0;

    bool operator==(const Context&) const = default;
};    

}