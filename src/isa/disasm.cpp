#include "isa/isa.hpp"

#include <format>

namespace isa {

std::string disassemble(const Instruction& inst) {
    const OpInfo& op = info(inst.op);
    const std::string_view rd = reg_name(inst.rd);
    const std::string_view rs1 = reg_name(inst.rs1);
    const std::string_view rs2 = reg_name(inst.rs2);
    const bool is_load = inst.op >= Op::LB && inst.op <= Op::LHU;

    switch (op.format) {
    case Format::R:
        return std::format("{} {}, {}, {}", op.name, rd, rs1, rs2);
    case Format::I:
        if (inst.op == Op::FENCE) return "fence";  // its operands don't matter on one core
        if (is_load || inst.op == Op::JALR)
            return std::format("{} {}, {}({})", op.name, rd, inst.imm, rs1);
        return std::format("{} {}, {}, {}", op.name, rd, rs1, inst.imm);
    case Format::Shift:
        return std::format("{} {}, {}, {}", op.name, rd, rs1, inst.imm);
    case Format::S:
        return std::format("{} {}, {}({})", op.name, rs2, inst.imm, rs1);
    case Format::B:
        return std::format("{} {}, {}, {}", op.name, rs1, rs2, inst.imm);
    case Format::U:
        // Written the way assemblers take it: the upper 20 bits, in hex.
        return std::format("{} {}, {:#x}", op.name, rd, static_cast<uint32_t>(inst.imm) >> 12);
    case Format::J:
        return std::format("{} {}, {}", op.name, rd, inst.imm);
    case Format::System:
    case Format::None:
        break;
    }
    return std::string(op.name);
}

}  // namespace isa
