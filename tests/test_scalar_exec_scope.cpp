// SPDX-License-Identifier: GPL-2.0-or-later
// Execute a register-index waterfall through the production control-flow graph.
// The scalar remaining-lane mask must advance even for lanes outside EXEC.
#include <array>
#include <cassert>
#include <cstdio>
#include <map>
#include <vector>
#include "shader_recompiler/frontend/control_flow_graph.h"

using namespace Shader;
using namespace Shader::Gcn;
using Field = OperandField;

static InstOperand MakeOperand(Field field, u32 code) {
    return {.field = field, .code = code};
}
static InstOperand S(u32 n) { return MakeOperand(Field::ScalarGPR, n); }
static InstOperand V(u32 n) { return MakeOperand(Field::VectorGPR, n); }
static InstOperand E() { return MakeOperand(Field::ExecLo, u32(Field::ExecLo)); }
static InstOperand M() { return MakeOperand(Field::M0, u32(Field::M0)); }
static GcnInst Inst(Opcode op, InstOperand dst = {}, InstOperand a = {}, InstOperand b = {}) {
    GcnInst inst{};
    inst.opcode = op;
    inst.category = op == Opcode::V_MOVRELS_B32 || op == Opcode::V_READFIRSTLANE_B32 ||
                            op == Opcode::V_CMPX_EQ_U32
                        ? InstCategory::VectorALU : InstCategory::ScalarALU;
    inst.length = 4;
    inst.dst[0] = dst;
    inst.src[0] = a;
    inst.src[1] = b;
    inst.dst_count = dst.field == Field::Undefined ? 0 : 1;
    inst.src_count = (a.field != Field::Undefined) + (b.field != Field::Undefined);
    return inst;
}

static std::vector<GcnInst> Program(unsigned mask_position) {
    std::vector<GcnInst> code{
        Inst(Opcode::S_MOV_B64, S(0), E()), // saved EXEC
        Inst(Opcode::S_MOV_B64, S(2), E()), // remaining lanes
        Inst(Opcode::V_READFIRSTLANE_B32, S(4), V(0)),
        Inst(Opcode::V_CMPX_EQ_U32, S(6), S(4), V(0)),
        Inst(Opcode::S_MOV_B32, M(), S(4)),
    };
    const auto mask = Inst(Opcode::S_ANDN2_B64, S(2), S(2), S(6));
    if (mask_position == 0) code.push_back(mask);
    code.push_back(Inst(Opcode::V_MOVRELS_B32, V(2), V(16)));
    if (mask_position == 1) code.push_back(mask);
    code.push_back(Inst(Opcode::V_MOVRELS_B32, V(3), V(32)));
    if (mask_position == 2) code.push_back(mask);
    code.push_back(Inst(Opcode::S_MOV_B64, E(), S(2)));
    auto branch = Inst(Opcode::S_CBRANCH_SCC1);
    branch.category = InstCategory::FlowControl;
    branch.control.sopp.simm = u16(s16(2 - s32(code.size()) - 1));
    code.push_back(branch);
    code.push_back(Inst(Opcode::S_MOV_B64, E(), S(0)));
    auto end = Inst(Opcode::S_ENDPGM);
    end.category = InstCategory::FlowControl;
    code.push_back(end);
    return code;
}

struct Lane {
    std::array<u32, 104> scalar{};
    std::array<u32, 64> vector{};
    u32 m0{};
    bool exec = true, scc = false;
};

// A tiny lane interpreter for this integer-only fixture. Forward edges merge before
// the next block; a back edge starts the next SIMD loop iteration. Only cross-lane
// communication is READFIRSTLANE. This models the graph, not a second graph builder.
static bool Check(const std::array<u32, 4>& indices, unsigned mask_position) {
    auto code = Program(mask_position);
    Common::ObjectPool<Block> pool{64};
    CFG cfg{pool, code};
    std::array<Lane, 4> lanes{};
    for (u32 i = 0; i < lanes.size(); ++i) {
        lanes[i].vector.fill(0xdeadbeef);
        lanes[i].vector[0] = indices[i];
        for (u32 n = 0; n < 4; ++n) {
            lanes[i].vector[16 + n] = 100 * i + n;
            lanes[i].vector[32 + n] = 1000 + 100 * i + n;
        }
    }
    const auto read = [&](const Lane& lane, const InstOperand& arg) -> u32 {
        switch (arg.field) {
        case Field::ScalarGPR: return lane.scalar[arg.code];
        case Field::VectorGPR: return lane.vector[arg.code];
        case Field::ExecLo: return lane.exec;
        case Field::M0: return lane.m0;
        default: assert(false); return 0;
        }
    };
    const auto write = [&](Lane& lane, const InstOperand& dst, u32 value) {
        switch (dst.field) {
        case Field::ScalarGPR: lane.scalar[dst.code] = value; break;
        case Field::VectorGPR: lane.vector[dst.code] = value; break;
        case Field::ExecLo: lane.exec = value; break;
        case Field::M0: lane.m0 = value; break;
        default: assert(false);
        }
    };
    std::map<const Block*, u32> pending;
    pending[&*cfg.begin()] = 15;
    unsigned rounds = 0;
    while (!pending.empty()) {
        assert(++rounds <= 8);
        auto current = std::move(pending);
        pending.clear();
        for (const auto& block : cfg) {
            const u32 active = current[&block];
            if (!active) continue;
            for (u32 pc = block.begin_index; pc <= block.end_index; ++pc) {
                const auto& inst = code[pc];
                if (inst.IsTerminateInstruction()) continue;
                u32 first = 0;
                while (!(active & (1u << first))) ++first;
                const u32 broadcast = read(lanes[first], inst.src[0]);
                for (u32 i = 0; i < lanes.size(); ++i) {
                    if (!(active & (1u << i))) continue;
                    auto& lane = lanes[i];
                    switch (inst.opcode) {
                    case Opcode::S_MOV_B32:
                    case Opcode::S_MOV_B64:
                        write(lane, inst.dst[0], read(lane, inst.src[0])); break;
                    case Opcode::V_READFIRSTLANE_B32:
                        write(lane, inst.dst[0], broadcast); break;
                    case Opcode::V_CMPX_EQ_U32: {
                        const bool matching = lane.exec && read(lane, inst.src[0]) == read(lane, inst.src[1]);
                        write(lane, inst.dst[0], matching);
                        lane.exec = matching;
                        break;
                    }
                    case Opcode::S_ANDN2_B64: {
                        const bool remaining = read(lane, inst.src[0]) && !read(lane, inst.src[1]);
                        write(lane, inst.dst[0], remaining);
                        lane.scc = remaining;
                        break;
                    }
                    case Opcode::V_MOVRELS_B32:
                        // The translator's vector writes are unconditional IR:
                        // their surrounding CFG edge must enforce guest EXEC.
                        write(lane, inst.dst[0], lane.vector.at(inst.src[0].code + lane.m0));
                        break;
                    default: assert(false);
                    }
                }
            }
            if (block.end_class == EndClass::Exit) continue;
            for (u32 i = 0; i < lanes.size(); ++i) {
                if (!(active & (1u << i))) continue;
                const auto& lane = lanes[i];
                bool cond = true;
                switch (block.cond) {
                case IR::Condition::True: break;
                case IR::Condition::Scc1: cond = lane.scc; break;
                case IR::Condition::Execnz: cond = lane.exec; break;
                default: assert(false);
                }
                const auto* target = cond ? block.branch_true : block.branch_false;
                assert(target);
                auto& work = target->begin <= block.begin ? pending : current;
                work[target] |= 1u << i;
            }
        }
    }
    for (u32 i = 0; i < lanes.size(); ++i) {
        if (lanes[i].vector[2] != 100 * i + indices[i] ||
            lanes[i].vector[3] != 1000 + 100 * i + indices[i]) return false;
        assert(lanes[i].exec);
    }
    return true;
}

static void CheckMaskedDependency(bool across_split) {
    auto code = Program(1);
    // Create a scalar source inside a masked vector region. It cannot be
    // consumed by lanes which skipped its producer, even after an earlier split.
    code.insert(code.begin() + 6, Inst(Opcode::S_MOV_B64, S(10), S(8)));
    code.insert(code.begin() + 7, Inst(Opcode::V_MOVRELS_B32, V(9), V(48)));
    u32 mask_register = 2;
    if (across_split) {
        code.insert(code.begin() + 10, Inst(Opcode::S_ANDN2_B64, S(12), S(12), S(10)));
        code.insert(code.begin() + 11, Inst(Opcode::V_MOVRELS_B32, V(4), V(48)));
        mask_register = 12;
    } else {
        code[8].src[1] = S(10);
    }
    for (size_t i = 0; i < code.size(); ++i) {
        if (code[i].opcode == Opcode::S_CBRANCH_SCC1)
            code[i].control.sopp.simm = s16(2 - s32(i) - 1);
    }
    Common::ObjectPool<Block> pool{64};
    CFG cfg{pool, code};
    bool found = false;
    for (const auto& block : cfg) {
        for (u32 i = block.begin_index; i <= block.end_index; ++i) {
            if (code[i].opcode == Opcode::S_ANDN2_B64 && code[i].dst[0].code == mask_register) {
                // Still inside the conditional vector block, rather than in an
                // epilogue reachable by both sides of the EXEC branch.
                assert(block.num_predecessors == 1);
                found = true;
            }
        }
    }
    assert(found);
}

int main() {
    CheckMaskedDependency(false);
    CheckMaskedDependency(true);
    for (unsigned position = 0; position < 3; ++position) {
        std::fprintf(stderr, "mask position %u (before / between / after gathers)\n", position);
        assert(Check({1,1,1,1}, position));
        assert(Check({0,1,2,3}, position));
        assert(Check({3,0,3,1}, position));
        for (u32 pattern = 0; pattern < 256; ++pattern) {
            assert(Check({pattern & 3, (pattern >> 2) & 3,
                          (pattern >> 4) & 3, (pattern >> 6) & 3}, position));
        }
    }
    std::puts("PASS: scalar remaining-mask updates and both gathers across mixed SIMD lanes");
}
