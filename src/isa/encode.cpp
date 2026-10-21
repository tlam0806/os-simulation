#include "isa/isa.hpp"

#include <stdexcept>
#include <string>

namespace isa {
namespace {

bool fits_signed(int32_t v, int width) {
    const int32_t limit = 1 << (width - 1);
    return v >= -limit && v < limit;
}

}  // namespace

uint32_t encode(const Instruction& inst) {
    const OpInfo& op = info(inst.op);
    auto require = [&](bool ok, const char* problem) {
        if (!ok) throw std::invalid_argument(std::string(op.name) + ": " + problem);
    };

    require(op.format != Format::None, "cannot encode an illegal instruction");
    require(inst.rd < 32 && inst.rs1 < 32 && inst.rs2 < 32, "register number must be 0..31");

    const Format f = op.format;
    const bool uses_rd = f == Format::R || f == Format::I || f == Format::Shift ||
                         f == Format::U || f == Format::J;
    const bool uses_rs1 = f == Format::R || f == Format::I || f == Format::Shift ||
                          f == Format::S || f == Format::B;
    const bool uses_rs2 = f == Format::R || f == Format::S || f == Format::B;
    const bool uses_imm = f != Format::R && f != Format::System;
    require(uses_rd || inst.rd == 0, "this format has no rd");
    require(uses_rs1 || inst.rs1 == 0, "this format has no rs1");
    require(uses_rs2 || inst.rs2 == 0, "this format has no rs2");
    require(uses_imm || inst.imm == 0, "this format has no immediate");

    const uint32_t rd = uint32_t{inst.rd} << 7;
    const uint32_t rs1 = uint32_t{inst.rs1} << 15;
    const uint32_t rs2 = uint32_t{inst.rs2} << 20;
    const uint32_t imm = static_cast<uint32_t>(inst.imm);

    switch (f) {
    case Format::R:
        return op.match | rd | rs1 | rs2;

    case Format::I:
        require(fits_signed(inst.imm, 12), "immediate must be -2048..2047");
        return op.match | rd | rs1 | (imm & 0xfff) << 20;

    case Format::Shift:
        require(inst.imm >= 0 && inst.imm <= 31, "shift amount must be 0..31");
        return op.match | rd | rs1 | imm << 20;

    case Format::S:
        require(fits_signed(inst.imm, 12), "offset must be -2048..2047");
        return op.match | rs1 | rs2 | (imm & 0x1f) << 7 | (imm >> 5 & 0x7f) << 25;

    case Format::B:
        require(fits_signed(inst.imm, 13), "branch offset must be -4096..4094");
        require(inst.imm % 2 == 0, "branch offset must be even");
        return op.match | rs1 | rs2 |
               (imm >> 11 & 0x1) << 7 | (imm >> 1 & 0xf) << 8 |
               (imm >> 5 & 0x3f) << 25 | (imm >> 12 & 0x1) << 31;

    case Format::U:
        require((imm & 0xfff) == 0, "low 12 bits of a U-type value must be 0");
        return op.match | rd | imm;

    case Format::J:
        require(fits_signed(inst.imm, 21), "jump offset must be -1048576..1048574");
        require(inst.imm % 2 == 0, "jump offset must be even");
        return op.match | rd |
               (imm >> 12 & 0xff) << 12 | (imm >> 11 & 0x1) << 20 |
               (imm >> 1 & 0x3ff) << 21 | (imm >> 20 & 0x1) << 31;

    case Format::System:
        return op.match;

    case Format::None:
        break;
    }
    throw std::logic_error("unreachable");
}

}  // namespace isa
