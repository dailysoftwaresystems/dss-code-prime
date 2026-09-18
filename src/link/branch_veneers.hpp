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
// ⚠⚠ THAT PARAGRAPH IS A DESIGN PREFERENCE AND IT WAS WRITTEN WITHOUT THE
// MEASUREMENT THAT DECIDES IT. ✔MEASURED 2026-09-17, `aarch64-linux-gnu-ld`
// 2.42, six probes in three shapes: the two bodies are NOT interchangeable, and
// the difference is not cost.
//
//   * A veneer whose body is a self-contained PC-relative branch word reaches
//     exactly as far as that word's field. It therefore needs a boundary that is
//     BOTH within reach of the call site AND closer to the callee — and an image
//     whose call site sits immediately past a function larger than the field has
//     no such boundary, so the pass refuses.
//   * GNU ld LINKS that image. Its stub is `adrp x16 / add x16 / br x16`, whose
//     final hop carries no PC-relative field at all, so it only ever needs a
//     boundary in reach of the CALL SITE. ✔MEASURED: stub at `0x9400088`,
//     `_start` at `0x94000a0` (24 bytes away), callee 150 994 960 bytes away —
//     a distance no `b` could span, and it does not matter. The same body, named
//     `__exit@@GLIBC_2.17_veneer`, bridges a PLT-bound call the same way.
//   * x30 is preserved identically by both: the `bl` into the stub sets it and
//     neither body touches it. So the return-address argument above does not
//     discriminate between them.
//
// ⇒ THE ONE-WORD BODY IS NOT A CHEAPER SPELLING OF THE SAME MECHANISM; it is a
// STRICTLY WEAKER one, and the images it cannot link are refused by name in
// `injectBranchVeneers`. What stands between here and the stronger body is not
// this preference but two things nothing in the tree declares: that a link-tier
// veneer may be a SEQUENCE with more than one relocation, and WHICH REGISTER a
// linker may clobber at a branch site. `callerSaved` does not answer the second
// — a caller-saved register is dead across a CALL, not across a BRANCH — and
// getting it wrong is a silent wrong answer rather than a refusal. That decision
// is the operator's; see the row.
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
// ⚠⚠ THAT IS TRUE OF FUNCTIONS AND IS NOT TRUE OF IMPORT STUBS, WHICH IS THIS
// PASS'S ONE BLIND SPOT AND THE ONE A LARGE IMAGE ACTUALLY HITS. A call whose
// target is an `externImport` resolves to no entry in `module.functions`, so
// both entry points below `continue` past it — and the stub's address is not a
// property of the module at all: `elf.cpp` puts `.plt` at
// `alignUp(rodataOff + rodataSz, 16)`, i.e. PAST the whole of `.text`, and
// chooses it long after this pass has run. ✔MEASURED 2026-09-17 on
// `arm64:elf64-aarch64-linux-exec`: a single-function program of 212 992
// statements is refused by `applyExecRelocations` for a `call26` of
// 139 722 816 bytes, and that call is the SYNTHETIC ENTRY's `bl exit@plt` —
// every shipped `processExit` block declares `"mechanism": "by-name-import"`,
// so the trampoline always makes it. ⇒ every image whose text exceeds the
// field's reach is refused at the linker's own entry, whatever it contains, and
// this pass never sees the relocation that did it.
// ★ The reference makes the opposite layout choice: ✔MEASURED `ld` 2.42 puts
// `.plt` BEFORE `.text` (`0x2b0` against `0x2e0`), so the entry's call to its
// own import is a few dozen bytes whatever the image's size — and where that is
// still not enough, it veneers.
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
