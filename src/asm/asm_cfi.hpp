#pragma once

#include "asm/asm.hpp"
#include "core/types/cfi.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "lir/lir_callconv.hpp"

#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <unordered_map>

// ═══════════════════════════════════════════════════════════════════════
// THE CALL-FRAME JOIN — instruction-anchored unwind rules × MEASURED byte
// offsets. Plan 15 CFI; D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED.
// ═══════════════════════════════════════════════════════════════════════
//
// ── THE ONE FACT NEITHER SIDE HOLDS ALONE ──
//
// A producer knows WHICH instruction changes the frame and WHAT the change
// means, and nothing about where that instruction lands. The assembler
// knows where every instruction landed and nothing about what any of them
// means. `CfiOp` needs both, so somebody has to join them — and this is
// the only place that does, for every producer.
//
// ★★★ WHY IT IS SHARED RATHER THAN ONE JOIN PER PRODUCER. There are two
//   producers today:
//     * `materializeCallingConvention` — a COMPILED function's prologue,
//       frame-pointer capture and epilogues, keyed by the `LirInstId` it
//       just emitted;
//     * `lowerAsmTextToLir` — a HAND-WRITTEN function's `.cfi_*`
//       directives, keyed by the `LirInstId` each directive follows.
//   They produce the same type (`LirFuncCfi`) because they are stating the
//   same kind of fact, and `core/types/cfi.hpp` opens by naming the failure
//   that follows from having two: *"the whole failure this closes is two
//   descriptions of one frame drifting apart"*. A second join would be
//   exactly that, one tier down — two byte-offset resolutions that agree
//   until the day one of them learns about a new anchor and the other does
//   not.
//
// ── THE THREE ANCHORS, AND WHY THE THIRD HAD TO BE NAMED ──
//
//   1. **After an instruction** (`inst` valid) — the ordinary case, and the
//      only one the callconv producer emits. Resolves to the byte offset
//      ONE PAST that instruction, which is `DW_CFA_advance_loc`'s target
//      and Win64's `CodeOffset` both.
//   2. **At a block's end** (`atBlockEnd`) — the `restore_state` that
//      re-arms the framed rules for the code FOLLOWING a `ret`.
//   3. **At function entry** (`inst` INVALID, `atBlockEnd` false) — offset
//      0. A `.s` writes `.cfi_def_cfa 7, 8` above the first instruction and
//      means "from byte 0". ⚠ THIS IS A DELIBERATE STATE, NOT A LOOKUP
//      MISS: a genuine miss — an op naming an instruction that produced no
//      bytes — is a substrate break and still fails loud below. Reading
//      "invalid" as "not found" would collapse the two and turn a real
//      producer/assembler disagreement into a silent rule at offset 0.
//      ✔MEASURED 2026-08-17, GNU as 2.42: gas HOISTS such rules into the
//      CIE instead. That is a DIFFERENT ENCODING OF THE SAME STATE — a CIE
//      initial instruction and an FDE op at offset 0 fold to identical
//      per-PC rows (`readelf --debug-dump=frames-interp` shows the same
//      table) — and DSS emits one shared CIE per module by construction, so
//      the FDE is where a per-function rule has to go.

namespace dss {

// Resolve one function's instruction-anchored ops against the byte offsets
// the assembler measured. Returns nullopt after reporting.
//
// `prologueOpCount` is how many leading ops belong to the PROLOGUE, **when
// the producer can state it**.
// ★★ nullopt IS NOT ZERO, AND CONFLATING THEM IS A WIN64 MISCOMPILE. The
//    callconv producer counts its own prologue ops, so `0` there is a
//    MEASURED "this frame allocates nothing and saves nothing" and yields
//    `prologueEndPc = 0`. GNU `.cfi_*` has **no prologue-end verb at all**
//    (✔MEASURED 2026-08-17 over gcc 13.3.0's full emitted spelling set on
//    x86_64 and aarch64: `.cfi_startproc`, `.cfi_endproc`,
//    `.cfi_def_cfa_offset`, `.cfi_offset`, `.cfi_restore`,
//    `.cfi_remember_state`, `.cfi_restore_state`, `.cfi_def_cfa_register`,
//    `.cfi_def_cfa` — nothing states where a prologue ends), so an assembly
//    producer passes nullopt and the function carries no `prologueEndPc`.
//    Win64 `UNWIND_INFO` REQUIRES that field, and `pe.cpp` refuses a
//    function that lacks it rather than inventing one — which is the
//    correct, loud outcome. Passing `0` instead would hand RtlVirtualUnwind
//    a "no prologue" claim for a function that plainly has one, and every
//    unwind through it would rebuild a frame that is not there.
[[nodiscard]] inline std::optional<CfiFunction>
resolveFuncCfi(AssembledFunction const&     fn,
               LirFuncCfi const&            src,
               CfiInitialState const&       initial,
               std::optional<std::uint32_t> prologueOpCount,
               std::size_t                  funcIndex,
               DiagnosticReporter&          reporter) {
    auto fail = [&](std::string msg) -> std::optional<CfiFunction> {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::K_UnwindRuleUnrepresentable;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::format("function index {}: {} (plan 15 CFI)",
                                 funcIndex, msg);
        reporter.report(std::move(d));
        return std::nullopt;
    };

    // LirInstId -> the byte offset ONE PAST that instruction. The source map
    // is stamped once per successfully encoded instruction, in emission
    // order, so instruction k spans
    // [sourceMap[k].byteOffset, sourceMap[k+1].byteOffset).
    std::unordered_map<std::uint32_t, std::uint32_t> pcAfter;
    pcAfter.reserve(fn.sourceMap.size());
    for (std::size_t k = 0; k < fn.sourceMap.size(); ++k) {
        std::uint32_t const endOff =
            (k + 1 < fn.sourceMap.size())
                ? fn.sourceMap[k + 1].byteOffset
                : static_cast<std::uint32_t>(fn.bytes.size());
        pcAfter[fn.sourceMap[k].lirInst.v] = endOff;
    }

    CfiFunction out;
    out.codeLength = static_cast<std::uint32_t>(fn.bytes.size());
    out.initial    = initial;
    out.ops.reserve(src.ops.size());

    std::optional<std::uint32_t> prologueEnd;
    if (prologueOpCount.has_value() && *prologueOpCount == 0) prologueEnd = 0u;

    for (std::size_t oi = 0; oi < src.ops.size(); ++oi) {
        LirCfiOp const& op = src.ops[oi];
        if (op.atBlockEnd) {
            // Resolve to the end of the named block -- the byte offset of
            // whatever block is laid out immediately after it, or the
            // function's extent when it is laid out last. Block LAYOUT order
            // is not creation order (the optimizer reorders by RPO), so this
            // is a scan for the smallest strictly-greater offset -- the
            // identical idiom the SEH scope-end binding uses, for the
            // identical reason.
            auto const bIt = fn.blockByteOffsets.find(op.block.v);
            if (bIt == fn.blockByteOffsets.end()) {
                return fail(std::format(
                    "the frame rule '{}' is anchored to LIR block {}, which "
                    "has no byte offset", cfiOpKindName(op.kind), op.block.v));
            }
            std::uint32_t end = out.codeLength;
            for (auto const& [blkV, off] : fn.blockByteOffsets) {
                (void)blkV;
                if (off > bIt->second && off < end) end = off;
            }
            // A restore that lands at the function's very end re-arms rules
            // for code that does not exist. Dropping it keeps the
            // single-return case -- the overwhelming majority -- byte-
            // identical to a stream that never had the mechanism, and a
            // dangling remember_state is harmless (nothing pops it).
            if (end >= out.codeLength) continue;
            out.ops.push_back(CfiOp{end, op.kind, op.reg, op.srcReg,
                                    op.offset});
            continue;
        }
        if (!op.inst.valid()) {
            // Anchor 3 -- function entry. See the header docblock: this is a
            // state a producer STATES, never one inferred from a miss.
            out.ops.push_back(CfiOp{0, op.kind, op.reg, op.srcReg, op.offset});
            if (prologueOpCount.has_value() && oi + 1 == *prologueOpCount) {
                prologueEnd = 0u;
            }
            continue;
        }
        auto const it = pcAfter.find(op.inst.v);
        if (it == pcAfter.end()) {
            // The producer said an instruction establishes a frame rule and
            // the assembler never emitted it. That is a substrate-invariant
            // break, and the WRONG response is to drop the op: the resulting
            // table would describe a frame that is only partly built. Refuse,
            // naming the rule.
            return fail(std::format(
                "the frame rule '{}' is attached to LIR instruction {}, which "
                "produced no bytes -- the unwind description and the emitted "
                "code disagree", cfiOpKindName(op.kind), op.inst.v));
        }
        out.ops.push_back(CfiOp{it->second, op.kind, op.reg, op.srcReg,
                                op.offset});
        if (prologueOpCount.has_value() && oi + 1 == *prologueOpCount) {
            prologueEnd = it->second;
        }
    }
    out.prologueEndPc = prologueEnd;
    if (auto const why = validateCfiFunction(out); !why.empty()) {
        return fail("malformed call-frame information -- " + why);
    }
    return out;
}

// ── The two producers' entry points ─────────────────────────────────────
//
// ★★ TWO NAMES RATHER THAN ONE OVERLOAD SET, because they differ on two
//    facts a reader must not have to infer from a span type:
//
//    (1) **WHICH FUNCTIONS ARE DESCRIBED.** The calling-convention
//        materializer describes EVERY function it lowered — a frameless
//        leaf's description is "the entry state, unchanged", which is a
//        real description and produces a real FDE. A `.s` describes only
//        the functions the programmer bracketed with the frame-start
//        directive; gas allows a file to describe some and not others, so
//        an assembly module carries `std::optional` per slot. ⚠ THE
//        PREDICATE IS **NOT** "are the ops empty": ✔MEASURED 2026-08-17,
//        gcc 13.3.0 emits `.cfi_startproc` / `ret` / `.cfi_endproc` for a
//        leaf function and gas emits an FDE with ZERO ops for it. An
//        empty-ops function is DESCRIBED and must get its FDE.
//        (The same `std::optional`-per-slot shape the format writers
//        already use — `elf.cpp` and `macho.cpp` both build
//        `std::vector<std::optional<CfiFunction>>`.)
//
//    (2) **WHETHER THE PROLOGUE END IS STATEABLE** — see `resolveFuncCfi`.
//
//    Collapsing them into one call with two booleans would put both facts
//    in argument positions a caller can transpose in silence.

// ★★★ A PARALLEL-INDEX MISMATCH IS A REFUSAL, NOT A SKIP.
//
// Both producers guarantee one entry per LIR function by construction, so this
// can only fire on a substrate break — and the WRONG response is the one that
// was here before: a size check that quietly declined to attach anything. The
// unwind information for the WHOLE module would then vanish with no
// diagnostic, which is D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED exactly, one
// tier further down and applying to compiled C as well as to assembly.
// A count mismatch also means "slot k" and "function k" have stopped being the
// same k, so attaching the entries that DO line up would describe some
// functions with another's frame.
[[nodiscard]] inline bool
attachSizesAgree(std::size_t producerCount, std::size_t functionCount,
                 DiagnosticReporter& reporter) {
    if (producerCount == functionCount) return true;
    ParseDiagnostic d;
    d.code     = DiagnosticCode::K_UnwindRuleUnrepresentable;
    d.severity = DiagnosticSeverity::Error;
    d.actual   = std::format(
        "call-frame information covers {} function(s) but the module assembled "
        "{} — the two are parallel-indexed by construction, so they no longer "
        "agree about which frame belongs to which function. Attaching the "
        "entries that happen to line up would describe some functions with "
        "another's frame; attaching none would drop every unwind table in this "
        "module without a word (plan 15 CFI)",
        producerCount, functionCount);
    reporter.report(std::move(d));
    return false;
}

// The COMPILED-function producer (`materializeCallingConvention`).
[[nodiscard]] inline bool
attachCallconvCfi(AssembledModule&            module,
                  std::span<LirFuncCfi const> perFunc,
                  CfiInitialState const&      initial,
                  DiagnosticReporter&         reporter) {
    if (!attachSizesAgree(perFunc.size(), module.functions.size(), reporter)) {
        return false;
    }
    for (std::size_t fi = 0; fi < perFunc.size(); ++fi) {
        auto resolved = resolveFuncCfi(module.functions[fi], perFunc[fi],
                                       initial, perFunc[fi].prologueOpCount,
                                       fi, reporter);
        if (!resolved.has_value()) return false;
        module.functions[fi].cfi = std::move(*resolved);
    }
    return true;
}

// The HAND-WRITTEN-assembly producer (`lowerAsmTextToLir`).
[[nodiscard]] inline bool
attachAssemblyCfi(AssembledModule&                           module,
                  std::span<std::optional<LirFuncCfi> const> perFunc,
                  CfiInitialState const&                     initial,
                  DiagnosticReporter&                        reporter) {
    if (!attachSizesAgree(perFunc.size(), module.functions.size(), reporter)) {
        return false;
    }
    for (std::size_t fi = 0; fi < perFunc.size(); ++fi) {
        if (!perFunc[fi].has_value()) continue;
        auto resolved = resolveFuncCfi(module.functions[fi], *perFunc[fi],
                                       initial, std::nullopt, fi, reporter);
        if (!resolved.has_value()) return false;
        module.functions[fi].cfi = std::move(*resolved);
    }
    return true;
}

} // namespace dss
