#pragma once

#include "asm/format/fixed32.hpp"
#include "asm/format/walker_util.hpp"
#include "asm/format/x86_variable.hpp"
#include "core/export.hpp"
#include "core/types/target_schema.hpp"

// ─────────────────────────────────────────────────────────────────────
// [[D-CSUBSET-LONG-BRANCH]] — ONE CONCEPT, ASKED OF ANY ENCODING SHAPE
// ─────────────────────────────────────────────────────────────────────
//
// ★★★ A BRANCH ISLAND IS ONE IDEA WITH TWO DECLARED BODIES, AND THIS IS WHERE
// THE TWO MEET. An island is a self-contained unconditional branch, quoted from
// the opcode's own encoding row, standing between a branch and a target it
// cannot reach. On a fixed-width ISA that body is a WORD; on a byte-oriented
// one it is an opcode byte plus a trailing displacement field. Both walkers
// answer the same question about their own shape, and the two tiers that need
// the answer — the assembler's intra-function resolver and the linker's
// inter-function veneer pass — ask it here rather than each carrying its own
// shape switch.
//
// The dispatch is shape-keyed (plan 13 §2.4), exactly like `encodeInst`'s:
// there is no CPU name anywhere in it, and a third encoding shape joins by
// adding its walker's own `islandBody` and one arm.
namespace dss::asm_island {

[[nodiscard]] inline walker_util::BranchIslandBody
branchIslandBody(TargetOpcodeInfo const& info) {
    switch (info.encoding.shape) {
        case TargetEncodingShape::Fixed32:
            return fixed32::islandBody(info);
        case TargetEncodingShape::X86Variable:
            return x86_variable::islandBody(info);
        case TargetEncodingShape::None:
            // An opcode with no declared encoding has nothing to quote.
            return {};
    }
    return {};
}

// The widest island body this TARGET declares anywhere — the body a link-tier
// veneer is made of.
//
// ⚠ IT IS FOUND BY A DECLARED PROPERTY, NOT BY A MNEMONIC. Asking for `"jmp"`
// by name would put one target vocabulary's spelling into a target-blind pass;
// `terminatorKind == Br` is the config's own sentence for "this opcode
// transfers control unconditionally", which is precisely the property a veneer
// body needs. A target that declares no such opcode gets no veneer and keeps
// its loud out-of-range refusal.
[[nodiscard]] inline walker_util::BranchIslandBody
widestDeclaredIslandBody(TargetSchema const& target) {
    walker_util::BranchIslandBody best;
    std::int64_t bestReach = 0;
    for (auto const& info : target.opcodes()) {
        if (info.terminatorKind != TargetTerminatorKind::Br) continue;
        auto const body = branchIslandBody(info);
        if (!body.declared()) continue;
        auto const reach = walker_util::blockRelByteReach(body.kind);
        if (!best.declared() || reach > bestReach) {
            best      = body;
            bestReach = reach;
        }
    }
    return best;
}

}  // namespace dss::asm_island
