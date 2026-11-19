#pragma once

#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"

#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

// ★★★ WHAT A `__try` FILTER IS AT THE MIR TIER — ONE STATEMENT, READ BY THE
// VERIFIER (`MirVerifier::checkSehStructure`) AND BY THE PASS THAT MAKES A
// FUNCTION OF IT (`synthesizeSehFunclets`).
//
// The source writes a filter as ONE EXPRESSION, and an expression is not one
// basic block: `&&`, `||` and `?:` evaluate an operand on one path only, so the
// lowering emits a diamond joined by a `Phi` wherever they appear — a filter
// included. Until this header the region's skeleton said "the filter is the one
// block `SehTryBegin` names, and it ends in `SehFilterReturn`", in two places,
// and every filter with a second condition was refused
// (`__except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION && g == 1)`,
// and the shape the vendor's documentation writes a filter in,
// `cond ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH`).
//
// A FILTER IS A SUB-GRAPH WITH ONE WAY IN AND ONE WAY OUT:
//   * it is ENTERED at one block — the second successor of its `SehTryBegin` —
//     and nowhere else: no block outside it branches to a block inside it, and
//     no block inside it branches back to the entry (the entry becomes the first
//     block of a function, which nothing branches to);
//   * it is LEFT by returning its value, at exactly ONE block, the one ending in
//     this region's `SehFilterReturn`; every other block of it ends in a branch
//     to blocks of the filter (`Br`, `CondBr`, `Switch`) or asserts that control
//     does not go on (`Unreachable`). A `Return`, an indirect branch, another
//     region's opener or another region's `SehFilterReturn` is a way out the
//     system that runs a filter does not have: the filter is a function called
//     while a handler is being searched for, and it can only answer;
//   * its handler — the one successor of the exit — is not one of its blocks.
// The membership is therefore every block reached from the entry without
// passing the exit, and the rules above are what make that set a function body.
//
// `blocks` comes back in REVERSE POST-ORDER from the entry (the entry first): a
// block's dominators precede it, so a pass that copies the filter block by block
// has every value an instruction names — other than a `Phi`'s — already copied.
// The walk keeps its own stack: how deep a filter's branches nest is the
// source's to choose.

namespace dss {

struct SehFilterRegion {
    MirBlockId entry{};
    MirBlockId exit{};       // the block ending in this region's SehFilterReturn
    MirBlockId handler{};    // the exit's one successor
    std::vector<MirBlockId> blocks;   // every block of the filter, reverse post-order
    // Empty when the shape holds. Otherwise ONE sentence naming the block and
    // the rule it breaks; the fields above are then not to be read.
    std::string problem;

    [[nodiscard]] bool ok() const noexcept { return problem.empty(); }
};

// `preds` is `mirBuildPredecessors(mir)` (indexed by a block's id). `entry` is
// the second successor of the `SehTryBegin` that opens region `regionId`.
[[nodiscard]] inline SehFilterRegion
readSehFilterRegion(Mir const& mir,
                    std::span<std::vector<MirBlockId> const> preds,
                    MirBlockId entry, std::uint32_t regionId) {
    SehFilterRegion out;
    out.entry = entry;
    auto const fail = [&](std::string text) {
        if (out.problem.empty()) out.problem = std::move(text);
    };

    // A block id the module does not hold (slot 0 is the arena's sentinel) is
    // refused by name before anything is read through it: the verifier calls
    // this on modules a reader of untrusted text built.
    auto const isABlock = [&](MirBlockId b) {
        return b.v != 0 && b.v < mir.blockCount();
    };
    if (!isABlock(entry)) {
        fail(std::format("SEH region {}: the filter's entry #{} is not a block "
                         "of the module", regionId, entry.v));
        return out;
    }

    struct Frame {
        MirBlockId    block;
        std::uint32_t next = 0;
    };
    std::unordered_set<std::uint32_t> seen{entry.v};
    std::vector<Frame> stack{Frame{entry, 0}};
    std::vector<MirBlockId> postOrder;
    while (!stack.empty() && out.ok()) {
        MirBlockId const b = stack.back().block;
        if (stack.back().next == 0) {
            // First look at the block: how it ends decides whether it is the
            // exit, a block that goes on inside the filter, or a way out the
            // filter does not have.
            std::uint32_t const n = mir.blockInstCount(b);
            MirOpcode const op = n == 0
                ? MirOpcode::Invalid
                : mir.instOpcode(mir.blockInstAt(b, n - 1));
            if (n == 0 || !opcodeInfo(op).isTerminator) {
                fail(std::format("SEH region {}: block #{} of the filter has no "
                                 "terminator", regionId, b.v));
                break;
            }
            MirInstId const term = mir.blockInstAt(b, n - 1);
            if (op == MirOpcode::SehFilterReturn) {
                if (mir.instPayload(term) != regionId) {
                    fail(std::format(
                        "SEH region {}: block #{} is reached from the filter and "
                        "returns the filter value of region {} — a filter returns "
                        "its own region's value and no other",
                        regionId, b.v, mir.instPayload(term)));
                    break;
                }
                if (out.exit.valid()) {
                    fail(std::format(
                        "SEH region {}: blocks #{} and #{} both return the "
                        "filter's value — a filter has one exit, the block its "
                        "handler follows", regionId, out.exit.v, b.v));
                    break;
                }
                auto const succs = mir.blockSuccessors(b);
                if (succs.size() != 1) {
                    fail(std::format(
                        "SEH region {}: the SehFilterReturn of block #{} has {} "
                        "successor(s); it has exactly one, the handler",
                        regionId, b.v, succs.size()));
                    break;
                }
                out.exit    = b;
                out.handler = succs[0];
                postOrder.push_back(b);
                stack.pop_back();
                continue;   // the exit's successor is the handler: not followed
            }
            if (op != MirOpcode::Br && op != MirOpcode::CondBr
                && op != MirOpcode::Switch && op != MirOpcode::Unreachable) {
                fail(std::format(
                    "SEH region {}: block #{} of the filter ends in '{}' — a "
                    "filter is left only by returning its value (the system "
                    "calls it while it searches for a handler, and it can only "
                    "answer)", regionId, b.v, opcodeInfo(op).mnemonic));
                break;
            }
        }
        auto const succs = mir.blockSuccessors(b);
        std::uint32_t const k = stack.back().next;
        if (k < succs.size()) {
            stack.back().next = k + 1;
            MirBlockId const s = succs[k];
            if (!isABlock(s)) {
                fail(std::format("SEH region {}: block #{} of the filter names "
                                 "successor #{}, which is not a block of the "
                                 "module", regionId, b.v, s.v));
                break;
            }
            if (s.v == entry.v) {
                fail(std::format(
                    "SEH region {}: block #{} of the filter branches back to "
                    "the filter's entry #{} — the entry is where the filter is "
                    "entered from outside, and nothing inside it returns there",
                    regionId, b.v, entry.v));
                break;
            }
            if (seen.insert(s.v).second) stack.push_back(Frame{s, 0});
            continue;
        }
        postOrder.push_back(b);
        stack.pop_back();
    }
    if (!out.ok()) return out;
    if (!out.exit.valid()) {
        fail(std::format(
            "SEH region {}: no block reached from the filter's entry #{} ends "
            "in the region's SehFilterReturn — the filter never returns a value",
            regionId, entry.v));
        return out;
    }
    if (seen.contains(out.handler.v)) {
        fail(std::format(
            "SEH region {}: the handler block #{} is also a block of the filter "
            "— the handler is what runs after the filter has answered",
            regionId, out.handler.v));
        return out;
    }
    out.blocks.assign(postOrder.rbegin(), postOrder.rend());
    // ONE WAY IN. The entry's own predecessor is its `SehTryBegin` (the verifier
    // states that rule beside this call); every other block of the filter is
    // reached from blocks of the filter alone.
    for (MirBlockId const b : out.blocks) {
        if (b.v == entry.v || b.v >= preds.size()) continue;
        for (MirBlockId const p : preds[b.v]) {
            if (seen.contains(p.v)) continue;
            fail(std::format(
                "SEH region {}: block #{} is reached from the filter and also "
                "from block #{} outside it — a filter is entered at its entry "
                "and nowhere else, and a block the rest of the function shares "
                "is a way out of it", regionId, b.v, p.v));
            return out;
        }
    }
    return out;
}

} // namespace dss
