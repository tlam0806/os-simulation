#pragma once
#include <vector>
#include <cstddef>
#include <optional>
#include <cstdint>
#include <cassert>

namespace mem {

class Ram {
public :
    static constexpr std::size_t kRamSize = 1 << 20;

    std::optional<uint32_t> read(uint32_t starting_addr, uint8_t size) const noexcept { // size is in byte
        assert(size == 1 || size == 2 || size == 4);
        if (starting_addr > memory.size() - size) {
            return std::nullopt;
        }
        uint32_t returned = 0;
        for (std::size_t i = 0; i < size; ++i) {
            uint32_t addr = starting_addr + size - i - 1;
            returned = (returned << 8) | memory[addr];
        }
        return returned;
    }

    bool write(uint32_t starting_addr, uint8_t size, uint32_t value) noexcept {
        assert(size == 1 || size == 2 || size == 4);
        if (starting_addr > memory.size() - size) {
            return false;
        }
        for (uint32_t addr = starting_addr; addr < starting_addr + size; ++addr) {
            memory[addr] = static_cast<uint8_t>(value % 256);
            value /= 256;
        }
        return true;
    }   

    bool copy(uint32_t from, uint32_t to, uint32_t size) {
        // [from, from + size) should not overlap with [to, to + size), otherwise it is UB
        uint64_t max_reach = static_cast<uint64_t>(std::max(from, to) + size);
        if (max_reach > static_cast<uint64_t>(memory.size())) {
            return false;
        }
        for (uint32_t i = 0; i < size; ++i) {
            uint32_t from_addr = from + i;
            uint32_t to_addr = to + i;
            assert(from_addr < memory.size());
            assert(to_addr < memory.size());
            memory[to_addr] = memory[from_addr];
        }
        return true;
    }

private :
    std::vector<uint8_t> memory = std::vector<uint8_t>(kRamSize);
};

}