#pragma once

#include "asm/asm.hpp"
#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"

// ─────────────────────────────────────────────────────────────────────
// [[D-LK-AARCH64-CALL26-BEYOND-RANGE-HAS-NO-VENEER]]
//
// ⓘ Its sibling one tier down is [[D-CSUBSET-LONG-BRANCH]], which answers the
// same wall INSIDE a function. The two differ in one way worth a reader's
// attention: an island goes between two INSTRUCTIONS and can always be placed,
// a veneer goes between two FUNCTIONS and can fail to have anywhere to stand.
// ─────────────────────────────────────────────────────────────────────
//
// ★★★ THE SAME WALL, ONE TIER UP, AND THE SAME ANSWER. The assembler refuses an
// INTRA-function branch whose displacement leaves its field; the linker refuses
// an INTER-function one, in `applyExecRelocations`'s `Aarch64Call26` arm, with
// *"does not fit signed 26-bit — branch out of ±128 MiB range"*. Both walls are
// one fact: a PC-relative field has a reach, and a program can be bigger than
// it. The assembler's answer is a branch ISLAND — the same field aimed at a
// nearer point of the same function. This is that answer between functions,
// where the nearer point is a VENEER: a one-instruction synthetic function
// holding the same unconditional branch, re-aimed at the callee.
//
// ★★★ AND IT NEEDS NO SCRATCH REGISTER EITHER, WHICH IS WHY IT IS THIS SHAPE
// AND NOT THE ABI THUNK. At a call boundary AAPCS64 leaves IP0/IP1 (x16/x17)
// free, so the link tier COULD legitimately spend a register on an
// `ADRP`+`ADD`+`BR` veneer with unlimited reach — the option the assembler
// genuinely does not have, because it runs after register allocation. It is not
// taken here: `BL veneer` / `veneer: B callee` leaves x30 holding the real
// return address (which is the whole reason linkers veneer CALLS this way), it
// costs one instruction instead of three, and it keeps ONE concept across both
// tiers instead of two that have to be kept in step. If a target ever declares
// a register-materialized long branch, it joins as a second declared BODY, not
// as a second mechanism.
//
// ★★★ WHY THE DECISION CAN BE TAKEN HERE, BEFORE ANY IMAGE EXISTS. ✔MEASURED:
// all three format writers build `.text` by CONCATENATING
// `module.functions[i].bytes` in order with no padding between them, so a
// function's text offset is exactly the sum of the sizes before it — knowable
// from the module alone. The one departure is PE's object path, which SKIPS a
// weak definition's bytes; skipping only ever makes the real image SMALLER, so
// this pass's distances are an UPPER BOUND on the true ones and it can place a
// veneer that turns out to be unnecessary but never miss one that was needed.
// Nothing in any writer changes.
//
// The `applyExecRelocations` refusal stays exactly where it is, and stays the
// backstop: a veneer pass that got the arithmetic wrong is caught there, loudly,
// rather than emitting a branch to the wrong place.
namespace dss::linker {

// Does this module hold an intra-text branch relocation whose target is beyond
// its field's reach? Read-only and cheap — the linker asks BEFORE taking the
// copy-on-write clone of the input module (D-LK10-ENTRY-MODULE-COW), so a
// program that needs no veneer pays one scan and no copy.
[[nodiscard]] DSS_EXPORT bool
branchVeneersNeeded(AssembledModule const& module, TargetSchema const& target);

// Insert veneers until every intra-text branch relocation reaches its target,
// re-pointing each out-of-range relocation at the veneer standing in for it.
// Returns false (with a diagnostic) when the target declares nothing to build a
// veneer out of, or when the placement does not settle within its stated bound.
[[nodiscard]] DSS_EXPORT bool
injectBranchVeneers(AssembledModule&    module,
                    TargetSchema const& target,
                    DiagnosticReporter& reporter);

} // namespace dss::linker
