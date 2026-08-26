#include "mir/mir_verifier.hpp"

#include "core/types/call_payload.hpp"   // TF-C112: hasIndirectResult (sret prepend)
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/core_type.hpp"   // typeKindNameOrEmpty (describeType)
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_layout.hpp"   // TF-C94: isWideInt (return ABI);
                                                     // TF-C112: isByValueClass /
                                                     // isMemoryResidentType (call gate)
#include "mir/mir_cfg.hpp"   // shared mirReversePostOrder
#include "mir/mir_dom.hpp"   // shared computeMirDomTree + mirBuildPredecessors
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"  // deriveStructCfMarkers + structCfMarkerName

#include <format>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace dss {

namespace {

// Centralized diagnostic emission. The node-kind-typed overloads
// format the "mir inst/block/func #N" prefix once so callsites don't
// re-format the same "actual" prefix every time. Future-proof for the
// pending MirSourceMap injection (the optional `MirSourceMap const*`
// can be added here without touching the rules).
void report(DiagnosticReporter& reporter, DiagnosticCode code,
            std::string actual) {
    ParseDiagnostic d;
    d.code     = code;
    d.severity = DiagnosticSeverity::Error;
    d.actual   = std::move(actual);
    reporter.report(std::move(d));
}

void reportInst(DiagnosticReporter& reporter, DiagnosticCode code,
                MirInstId id, std::string detail) {
    report(reporter, code, std::format("mir inst #{}: {}", id.v, detail));
}

void reportBlock(DiagnosticReporter& reporter, DiagnosticCode code,
                 MirBlockId id, std::string detail) {
    report(reporter, code, std::format("mir block #{}: {}", id.v, detail));
}

void reportFunc(DiagnosticReporter& reporter, DiagnosticCode code,
                MirFuncId id, std::string detail) {
    report(reporter, code, std::format("mir func #{}: {}", id.v, detail));
}

// Iterate over real instruction slots (slot 0 is the sentinel).
// Strong-id constructor is `(value, arenaTag)` — value first.
template <typename Fn>
void forEachInst(Mir const& mir, Fn fn) {
    for (std::uint32_t i = 1; i < mir.instCount(); ++i) {
        fn(MirInstId{i, mir.id().v});
    }
}

// Iterate over real block slots (slot 0 is the sentinel).
template <typename Fn>
void forEachBlock(Mir const& mir, Fn fn) {
    for (std::uint32_t i = 1; i < mir.blockCount(); ++i) {
        fn(MirBlockId{i, mir.id().v});
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// THE ONE VALUE↔SLOT TYPE-COMPATIBILITY NOTION
// ═══════════════════════════════════════════════════════════════════════════
//
// P36 (D-MIR-VERIFIER-STORE-CALLARG-TYPE-BLIND +
// D-MIR-VERIFIER-CALLSITE-RESULT-TYPE-UNCHECKED). MIR has THREE seams at which
// a VALUE is placed into a DECLARED SLOT:
//
//   * a Call ARGUMENT into its declared parameter   (checkCallSignatures §5)
//   * a Call RESULT into the callee's declared return (checkCallSignatures §2)
//   * a Store VALUE into its address's pointee      (checkStoreValueTypes)
//
// They are ONE question and they must have ONE answer. The row that opened the
// result half said so in as many words — "reuse the SAME compatibility notion
// the operand half uses, never a second one, or the two halves will drift" —
// so the notion lives HERE, in one function, with its reasons attached, and
// every seam calls it. Before this cycle it existed only as a run of `continue`
// arms inline in the argument loop, reachable by nothing else, which is exactly
// why the other two seams shipped with NO type check at all.
//
// ⚠ A SEAM-SPECIFIC EXEMPTION IS NOT PART OF THIS NOTION AND MUST STAY AT ITS
// SEAM. The F80 `ByValueStackArg` carrier is a fact about how hir_to_mir passes
// an x87 `long double` ARGUMENT; struct-return piece 0 is a fact about how a
// by-value-class RESULT is lowered. Neither is a statement about when two types
// are compatible, and folding either in here would silently widen the other two
// seams by the same amount.
//
// ★ THE NOTION IS DELIBERATELY TIGHT, because MIR is POST-conversion. Every
// implicit conversion C admits was already materialized as an explicit
// Cast/Bitcast by cst_to_hir's coerce arm — that is precisely what
// `HirVerifier::checkCallArguments` enforces with its strict
// `PointerConversionRules{}` (D-HIR-VERIFIER-POINTER-CONVERT-CONTRACT). So a
// value's type must EQUAL its slot's, modulo the four arms below and nothing
// looser. In particular there is NO "all pointers are interchangeable" slack:
// MIR does have one pointer representation, but admitting that would bless a
// shim wiring a `FILE*` into a `char*` slot — the exact class these rules exist
// for.
// ── A SHORT TYPE SPELLING FOR A DIAGNOSTIC ──────────────────────────────────
//
// ⚠ AN INTERNER ID IS NOT EVIDENCE, AND THESE MESSAGES USED TO CARRY NOTHING
// ELSE. ✔MEASURED (P36): the sqlite release build reported *"value typed 5167
// (kind 27) into an address whose pointee is 6074 (kind 27)"* — kind 27 is
// `Ptr` on both sides and a vocabulary name is empty for every pointer, so the
// message said "two pointers, different ids" and stopped. What it was actually
// looking at was `ptr<struct 'Bitvec'>` against `ptr<struct 'Bitvec' incomplete>`
// — ONE C type the merge had forked — and reading that out of the ids took a
// throwaway instrument that then died with the session that wrote it. A
// diagnostic whose reader has to build a tool is not a diagnostic.
//
// ★ THE ID STAYS. It is the only thing that distinguishes two forks of one
// spelling, which is precisely the defect class these messages keep finding;
// dropping it for the prettier text would delete the evidence.
//
// ⓘ SHORT ON PURPOSE, and NOT `mir_text.cpp`'s `appendType`: that one expands a
// composite's whole field list, which is right for a `.dssmir` dump and wrong
// here — the reader wants the shape, and a `struct sqlite3` would bury the
// message. `kDescribeMaxDepth` also makes it total on a cyclic type without a
// visited set, since a self-referential composite is reached through a Ptr and
// the depth cap cuts the walk before it can return to its own tag.
constexpr int kDescribeMaxDepth = 3;

[[nodiscard]] std::string describeType(TypeInterner const& in, TypeId t,
                                       int depth = 0) {
    if (!t.valid()) return "<invalid>";
    if (depth > kDescribeMaxDepth) return "...";
    // The RAW record kind, so a qualifier skin is visible rather than seen
    // through: `ptr<T>` vs `ptr<volatile T>` is a real distinction here, and it
    // is one `sameSlotType`'s ARM 4 exists to admit — a reader comparing two
    // spellings must be able to see which arm applies.
    TypeKind const k = in.get(t).kind;
    switch (k) {
        case TypeKind::Ptr:
        case TypeKind::Ref:
        case TypeKind::Nullable:
        case TypeKind::Optional:
        case TypeKind::Slice: {
            auto const ops = in.operands(t);
            std::string const inner =
                ops.empty() ? std::string{"?"}
                            : describeType(in, ops[0], depth + 1);
            return std::format("{}<{}>", typeKindNameOrEmpty(k), inner);
        }
        case TypeKind::VolatileQual:
            return std::format("qual<{}>",
                               describeType(in, in.stripVolatile(t), depth + 1));
        case TypeKind::Struct:
        case TypeKind::Union: {
            std::string const tag = in.name(t).empty()
                ? std::string{"<anonymous>"}
                : std::format("'{}'", in.name(t));
            // ★ THE INCOMPLETENESS MARKER IS THE LOAD-BEARING HALF. A forward
            // declaration and its definition are ONE C type that the merge can
            // fork, and without this word the two spell identically — which is
            // exactly the reading that made the original message useless.
            return std::format("{} {}{}", typeKindNameOrEmpty(k), tag,
                               in.isIncompleteComposite(t)
                                   ? " incomplete" : "");
        }
        default: {
            std::string const kn{typeKindNameOrEmpty(k)};
            std::string const base =
                kn.empty() ? std::format("kind#{}", static_cast<int>(k)) : kn;
            std::string_view const vocab = in.vocabularyName(t);
            return vocab.empty() ? base : std::format("{} '{}'", base, vocab);
        }
    }
}

[[nodiscard]] bool sameSlotType(TypeInterner const& in, TypeId value,
                                TypeId slot) {
    // Cascade suppression: an untyped side is a violation some OTHER rule owns
    // (checkStructuralInvariants' result-type rule). Whole-verifier convention.
    if (!value.valid() || !slot.valid()) return true;

    // ── ARM 1: IDENTITY ──────────────────────────────────────────────────
    if (value.v == slot.v) return true;

    // ── ARM 2: SAME REPRESENTATION ───────────────────────────────────────
    // Identity-distinct but bit-identical (D-LANG-TYPE-IDENTITY-VOCABULARY:
    // `long` vs `long long` where the data model makes both I64). A
    // same-representation conversion changes NO bits, so the tree deliberately
    // RETAGS rather than emitting a Cast for it; rejecting it here would make
    // the verifier contradict the lowering.
    if (in.sameRepresentation(value, slot)) return true;

    if (in.kind(value) != TypeKind::Ptr || in.kind(slot) != TypeKind::Ptr) {
        return false;
    }

    // ── ARM 3: `void*` ON EITHER SIDE ────────────────────────────────────
    // ★ MEASURED, not assumed. `ptr<void>` is MIR's canonical spelling for "an
    // ADDRESS whose pointee is unknown or irrelevant" — it is the ABSENCE of a
    // type claim, not a claim of `void`. Every va_list frame leaf
    // (Va{Home,Overflow,RegSave}ArgAreaAddr) is emitted `ptr<void>` by
    // hir_to_mir, and every synthesis pass spells an opaque OS/CRT handle
    // (`FILE*`, `mtx_t*`, HANDLE, PINIT_ONCE_FN) `ptr<void>`. C agrees: `void*`
    // converts to and from any object pointer with no cast at all (6.3.2.3p1).
    //
    // THE SHAPE THAT FORCED IT (MEASURED at TF-C112): for
    // `int vsum(int, va_list)` (examples/c/va_list_param_forward) the BASELINE
    // module passes `ap` as a `Load` typed with the declared `va_list`
    // (= `ptr<i8>` under Win64) — an exact match. Running **Mem2Reg** promotes
    // the slot and forwards the DEFINITION — the `VaHomeArgAreaAddr` leaf,
    // typed `ptr<void>` — into the operand, ERASING the pointee with no
    // retagging Cast, because such a retag changes no bits and materializing
    // one would invent a runtime instruction. So pointee identity is NOT a MIR
    // invariant after optimization, and a rule demanding it would demand
    // something the tier does not provide.
    //
    // Scoped as tightly as the evidence allows: ONLY a `void*` on one side.
    // `ptr<struct FILE>` into a `ptr<char>` slot is still REJECTED. Do NOT
    // widen this to "all pointers are interchangeable" — that is the
    // relaxation that would make these rules stop paying for themselves.
    auto const isVoidPtr = [&](TypeId t) {
        auto const o = in.operands(t);
        return !o.empty() && o[0].valid() && in.kind(o[0]) == TypeKind::Void;
    };
    if (isVoidPtr(value) || isVoidPtr(slot)) return true;

    // ── ARM 4: A QUALIFIER SKIN ONE LEVEL DOWN ───────────────────────────
    // ★ MEASURED, not assumed. A qualifier skin (`volatile T` / `_Atomic T` —
    // kind=VolatileQual carrying a QualBit mask) is by the interner's OWN
    // construction a TRANSPARENT, REPRESENTATION-NEUTRAL wrapper:
    // `kind()`/`operands()`/`scalars()` see straight through it, and
    // `sameRepresentation`'s contract says in as many words that
    // "`volatile long` and `long` compare equal here". But `sameRepresentation`
    // compares a composite's OPERANDS by raw TypeId identity, so that
    // neutrality does NOT survive one level of indirection: `ptr<T>` vs
    // `ptr<volatile T>` compares the pointees as raw ids and reports UNEQUAL.
    // This arm restores, for exactly one level, the neutrality the interner
    // already declares.
    //
    // C requires it. `T*` → `volatile T*` is an IMPLICIT QUALIFICATION
    // CONVERSION (C17 6.5.16.1p1), legal with no cast, so the tree correctly
    // emits NO Cast for it — for the same reason it retags rather than Casts a
    // `sameRepresentation` pair.
    //
    // THE SHAPE THAT FORCED IT (MEASURED at TF-C112): sqlite `src/func.c`
    // declares `kahanBabuskaNeumaierInit/Step/StepInt64` as
    // `(volatile SumCtx *, …)` and calls all three with a plain `SumCtx *p`.
    // 11 call sites, 3 callees, one shape. The interner mints ONE `SumCtx`, and
    // gcc 13.2.0 compiles the reduction at `-std=c17 -Wall -Wextra -pedantic`
    // with rc=0 and ZERO diagnostics. It reproduces at `--config=debug`, so
    // unlike the Mem2Reg pointee erasure this is NOT an optimizer artefact:
    // pointee identity modulo qualifiers was never a property of the BASELINE
    // lowering either, because C never required it.
    //
    // Scoped as tightly as the evidence allows, and deliberately NOT
    // `sameRepresentation` on the pointees: this admits ONLY pointees that
    // strip to the IDENTICAL interned TypeId. `ptr<long>` vs `ptr<long long>`
    // stays REJECTED, and so does `ptr<struct FILE>` into `ptr<char>`. The arm
    // cannot fire unless one side actually carries a skin: with neither
    // qualified, `stripVolatile` is the identity and ARM 1 would already have
    // matched.
    //
    // SYMMETRIC on purpose. Discarding a qualifier (`volatile T*` into a `T*`
    // slot) is a C CONSTRAINT violation, not a representation error — it is the
    // front end's to diagnose (the analyzer and HirVerifier's
    // `PointerConversionRules`), and MIR must not encode one source language's
    // qualifier rules or it stops being language-agnostic. See
    // D-CSUBSET-QUALIFIER-DISCARD-AT-CALL-ARG-UNDIAGNOSED.
    //
    // ⚠ STATED, NOT OVERLOOKED: `stripVolatile` removes the WHOLE QualBit mask,
    // so this also excuses `ptr<T>` against a `ptr<_Atomic T>` slot. That is
    // sound HERE and only here: the value is an ADDRESS, and atomicity governs
    // the ACCESSES — which `checkAtomicAccessLowered` guards independently (a
    // plain Load/Store of an atomic-qualified type is its own violation,
    // untouched by this arm). Using the interner's single documented strip
    // chokepoint is also deliberate: a bespoke "volatile bit only" strip would
    // mint a SECOND notion of qualifier identity beside
    // `qualifierBits`/`stripVolatile`, and two notions drift.
    auto const strippedPointee = [&](TypeId t) -> TypeId {
        auto const o = in.operands(t);
        if (o.size() != 1 || !o[0].valid()) return TypeId{};
        return in.stripVolatile(o[0]);
    };
    TypeId const uv = strippedPointee(value);
    TypeId const us = strippedPointee(slot);
    if (uv.valid() && us.valid() && uv == us) return true;

    return false;
}

// The register/`sret` PIECE kinds an ABI-lowered by-value-class value is
// legitimately carried in. FC7 C1c established this exact set for the multi-
// operand `Return` (checkTypeInvariants); the Call RESULT half reuses it rather
// than minting a second list, because both are asking the identical question —
// "is this a register piece of a lowered aggregate?" — and two lists drift.
// F128 is an AAPCS64 binary128 HFA piece (a Q-register); F80 is the x87
// counterpart, defensive (x87 struct returns classify ByReference).
[[nodiscard]] bool isAbiPieceKind(TypeKind k) {
    return k == TypeKind::I64 || k == TypeKind::F64 || k == TypeKind::F32
        || k == TypeKind::Ptr || k == TypeKind::F80 || k == TypeKind::F128;
}

} // namespace

bool MirVerifier::verify(DiagnosticReporter& reporter) const {
    std::size_t const errorsBefore = reporter.errorCount();
    checkStructuralInvariants(reporter);
    checkEntryBlocks(reporter);
    checkBlockTermination(reporter);
    checkPhiIncomings(reporter);
    checkSehStructure(reporter);
    checkVlaStackTeardown(reporter);
    // StructCfMarker equality lives INSIDE checkDomination — the
    // derivation needs the same per-function preds/RPO/dom the
    // use-dom-def scan computes, so they share one computation.
    checkDomination(reporter);
    checkTypeInvariants(reporter);
    checkCallSignatures(reporter);
    checkAtomicAccessLowered(reporter);
    checkStoreValueTypes(reporter);
    if (reporter.hitCap()) return false;
    return reporter.errorCount() == errorsBefore;
}

void MirVerifier::checkStructuralInvariants(DiagnosticReporter& reporter) const {
    forEachInst(mir_, [&](MirInstId id) {
        MirOpcode const op = mir_.instOpcode(id);
        if (op == MirOpcode::Invalid) {
            reportInst(reporter, DiagnosticCode::I_VerifierFailure, id,
                "Invalid opcode");
            return;
        }
        MirOpcodeInfo const& info = opcodeInfo(op);
        if (op != MirOpcode::Phi) {
            auto operands = mir_.instOperands(id);
            // c70 (D-MIR-VERIFIER-UNBOUNDED-OPERAND-SENTINEL): a VARIADIC-operand
            // opcode declares `maxOperands == kMirUnboundedOperands` (0xFF); the
            // upper bound does NOT apply to it — mirror the builder's exemption
            // (mir.cpp addInst: `maxOperands != kMirUnboundedOperands && count >
            // maxOperands`). Without this the verifier read the 0xFF sentinel as a
            // literal max of 255 and rejected a legitimately-variadic Switch/
            // IndirectBr with >255 operands (sqlite has a switch with ~348 cases →
            // 349 operands) that the builder had already accepted.
            bool const overMax = info.maxOperands != kMirUnboundedOperands
                                 && operands.size() > info.maxOperands;
            if (operands.size() < info.minOperands || overMax) {
                reportInst(reporter, DiagnosticCode::I_VerifierFailure, id,
                    std::format("opcode {} operand count {} outside [{}, {}]",
                        info.mnemonic, operands.size(),
                        info.minOperands, info.maxOperands));
            }
        }
        bool const hasType = mir_.instType(id).valid();
        if (info.result == MirResultRule::Value && !hasType) {
            reportInst(reporter, DiagnosticCode::I_VerifierFailure, id,
                std::format("opcode {} is value-producing but has invalid typeId",
                    info.mnemonic));
        } else if (info.result == MirResultRule::None && hasType) {
            reportInst(reporter, DiagnosticCode::I_VerifierFailure, id,
                std::format("opcode {} is value-less but carries a typeId",
                    info.mnemonic));
        }
        if (op == MirOpcode::Const) {
            std::uint32_t const idx = mir_.instPayload(id);
            if (idx >= mir_.literalPool().size()) {
                reportInst(reporter, DiagnosticCode::I_VerifierFailure, id,
                    std::format("const payload {} out of literal-pool range [0, {})",
                        idx, mir_.literalPool().size()));
            }
        }
        // ★ THE SAME RULE FOR THE ASM POOL, AND IT WAS MISSING. Both asm opcodes
        // carry a `MirAsmDescriptorPool` index in their payload, and
        // `Mir::asmDescriptor` ABORTS on an out-of-range one — so a directly
        // constructed module (the merge output, a deserializer, a hand-built
        // fixture) with a stale index killed the process inside the verifier
        // instead of being reported by it. A refusal that crashes is not a
        // refusal; report it here, exactly as the literal pool's index is.
        if (op == MirOpcode::InlineAsm || op == MirOpcode::InlineAsmGoto) {
            std::uint32_t const idx = mir_.instPayload(id);
            if (idx >= mir_.asmDescriptorPool().size()) {
                reportInst(reporter, DiagnosticCode::I_VerifierFailure, id,
                    std::format("{} payload {} out of inline-asm descriptor-pool "
                                "range [0, {})",
                        info.mnemonic, idx, mir_.asmDescriptorPool().size()));
            }
        }
        if (op == MirOpcode::Alloca) {
            // D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN: the Alloca's secondary payload
            // is the local's EFFECTIVE alignment in bytes (0 = no over-alignment
            // recorded). A non-zero value MUST be a power of two ≤ 256 (the
            // `Alignment` newtype cap) — the frame layout places each alloca on
            // this boundary, so a dropped/garbled value (a rebuild/merge site
            // zeroing or corrupting payload2) would mis-align the slot. Fail loud
            // here rather than silently mis-place the stack local.
            std::uint32_t const a = mir_.instPayload2(id);
            if (a != 0 && ((a & (a - 1)) != 0 || a > 256)) {
                reportInst(reporter,
                    DiagnosticCode::I_AllocaAlignmentNotPowerOfTwo, id,
                    std::format("alloca alignment payload {} is not a power of "
                                "two in [1, 256]", a));
            }
            // VLA C1a (D-CSUBSET-VLA): the runtime-sized-Alloca invariant. A VLA
            // alloca (its pointee `isVlaArray`) MUST carry exactly ONE operand (the
            // total runtime byte size) and a ZERO primary payload (the runtime-sized
            // sentinel); a FIXED (non-VLA) alloca MUST carry NO operand. Ties operand
            // presence to the type so a regression that drops the size operand (→ a
            // 0-sized fixed slot) or bolts one onto a fixed alloca is caught loud.
            std::size_t const allocaOperands = mir_.instOperands(id).size();
            std::uint32_t const primaryPayload = mir_.instPayload(id);
            // Interner-free half: an operand present ⇒ the byte payload MUST be 0
            // (exactly one channel encodes the size).
            if (allocaOperands >= 1 && primaryPayload != 0) {
                reportInst(reporter,
                    DiagnosticCode::I_VlaAllocaOperandInvalid, id,
                    std::format("runtime-sized (VLA) alloca carries a size operand "
                                "AND a non-zero byte payload {} — exactly one must "
                                "encode the size", primaryPayload));
            }
            // Interner-gated half: tie operand-presence to VLA-ness of the pointee.
            if (interner_ != nullptr) {
                TypeId const resTy = mir_.instType(id);   // ptr<allocated>
                TypeId pointee{};
                if (resTy.valid() && interner_->kind(resTy) == TypeKind::Ptr) {
                    auto const ops = interner_->operands(resTy);
                    if (!ops.empty()) pointee = ops[0];
                }
                // VLA C3: `||typeContainsVla` so a FIXED-outer multi-dim VLA
                // (`int a[5][n]` — pointee array(vlaArray,5), a fixed Array whose top
                // is NOT isVlaArray but which STILL takes the runtime-operand alloca)
                // is correctly classified as runtime-sized (else its size operand
                // trips the "fixed alloca must carry no operand" arm below).
                bool const isVla =
                    pointee.valid()
                    && (interner_->isVlaArray(pointee)
                        || interner_->typeContainsVla(pointee));
                if (isVla && allocaOperands != 1) {
                    reportInst(reporter,
                        DiagnosticCode::I_VlaAllocaOperandInvalid, id,
                        std::format("VLA-typed alloca must carry exactly one runtime "
                                    "size operand, found {}", allocaOperands));
                } else if (!isVla && allocaOperands != 0) {
                    reportInst(reporter,
                        DiagnosticCode::I_VlaAllocaOperandInvalid, id,
                        std::format("fixed (non-VLA) alloca must carry no runtime "
                                    "size operand, found {}", allocaOperands));
                }
            }
        }
    });
    // CFG-successor range validation. `mirBuildPredecessors` (the
    // shared dom helper) silently skips out-of-range successor edges
    // — the diagnostic is the verifier's responsibility, emitted
    // here once per bad edge so downstream consumers see the actual
    // corruption (the edge), not a cascade of follow-on phi / dom
    // failures. The check was previously embedded in `buildPredecessors`
    // before extraction to `mir_dom.hpp` (D-OPT-DOMTREE-EXTRACTION).
    std::size_t const blockCount = mir_.blockCount();
    for (std::uint32_t i = 1; i < blockCount; ++i) {
        MirBlockId const from{i, mir_.id().v};
        for (MirBlockId const to : mir_.blockSuccessors(from)) {
            if (to.v >= blockCount) {
                reportBlock(reporter, DiagnosticCode::I_VerifierFailure, from,
                    std::format("mir cfg edge #{} → #{}: successor block "
                                "out of range (blockCount = {})",
                                from.v, to.v, blockCount));
            }
        }
        checkTerminatorSuccessorArity(reporter, from);
    }
}

// ★★★ THE SUCCESSOR-ARITY BACKSTOP, AND IT WAS CLAIMED TO EXIST BEFORE IT DID.
// `MirBuilder::recordSuccessors_`'s comment justified itself by saying "ML3's
// verifier re-runs the same descriptor check on any frozen module — including the
// direct-Mir-ctor path this builder doesn't own". ✔MEASURED: this file validated
// OPERAND arity against `opcodeInfo` and range-checked edge TARGETS, and carried
// no successor-count check of any kind — so a `Mir` built through the direct
// constructor (the merge output, a deserializer, a hand-built fixture) could hold
// a terminator with the wrong number of edges and reach the backend with no
// diagnostic. This is that check, and it is the frozen-module half of the defence
// behind `cloneInlineAsmGoto`'s descriptor-vs-successor rule: the builder guards
// the paths it owns, the verifier guards the ones it does not.
void MirVerifier::checkTerminatorSuccessorArity(DiagnosticReporter& reporter,
                                                MirBlockId block) const {
    // An empty block or a block whose last instruction is not a terminator is
    // `checkBlockTermination`'s report to make — one defect, one diagnostic.
    if (mir_.blockInstCount(block) == 0) return;
    MirInstId const term = mir_.blockTerminator(block);
    MirOpcode const op   = mir_.instOpcode(term);
    if (!isTerminator(op)) return;

    MirOpcodeInfo const& info = opcodeInfo(op);
    auto const n = mir_.blockSuccessors(block).size();
    bool const overMax = info.maxSuccessors != kMirUnboundedSuccessors
                         && n > info.maxSuccessors;
    if (n < info.minSuccessors || overMax) {
        reportBlock(reporter, DiagnosticCode::I_VerifierFailure, block,
            std::format("terminator {} carries {} CFG successor(s), outside [{}, {}]",
                        info.mnemonic, n, info.minSuccessors,
                        info.maxSuccessors == kMirUnboundedSuccessors
                            ? std::string{"inf"}
                            : std::format("{}", info.maxSuccessors)));
        return;
    }
    // ⚠ THE RANGE IS NOT ENOUGH FOR `asm goto`, WHICH IS THE WHOLE REASON THE
    // DESCRIPTOR CARRIES ITS LABELS. Its successors are [label…, fall-through] and
    // the range is `[2, ∞)`, so a two-label goto that LOST its fall-through edge is
    // still inside the range — and losing it deletes the code after the statement
    // (the unreachable prune drops a block nothing reaches). The descriptor's
    // per-label spellings are the carried fact that makes the loss visible.
    // ⓘ Guarded on the pool range because reading a stale index ABORTS: the
    // per-instruction sweep above has already REPORTED that case, so skipping it
    // here costs no diagnostic and keeps this rule from crashing on a module the
    // verifier is meant to describe.
    if (op == MirOpcode::InlineAsmGoto
        && mir_.instPayload(term) < mir_.asmDescriptorPool().size()) {
        std::size_t const labels = mir_.asmDescriptor(term).labelSpellings.size();
        if (labels + 1 != n) {
            reportBlock(reporter, DiagnosticCode::I_VerifierFailure, block,
                std::format("inlineasmgoto declares {} label(s) but carries {} CFG "
                            "successor(s) — the successors are the labels plus the "
                            "fall-through edge, so they must differ by exactly one",
                            labels, n));
        }
    }
}

void MirVerifier::checkEntryBlocks(DiagnosticReporter& reporter) const {
    for (std::uint32_t fi = 0; fi < mir_.moduleFuncCount(); ++fi) {
        MirFuncId const f = mir_.funcAt(fi);
        std::uint32_t const nBlocks = mir_.funcBlockCount(f);
        if (nBlocks == 0) {
            reportFunc(reporter, DiagnosticCode::I_NoEntryBlock, f,
                "function has zero blocks");
            continue;
        }
        std::uint32_t entryCount = 0;
        MirBlockId firstEntry{};
        for (std::uint32_t bi = 0; bi < nBlocks; ++bi) {
            MirBlockId const b = mir_.funcBlockAt(f, bi);
            if (mir_.blockMarker(b) == StructCfMarker::EntryBlock) {
                ++entryCount;
                if (!firstEntry.valid()) firstEntry = b;
            }
        }
        if (entryCount == 0) {
            reportFunc(reporter, DiagnosticCode::I_NoEntryBlock, f,
                "no block marked EntryBlock");
        } else if (entryCount > 1) {
            reportFunc(reporter, DiagnosticCode::I_MultipleEntryBlocks, f,
                std::format("{} blocks marked EntryBlock (expected exactly 1)",
                    entryCount));
        } else {
            MirBlockId const slot0 = mir_.funcBlockAt(f, 0);
            if (slot0.v != firstEntry.v) {
                reportFunc(reporter, DiagnosticCode::I_EntryBlockNotFirst, f,
                    std::format("EntryBlock is #{}, but funcBlockAt(f, 0) is #{}",
                        firstEntry.v, slot0.v));
            }
        }
    }
}

void MirVerifier::checkBlockTermination(DiagnosticReporter& reporter) const {
    forEachBlock(mir_, [&](MirBlockId b) {
        std::uint32_t const n = mir_.blockInstCount(b);
        if (n == 0) {
            reportBlock(reporter, DiagnosticCode::I_BlockNotTerminated, b,
                "block is empty (no terminator)");
            return;
        }
        MirInstId const last = mir_.blockInstAt(b, n - 1);
        if (!isTerminator(mir_.instOpcode(last))) {
            reportBlock(reporter, DiagnosticCode::I_BlockNotTerminated, b,
                std::format("last inst #{} opcode {} is not a terminator",
                    last.v, opcodeInfo(mir_.instOpcode(last)).mnemonic));
        }
    });
}

void MirVerifier::checkPhiIncomings(DiagnosticReporter& reporter) const {
    auto preds = mirBuildPredecessors(mir_);
    forEachInst(mir_, [&](MirInstId id) {
        if (mir_.instOpcode(id) != MirOpcode::Phi) return;
        MirBlockId const phiBlock = mir_.instBlock(id);
        if (phiBlock.v >= preds.size()) {
            reportInst(reporter, DiagnosticCode::I_VerifierFailure, id,
                std::format("phi's enclosing block #{} is out of range",
                    phiBlock.v));
            return;
        }
        auto const& blockPreds = preds[phiBlock.v];
        std::unordered_set<std::uint32_t> predSet;
        for (auto p : blockPreds) predSet.insert(p.v);
        for (MirPhiIncoming const& inc : mir_.phiIncomings(id)) {
            if (!predSet.contains(inc.pred.v)) {
                reportInst(reporter, DiagnosticCode::I_PhiPredNotInCfg, id,
                    std::format("(phi in block #{}) names predecessor #{} but "
                                "that block is not a CFG-predecessor",
                        phiBlock.v, inc.pred.v));
            }
        }
    });
}

// c115 SEH (D-WIN64-SEH-FUNCLETS): the region-skeleton pairing rules. Runs
// verify-after-every-pass, so an optimizer transform that damages the skeleton
// (SimplifyCfg merging a filter/handler block, a rebuild dropping a marker's
// pairing) reds AT the pass that did it. Zero-cost when no SehTryBegin exists.
void MirVerifier::checkSehStructure(DiagnosticReporter& reporter) const {
    // One flat scan; bail before building predecessors when SEH-free.
    bool anySeh = false;
    forEachInst(mir_, [&](MirInstId id) {
        if (mir_.instOpcode(id) == MirOpcode::SehTryBegin) anySeh = true;
    });
    if (!anySeh) return;

    auto const preds = mirBuildPredecessors(mir_);
    auto predCount = [&](MirBlockId b) -> std::size_t {
        return b.v < preds.size() ? preds[b.v].size() : 0u;
    };

    for (std::uint32_t fi = 0; fi < mir_.moduleFuncCount(); ++fi) {
        MirFuncId const f = mir_.funcAt(fi);
        // Gather the function's region ids (SehTryBegin payloads) + check the
        // per-Begin skeleton; then validate End pairing + Code/Info containment.
        std::unordered_set<std::uint32_t> regionIds;
        bool fnHasBegin = false;
        for (std::uint32_t bi = 0; bi < mir_.funcBlockCount(f); ++bi) {
            MirBlockId const b = mir_.funcBlockAt(f, bi);
            std::uint32_t const n = mir_.blockInstCount(b);
            if (n == 0) continue;
            MirInstId const term = mir_.blockInstAt(b, n - 1);
            if (mir_.instOpcode(term) != MirOpcode::SehTryBegin) continue;
            fnHasBegin = true;
            std::uint32_t const region = mir_.instPayload(term);
            regionIds.insert(region);

            auto const succs = mir_.blockSuccessors(b);
            if (succs.size() != 2) continue;   // structural check owns arity
            MirBlockId const filterBB = succs[1];
            if (predCount(filterBB) != 1) {
                reportInst(reporter, DiagnosticCode::I_SehStructure, term,
                    std::format("SEH region {}: filter block #{} must have "
                                "exactly one predecessor (its SehTryBegin), "
                                "found {}",
                        region, filterBB.v, predCount(filterBB)));
                continue;
            }
            std::uint32_t const fn2 = mir_.blockInstCount(filterBB);
            MirInstId const fterm = fn2 > 0
                ? mir_.blockInstAt(filterBB, fn2 - 1) : MirInstId{};
            if (!fterm.valid()
                || mir_.instOpcode(fterm) != MirOpcode::SehFilterReturn) {
                reportInst(reporter, DiagnosticCode::I_SehStructure, term,
                    std::format("SEH region {}: filter block #{} must terminate "
                                "in SehFilterReturn", region, filterBB.v));
                continue;
            }
            if (mir_.instPayload(fterm) != region) {
                reportInst(reporter, DiagnosticCode::I_SehStructure, fterm,
                    std::format("SehFilterReturn payload {} does not match its "
                                "SehTryBegin region {}",
                        mir_.instPayload(fterm), region));
            }
            auto const fsuccs = mir_.blockSuccessors(filterBB);
            if (fsuccs.size() == 1 && predCount(fsuccs[0]) != 1) {
                reportInst(reporter, DiagnosticCode::I_SehStructure, fterm,
                    std::format("SEH region {}: handler block #{} must have "
                                "exactly one predecessor (its filter), found {}",
                        region, fsuccs[0].v, predCount(fsuccs[0])));
            }
        }
        // SehTryEnd pairing + intrinsic containment (per function).
        for (std::uint32_t bi = 0; bi < mir_.funcBlockCount(f); ++bi) {
            MirBlockId const b = mir_.funcBlockAt(f, bi);
            for (std::uint32_t ii = 0; ii < mir_.blockInstCount(b); ++ii) {
                MirInstId const inst = mir_.blockInstAt(b, ii);
                MirOpcode const op = mir_.instOpcode(inst);
                if (op == MirOpcode::SehTryEnd
                    && !regionIds.contains(mir_.instPayload(inst))) {
                    reportInst(reporter, DiagnosticCode::I_SehStructure, inst,
                        std::format("SehTryEnd names region {} but this function "
                                    "has no SehTryBegin with that id",
                            mir_.instPayload(inst)));
                }
                if ((op == MirOpcode::SehExceptionCode
                     || op == MirOpcode::SehExceptionInfo)
                    && !fnHasBegin) {
                    reportInst(reporter, DiagnosticCode::I_SehStructure, inst,
                        "SehExceptionCode/Info in a function with no SehTryBegin "
                        "(the HIR-tier context rule should have rejected this)");
                }
            }
        }
    }
}

// VLA C5 (D-CSUBSET-VLA): the block-scope stack-teardown pairing invariant.
// Runs verify-after-every-pass so an optimizer transform that mis-pairs or
// re-points a save/restore reds AT the pass that did it. Zero-cost when no
// StackSave exists.
void MirVerifier::checkVlaStackTeardown(DiagnosticReporter& reporter) const {
    // One flat scan; bail before the per-restore check when teardown-free.
    bool anyTeardown = false;
    forEachInst(mir_, [&](MirInstId id) {
        MirOpcode const op = mir_.instOpcode(id);
        if (op == MirOpcode::StackSave || op == MirOpcode::StackRestore)
            anyTeardown = true;
    });
    if (!anyTeardown) return;

    forEachInst(mir_, [&](MirInstId id) {
        if (mir_.instOpcode(id) != MirOpcode::StackRestore) return;
        auto const ops = mir_.instOperands(id);
        // Arity is builder-enforced (1 operand); re-check defensively — a
        // direct-Mir-ctor path (fixtures / synthetic IR) could bypass it.
        if (ops.size() != 1) {
            reportInst(reporter, DiagnosticCode::I_VlaStackRestorePairing, id,
                std::format("StackRestore must have exactly one operand (the "
                            "saved-SP value), found {}", ops.size()));
            return;
        }
        MirInstId const savedDef = ops[0];
        if (!savedDef.valid()
            || mir_.instOpcode(savedDef) != MirOpcode::StackSave) {
            reportInst(reporter, DiagnosticCode::I_VlaStackRestorePairing, id,
                "StackRestore operand[0] must be a StackSave value (the "
                "captured scope-entry watermark)");
            return;
        }
        if (mir_.instPayload(id) != mir_.instPayload(savedDef)) {
            reportInst(reporter, DiagnosticCode::I_VlaStackRestorePairing, id,
                std::format("StackRestore scopeId {} does not match its paired "
                            "StackSave scopeId {}",
                    mir_.instPayload(id), mir_.instPayload(savedDef)));
        }
    });
}

void MirVerifier::checkDomination(DiagnosticReporter& reporter) const {
    auto preds = mirBuildPredecessors(mir_);
    // ══ EVERY WHOLE-MODULE SUBSTRATE IS ESTABLISHED ONCE, OUTSIDE THE LOOP ═══
    //
    // ✔MEASURED 2026-08-25 (cycle P36), release dsscp over the 103-TU
    // full-source sqlite corpus: a whole-program build runs this function TWICE
    // over the merged 86,411-block / 4,030-function module — once for the
    // optimizer's final verify and once inside `mergeCuMirs` — and each run was
    // paying FIVE O(module) establishments PER FUNCTION
    // ([[D-PERF-VERIFIER-REESTABLISHES-MODULE-SUBSTRATES-PER-FUNCTION]]):
    //   1. a fresh six-buffer dominator computation   -> `MirDomScratch`
    //   2. `indexInBlock`, sized to the module's INSTRUCTION count  -> hoisted
    //   3. `layoutPos`, sized to the module's block count           -> hoisted
    //   4. the self-loop index the marker derivation needs  -> `MirStructCfScratch`
    //   5. a fresh eight-buffer post-dominator computation  -> `MirStructCfScratch`
    // That is functions x O(module) — quadratic in module size, and the SAME
    // defect [[D-OPT-DOMTREE-SCRATCH-REUSE]] / [[D-OPT-POSTDOM-SCRATCH-REUSE]] /
    // [[D-OPT-NATURAL-LOOPS-MODULE-WIDE-SCAN]] already removed from the marker
    // applier. The verifier could not reach those fixes; now it does.
    //
    // ⚠ ANYTHING ADDED BELOW THAT IS O(module) PER FUNCTION PUTS IT BACK.
    //
    // (2) and (3) are hoisted with an EXPLICIT touched-slot reset rather than a
    // re-fill: the fill loops below are the ONLY writers, so walking the same
    // two loops on the way out restores exactly the fresh-vector default. A
    // whole-vector `assign` would be the O(module)-per-function cost again.
    MirDomScratch      domScratch;
    MirStructCfScratch cfScratch;
    std::vector<std::uint32_t> indexInBlock(mir_.instCount(),
        static_cast<std::uint32_t>(-1));
    std::vector<std::uint32_t> layoutPos(mir_.blockCount(),
        static_cast<std::uint32_t>(-1));
    for (std::uint32_t fi = 0; fi < mir_.moduleFuncCount(); ++fi) {
        MirFuncId const f = mir_.funcAt(fi);
        if (mir_.funcBlockCount(f) == 0) continue;
        MirBlockId const entry = mir_.funcEntry(f);
        if (!entry.valid()) {
            reportFunc(reporter, DiagnosticCode::I_NoEntryBlock, f,
                "funcEntry() returned InvalidMirBlock; skipping dominance check");
            continue;
        }
        auto rpo  = mirReversePostOrder(mir_, entry);
        MirDomTree const& domState =
            computeMirDomTree(mir_, entry, rpo, preds, domScratch);
        // Emit `I_VerifierFailure` for every block whose idom couldn't
        // be computed (intersect bailed). Without this signal the
        // caller would silently see an under-conservative idom and
        // miss real use-dom-def violations on that block's operands.
        for (std::uint32_t bi = 0; bi < domState.gaveUp.size(); ++bi) {
            if (!domState.gaveUp[bi]) continue;
            reportBlock(reporter, DiagnosticCode::I_VerifierFailure,
                MirBlockId{bi, mir_.id().v},
                "dominator analysis gave up (idom intersect bailed — input "
                "likely has an idom cycle from direct-`Mir`-ctor construction)");
        }
        // Vector-indexed same-block-position map (replaces unordered_map
        // for a tighter inner loop). Slot 0 unused. Hoisted out of this loop
        // (see the header note above) and reset at function EXIT; it enters
        // every iteration all-(-1), exactly as the per-function vector did.
        for (MirBlockId const b : rpo) {
            std::uint32_t const n = mir_.blockInstCount(b);
            for (std::uint32_t i = 0; i < n; ++i) {
                indexInBlock[mir_.blockInstAt(b, i).v] = i;
            }
        }
        // The RAII reset for BOTH hoisted maps, so each iteration observes them
        // exactly as it observed a freshly-allocated vector. It is a guard
        // object and not a trailing loop because this body `continue`s.
        //
        // ⓘ HONEST SCOPE, since an over-claiming comment is worse than none: a
        // leak is UNREACHABLE today. `indexInBlock[op.v]` is read only when
        // `op`'s block IS the use block (∈ rpo, so filled this iteration), and
        // `layoutPos` is read only behind `mirDominatesBlock(...) == Dominates`
        // against a ONE-FUNCTION tree, which no foreign block can satisfy. The
        // reset is what makes the hoist equivalent UNCONDITIONALLY rather than
        // by that two-step argument — widen either read and the argument dies
        // silently, whereas the reset does not. It is O(function), the same
        // walk the fill just did.
        struct ScratchReset {
            std::vector<std::uint32_t>&    indexInBlock;
            std::vector<std::uint32_t>&    layoutPos;
            Mir const&                     mir;
            std::vector<MirBlockId> const& rpo;
            MirFuncId                      f;
            ~ScratchReset() {
                for (MirBlockId const b : rpo) {
                    std::uint32_t const n = mir.blockInstCount(b);
                    for (std::uint32_t i = 0; i < n; ++i) {
                        indexInBlock[mir.blockInstAt(b, i).v] =
                            static_cast<std::uint32_t>(-1);
                    }
                }
                std::uint32_t const nb = mir.funcBlockCount(f);
                for (std::uint32_t bi = 0; bi < nb; ++bi) {
                    layoutPos[mir.funcBlockAt(f, bi).v] =
                        static_cast<std::uint32_t>(-1);
                }
            }
        } const scratchReset{indexInBlock, layoutPos, mir_, rpo, f};
        // Block LAYOUT-position map (funcBlockAt order — the order the
        // MIR→LIR lowering + every MirFunctionRebuilder emits blocks).
        // Indexed by block .v; slot 0 unused. Drives the layout rule
        // below: a cross-block operand whose definition DOMINATES its use
        // (SSA-legal) must ALSO be laid out before it, or no linear
        // consumer can resolve it (D-OPT2 layout contract, I0010).
        {
            std::uint32_t const nBlocksF = mir_.funcBlockCount(f);
            for (std::uint32_t bi = 0; bi < nBlocksF; ++bi) {
                layoutPos[mir_.funcBlockAt(f, bi).v] = bi;
            }
        }
        // Orphan-block diagnostic: any block in the function that is
        // NOT in `rpo` is unreachable from entry. ExitBlock + LoopExit
        // / IfJoin blocks ARE reachable via the CFG (their preds carry
        // br/condbr), so genuine reachable blocks pass; only orphans
        // fail.
        std::unordered_set<std::uint32_t> reachable;
        for (MirBlockId const b : rpo) reachable.insert(b.v);
        std::uint32_t const nBlocks = mir_.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nBlocks; ++bi) {
            MirBlockId const b = mir_.funcBlockAt(f, bi);
            if (!reachable.contains(b.v)) {
                reportBlock(reporter, DiagnosticCode::I_UnreachableBlock, b,
                    std::format("block in func #{} is not reachable from entry",
                        f.v));
            }
        }
        // StructCfMarker equality (the derivation model, D-OPT4-1): the
        // verifier RECOMPUTES the canonical derivation independently —
        // never trusting a producer-supplied vector — and requires
        // stored == derived for every REACHABLE block. PLACEMENT
        // PRINCIPLE: producers rederive at their own sites (lowering /
        // SimplifyCfg / inliner / merge call rederiveStructCfMarkers
        // after finish()); a central rederive-before-verify here would
        // make this equality tautological. Unreachable blocks are
        // skipped — I_UnreachableBlock (above) owns them. This subsumed
        // the old count-parity switch, the ExitBlock-terminator rule,
        // and the LoopHeader-back-edge rule (a no-back-edge "header" now
        // simply derives non-LoopHeader).
        // Sharing: the derivation reuses THIS function's preds/rpo/dom
        // (one computation per function per verify; the post-dominator
        // tree is the only addition, built inside the derivation).
        {
            auto const derived =
                deriveStructCfMarkers(mir_, f, preds, rpo, domState, cfScratch);
            for (MirBlockId const b : rpo) {
                StructCfMarker const stored = mir_.blockMarker(b);
                if (b.v >= derived.size()) continue;  // defensive — derived is blockCount-sized
                if (stored != derived[b.v]) {
                    reportBlock(reporter, DiagnosticCode::I_StructCfMismatch, b,
                        std::format("stored marker {} != derived marker {} "
                                    "(markers must equal the canonical CFG "
                                    "derivation - mir_struct_markers.hpp)",
                            structCfMarkerName(stored),
                            structCfMarkerName(derived[b.v])));
                }
            }
        }
        // Use-dom-def scan over reachable blocks.
        for (MirBlockId const useBlock : rpo) {
            std::uint32_t const n = mir_.blockInstCount(useBlock);
            for (std::uint32_t i = 0; i < n; ++i) {
                MirInstId const use = mir_.blockInstAt(useBlock, i);
                MirOpcode const useOp = mir_.instOpcode(use);
                if (useOp == MirOpcode::Phi) {
                    for (MirPhiIncoming const& inc : mir_.phiIncomings(use)) {
                        if (!inc.value.valid()) continue;
                        MirBlockId const defBlock = mir_.instBlock(inc.value);
                        MirDomResult const dr = mirDominatesBlock(defBlock, inc.pred, domState);
                        if (dr == MirDomResult::DoesNot) {
                            reportInst(reporter, DiagnosticCode::I_NotDominated, use,
                                std::format("(phi in block #{}) incoming value "
                                            "#{} defined in block #{} does not "
                                            "dominate predecessor block #{}",
                                    useBlock.v, inc.value.v,
                                    defBlock.v, inc.pred.v));
                        } else if (dr == MirDomResult::GaveUp) {
                            reportInst(reporter, DiagnosticCode::I_VerifierFailure, use,
                                std::format("dominance check aborted for phi-"
                                            "incoming value #{} against pred #{} "
                                            "(idom chain step-cap exceeded)",
                                    inc.value.v, inc.pred.v));
                        }
                    }
                    continue;
                }
                auto operands = mir_.instOperands(use);
                for (MirInstId const op : operands) {
                    if (!op.valid()) continue;
                    MirBlockId const defBlock = mir_.instBlock(op);
                    if (defBlock.v == useBlock.v) {
                        std::uint32_t const defIdx =
                            (op.v < indexInBlock.size())
                                ? indexInBlock[op.v]
                                : static_cast<std::uint32_t>(-1);
                        if (defIdx != static_cast<std::uint32_t>(-1)
                         && defIdx >= i) {
                            reportInst(reporter, DiagnosticCode::I_NotDominated, use,
                                std::format("uses value #{} defined later in the "
                                            "same block #{} (use at index {}, "
                                            "def at index {})",
                                    op.v, useBlock.v, i, defIdx));
                        }
                    } else {
                        MirDomResult const dr = mirDominatesBlock(defBlock, useBlock, domState);
                        if (dr == MirDomResult::DoesNot) {
                            reportInst(reporter, DiagnosticCode::I_NotDominated, use,
                                std::format("uses value #{} defined in block #{} "
                                            "which does not dominate use block #{}",
                                    op.v, defBlock.v, useBlock.v));
                        } else if (dr == MirDomResult::GaveUp) {
                            reportInst(reporter, DiagnosticCode::I_VerifierFailure, use,
                                std::format("dominance check aborted for value "
                                            "#{} (def block #{}, use block #{}) "
                                            "— idom chain step-cap exceeded",
                                    op.v, defBlock.v, useBlock.v));
                        } else {
                            // LAYOUT RULE (I_LayoutUseBeforeDef, D-OPT2
                            // layout contract). GATED on Dominates: a
                            // non-dominating def already reported above —
                            // one bad operand must not double-report. When
                            // the def DOES dominate (SSA-legal), the linear
                            // lowering ALSO requires it to be laid out
                            // before the use. Phi incomings are EXEMPT
                            // (handled by the Phi arm above): a loop
                            // back-edge legitimately carries a def whose
                            // layout FOLLOWS the use, and the dominance arm
                            // owns that semantics — only NON-Phi linear
                            // operands flow here. Defensive index guard:
                            // both blocks are reachable (in rpo ⊆ this
                            // function), so both have a valid layoutPos.
                            std::uint32_t const defPos =
                                (defBlock.v < layoutPos.size())
                                    ? layoutPos[defBlock.v]
                                    : static_cast<std::uint32_t>(-1);
                            std::uint32_t const usePos =
                                (useBlock.v < layoutPos.size())
                                    ? layoutPos[useBlock.v]
                                    : static_cast<std::uint32_t>(-1);
                            if (defPos != static_cast<std::uint32_t>(-1)
                             && usePos != static_cast<std::uint32_t>(-1)
                             && defPos >= usePos) {
                                reportInst(reporter,
                                    DiagnosticCode::I_LayoutUseBeforeDef, use,
                                    std::format("uses value #{} defined in block "
                                                "#{} (layout pos {}) which "
                                                "dominates but is laid out at or "
                                                "after use block #{} (layout pos "
                                                "{}) — no linear consumer can "
                                                "resolve a def emitted after its "
                                                "use",
                                        op.v, defBlock.v, defPos,
                                        useBlock.v, usePos));
                            }
                        }
                    }
                }
            }
        }
    }
}

void MirVerifier::checkTypeInvariants(DiagnosticReporter& reporter) const {
    if (interner_ == nullptr) return;
    forEachInst(mir_, [&](MirInstId id) {
        TypeId const t = mir_.instType(id);
        if (!t.valid()) return;
        if (interner_->kind(t) == TypeKind::Extension) {
            reportInst(reporter, DiagnosticCode::I_ExtensionTypeInMir, id,
                std::format("typeId {} resolves to TypeKind::Extension (every "
                            "extension type must be resolved to a core kind at "
                            "the HIR→MIR boundary)",
                    t.v));
        }
        // C23 nullptr_t (D-CSUBSET-NULLPTR): a never-fires backstop for the keystone
        // invariant — the `nullptr` literal lowers to the integer-0 null constant at
        // the HIR tier (cst_to_hir `lowerLiteral`), so NullptrT is a semantic-tier-
        // only kind that must not reach MIR. A NullptrT result type here means a
        // regression let a NullptrT-typed Const materialize.
        if (interner_->kind(t) == TypeKind::NullptrT) {
            reportInst(reporter, DiagnosticCode::I_NullptrTypeInMir, id,
                std::format("typeId {} resolves to TypeKind::NullptrT (C23 nullptr_t "
                            "is semantic-tier-only — `nullptr` must lower to the "
                            "integer-0 null constant and never reach MIR)",
                    t.v));
        }
        // C23 _BitInt(N) (D-CSUBSET-BITINT): the by-construction WRAP-CHOKEPOINT
        // tripwire (CRIT-2). Only the ARITHMETIC opcodes the wrap chokepoint PRODUCES
        // carry the single-width-operand invariant: their `_BitInt(N)` result computes
        // at width N, so each `_BitInt`-typed VALUE operand must ALSO be width N. This
        // is an ALLOWLIST, NOT a `!conversion` denylist — the denylist wrongly admitted
        //   • Phi — `instOperands` ABORTS on a Phi (mir.cpp), and a `_BitInt` ternary /
        //     a `for (_BitInt i; …)` induction var (mem2reg) legitimately materialize a
        //     `_BitInt(N)` Phi that MERGES same-N arms (already width N);
        //   • Call — its result width is its RETURN type, UNRELATED to argument widths
        //     (`unsigned _BitInt(4) f(unsigned _BitInt(40))` is a valid mixed-width call);
        //   • Load / Const / GEP / conversions — no width-N-operand invariant either.
        // A shift's COUNT (operand 1 of Shl/LShr/AShr) is NOT width-constrained (C 6.5.7)
        // and is exempt; a non-`_BitInt` operand (an int count, a container mask const)
        // is skipped. Never fires under the chokepoint; catches a mixed-width arith op.
        if (interner_->kind(t) == TypeKind::BitInt) {
            MirOpcode const op = mir_.instOpcode(id);
            bool const isBitIntArith =
                op == MirOpcode::Add  || op == MirOpcode::Sub  || op == MirOpcode::Mul
             || op == MirOpcode::SDiv || op == MirOpcode::UDiv
             || op == MirOpcode::SMod || op == MirOpcode::UMod
             || op == MirOpcode::And  || op == MirOpcode::Or   || op == MirOpcode::Xor
             || op == MirOpcode::Shl  || op == MirOpcode::LShr || op == MirOpcode::AShr
             || op == MirOpcode::Neg  || op == MirOpcode::Not;
            if (isBitIntArith) {
                bool const isShift = op == MirOpcode::Shl || op == MirOpcode::LShr
                                  || op == MirOpcode::AShr;
                std::int64_t const n = interner_->bitIntWidth(t);
                auto const ops = mir_.instOperands(id);
                for (std::size_t i = 0; i < ops.size(); ++i) {
                    if (isShift && i == 1) continue;   // shift count — width-free (6.5.7)
                    TypeId const ot = mir_.instType(ops[i]);
                    if (ot.valid() && interner_->kind(ot) == TypeKind::BitInt
                        && interner_->bitIntWidth(ot) != n) {
                        reportInst(reporter, DiagnosticCode::I_BitIntWidthInconsistent,
                            id, std::format(
                                "_BitInt({}) arithmetic result has a _BitInt({}) operand "
                                "(#{}) — a bit-precise op must compute at ONE width; the "
                                "wrap chokepoint coerces operands to the common width "
                                "first", n, interner_->bitIntWidth(ot), ops[i].v));
                    }
                }
            }
        }
    });
    for (std::uint32_t fi = 0; fi < mir_.moduleFuncCount(); ++fi) {
        MirFuncId const f = mir_.funcAt(fi);
        TypeId const fnSig = mir_.funcSignature(f);
        if (!fnSig.valid() || interner_->kind(fnSig) != TypeKind::FnSig) {
            continue;
        }
        // FnSig layout convention (HR4-established, project-wide,
        // language-agnostic): `operands[0]` is the return type;
        // `operands[1..]` are the parameter types. Documented here
        // so the verifier doesn't silently misfire if any future
        // language schema deviates — adding a `TypeInterner::
        // fnSigReturnType()`/`fnSigParamCount()` accessor pair and
        // routing through it is the long-term cure (tracked as a
        // type-lattice followup; tier-2 — no current consumer would
        // benefit).
        auto operands = interner_->operands(fnSig);
        TypeId const returnTy = operands.empty() ? InvalidType : operands[0];
        std::uint32_t const nBlocks = mir_.funcBlockCount(f);
        // FC7 (D-FC7-SYSV-STRUCT-ARG-MULTIREG): the physical-arg count is NOT the
        // FnSig param count — a by-value struct param expands to MULTIPLE register
        // `Arg`s (one per SysV eightbyte / AAPCS64 piece). The `Arg` payload is the
        // PER-CLASS (or, for a slot-aligned CC, flat) physical register ordinal,
        // which HIR→MIR emits with a MONOTONIC per-class counter — so every payload
        // is < the number of `Arg` instructions in the function (a per-class payload
        // < its class count <= the total; a flat payload < the total). Bound the
        // check on THAT count, not the FnSig paramCount, so a multi-register struct
        // param verifies while a stray out-of-range `Arg` is still rejected.
        std::uint32_t argCount = 0;
        // FC7 C3 (AAPCS64/Apple x8 sret): a function that reads the indirect-result
        // register (ReadIndirectResult at entry) is a register-based-sret struct
        // returner — its by-value aggregate result is written THROUGH x8 and the MIR
        // `Return` is legitimately VOID (the SysV/Win64 hidden-arg path instead
        // returns the sret pointer). This op is the CC-config-free marker that lets
        // the return check below accept a void return for a non-void (struct) func.
        bool hasIndirectResultRead = false;
        for (std::uint32_t bi = 0; bi < nBlocks; ++bi) {
            MirBlockId const b = mir_.funcBlockAt(f, bi);
            std::uint32_t const n = mir_.blockInstCount(b);
            for (std::uint32_t i = 0; i < n; ++i) {
                MirOpcode const o = mir_.instOpcode(mir_.blockInstAt(b, i));
                if (o == MirOpcode::Arg) ++argCount;
                else if (o == MirOpcode::ReadIndirectResult)
                    hasIndirectResultRead = true;
            }
        }
        // D-OPT-RELEASE-SYSV-MIXED-CLASS-REG-ARG-DROP: no two Args may share a
        // flat call-operand `position` (arg_payload.hpp). A duplicate is the
        // signature of a payload wipe at a rebuild/merge site (both defaulting
        // to a colliding ordinal), which would make the inliner map two callee
        // params to the same actual. NOT a `position < argCount` check: the
        // x8-sret slot / straddle carrier legitimately consume positions with
        // no Arg, so positions can exceed the Arg count.
        std::unordered_set<std::uint32_t> seenArgPositions;
        for (std::uint32_t bi = 0; bi < nBlocks; ++bi) {
            MirBlockId const b = mir_.funcBlockAt(f, bi);
            std::uint32_t const n = mir_.blockInstCount(b);
            for (std::uint32_t i = 0; i < n; ++i) {
                MirInstId const id = mir_.blockInstAt(b, i);
                MirOpcode const op = mir_.instOpcode(id);
                if (op == MirOpcode::Arg) {
                    std::uint32_t const idx = mir_.argIndex(id);
                    if (idx >= argCount) {
                        reportInst(reporter, DiagnosticCode::I_ArgIndexOutOfRange, id,
                            std::format("argIndex {} >= physical-arg count {} "
                                        "for func #{}",
                                idx, argCount, f.v));
                    }
                    std::uint32_t const pos = mir_.argPosition(id);
                    if (!seenArgPositions.insert(pos).second) {
                        reportInst(reporter, DiagnosticCode::I_ArgPositionDuplicate, id,
                            std::format("two Args share flat call-operand "
                                        "position {} in func #{} — a payload "
                                        "wipe at a rebuild/merge site "
                                        "(D-OPT-RELEASE-SYSV-MIXED-CLASS-REG-"
                                        "ARG-DROP)",
                                pos, f.v));
                    }
                } else if (op == MirOpcode::CondBr) {
                    auto condOps = mir_.instOperands(id);
                    if (!condOps.empty()) {
                        TypeId const ct = mir_.instType(condOps[0]);
                        if (ct.valid() && interner_->kind(ct) != TypeKind::Bool) {
                            reportInst(reporter,
                                DiagnosticCode::I_TerminatorTypeMismatch, id,
                                std::format("(condbr) condition value #{} has "
                                            "type kind {} (expected Bool)",
                                    condOps[0].v,
                                    static_cast<int>(interner_->kind(ct))));
                        }
                    }
                } else if (op == MirOpcode::Return) {
                    auto retOps = mir_.instOperands(id);
                    bool const hasValue  = !retOps.empty();
                    bool const wantValue = returnTy.valid()
                        && interner_->kind(returnTy) != TypeKind::Void;
                    if (hasValue && !wantValue) {
                        reportInst(reporter,
                            DiagnosticCode::I_TerminatorTypeMismatch, id,
                            std::format("(return) has a value but func #{} returns void",
                                f.v));
                    } else if (!hasValue && wantValue && !hasIndirectResultRead) {
                        // x8-sret functions (hasIndirectResultRead) legitimately
                        // return void — the result is written through the indirect-
                        // result register, not returned. Every other non-void func
                        // with a value-less return is a real lowering bug.
                        reportInst(reporter,
                            DiagnosticCode::I_TerminatorTypeMismatch, id,
                            std::format("(return) has no value but func #{} "
                                        "returns a non-void type", f.v));
                    } else if (hasValue && wantValue) {
                        TypeKind const rk = interner_->kind(returnTy);
                        // D-CSUBSET-BITINT-C2-WIDE + D-CSUBSET-UINT128-TYPE: a WIDE
                        // integer return uses the SAME by-value ABI as a struct/union
                        // (2-GPR pieces or an sret pointer) — admit it into the
                        // aggregate-return arm so the Ptr/I64-piece operands are not
                        // mis-flagged against the wide declared return type.
                        // ★ TF-C94: routed through the `isWideInt` FACADE rather than
                        // an inline `kind==BitInt && width>64` test, because this
                        // admit-list MUST agree with `isByValueClass` — that predicate
                        // is what decided to lower the return into pieces in the first
                        // place, and it now returns true for I128/U128. Left as a
                        // BitInt-only test, a 128-bit return would be lowered by
                        // hir_to_mir into ABI pieces and then REJECTED here with a
                        // spurious I_TerminatorTypeMismatch — the verifier contradicting
                        // the lowering. Unreachable today (`__int128` is not yet a
                        // spellable type name — MEASURED: S0006), but it is exactly the
                        // path the front-end half of this anchor opens.
                        bool const wideIntRet = isWideInt(*interner_, returnTy);
                        // C99 _Complex (D-CSUBSET-COMPLEX): a complex return uses the
                        // SAME by-value ABI as a struct/union (register pieces or an
                        // sret pointer) — admit it into the aggregate-return arm so the
                        // Ptr/F64-piece operands are not mis-flagged against the complex
                        // declared return type (the wide-BitInt precedent).
                        if (rk == TypeKind::Struct || rk == TypeKind::Union
                            || rk == TypeKind::Complex || wideIntRet) {
                            // FC7 C1c (D-FC7-SYSV-STRUCT-RETURN-IN-REGS): a by-value
                            // struct/union return is EITHER the first-class aggregate
                            // VALUE (a single operand of the return type — the const-
                            // fold / .dssir-text form, never lowered to LIR as-is) OR
                            // the lowered ABI form: N register PIECES (I64/F64) or an
                            // sret POINTER (>16B). Each operand must be one of those —
                            // a DIFFERENT aggregate type is a real mismatch (and a
                            // single struct-typed value reaching codegen is caught at
                            // the HIR→MIR lowering, which always emits pieces/sret).
                            for (MirInstId const opnd : retOps) {
                                TypeId const vt = mir_.instType(opnd);
                                if (!vt.valid() || vt.v == returnTy.v) continue;
                                TypeKind const vk = interner_->kind(vt);
                                // D-CSUBSET-LONG-DOUBLE-AGGREGATE-ABI (LD-4): a
                                // binary128 HFA piece (F128) is a legitimate
                                // register return piece (AAPCS64, a Q-register), as
                                // is an F80 x87 piece (symmetric; x87 struct
                                // returns are ByReference so this is defensive).
                                if (vk != TypeKind::I64 && vk != TypeKind::F64
                                    && vk != TypeKind::F32 && vk != TypeKind::Ptr
                                    && vk != TypeKind::F80 && vk != TypeKind::F128) {
                                    reportInst(reporter,
                                        DiagnosticCode::I_TerminatorTypeMismatch, id,
                                        std::format("(return) of by-value struct/union "
                                                    "func #{} must carry the aggregate "
                                                    "value, register pieces "
                                                    "(I64/F64/F128), or an sret pointer, "
                                                    "not a kind-{} value (FC7 C1c)",
                                            f.v, static_cast<int>(vk)));
                                }
                            }
                        } else if (retOps.size() != 1) {
                            reportInst(reporter,
                                DiagnosticCode::I_TerminatorTypeMismatch, id,
                                std::format("(return) of scalar func #{} must carry "
                                            "exactly one value, has {}",
                                    f.v, retOps.size()));
                        } else {
                            TypeId const vt = mir_.instType(retOps[0]);
                            if (vt.valid() && vt.v != returnTy.v) {
                                reportInst(reporter,
                                    DiagnosticCode::I_TerminatorTypeMismatch, id,
                                    std::format("(return) value type {} does not "
                                                "match func #{} return type {}",
                                        vt.v, f.v, returnTy.v));
                            }
                        }
                    }
                }
            }
        }
    }
}

void MirVerifier::checkCallSignatures(DiagnosticReporter& reporter) const {
    // TF-C112 (D-MIR-VERIFIER-NO-CALLSITE-SIGNATURE-CHECK). Needs the interner to
    // decode the callee's FnSig; without one (a raw test fixture whose TypeIds are
    // untagged stand-ins) the rule is skipped exactly like the other interner-gated
    // checks. The rule DELIBERATELY mirrors `HirVerifier::checkCallArguments`'s
    // arity convention (variadic ⇒ `>= fixed`, non-variadic ⇒ `==`) so the two
    // tiers cannot disagree about what a well-formed call is.
    if (interner_ == nullptr) return;
    forEachInst(mir_, [&](MirInstId id) {
        if (mir_.instOpcode(id) != MirOpcode::Call) return;
        auto const ops = mir_.instOperands(id);
        // Operand[0] is the callee (mir_opcode.hpp: `operands [callee, args...]`).
        // A callee-less Call is already reported by checkStructuralInvariants
        // (minOperands == 1) — do not double-report it here.
        if (ops.empty()) return;
        MirInstId const callee = ops[0];

        // ── (1) ONLY A STATICALLY RESOLVABLE CALLEE ──────────────────────────
        // An INDIRECT call — through a function-pointer value in a register (a
        // Load of an fnptr variable, an `Arg`, a Phi) — has no static callee and
        // therefore no signature to check against. Skip it CLEANLY: it is a
        // first-class legal shape (D-CSUBSET-FNPTR-INDIRECT-CALL; synth_threads_
        // shim's once-adapter calls its `Arg` directly), never a violation. The
        // GlobalAddr test is the same static-edge test `call_graph_scc` uses.
        if (mir_.instOpcode(callee) != MirOpcode::GlobalAddr) return;
        TypeId const calleeTy = mir_.instType(callee);
        if (!calleeTy.valid()) return;
        // Two shipped spellings reach here and BOTH are legitimate:
        //   * the FnSig DIRECTLY — hir_to_mir's direct-call arm types the callee
        //     GlobalAddr with the HIR Ref's own type (hir_to_mir.cpp, the
        //     `functionSymbols` rvalue arm: `addGlobalAddr(sym, t)`);
        //   * Ptr<FnSig> / FnPtr<FnSig> — `&fn` (FC4 c1) and every MIR-tier
        //     synthesis pass (`addGlobalAddr(sym, interner.pointer(fnSig))`).
        // Anything else (a GlobalAddr of DATA, an opaque/extension callee type)
        // carries no signature: skip, exactly as HirVerifier does.
        TypeId sig = calleeTy;
        if (TypeKind const ck = interner_->kind(sig);
            ck == TypeKind::Ptr || ck == TypeKind::FnPtr) {
            auto const pointee = interner_->operands(sig);
            if (pointee.empty() || !pointee[0].valid()) return;
            sig = pointee[0];
        }
        if (interner_->kind(sig) != TypeKind::FnSig) return;

        auto const   params   = interner_->fnParams(sig);
        TypeId const retTy    = interner_->fnResult(sig);
        bool const   variadic = interner_->fnIsVariadic(sig);
        std::uint32_t const symV = mir_.globalAddrSymbol(callee).v;

        // ── (2) THE RESULT TYPE ──────────────────────────────────────────────
        // P36 (D-MIR-VERIFIER-CALLSITE-RESULT-TYPE-UNCHECKED). TF-C112 landed
        // the OPERAND half and recorded this as a deliberate scope boundary so
        // the ✅ next door would not be over-read. Same defect class, same blind
        // spot shape: a hand-built call in a synthesis pass could take a result
        // of the wrong type and no tier objected.
        //
        // ⚠ IT RUNS BEFORE THE OPERAND GATE BELOW, AND THAT ORDER IS THE WHOLE
        // POINT. The gate asks whether the PHYSICAL OPERAND LIST can be aligned
        // with the semantic parameter list — a question about ARGUMENTS that
        // says nothing about the result. Its very first clause bails on a
        // by-value-class return, so placing the result check after it would
        // silence precisely the struct-return shape the row asks for.
        {
            TypeId const res = mir_.instType(id);
            // A `Void`-typed result is the same claim as NO result. MIR's
            // opcode table makes Call `R::Optional` for exactly this reason
            // ("produces a value iff the callee's return type is non-void —
            // both a valid and an invalid result type are legitimate"), and
            // both spellings of "no value" reach here.
            bool const hasResult =
                res.valid() && interner_->kind(res) != TypeKind::Void;
            // ⚠ AN INVALID `retTy` IS THE ABSENCE OF A RETURN CLAIM, NOT A
            // CLAIM OF `void` — the same distinction `ptr<void>` carries at a
            // pointee, and the reason it is drawn here rather than folded into
            // the void arm: a rule that reported on absence would be accusing
            // on SILENCE. So when the signature declares no return type at all,
            // no result check runs. `checkStructuralInvariants` owns malformed
            // FnSigs; this rule judges only what a signature actually says.
            bool const calleeIsVoid =
                retTy.valid() && interner_->kind(retTy) == TypeKind::Void;
            if (!retTy.valid()) {
                // No claim to check against — see the note above.
            } else if (!hasResult) {
                // ── ARM A: NO RESULT — always legal, and deliberately so. ──
                // Three distinct shipped shapes land here and none is a defect:
                //   * a call whose value is DISCARDED (an expression statement);
                //   * a call to a genuinely void callee;
                //   * a ByReference/sret struct-returning call, which hir_to_mir
                //     emits with `InvalidType` because the callee writes THROUGH
                //     the hidden pointer and the MIR Call is valueless
                //     (emitStructReturningCall's ByReference arm).
                // The tier cannot distinguish the first from a lowering that
                // LOST a needed result, so it must not guess. Stated rather
                // than left implicit, because "no result is always fine" is a
                // real coverage hole and the next reader is owed it.
            } else if (calleeIsVoid) {
                // ── ARM B: A RESULT FROM A VOID CALLEE ──
                // Unambiguous: there is no value to take. This is the arm that
                // catches a synthesis pass wiring a result off a `void` shim.
                reportInst(reporter, DiagnosticCode::I_CallSignatureMismatch, id,
                    std::format("call of symbol #{} (callee fnsig type {}) takes "
                                "a RESULT of type {} (kind {}{}) but the "
                                "signature declares a void return",
                        symV, sig.v, res.v,
                        static_cast<int>(interner_->kind(res)),
                        interner_->vocabularyName(res).empty()
                            ? std::string{}
                            : std::format(" \"{}\"",
                                          interner_->vocabularyName(res))));
            } else if (isByValueClass(*interner_, retTy)) {
                // ── ARM C: A BY-VALUE-CLASS RETURN — PIECE 0 ──
                // An InRegisters struct/union/_Complex/wide-int return is
                // lowered so that THE CALL'S OWN RESULT IS PIECE 0 (measured in
                // emitStructReturningCall: `mir.addInst(Call, …, p0ty, …)`, with
                // pieces 1..N-1 as `ReturnPiece` reads). Piece 0's type is
                // chosen by the TARGET's ABI classifier, which MIR must not know
                // — the agnosticism bar — so the honest check is the same one
                // FC7 C1c already applies to the multi-operand `Return`: the
                // result is either the aggregate VALUE itself (the const-fold /
                // .dssir-text form) or a register PIECE. Reusing
                // `isAbiPieceKind` rather than re-listing the kinds is the point
                // — two lists of "what an ABI piece looks like" would drift.
                //
                // ⓘ HONEST LIMIT, stated because the arm is genuinely weak:
                // this accepts ANY register-piece kind, so a WRONG piece type is
                // invisible here. Narrowing it needs the ABI classification
                // recorded ON the Call (a MIR-tier change), exactly as the
                // operand gate below needs. It is still strictly more than the
                // nothing that preceded it: a struct-returning call taking a
                // `ptr<struct FILE>` or a `struct` of the wrong identity is now
                // named.
                if (!sameSlotType(*interner_, res, retTy)
                    && !isAbiPieceKind(interner_->kind(res))) {
                    reportInst(reporter, DiagnosticCode::I_CallSignatureMismatch,
                        id,
                        std::format("call of symbol #{} (callee fnsig type {}) "
                                    "takes a RESULT of type {} (kind {}{}) but "
                                    "the signature declares a by-value-class "
                                    "return of type {} (kind {}{}) — the result "
                                    "must be the aggregate value or ABI piece 0 "
                                    "(FC7 C1c)",
                            symV, sig.v, res.v,
                            static_cast<int>(interner_->kind(res)),
                            interner_->vocabularyName(res).empty()
                                ? std::string{}
                                : std::format(" \"{}\"",
                                              interner_->vocabularyName(res)),
                            retTy.v, static_cast<int>(interner_->kind(retTy)),
                            interner_->vocabularyName(retTy).empty()
                                ? std::string{}
                                : std::format(" \"{}\"",
                                              interner_->vocabularyName(retTy))));
                }
            } else if (!sameSlotType(*interner_, res, retTy)) {
                // ── ARM D: A SCALAR RETURN — the full notion, nothing looser. ──
                reportInst(reporter, DiagnosticCode::I_CallSignatureMismatch, id,
                    std::format("call of symbol #{} (callee fnsig type {}) takes "
                                "a RESULT of type {} (kind {}{}) but the "
                                "signature declares a return type of {} "
                                "(kind {}{})",
                        symV, sig.v, res.v,
                        static_cast<int>(interner_->kind(res)),
                        interner_->vocabularyName(res).empty()
                            ? std::string{}
                            : std::format(" \"{}\"",
                                          interner_->vocabularyName(res)),
                        retTy.v, static_cast<int>(interner_->kind(retTy)),
                        interner_->vocabularyName(retTy).empty()
                            ? std::string{}
                            : std::format(" \"{}\"",
                                          interner_->vocabularyName(retTy))));
            }
        }

        // ── (3) THE PHYSICAL-vs-SEMANTIC GATE ────────────────────────────────
        // A MIR Call's operand list is the ABI-LOWERED (physical) list, NOT the
        // FnSig's semantic parameter list. hir_to_mir has already expanded the
        // by-value-aggregate shapes, and the expansion factor is not derivable
        // here (it depends on the target's classifier, which MIR must not know —
        // the agnosticism bar). Concretely:
        //   * a by-value-class PARAM (struct/union/_Complex/wide int) becomes
        //     EITHER N register-piece operands, OR one by-reference pointer, OR
        //     one `ByValueStackArg` carrier (hir_to_mir emitByValueStructCallArg);
        //   * a by-value-class RETURN classified ByReference PREPENDS an sret
        //     pointer at operand[1] — and on the SysV/Win64 hidden-arg path that
        //     prepend carries NO marker at all (only the x8 path sets
        //     `call_payload::kIndirectResultBit`), so it cannot be detected.
        // Where either holds, neither the arity nor the positions correspond, so
        // the call is SKIPPED WHOLE. That is a real coverage hole, stated rather
        // than papered over: it is NOT a relaxation of the check, it is the check
        // declining to judge a list it cannot align. Narrowing it needs the ABI
        // classification recorded ON the Call (a MIR-tier change), not a guess.
        if (retTy.valid() && isByValueClass(*interner_, retTy)) return;
        if (::dss::call_payload::hasIndirectResult(mir_.instPayload(id))) return;
        for (TypeId const p : params) {
            // `isMemoryResidentType` == `isByValueClass` ∪ {Array}. Array is
            // included defensively: C decays an array parameter to a pointer so a
            // FnSig should never carry one, but if some future language schema
            // declares one, hir_to_mir's scalar arm would push an operand of an
            // unrelated shape — skipping beats a false accusation.
            if (p.valid() && isMemoryResidentType(*interner_, p)) return;
        }

        // ── (4) ARITY ────────────────────────────────────────────────────────
        // A VARIADIC callee's declared params are the FIXED prefix; positions at
        // and beyond `params.size()` are the vararg region, which the FnSig does
        // not type at all (C's default argument promotions + the platform vararg
        // ABI own them). So `>=` for variadic, `==` otherwise — the HirVerifier
        // convention verbatim.
        std::size_t const nArgs = ops.size() - 1;
        bool const arityBad = variadic ? (nArgs < params.size())
                                       : (nArgs != params.size());
        if (arityBad) {
            reportInst(reporter, DiagnosticCode::I_CallSignatureMismatch, id,
                std::format("call of symbol #{} (callee fnsig type {}) passes {} "
                            "argument operand(s) but the signature declares {} "
                            "{}parameter(s)",
                    symV, sig.v, nArgs, params.size(),
                    variadic ? "fixed " : ""));
            return;   // positions no longer correspond — do not cascade
        }

        // ── (5) PER-POSITION TYPE ────────────────────────────────────────────
        for (std::size_t i = 0; i < params.size(); ++i) {
            TypeId const p = params[i];
            TypeId const a = mir_.instType(ops[i + 1]);
            // Cascade suppression: an operand or parameter with no type is a
            // violation some OTHER rule owns (checkStructuralInvariants' result-
            // type rule) — the whole-verifier convention, matching the Return-
            // value check's `if (vt.valid() && …)`.
            if (!p.valid() || !a.valid()) continue;
            // D-CSUBSET-LONG-DOUBLE-AGGREGATE-ABI (LD-4): an F80 (x87 `long
            // double`) argument is MEMORY-class — hir_to_mir wraps its value in a
            // `ByValueStackArg` carrier, so the operand at an F80 parameter's
            // position is legitimately a `ptr<f80>`, not an `f80`. Exactly ONE
            // operand either way, so the POSITION still corresponds (unlike the
            // aggregate expansions gated above) — only this position's TYPE is
            // unjudgeable.
            if (interner_->kind(p) == TypeKind::F80) continue;
            // ── THE COMPATIBILITY NOTION ─────────────────────────────────
            // ONE notion, shared with the RESULT half above and with
            // `checkStoreValueTypes` — see `sameSlotType` at the top of this
            // file for every arm and the measurement behind it. It used to live
            // here, inline, reachable by nothing else, which is exactly why the
            // other two seams shipped with no type check at all.
            if (sameSlotType(*interner_, a, p)) continue;
            reportInst(reporter, DiagnosticCode::I_CallSignatureMismatch, id,
                std::format("call of symbol #{} (callee fnsig type {}): argument "
                            "at POSITION {} (value #{}) has type {} ({}) "
                            "but the signature declares parameter {} as type {} "
                            "({})",
                    symV, sig.v,
                    i, ops[i + 1].v,
                    a.v, describeType(*interner_, a),
                    i, p.v, describeType(*interner_, p)));
        }
    });
}

void MirVerifier::checkStoreValueTypes(DiagnosticReporter& reporter) const {
    // P36 (D-MIR-VERIFIER-STORE-CALLARG-TYPE-BLIND). The MEMORY-WRITE seam.
    // Needs the interner to read the address's pointee; without one (a raw test
    // fixture whose TypeIds are untagged stand-ins) the rule is skipped exactly
    // like every other interner-gated check.
    //
    // ★ WHY THIS IS THE SAME DEFECT AS THE TWO CALL RULES, not a third one.
    // `checkAtomicAccessLowered` already walks every Store — and reads ONLY the
    // pointee's ATOMIC QUALIFICATION. Nothing anywhere compared the stored
    // VALUE's type against the slot it lands in. So `MirVerifier` green meant
    // "no terminator or operand-count violations", which is far narrower than
    // the "MIR is well-typed" its name implies, and every tier above it read
    // the name. D-CSUBSET-INT128-NARROWING-CAST-SITE-INCOMPLETE is the measured
    // consequence: `return (u64)(r>>64);` was diagnosed loudly
    // (I_TerminatorTypeMismatch) while the STORE-SHAPED and CALL-ARGUMENT forms
    // carried the SAME wrong TypeId with no diagnostic at all — and a hand
    // probe over the one working site read as proof of a contract that held at
    // one site out of seven.
    if (interner_ == nullptr) return;
    forEachInst(mir_, [&](MirInstId id) {
        MirOpcode const op = mir_.instOpcode(id);
        // AtomicStore is included DELIBERATELY. It is the identical seam with
        // the identical operand order ([value, ptr] — mir_opcode.hpp says so in
        // as many words: "the plain-Store operand order, so the funnel is a
        // drop-in for the Store emit"), so excluding it would leave a hole of
        // exactly the shape this rule exists to close, and an unstated one.
        if (op != MirOpcode::Store && op != MirOpcode::AtomicStore) return;
        auto const ops = mir_.instOperands(id);
        // Operand arity is checkStructuralInvariants' rule ([min,max] == [2,2]);
        // do not double-report it here.
        if (ops.size() != 2) return;
        TypeId const valTy  = mir_.instType(ops[0]);
        TypeId const addrTy = mir_.instType(ops[1]);
        // Cascade suppression — the whole-verifier convention.
        if (!valTy.valid() || !addrTy.valid()) return;
        // A non-pointer address is either another rule's violation or a shape
        // this rule cannot read a slot type out of. Either way it is not this
        // rule's to accuse.
        if (interner_->kind(addrTy) != TypeKind::Ptr) return;
        auto const pointeeSpan = interner_->operands(addrTy);
        if (pointeeSpan.size() != 1 || !pointeeSpan[0].valid()) return;
        TypeId const pointee = pointeeSpan[0];

        // ── THE ONE NARROWING, and it is the `void*` arm read one level down ──
        // `ptr<void>` is MIR's canonical spelling for "an ADDRESS whose pointee
        // is unknown or irrelevant" — the ABSENCE of a type claim, not a claim
        // of `void`. A store THROUGH such an address therefore makes no claim
        // this rule could contradict. `sameSlotType`'s ARM 3 already says this
        // for a pointer-typed VALUE; it cannot say it here because the slot is
        // the pointee itself and the value is typically a scalar, so neither
        // side is a pointer and ARM 3 never runs. Stating it at the seam is the
        // honest placement: it is a fact about what a `void` pointee MEANS, not
        // about type compatibility.
        // `kind()` sees through a qualifier skin, so `ptr<volatile void>` is
        // covered without a strip.
        if (interner_->kind(pointee) == TypeKind::Void) return;

        if (sameSlotType(*interner_, valTy, pointee)) return;

        // ── THE SECOND NARROWING: A TIER-DECLARED REPRESENTATION IDENTITY ────
        // ⚠ THIS IS A NARROWING WITH A MEASURED REASON, NOT A RULE TUNED UNTIL
        // THE CORPUS WENT GREEN — the distinction matters, so here is the
        // evidence and the line it is drawn on.
        //
        // ✔MEASURED (P36, 15 corpus examples x 2 configs, 113 diagnostics): 16 of
        // them are `store <i32> into ptr<enum "Color">` and
        // `store <u8|i8|u64> into ptr<_BitInt(N)>`. Both come from the BIT-FIELD
        // path, where hir_to_mir types the allocation-unit ADDRESS with the
        // member's DECLARED type while `emitBitfieldInsert`/`emitBitfieldExtract`
        // reassign the value's type to the CONTAINER on their first line
        // (`bitIntReprType(enumReprType(fieldTy))`).
        //
        // ★ WHY THAT IS NOT AN ILL-TYPED STORE, WHILE THE OTHER TWO CLASSES THIS
        // RULE FOUND ARE. An enum's underlying type IS the container: the
        // interner stores it, and `core_type.hpp` says an Enum has "distinct
        // nominal identity … but int-compatible at all arithmetic / cast sites".
        // A memory slot is such a site. Width, signedness and every bit agree, so
        // NOTHING a later consumer does can differ — which is exactly the test
        // that FAILS for the two classes left rejected: `i64` vs `u64` agree on
        // width and bits but NOT on signedness, and signedness changes what a
        // later division, shift, comparison or widening does. That is the line,
        // and it is a property of the types rather than of the corpus.
        //
        // ★★ IT READS THE MAPPING, IT DOES NOT RESTATE IT. The container comes
        // from `scalars(enum)[0]` and `bitIntContainerKind` — the interner's own
        // declarations, the SAME ones `enumReprType`/`bitIntReprType` read. There
        // is no name, size or signedness table here; a language that declares a
        // different underlying type gets a different answer with no edit, which is
        // the agnosticism bar. A hardcoded "Enum ~ I32" would have been the break.
        //
        // ⓘ SEAM-SCOPED ON PURPOSE — it is deliberately NOT in `sameSlotType`.
        // A memory slot is where REPRESENTATION governs (the object's storage). A
        // call ARGUMENT is a value conversion the front end must materialize, and
        // the corpus is green with the call seam rejecting this class today, so
        // widening the shared notion would loosen two seams to fix one.
        {
            TypeKind const slotKind = interner_->kind(pointee);
            TypeKind container      = TypeKind::Void;   // Void = "no declaration"
            if (slotKind == TypeKind::Enum) {
                auto const sc = interner_->scalars(pointee);
                if (!sc.empty()) container = static_cast<TypeKind>(sc[0]);
            } else if (slotKind == TypeKind::BitInt) {
                // Returns Void for N>64 (the fail-loud sentinel); a multi-limb
                // `_BitInt` has no single container and is left judged.
                container = interner_->bitIntContainerKind(pointee);
            }
            if (container != TypeKind::Void
                && interner_->kind(valTy) == container) {
                return;
            }
        }

        reportInst(reporter, DiagnosticCode::I_StoreValueTypeMismatch, id,
            std::format("`{}` of value #{} typed {} ({}) into an address "
                        "whose pointee is {} ({}) — a stored value must "
                        "have its slot's type; MIR is POST-conversion, so a "
                        "narrowing/widening/retagging conversion must already "
                        "have been materialized as a Cast",
                mnemonic(op),
                ops[0].v,
                valTy.v, describeType(*interner_, valTy),
                pointee.v, describeType(*interner_, pointee)));
    });
}

void MirVerifier::checkAtomicAccessLowered(DiagnosticReporter& reporter) const {
    // FC17.9(d) cycle 1b (D-CSUBSET-ATOMIC): the atomic-lowering belt. Needs the
    // interner to decode `isAtomicQualified`; without one (a raw test fixture) the
    // rule is skipped exactly like the other interner-gated checks.
    if (interner_ == nullptr) return;
    forEachInst(mir_, [&](MirInstId id) {
        MirOpcode const op = mir_.instOpcode(id);
        if (op == MirOpcode::Load) {
            // A Load's ACCESSED type IS its result type. Atomic ⇒ it should have
            // funnelled to AtomicLoad; a surviving plain Load is a missed site.
            TypeId const acc = mir_.instType(id);
            if (acc.valid() && interner_->isAtomicQualified(acc)) {
                reportInst(reporter, DiagnosticCode::I_AtomicAccessNotLowered, id,
                    "plain `load` of an _Atomic-qualified type — must lower to "
                    "`atomic_load` (a missed FC17.9(d) scalar-access funnel site; "
                    "a plain load would silently perform a NON-atomic read)");
            }
        } else if (op == MirOpcode::Store) {
            // Object-INITIALIZATION stores are the ONE exemption (C11 7.17.2.1 —
            // atomic initialization is not itself atomic); they stay plain by
            // design and carry AtomicInitExempt.
            if (has(mir_.instFlags(id), MirInstFlags::AtomicInitExempt)) return;
            // A Store has NO result type; its ACCESSED type is the POINTEE of its
            // address operand. Store operand order is [value, ptr] (mir_opcode.hpp),
            // so operand[1] is the address.
            auto const ops = mir_.instOperands(id);
            if (ops.size() != 2) return;
            TypeId const addrTy = mir_.instType(ops[1]);
            if (!addrTy.valid() || interner_->kind(addrTy) != TypeKind::Ptr) return;
            auto const pointee = interner_->operands(addrTy);
            if (pointee.empty() || !pointee[0].valid()) return;
            if (interner_->isAtomicQualified(pointee[0])) {
                reportInst(reporter, DiagnosticCode::I_AtomicAccessNotLowered, id,
                    "plain `store` to an _Atomic-qualified pointee — must lower to "
                    "`atomic_store` (a missed FC17.9(d) scalar-access funnel site; "
                    "a plain store would silently perform a NON-atomic write). An "
                    "initialization store must carry MirInstFlags::AtomicInitExempt");
            }
        }
    });
}

} // namespace dss
