#include "asm/asm.hpp"

#include "asm/format/byte_emit.hpp"
#include "asm/format/fixed32.hpp"
#include "asm/format/walker_util.hpp"
#include "asm/format/x86_variable.hpp"
#include "core/types/bit_int_value.hpp"   // the ONE `_BitInt` padding policy (BitIntValue::paddingByte)
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_layout.hpp"   // computeLayout, scalarByteSize
#include "lir/lir_pass_util.hpp"

#include <algorithm>  // D-CSUBSET-LONG-BRANCH: sort / unique / binary_search over the promoted set
#include <bit>
#include <cmath>     // D-MIR-OVERLAP-STRUCT-ZERO-INIT: std::signbit (rejects -0.0)
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace dss {

namespace {

// Diagnostic-emit shorthand, same convention as ML6/ML7.
using dss::report;

// Dispatch shell for a single LIR instruction's byte encoding. Cycle 1
// substrate has no format walkers registered yet — every shape returns
// the appropriate "fail-loud" diagnostic code. AS2 will add the
// `X86Variable` arm (consuming the schema's `encoding.variants` rows);
// AS3 will add the `Fixed32` arm.
//
// Returns true iff the instruction was successfully encoded (bytes
// appended to `out`, relocations added, source-map entry appended).
// Returns false iff the instruction failed encoding — the caller is
// responsible for proceeding without aborting the whole function (the
// parallel-index discipline keeps the function slot alive even on
// per-inst failure).
[[nodiscard]] bool encodeInst(Lir const&              lir,
                              TargetSchema const&     schema,
                              LirInstId               inst,
                              std::vector<std::uint8_t>& out,
                              std::vector<Relocation>&   relocs,
                              std::vector<SourceMapEntry>& srcMap,
                              std::vector<walker_util::BlockRelPatch>& blockPatches,
                              std::vector<walker_util::BlockSymPatch>& blockSymPatches,
                              std::span<MirInstId const> lirToMir,
                              // D-CSUBSET-LONG-BRANCH: the sorted set of
                              // `LirInstId.v` this function's relaxation
                              // fixed point has promoted to their escape
                              // form. Empty on pass 0 of every function.
                              std::span<std::uint32_t const> relaxedInsts,
                              DiagnosticReporter&     reporter) {
    auto const opcode = lir.instOpcode(inst);
    auto const* info  = schema.opcodeInfo(opcode);

    // Unknown opcode — defensive; `addInst` in the LIR builder already
    // rejects opcode 0 and the post-regalloc verifier checks the
    // operand vs schema-arity. Surface here as the substrate's
    // boundary check so a malformed `Lir` (e.g. hand-constructed in a
    // test) fails loud rather than dereferencing nullptr below.
    if (info == nullptr) {
        report(reporter, DiagnosticCode::A_NoEncodingDeclared,
               DiagnosticSeverity::Error,
               std::format("opcode {} is not declared in target schema '{}'",
                           opcode, schema.name()));
        return false;
    }

    // ★★ AN EARLY-CLOBBER RESULT THAT IS ALSO AN OPERAND HAS NO ENCODING THAT
    // IS A PROGRAM (P68 round 8, `TargetOpcodeInfo::resultEarlyClobber`), and
    // this is the one place every encoder passes, so no format can emit one.
    // The allocator never produces the overlap (the builder marks the result
    // early); what reaches here is a register the programmer WROTE, or an
    // inline-asm output the allocator was free to share because it carried no
    // `&`. Identity is the physical register's — `wzr` and `sp` encode the same
    // field and are two registers — so only a TRUE overlap is refused.
    if (info->resultEarlyClobber) {
        LirReg const result = lir.instResult(inst);
        if (result.valid() && result.isPhysical) {
            for (LirOperand const& op : lir.instOperands(inst)) {
                if (op.kind != LirOperandKind::Reg || !op.reg.valid()
                    || !op.reg.isPhysical || op.reg.id != result.id) {
                    continue;
                }
                auto const* reg = schema.registerInfo(
                    static_cast<std::uint16_t>(result.id));
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format(
                           "opcode '{}': its result register '{}' is also one of "
                           "the registers it reads, and target '{}' declares that "
                           "this instruction's result must differ from every "
                           "register operand (the architecture makes the "
                           "overlapping encoding UNPREDICTABLE, and clang refuses "
                           "it) — write a different register, or give an "
                           "inline-asm output written here the early-clobber "
                           "constraint (`=&r`) so it is never shared with an input",
                           info->mnemonic,
                           reg != nullptr ? std::string_view{reg->name}
                                          : std::string_view{"?"},
                           schema.name()));
                return false;
            }
        }
    }

    // SourceMapEntry stamping (plan 13 AS6). Capture the byte
    // offset BEFORE any encoding write so the entry points at the
    // instruction's first byte. We capture the pre-encode byte
    // position now, stamp AFTER the walker succeeds (so a walker
    // failure doesn't leave behind a dangling entry pointing at
    // bytes that were never written). `assemble()`'s entry-time
    // bounds check guarantees `lirToMir.size() == lir.instCount()`,
    // so `inst.v` is always in range here.
    std::uint32_t const preEncodeOffset =
        static_cast<std::uint32_t>(out.size());

    bool const encoded = [&]() -> bool {
        switch (info->encoding.shape) {
        case TargetEncodingShape::None:
            report(reporter, DiagnosticCode::A_NoEncodingDeclared,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}' has no encoding declared "
                               "in target schema '{}'",
                               info->mnemonic, schema.name()));
            return false;

        case TargetEncodingShape::X86Variable:
            return x86_variable::encode(lir, schema, inst, info,
                                         lirToMir, out, relocs, srcMap,
                                         blockPatches, blockSymPatches,
                                         reporter);

        case TargetEncodingShape::Fixed32:
            return fixed32::encode(lir, schema, inst, info, lirToMir,
                                    out, relocs, srcMap, blockPatches,
                                    blockSymPatches, relaxedInsts, reporter);
        }

        // Enum-drift fallback. A new `TargetEncodingShape` value
        // added without a matching switch arm would otherwise
        // silently `return false` with no diagnostic — a future
        // silent-skip the silent-failure review specifically
        // called out. Surface it.
        report(reporter, DiagnosticCode::A_NoEncodingShapeWalker,
               DiagnosticSeverity::Error,
               std::format("opcode '{}': unknown encoding-shape ordinal {} "
                           "(internal-invariant: a new TargetEncodingShape "
                           "value was added without updating the assembler "
                           "dispatch)",
                           info->mnemonic,
                           static_cast<int>(info->encoding.shape)));
        return false;
    }();

    // Plan 13 AS6: stamp SourceMapEntry IFF the walker actually
    // wrote bytes. A walker that returned false (no matching
    // variant, malformed input, etc.) has not advanced `out`, so
    // a stamp here would point at the NEXT instruction's bytes
    // instead — silently corrupting the source-map.
    //
    // A walker that returns true MUST write at least one byte —
    // that's the encoder contract for the assembler tier. A
    // walker-returns-true-with-zero-bytes case is a hard substrate
    // invariant violation; surface it loudly (multi-agent review
    // convergence: silent-failure + code-reviewer + architect).
    // Without this gate, a future regression in any walker that
    // claims success without emission would desynchronize the
    // parallel-index `srcMap.size() == encodedInsts` invariant the
    // round-trip oracle relies on (test_asm_roundtrip.cpp).
    if (encoded) {
        if (out.size() <= preEncodeOffset) {
            report(reporter, DiagnosticCode::A_NoEncodingShapeWalker,
                   DiagnosticSeverity::Error,
                   std::format("opcode '{}': walker reported success but "
                               "emitted zero bytes (substrate-invariant "
                               "violation — every encoded instruction must "
                               "produce at least one output byte)",
                               info->mnemonic));
            return false;
        }
        srcMap.push_back(SourceMapEntry{
            preEncodeOffset,
            lirToMir[inst.v],
            // The LIR instruction these bytes came from. Always valid (unlike
            // the MIR anchor beside it) -- this is the join key an unwind
            // producer uses to turn "the instruction that establishes the
            // frame" into a measured byte offset.
            inst
        });
    }
    return encoded;
}

} // namespace

AssembledModule assemble(Lir const&                 lir,
                         TargetSchema const&        schema,
                         std::span<MirInstId const> lirToMir,
                         DiagnosticReporter&        reporter,
                         std::span<ExternImport const> externs) {
    AssembledModule result;
    // Copy extern descriptors verbatim so the linker can consume
    // them. The assembler itself does not validate the contents
    // (per-extern non-empty `mangledName` + `libraryPath` checks
    // live on the linker side); the upstream HIR→MIR pre-pass
    // (`collectExterns` in `hir_to_mir.cpp`) is the canonical
    // source of these rows when threading from real source
    // declarations (LK6 cycle 2d — D-LK6-6 closure).
    result.externImports.assign(externs.begin(), externs.end());
    // D-C-GNU-CONSTRUCTOR-ATTRIBUTE-IS-WARNED-AND-IGNORED-NOT-RUN: the module's
    // static-initializer schedule crosses the LIR→object boundary verbatim. No
    // filtering here on purpose — `staticInitAdd` already refused the empty and
    // symbol-less entries, and deciding WHICH entries survive is a whole-program
    // question the linker answers with every module in hand.
    auto const schedule = lir.staticInitSchedule();
    result.staticInitSchedule.assign(schedule.begin(), schedule.end());
    std::size_t const funcCount = lir.moduleFuncCount();
    result.expectedFuncCount = funcCount;

    // Empty-in is empty-out without error: a default-constructed `Lir`
    // legitimately produces a zero-function module (e.g. a declaration-only
    // TU that emitted no top-level function definitions). This is a VALID
    // success — `ok()` returns true (0 == 0) and the module lowers to a valid
    // empty relocatable object (D-CSUBSET-TESTTU-SILENT-EXIT1). Callers that
    // specifically require a NON-EMPTY module must check `!functions.empty()`.
    if (funcCount == 0) {
        return result;
    }

    // Source-map contract: `lirToMir[LirInstId.v]` is read once AS2/
    // AS3 wire the per-inst `MirInstId` stamping. A shorter span
    // would silently UB. Empty span is allowed only when the LIR
    // module is itself empty of instructions (e.g. a CU with no
    // function bodies); compare against the inst-arena size so the
    // contract is precise at entry.
    if (lirToMir.size() != lir.instCount()) {
        report(reporter, DiagnosticCode::A_LirToMirSizeMismatch,
               DiagnosticSeverity::Error,
               std::format("lirToMir.size() = {} does not match "
                           "lir.instCount() = {} for target '{}'",
                           lirToMir.size(), lir.instCount(),
                           schema.name()));
        // Returning with `expectedFuncCount > 0` but
        // `functions.empty()` makes `ok()` return false — the parallel-
        // index discipline is broken on purpose so the caller sees
        // the shape failure (in addition to the diagnostic).
        return result;
    }

    result.functions.resize(funcCount);

    for (std::uint32_t fi = 0; fi < funcCount; ++fi) {
        LirFuncId const fn       = lir.funcAt(fi);
        AssembledFunction& outFn = result.functions[fi];
        // Carry the originating symbol forward so the linker can
        // place this function's bytes without re-consulting the Lir.
        outFn.symbol = lir.funcSymbol(fn);

        std::uint32_t const blockCount = lir.funcBlockCount(fn);

        // ─────────────────────────────────────────────────────────────
        // D-CSUBSET-LONG-BRANCH — BRANCH RELAXATION AS A FIXED POINT
        // ─────────────────────────────────────────────────────────────
        //
        // ★★★ A SINGLE PATCHING PASS CANNOT BE RIGHT, AND THE ROW SAID SO
        // BEFORE THE CODE DID. An intra-function branch whose displacement
        // leaves its field's reach is rescued by an ESCAPE — real extra
        // instructions the encoder emits. Emitting them changes the
        // function's byte layout, so every block offset captured before
        // them is stale, so the decision cannot be made by the resolver
        // after the block-offset table is built. Worse, the growth is not
        // local: widening one branch pushes the spans that CONTAIN it
        // further apart, and a branch that was exactly in reach falls out
        // of it. That is why this is a LOOP and not a fix-up.
        //
        // ★★★ WHY IT TERMINATES, AND WHY THE BOUND IS A BACKSTOP RATHER
        // THAN THE ARGUMENT. `relaxedInsts` is MONOTONE: an instruction is
        // promoted to its escape form and never demoted, and the loop only
        // takes another pass when the pass just finished promoted at least
        // one instruction that was not already in the set. So the set
        // strictly grows every iteration, and it is bounded above by the
        // number of instructions in the function. The iteration count is
        // therefore at most `instCount + 1` by construction, with no appeal
        // to displacements shrinking or to any convergence property of the
        // layout. `relaxBound` re-states that same number and refuses
        // LOUDLY if it is ever reached — because a monotonicity argument
        // that is true of the code today is not a guarantee about the code
        // tomorrow, and an assembler that spins is worse than one that
        // refuses. There is no recursion here and no input-proportional
        // stack: one `for`, one explicit vector.
        //
        // ⚠ THE COMMON PATH IS EXACTLY ONE PASS. A function whose branches
        // all fit promotes nothing, so `relaxedInsts` stays empty, so the
        // encode is the encode that ran before this loop existed and the
        // bytes are identical to the byte. The scan below is arithmetic
        // over the patch list; it writes nothing and reports nothing.
        std::vector<std::uint32_t> relaxedInsts;
        std::uint32_t const relaxBound = [&] {
            std::uint32_t n = 1;
            for (std::uint32_t bi = 0; bi < blockCount; ++bi)
                n += lir.blockInstCount(lir.funcBlockAt(fn, bi));
            return n;
        }();

        // ─────────────────────────────────────────────────────────────
        // D-CSUBSET-LONG-BRANCH — THE SECOND MONOTONE SET: BRANCH ISLANDS
        // ─────────────────────────────────────────────────────────────
        //
        // ★★★ THE WIDEST FIELD NEEDS A NEARER TARGET, NOT A WIDER FIELD.
        // The escape above rescues a branch by MOVING IT INTO A WIDER FIELD,
        // which is why the widest field has no escape and why the row this
        // anchor names concluded the residue needed an absolute address — a
        // multi-word veneer, a relocation, a synthetic symbol the assembler
        // cannot mint. It does not. The residue is INTRA-FUNCTION: every byte
        // offset in this function is known right here, so aiming the SAME
        // field at a nearer point of the SAME function is the same
        // subtraction. That nearer point is an ISLAND holding the branch
        // again — `B island` / `island: B far`, the same field twice, chained
        // as far as the distance demands. It is what ld64 does for AArch64
        // text over 128 MiB, and it needs NO scratch register, which is what
        // makes it available to an assembler running after register
        // allocation with no liveness to consult.
        //
        // ★★★ WHAT IS MONOTONE, AND WHAT IS RE-DERIVED EVERY PASS. A SITE
        // ("after LIR instruction I, place a landing pad for block T") is
        // monotone: once requested it is never withdrawn, and its identity is
        // a LIR instruction id, which survives a re-layout exactly as
        // `relaxedInsts`'s members do. Everything positional — where the
        // cluster landed, which islands exist at which byte offsets — is
        // re-derived from scratch on every pass, because byte offsets do not
        // survive relaxation. That split is the same one the promoted set
        // already makes, which is why islands ride this loop rather than
        // needing one of their own.
        //
        // ★★★ THE ISLAND BOUND, AS ARITHMETIC.
        //   Let R be the field's byte reach and S = R/2 its placement STRIDE
        //   (`blockRelIslandStride`; the half is what leaves a placed island
        //   slack against later layout growth). The resolver only ever aims at
        //   an island STRICTLY CLOSER to the target than the site it is
        //   standing on, and it only requests one S bytes further along, so
        //   every hop of a chain advances at least S bytes and no target is
        //   further away than the function's own size N. One branch's chain
        //   therefore holds at most ceil(N / S) islands, and over the
        //   function's B block-relative branches:
        //
        //       islandBound = B * (ceil(N / S) + 1)
        //
        //   The `+ 1` is SLACK, not arithmetic: the bound is re-derived each
        //   pass from a layout the previous pass just grew, and without the
        //   cushion it could refuse a placement it had already justified. The
        //   argument is `B * ceil(N / S)`; the cushion is the `+ 1`, and which
        //   is which is said rather than left for a reader to reconcile
        //   against the code.
        //
        //   `islandCap` below restates exactly that and refuses LOUDLY if it
        //   is ever exceeded, for the same reason `relaxBound` does: a
        //   termination argument true of today's code is not a guarantee
        //   about tomorrow's, and an assembler that spins is worse than one
        //   that refuses. The chain cannot cycle either — each hop strictly
        //   decreases a non-negative integer (the distance to the target) —
        //   but that argument, too, is backed by a counter rather than
        //   trusted. One `for`, explicit vectors: no recursion anywhere.
        struct IslandSite {
            std::uint32_t                 afterInstV;   // emitted after this inst
            std::uint32_t                 targetBlock;  // where it branches
            walker_util::BranchIslandBody body;         // what it is made of
        };
        auto const siteKeyLess = [](IslandSite const& a, IslandSite const& b) {
            if (a.afterInstV != b.afterInstV) return a.afterInstV < b.afterInstV;
            if (a.targetBlock != b.targetBlock) return a.targetBlock < b.targetBlock;
            return static_cast<std::uint8_t>(a.body.kind)
                 < static_cast<std::uint8_t>(b.body.kind);
        };
        std::vector<IslandSite> islandSites;

        // ⓘ THE LOOP BODY IS DELIBERATELY NOT RE-INDENTED, and the closing
        // brace below says so again. Indenting the ~280 lines this loop now
        // wraps would have produced a whitespace-only diff large enough to
        // hide the six lines that actually changed — and the block it wraps
        // is the pre-existing per-function encode, unchanged except where
        // this comment's anchor is named.
        for (std::uint32_t relaxPass = 0; ; ++relaxPass) {
        // Every pass re-encodes the function FROM SCRATCH. Nothing survives
        // a pass except `relaxedInsts` — carrying stale bytes, relocations,
        // source-map entries or block symbols into a re-layout is precisely
        // the partial-output failure `D-ASM-PATCH-PARTIAL-OUTPUT-FAILLOUD`
        // exists to prevent, one tier up.
        outFn.bytes.clear();
        outFn.relocations.clear();
        outFn.sourceMap.clear();
        outFn.blockSymbols.clear();
        outFn.blockByteOffsets.clear();

        // D-CSUBSET-WHILE-LOOP-SUBSTRATE (step 13.5 cycle 1):
        // intra-function block-relative branch patching. Build the
        // block-offset table while emitting block-by-block, then
        // resolve patches after the function is fully assembled.
        // Distinct from the symbol-relative `outFn.relocations` —
        // those go to the linker; these resolve at assemble time
        // and never leak past this function.
        std::unordered_map<std::uint32_t, std::uint32_t> blockOffsets;
        blockOffsets.reserve(blockCount);
        std::vector<walker_util::BlockRelPatch> blockPatches;
        // D-CSUBSET-COMPUTED-GOTO: synthetic-symbol ↔ block bindings a
        // block-address `lea` accumulates (its trailing BlockRef). Resolved
        // into `outFn.blockSymbols` once `blockOffsets` is complete (after
        // the funcEncodeOk check), mirroring the `blockPatches` discipline.
        std::vector<walker_util::BlockSymPatch> blockSymPatches;

        // D-CSUBSET-LONG-BRANCH: the POSITIONAL half of the island machinery,
        // rebuilt from scratch every pass because byte offsets do not survive
        // a re-layout. `instEnds` is the offset→instruction index the site
        // request needs (it is built in emit order, so it is sorted by offset
        // by construction); `emittedIslands` is where this pass's landing pads
        // actually landed, which is what an out-of-reach patch aims at.
        struct EmittedIsland {
            std::uint32_t                  bodyStart;    // first byte of the pad
            std::uint32_t                  fieldOffset;  // its field's patch site
            std::uint32_t                  targetBlock;
            walker_util::BlockRelPatchKind kind;
        };
        std::vector<EmittedIsland> emittedIslands;
        struct InstEnd { std::uint32_t endOffset; std::uint32_t instV; };
        std::vector<InstEnd> instEnds;
        instEnds.reserve(relaxBound);

        // D-ASM-ENCODE-FAILURE-FUNCTION-ROLLBACK (step 13.5 cycle 1
        // post-fold, silent-failure-hunter CRITICAL #2): track
        // per-inst encode failures. Continue past failures (so the
        // user sees ALL per-inst diagnostics in one compile pass —
        // the parallel-index-discipline invariant: every unencoded
        // inst surfaces its own diagnostic) BUT truncate any
        // partial bytes the failing encoder may have emitted, so
        // subsequent block-offset captures and intra-function
        // branch patches don't read partial-byte tails. After all
        // insts are encoded, if ANY failed, drop the entire
        // function's bytes from the AssembledModule — a function
        // with one wrong byte cannot ship correctly.
        bool funcEncodeOk = true;
        for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
            LirBlockId const blk = lir.funcBlockAt(fn, bi);
            blockOffsets[blk.v] =
                static_cast<std::uint32_t>(outFn.bytes.size());
            std::uint32_t const instCount = lir.blockInstCount(blk);
            for (std::uint32_t ii = 0; ii < instCount; ++ii) {
                LirInstId const inst = lir.blockInstAt(blk, ii);
                std::size_t const preInstByteCount = outFn.bytes.size();
                std::size_t const prePatchCount = blockPatches.size();
                bool const ok = encodeInst(lir, schema, inst,
                                 outFn.bytes, outFn.relocations,
                                 outFn.sourceMap, blockPatches,
                                 blockSymPatches, lirToMir,
                                 relaxedInsts, reporter);
                // D-CSUBSET-LONG-BRANCH: stamp the originating instruction
                // on every patch this instruction just appended. Done HERE,
                // once, rather than in each walker: the promoted set is
                // keyed on instruction identity because byte offsets do not
                // survive a re-layout, and a walker that forgot to stamp
                // would silently key the whole fixed point on instruction 0.
                for (std::size_t pi = prePatchCount; pi < blockPatches.size(); ++pi)
                    blockPatches[pi].instV = inst.v;
                if (!ok) {
                    outFn.bytes.resize(preInstByteCount);
                    funcEncodeOk = false;
                    continue;
                }
                instEnds.push_back(InstEnd{
                    static_cast<std::uint32_t>(outFn.bytes.size()), inst.v});
                // ── D-CSUBSET-LONG-BRANCH: THE ISLAND CLUSTER ────────────
                //
                // Every site requested after THIS instruction is materialized
                // here, as one cluster:
                //
                //     B over        <- the jump-over, same declared body
                //     B far         <- island, one per (target, field) site
                //     B far'
                //   over:           <- the next instruction, untouched
                //
                // ⚠ THE JUMP-OVER IS NOT OPTIONAL AND IT IS NOT A DETAIL.
                // A landing pad is reached ONLY by a branch aimed at it; the
                // instruction before it has no idea it is there. Without the
                // jump-over the instruction stream falls straight into the
                // first pad and takes a branch nobody asked for — valid
                // bytes, wrong destination, no diagnostic. Its displacement
                // is the cluster's own size, which is known HERE, at emit
                // time, so it is written here rather than queued as a patch:
                // a patch names a target BLOCK, and the landing point of a
                // jump-over is a byte offset that belongs to no block.
                //
                // ⓘ A CLUSTER CARRIES NO SOURCE-MAP ENTRY, DELIBERATELY. The
                // CFI producer derives each unwind range from consecutive
                // `sourceMap` byte offsets, so an unmapped cluster extends the
                // PRECEDING instruction's range over it — which is the correct
                // description, because a branch changes no unwind state: it
                // touches neither the stack pointer nor a callee-saved
                // register. Attributing these bytes to a LIR instruction that
                // did not emit them would be the false claim.
                if (islandSites.empty()) continue;
                auto const siteLo = std::lower_bound(
                    islandSites.begin(), islandSites.end(), inst.v,
                    [](IslandSite const& s, std::uint32_t v) {
                        return s.afterInstV < v;
                    });
                auto siteHi = siteLo;
                while (siteHi != islandSites.end()
                       && siteHi->afterInstV == inst.v)
                    ++siteHi;
                if (siteLo == siteHi) continue;
                std::size_t padBytes = 0;
                for (auto it = siteLo; it != siteHi; ++it)
                    padBytes += it->body.byteCount;
                auto const appendBody =
                    [&](walker_util::BranchIslandBody const& b) {
                        for (std::uint8_t k = 0; k < b.byteCount; ++k)
                            outFn.bytes.push_back(b.bytes[k]);
                    };
                auto const& over = siteLo->body;
                auto const overStart =
                    static_cast<std::uint32_t>(outFn.bytes.size());
                appendBody(over);
                {
                    auto const g =
                        walker_util::blockRelFieldGeometry(over.kind);
                    auto const field = overStart + over.fieldOffset;
                    std::int64_t const landing =
                        static_cast<std::int64_t>(overStart)
                      + over.byteCount + static_cast<std::int64_t>(padBytes);
                    std::int64_t const hop =
                        (landing - (static_cast<std::int64_t>(field) + g.pcBias))
                            >> g.scaleLog2;
                    // ⚠ A CLUSTER BIG ENOUGH TO OUTRUN ITS OWN JUMP-OVER IS A
                    // REFUSAL, NOT A MASKED WRITE. The field write below
                    // truncates to the field's width by construction, so an
                    // unrepresentable hop would silently land somewhere inside
                    // the cluster and execute a branch nobody asked for. It
                    // cannot happen while the island bound holds — that is the
                    // point of checking it rather than assuming it.
                    if (hop < walker_util::blockRelFieldMin(g)
                     || hop > walker_util::blockRelFieldMax(g)) {
                        report(reporter, DiagnosticCode::A_FunctionEncodeAborted,
                               DiagnosticSeverity::Error,
                               std::format("function symbol id {} dropped — a "
                                           "branch-island cluster of {} byte(s) "
                                           "is larger than its own jump-over's "
                                           "field can span, so control could "
                                           "not be carried past it "
                                           "(D-CSUBSET-LONG-BRANCH)",
                                           outFn.symbol.v,
                                           over.byteCount + padBytes));
                        funcEncodeOk = false;
                        continue;
                    }
                    walker_util::writeBlockRelField(outFn.bytes, field, g, hop);
                }
                for (auto it = siteLo; it != siteHi; ++it) {
                    auto const start =
                        static_cast<std::uint32_t>(outFn.bytes.size());
                    appendBody(it->body);
                    // The pad's OWN branch is an ordinary block-relative
                    // patch, which is what makes a CHAIN free: if this pad
                    // cannot reach the target either, the scan below asks for
                    // one nearer, and this one aims at that.
                    blockPatches.push_back(walker_util::BlockRelPatch{
                        start + it->body.fieldOffset,
                        it->targetBlock,
                        it->body.kind,
                        /*relaxable=*/false,
                        /*widerFieldDeclared=*/false,
                        /*instV=*/it->afterInstV,
                        it->body});
                    emittedIslands.push_back(EmittedIsland{
                        start, it->body.fieldOffset, it->targetBlock,
                        it->body.kind});
                }
            }
        }

        if (!funcEncodeOk) {
            // The per-inst diagnostic above already reported the
            // root cause; emit a function-level summary so the
            // user knows WHICH function got dropped. Uses the
            // distinct `A_FunctionEncodeAborted` code so unit-test
            // invariants counting per-inst-failure codes (e.g.
            // EveryUnencodedInstFiresNoEncodingDiagnostic) don't
            // double-count this function-level wrapper.
            report(reporter, DiagnosticCode::A_FunctionEncodeAborted,
                   DiagnosticSeverity::Error,
                   std::format("function symbol id {} dropped from "
                               "AssembledModule — at least one "
                               "instruction failed to encode (see "
                               "preceding diagnostic); "
                               "D-ASM-ENCODE-FAILURE-FUNCTION-ROLLBACK "
                               "preserves byte-offset integrity by "
                               "aborting the function on first per-inst "
                               "failure",
                               outFn.symbol.v));
            // Clear the function's bytes/relocs entirely so the
            // partial output cannot leak past assemble().
            outFn.bytes.clear();
            outFn.relocations.clear();
            outFn.sourceMap.clear();
            break;  // leave the relaxation loop — this function is dropped
        }

        // D-OPT-SWITCH-JUMP-TABLE (c70): publish the completed block-byte-offset
        // table on the AssembledFunction. A dense `switch` lowers to a jump table
        // whose `.data` slots hold the runtime addresses of the case-target blocks
        // (abs64 relocations to synthetic per-block symbols). Those block symbols
        // have no live block-address `lea`, so the BlockSymPatch loop below never
        // binds them — `compile_pipeline.cpp` binds them directly from THIS map
        // after assemble() returns. Copied once per function (cheap), only
        // consumed when a jump-table descriptor names this function.
        outFn.blockByteOffsets = blockOffsets;

        // D-CSUBSET-COMPUTED-GOTO: resolve each pending synthetic-symbol
        // ↔ block binding now that every block's byte offset is known.
        // Each binds a synthetic local symbol (the `&&label` block-address
        // `lea`'s relocation source) to its target block's byte offset
        // within THIS function; the linker turns each into an interior-
        // block VA. A target block id absent from `blockOffsets` is
        // malformed LIR (the BlockRef survived the LIR passes but names no
        // emitted block) — fail loud, mirroring the blockPatches missing-
        // target guard below. Unlike `blockPatches`, this binds a SYMBOL,
        // not a code site: there is no in-function byte to patch (the
        // linker writes the symbol's bytes via the adjacent `lea`
        // relocation), so no rollback of bytes is needed on failure — the
        // diagnostic + the function-shape invariant carry it.
        //
        // ⚠⚠ ONE ENTRY PER SYMBOL, NOT PER PATCH — AND THIS WAS A LIVE BUG,
        // NOT A PRECAUTION. ✔MEASURED 2026-08-13: `void *a = &&L; void *b =
        // &&L;` FAILED TO COMPILE with `K_SymbolUndefined: symbol #N is
        // declared more than once`. `mintBlockSymbol` is memoized per target
        // block, so N block-address `lea`s of the SAME label are N patches
        // carrying ONE SymbolId — and pushing one `SyntheticBlockSymbol` per
        // patch declared that symbol N times to the linker. Every `.s` and
        // every C function that took one label's address TWICE was refused.
        // The duplicates are IDENTICAL (same symbol, same block, therefore
        // same offset), so collapsing them is not a choice between two
        // answers. The jump-table arm of `compile_pipeline.cpp` already had
        // this guard (`alreadyBound`); the encoder-driven arm did not, which
        // is precisely how one path can be right while its sibling is wrong.
        bool blockSymOk = true;
        std::unordered_set<std::uint32_t> boundBlockSyms;
        for (auto const& bsp : blockSymPatches) {
            auto it = blockOffsets.find(bsp.targetBlock);
            if (it == blockOffsets.end()) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("block-address binding in fn '{}' targets "
                                   "block id {} which is not in the function's "
                                   "block list — malformed LIR ("
                                   "D-CSUBSET-COMPUTED-GOTO)",
                                   outFn.symbol.v, bsp.targetBlock));
                blockSymOk = false;
                break;
            }
            if (!boundBlockSyms.insert(bsp.symbol.v).second) continue;
            outFn.blockSymbols.push_back(SyntheticBlockSymbol{
                bsp.symbol, it->second});
        }
        if (!blockSymOk) {
            outFn.bytes.clear();
            outFn.relocations.clear();
            outFn.sourceMap.clear();
            outFn.blockSymbols.clear();
            break;  // leave the relaxation loop — this function is dropped
        }

        // Resolve intra-function block-relative branch patches now
        // that every block's byte offset is known. Each patch wrote
        // 4 zero placeholder bytes; we overwrite the field the
        // patch's GEOMETRY ROW describes with the displacement that
        // row's own formula produces — `(target - (patch + pcBias))
        // >> scaleLog2`, written whole or as a bit-window.
        //
        // D-ASM-PATCH-PARTIAL-OUTPUT-FAILLOUD (post-fold, silent-
        // failure-hunter HIGH #3): on ANY patch failure, abort the
        // whole function's emission rather than partially patching.
        // The previous shape `continue`-d past failures and shipped
        // a partial-patched binary — a missing-target patch left 4
        // zero bytes (rel32=0 → branch-to-self → infinite loop).
        // `patch.kind` selects a ROW rather than a code path, so the
        // shared resolver bakes in no ISA's arithmetic at all: every
        // number it uses is data, and the SAME data the scan phase
        // and the escape election read. Architect FOLD-NOW post-
        // fold: pre-fix the `target - (patch + 4)` formula and
        // 4-byte LE write lived as raw arithmetic here — an
        // agnosticism break per the project's standing rules
        // (shared substrate, zero CPU-name branches).
        // ── D-CSUBSET-LONG-BRANCH: THE SCAN PHASE ────────────────────
        //
        // A pure arithmetic sweep of the patch list that WRITES NOTHING and
        // REPORTS NOTHING. Its only question is: does this layout ask a
        // field to hold a displacement it cannot hold, when an escape for
        // that field exists and has not been taken yet? Every such patch's
        // instruction joins the promoted set and the function is re-encoded.
        //
        // ★★★ IT RUNS IN FRONT OF THE RESOLVER RATHER THAN INSIDE IT, AND
        // THAT IS DELIBERATE. The resolver below is byte-for-byte the code
        // that shipped before relaxation existed — same range checks, same
        // refusals, same diagnostics. Folding the promotion decision into it
        // would have made every out-of-range path conditional on a fixed
        // point that, for every function in the corpus today, never runs.
        // Kept separate, an unrelaxed function reaches the resolver having
        // been asked one extra subtraction per branch.
        //
        // ⚠ A NON-ESCAPABLE OVERFLOW IS NOT REPORTED FROM HERE. If this pass
        // promotes anything, the layout it just measured is PROVISIONAL and
        // every other displacement in it is provisional too — reporting a
        // refusal against a layout that is about to change would name a
        // number the final binary never had. Relaxation only ever GROWS the
        // function, so an out-of-reach non-escapable branch cannot come back
        // into reach: it will be measured again, against the settled layout,
        // and refused there with the number that is actually true.
        //
        // ★★★ WHERE A PATCH ACTUALLY AIMS — its target block, or the best
        // island standing in for it. ONE function, read by the scan phase and
        // by the resolver alike, for the same reason the geometry row is read
        // by both: two components deciding one displacement from two rules
        // agree only by review, and their disagreement is a valid instruction
        // with the wrong destination.
        //
        // ⚠ AN ISLAND IS ADMISSIBLE ONLY IF IT IS STRICTLY CLOSER TO THE
        // TARGET THAN THIS PATCH SITE IS. Each hop then strictly decreases a
        // non-negative integer, so a chain terminates and no cycle of pads can
        // form however they are laid out.
        //
        // ⚠ AND A PAD IS EXCLUDED FROM ITS OWN AIM BY THE IDENTITY TEST BELOW,
        // NOT BY THAT STRICTNESS — a claim this comment made until a mutant
        // refuted it. ✔MEASURED 2026-09-17: relaxing the gap comparison from
        // `>=` to `>` left the whole suite GREEN, because the identity test is
        // what stops a pad resolving to ITSELF (a branch to itself, the
        // infinite loop D-ASM-PATCH-PARTIAL-OUTPUT-FAILLOUD keeps out of a
        // binary) and the strictness only ever decided TIES between two
        // different pads. Both lines stay — one is the termination argument,
        // the other the self-exclusion — but they are no longer described as
        // one thing doing two jobs.
        auto const aimOffsetFor =
            [&](walker_util::BlockRelPatch const& patch,
                std::int64_t targetOffset) -> std::optional<std::int64_t> {
            auto const g = walker_util::blockRelFieldGeometry(patch.kind);
            std::int64_t const alignMask =
                (std::int64_t{1} << g.scaleLog2) - 1;
            auto const inReach = [&](std::int64_t dest) {
                std::int64_t const delta =
                    dest - (static_cast<std::int64_t>(patch.patchOffset)
                            + g.pcBias);
                if ((delta & alignMask) != 0) return false;
                std::int64_t const disp = delta >> g.scaleLog2;
                return disp >= walker_util::blockRelFieldMin(g)
                    && disp <= walker_util::blockRelFieldMax(g);
            };
            if (inReach(targetOffset)) return targetOffset;
            std::optional<std::int64_t> best;
            std::int64_t bestGap =
                targetOffset > static_cast<std::int64_t>(patch.patchOffset)
                    ? targetOffset - static_cast<std::int64_t>(patch.patchOffset)
                    : static_cast<std::int64_t>(patch.patchOffset) - targetOffset;
            // ⚠ A PAD'S OWN FIELD KIND IS NOT FILTERED ON, AND THAT IS
            // DELIBERATE. What this patch needs is a landing point IT can
            // encode a displacement to — which `inReach` asks with THIS
            // patch's geometry — and which is closer to the target than it is.
            // How far the pad's own branch reaches is the pad's own problem:
            // its field is an ordinary patch and resolves through this same
            // function, chaining again if it must. Filtering on kind would
            // make a narrow branch unable to use a wide pad standing right
            // next to it.
            for (auto const& pad : emittedIslands) {
                if (pad.targetBlock != patch.targetBlock) continue;
                if (pad.bodyStart + pad.fieldOffset == patch.patchOffset)
                    continue;  // this patch IS that pad's own branch
                std::int64_t const at = static_cast<std::int64_t>(pad.bodyStart);
                std::int64_t const gap =
                    targetOffset > at ? targetOffset - at : at - targetOffset;
                if (gap >= bestGap) continue;
                if (!inReach(at)) continue;
                best    = at;
                bestGap = gap;
            }
            return best;
        };

        std::vector<std::uint32_t> newlyPromoted;
        std::vector<IslandSite>    newSites;
        for (auto const& patch : blockPatches) {
            auto const it = blockOffsets.find(patch.targetBlock);
            if (it == blockOffsets.end()) continue;  // resolver reports this
            auto const target = static_cast<std::int64_t>(it->second);
            bool const escapable =
                patch.relaxable
                && !std::binary_search(relaxedInsts.begin(), relaxedInsts.end(),
                                       patch.instV);
            // ⚠ THE ORDER IS NOT ARBITRARY. A patch that can escape into a
            // WIDER field is escaped first: that costs one appended word and
            // no jump-over, where an island costs a cluster and a second hop.
            // Islands are what the widest field has INSTEAD of an escape, not
            // a cheaper alternative to one.
            if (escapable) {
                auto const g = walker_util::blockRelFieldGeometry(patch.kind);
                std::int64_t const delta =
                    target - (static_cast<std::int64_t>(patch.patchOffset)
                              + g.pcBias);
                std::int64_t const disp = delta >> g.scaleLog2;
                if (disp >= walker_util::blockRelFieldMin(g)
                 && disp <= walker_util::blockRelFieldMax(g))
                    continue;
                newlyPromoted.push_back(patch.instV);
                continue;
            }
            if (!patch.island.declared()) continue;  // resolver refuses it
            if (aimOffsetFor(patch, target).has_value()) continue;
            // ── ASK FOR A LANDING PAD, ONE STRIDE ALONG THE WAY ──────────
            // Half the field's reach towards the target, at the nearest
            // instruction boundary. The stride cannot overshoot the target:
            // control only arrives here when the target is further away than
            // the WHOLE reach, and the stride is half of it.
            std::int64_t const stride =
                walker_util::blockRelIslandStride(patch.kind);
            std::int64_t const from =
                static_cast<std::int64_t>(patch.patchOffset);
            std::int64_t desired = target >= from ? from + stride
                                                  : from - stride;
            if (desired < 0) desired = 0;
            if (instEnds.empty()) continue;  // nothing encoded to hang it on
            auto const nearest = [&]() -> std::uint32_t {
                auto lo = std::lower_bound(
                    instEnds.begin(), instEnds.end(), desired,
                    [](InstEnd const& e, std::int64_t v) {
                        return static_cast<std::int64_t>(e.endOffset) < v;
                    });
                if (lo == instEnds.end()) return instEnds.back().instV;
                if (lo == instEnds.begin()) return lo->instV;
                auto const prev = std::prev(lo);
                std::int64_t const dHi =
                    static_cast<std::int64_t>(lo->endOffset) - desired;
                std::int64_t const dLo =
                    desired - static_cast<std::int64_t>(prev->endOffset);
                return dLo <= dHi ? prev->instV : lo->instV;
            }();
            newSites.push_back(IslandSite{nearest, patch.targetBlock,
                                          patch.island});
        }
        if (!newSites.empty()) {
            std::sort(newSites.begin(), newSites.end(), siteKeyLess);
            newSites.erase(
                std::unique(newSites.begin(), newSites.end(),
                            [&](IslandSite const& a, IslandSite const& b) {
                                return !siteKeyLess(a, b) && !siteKeyLess(b, a);
                            }),
                newSites.end());
            std::size_t const before = islandSites.size();
            for (auto const& s : newSites) {
                auto const at = std::lower_bound(islandSites.begin(),
                                                 islandSites.end(), s,
                                                 siteKeyLess);
                if (at != islandSites.end() && !siteKeyLess(s, *at)) continue;
                islandSites.insert(at, s);
            }
            // A pass that asked only for sites it already has made no
            // progress; let it fall through to the resolver, which refuses
            // loudly against this settled layout rather than looping.
            if (islandSites.size() == before) newSites.clear();
        }
        if (!newlyPromoted.empty() || !newSites.empty()) {
            std::sort(newlyPromoted.begin(), newlyPromoted.end());
            newlyPromoted.erase(
                std::unique(newlyPromoted.begin(), newlyPromoted.end()),
                newlyPromoted.end());
            relaxedInsts.insert(relaxedInsts.end(),
                                newlyPromoted.begin(), newlyPromoted.end());
            std::sort(relaxedInsts.begin(), relaxedInsts.end());
            // ── THE ISLAND BOUND, EVALUATED ─────────────────────────────
            // `B * ceil(N / S)` from the block comment at the top of this
            // loop, with N this pass's byte size, S the narrowest stride any
            // declared field has, and B the function's own branches — the
            // patch count MINUS one patch per island, because each island
            // contributes exactly one and counting them would let the bound
            // chase its own tail.
            std::uint64_t const islandCap = [&] {
                std::int64_t stride = walker_util::blockRelIslandStride(
                    static_cast<walker_util::BlockRelPatchKind>(0));
                for (std::size_t k = 1;
                     k < walker_util::kBlockRelPatchKindCount; ++k) {
                    auto const s = walker_util::blockRelIslandStride(
                        static_cast<walker_util::BlockRelPatchKind>(k));
                    if (s < stride) stride = s;
                }
                std::uint64_t const branches =
                    blockPatches.size() >= islandSites.size()
                        ? blockPatches.size() - islandSites.size()
                        : 0u;
                std::uint64_t const hops =
                    (static_cast<std::uint64_t>(outFn.bytes.size())
                     + static_cast<std::uint64_t>(stride) - 1u)
                    / static_cast<std::uint64_t>(stride);
                return branches * (hops + 1u);
            }();
            // The monotonicity backstop. Reaching it means the two monotone
            // sets between them grew more times than the instruction count
            // plus the island bound allows, which no sequence of promotions
            // and placements can do — so it is an internal-invariant
            // violation, not a large program, and it is said that way.
            if (islandSites.size() > islandCap
             || relaxPass + 1 >= relaxBound + islandCap) {
                report(reporter, DiagnosticCode::A_FunctionEncodeAborted,
                       DiagnosticSeverity::Error,
                       std::format("function symbol id {} dropped — long-"
                                   "branch relaxation did not reach a fixed "
                                   "point within {} passes ({} branch(es) "
                                   "promoted, {} branch island(s) placed "
                                   "against a bound of {}). Both sets are "
                                   "monotone — one bounded by the instruction "
                                   "count, the other by branches x ceil(bytes "
                                   "/ half-reach) — so exceeding this bound is "
                                   "an internal-invariant violation, not an "
                                   "oversized function (D-CSUBSET-LONG-BRANCH)",
                                   outFn.symbol.v, relaxBound + islandCap,
                                   relaxedInsts.size(), islandSites.size(),
                                   islandCap));
                outFn.bytes.clear();
                outFn.relocations.clear();
                outFn.sourceMap.clear();
                outFn.blockSymbols.clear();
                outFn.blockByteOffsets.clear();
                break;
            }
            continue;  // re-encode with the larger promoted set
        }

        bool patchOk = true;
        for (auto const& patch : blockPatches) {
            auto it = blockOffsets.find(patch.targetBlock);
            if (it == blockOffsets.end()) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("intra-function branch patch in fn '{}' "
                                   "targets block id {} which is not in "
                                   "the function's block list — malformed "
                                   "LIR (D-CSUBSET-WHILE-LOOP-SUBSTRATE)",
                                   outFn.symbol.v, patch.targetBlock));
                patchOk = false;
                break;
            }
            // ── D-CSUBSET-LONG-BRANCH: ONE RESOLVER, ONE GEOMETRY ROW ───
            //
            // ★★★ THE `switch` THIS REPLACES CARRIED A SECOND COPY OF EVERY
            // NUMBER IN `blockRelFieldGeometry`, AND NOTHING KEPT THE TWO IN
            // STEP. The scan phase above was converted to read the table when
            // relaxation landed; the resolver was not, so it still derived
            // `lsb`/`width` from `isImm19 ? 5u : 0u` / `isImm19 ? 19u : 26u`
            // and spelled the scale as a bare `>> 2` and x86's PC bias as a
            // bare `+ 4`. Two components deciding the SAME field's shape from
            // two sources is the N-transforms-on-one-value shape: correcting a
            // width in the table would have moved the promotion boundary while
            // the bytes kept landing at the old one, and the disagreement
            // emits VALID INSTRUCTIONS WITH THE WRONG DISPLACEMENT — no
            // diagnostic, no crash. The table is now the only source.
            //
            // ⚠ AND THE `switch` HAD NO `default`. A fourth `BlockRelPatchKind`
            // matched no arm, left `patchOk` true, and shipped the four ZERO
            // placeholder bytes the walker wrote — `B #0` (a branch to itself)
            // on a fixed-width ISA, `rel32 = 0` on x86. The table's documented
            // enum-drift backstop (an unknown kind yields a ZERO-WIDTH field,
            // whose signed range is empty) was therefore INERT, because the
            // component it protects never asked it. Reading the row makes the
            // backstop live: a kind with no row now refuses every displacement
            // loudly instead of silently writing none.
            auto const g = walker_util::blockRelFieldGeometry(patch.kind);
            // D-CSUBSET-LONG-BRANCH: what this field actually holds is a
            // displacement to the AIM POINT — the target block when it is in
            // reach, and otherwise the landing pad standing in for it. When
            // neither is in reach the aim falls back to the target itself, so
            // the refusal below quotes the displacement the programmer's
            // branch really needs rather than a pad's.
            auto const aim = aimOffsetFor(patch,
                                          static_cast<std::int64_t>(it->second));
            std::int64_t const delta =
                aim.value_or(static_cast<std::int64_t>(it->second))
              - (static_cast<std::int64_t>(patch.patchOffset)
                 + static_cast<std::int64_t>(g.pcBias));
            // A scaled field cannot represent a displacement that is not a
            // multiple of its scale. On a fixed-width ISA that is a hard
            // invariant (every block boundary is instruction-aligned), so a
            // non-multiple delta means the block-offset table or the patch
            // offset is corrupt; fail loud rather than silently drop the low
            // bits. An UNSCALED field (x86 rel32, scaleLog2 = 0) has an empty
            // mask, so this check never fires there — byte-identical to the
            // arm it replaces, which did not perform it at all.
            std::int64_t const alignMask =
                (std::int64_t{1} << g.scaleLog2) - 1;
            if ((delta & alignMask) != 0) {
                report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                       DiagnosticSeverity::Error,
                       std::format("intra-function branch in fn '{}' has "
                                   "displacement {} which is not a multiple "
                                   "of this field's scale ({} bytes) — block "
                                   "offsets must be instruction-aligned "
                                   "(D-AS3-BLOCK-REL-IMM19-26)",
                                   outFn.symbol.v, delta, alignMask + 1));
                patchOk = false;
                break;
            }
            std::int64_t const disp = delta >> g.scaleLog2;  // arithmetic, signed
            std::int64_t const lo = walker_util::blockRelFieldMin(g);
            std::int64_t const hi = walker_util::blockRelFieldMax(g);
            if (disp < lo || disp > hi) {
                // ★★★ THE REMEDY IS CARRIED, NOT REMEMBERED. Both arms used to
                // end with a fixed prescription — "for thunks" on x86, "for
                // inverted-cond + long B thunks" on AArch64 — and the second
                // one is WRONG for the wider of the two fields it served: an
                // `Imm26` overflow cannot be rescued by a long `B`, because
                // `B` IS the Imm26 form. Which case this is can only be
                // answered by the OPCODE's own encoding variants, so the
                // walker stamps the answer and the resolver quotes it.
                // ★★★ AND THE PRESCRIPTION CHANGED WHEN THE FRAME DID. It used
                // to end, for the widest field, with "the escape is then a
                // different ADDRESSING MODE (an indirect branch through a
                // materialized absolute address), which needs a per-block
                // symbol the assembler cannot mint". That is REFUTED: the
                // widest field escapes into a NEARER TARGET, not a wider one,
                // and a landing pad is a copy of a branch this opcode already
                // declares. So the remaining ways to be here are three, and
                // each names something a reader can actually do.
                bool const escapableInPrinciple = patch.widerFieldDeclared;
                report(reporter, DiagnosticCode::A_ImmediateOperandOutOfRange,
                       DiagnosticSeverity::Error,
                       std::format("intra-function branch in fn '{}' needs "
                                   "displacement {} (scaled by {}) which "
                                   "exceeds its signed {}-bit field range "
                                   "[{}..{}] — function body too large for "
                                   "this branch's reach. {} "
                                   "(anchor D-CSUBSET-LONG-BRANCH)",
                                   outFn.symbol.v, disp, alignMask + 1,
                                   g.width, lo, hi,
                                   escapableInPrinciple
                                     ? "This opcode DOES declare a wider "
                                       "block-relative word, and this wire is "
                                       "not the one the escape rescues: wire "
                                       "it to the wider slot, or make it the "
                                       "narrowest block-relative field of the "
                                       "instruction so the election picks it"
                                     : patch.island.declared()
                                     ? "This opcode declares a branch island "
                                       "body and the resolver still could not "
                                       "place one within reach of this site — "
                                       "an internal-invariant violation of the "
                                       "island placement, not a property of "
                                       "the program: report it against this "
                                       "anchor"
                                     : "This opcode declares NO self-contained "
                                       "unconditional-branch word. That word "
                                       "is what both remedies are made of: a "
                                       "WIDER one is an escape, and one of "
                                       "EQUAL reach is a branch island, which "
                                       "rescues even the target's widest "
                                       "field by standing nearer. Declare the "
                                       "unconditional branch on this opcode's "
                                       "encoding row — one self-contained word "
                                       "(or, on a byte-oriented shape, one "
                                       "wire whose `prefixOpcodeBytes` are a "
                                       "whole unconditional branch) — and both "
                                       "elections will find it"));
                patchOk = false;
                break;
            }
            // The two write disciplines (whole 4-byte field vs. a bit-window
            // read-modify-write) live in `walker_util::writeBlockRelField`,
            // because the island cluster's jump-over writes the same shape at
            // emit time and two spellings of one write is the same
            // N-transforms-on-one-value shape the geometry table exists to
            // close.
            walker_util::writeBlockRelField(outFn.bytes, patch.patchOffset,
                                            g, disp);
            // (The trailing `if (!patchOk) break;` this loop used to carry was
            // the `switch`'s exit door: a failing arm's `break` left the SWITCH
            // and needed a second one to leave the loop. Without the switch,
            // every refusal above breaks the loop directly, so the re-test is
            // unreachable — and an unreachable guard reads as a live one.)
        }
        if (!patchOk) {
            outFn.bytes.clear();
            outFn.relocations.clear();
            outFn.sourceMap.clear();
        }
        // D-CSUBSET-LONG-BRANCH: THE FIXED POINT. Control only arrives here
        // when the scan promoted nothing, which means every block-relative
        // field in this layout holds a displacement it can hold — so the
        // layout is settled and the bytes above are final.
        break;
        }  // relaxation loop
    }

    return result;
}

bool validateAssembledData(std::span<AssembledData const> items,
                           DiagnosticReporter& reporter) {
    auto emit = [&](DiagnosticCode code, std::string msg) {
        ParseDiagnostic d;
        d.code     = code;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::move(msg);
        reporter.report(std::move(d));
    };

    bool ok = true;

    // Invariant 1: zero-fill (Bss/Tbss) items must have empty bytes.
    // TLS C1 audit fold M-3 (D-CSUBSET-THREAD-LOCAL): routed through the
    // ONE shared `isZeroFill` predicate — the former exact `== Bss` test
    // would have let a byte-carrying Tbss item slip past this invariant.
    for (std::size_t i = 0; i < items.size(); ++i) {
        auto const& d = items[i];
        if (isZeroFill(d.section) && !d.bytes.empty()) {
            emit(DiagnosticCode::K_BssDataHasBytes,
                 std::format("AssembledData[{}] has section={} "
                             "but bytes is non-empty ({} bytes). "
                             "A zero-fill section — the wire format "
                             "reserves the size without storing "
                             "bytes. Substrate-shape violation "
                             "(D-LK4-RODATA-BSS-INVARIANT).",
                             i, dataSectionKindName(d.section),
                             d.bytes.size()));
            ok = false;
        }
    }

    // Invariant 2: no two items share the same non-sentinel
    // SymbolId. Sentinel `SymbolId{}` (.v == 0) is exempt — it's
    // the "anonymous data" marker and multiple anonymous items
    // are legitimate.
    std::unordered_map<std::uint32_t, std::size_t> firstByV;
    firstByV.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        auto const v = items[i].symbol.v;
        if (v == 0u) continue;  // sentinel exempt
        auto const [it, inserted] = firstByV.emplace(v, i);
        if (!inserted) {
            emit(DiagnosticCode::K_DuplicateDataSymbol,
                 std::format("AssembledData[{}] has SymbolId={{ "
                             "{} }} which collides with item[{}]. "
                             "Duplicate SymbolIds would silently "
                             "let \"whichever was processed last\" "
                             "win the linker's symbol→VA "
                             "resolution. Mint distinct SymbolIds "
                             "per data item or use the sentinel "
                             "SymbolId{{}} for anonymous data.",
                             i, v, it->second));
            ok = false;
        }
    }

    // Invariant 3 (alignment power-of-two) is enforced structurally
    // by the `Alignment` newtype — see `asm.hpp` docblock.
    return ok;
}

bool validateInputSectionUnits(AssembledModule const& module,
                               DiagnosticReporter&    reporter) {
    auto refuse = [&](std::string msg) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::K_InputSectionSplit;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::move(msg);
        reporter.report(std::move(d));
    };
    bool ok = true;

    // CODE: consecutive, in increasing offset, contiguous — what a writer's
    // back-to-back concatenation needs to reproduce the section. A unit whose
    // members are not together has been split. So has one whose next member
    // does not start where the previous one's bytes end, because the
    // concatenation would close the gap.
    std::unordered_set<std::uint32_t> closedUnits;
    for (std::size_t k = 0; k < module.functions.size(); ++k) {
        auto const& slice = module.functions[k].inputSection;
        if (!slice.has_value()) continue;
        bool const continues =
            k > 0 && module.functions[k - 1].inputSection.has_value()
            && module.functions[k - 1].inputSection->section == slice->section;
        if (!continues) {
            if (!closedUnits.insert(slice->section).second) {
                refuse(std::format(
                    "code unit #{} (one input section of a relocatable object) "
                    "resumes at function #{} after other code: its members are "
                    "not consecutive, so the writers' concatenation would put "
                    "other bytes inside the section and move every member after "
                    "them.",
                    slice->section, k));
                ok = false;
            }
            continue;
        }
        auto const& prev = module.functions[k - 1];
        std::uint64_t const prevEnd = prev.inputSection->offset + prev.bytes.size();
        if (slice->offset != prevEnd) {
            refuse(std::format(
                "code unit #{} (one input section of a relocatable object): "
                "function #{} starts at section offset {}, but the member before "
                "it ends at {}. Concatenated, the section would not keep its "
                "layout, and code the producer assembled against it would reach "
                "the wrong bytes.",
                slice->section, k, slice->offset, prevEnd));
            ok = false;
        }
    }

    // DATA: one data-section kind per unit, and members that do not overlap.
    // `buildExecDataSection` places a unit's members by their offsets.
    struct DataMember {
        std::uint64_t   offset = 0;
        std::uint64_t   size   = 0;
        std::size_t     index  = 0;
        DataSectionKind kind   = DataSectionKind::Rodata;
    };
    std::unordered_map<std::uint32_t, std::vector<DataMember>> dataUnits;
    for (std::size_t i = 0; i < module.dataItems.size(); ++i) {
        auto const& d = module.dataItems[i];
        if (!d.inputSection.has_value()) continue;
        dataUnits[d.inputSection->section].push_back(DataMember{
            d.inputSection->offset, d.sizeInSection(), i, d.section});
    }
    for (auto& [unit, members] : dataUnits) {
        std::sort(members.begin(), members.end(),
                  [](DataMember const& a, DataMember const& b) {
                      return a.offset < b.offset;
                  });
        for (std::size_t k = 0; k < members.size(); ++k) {
            if (members[k].kind != members.front().kind) {
                refuse(std::format(
                    "data unit #{} (one input section of a relocatable object) "
                    "holds item #{} in section kind '{}' and item #{} in '{}'. "
                    "One input section is laid out as one block, so it cannot "
                    "be placed in two output sections.",
                    unit, members.front().index,
                    dataSectionKindName(members.front().kind), members[k].index,
                    dataSectionKindName(members[k].kind)));
                ok = false;
                break;
            }
            if (k > 0 && members[k].offset < members[k - 1].offset + members[k - 1].size) {
                refuse(std::format(
                    "data unit #{} (one input section of a relocatable object): "
                    "item #{} at section offset {} overlaps item #{}, which "
                    "runs from {} to {}. Two items cannot own the same bytes of "
                    "one section.",
                    unit, members[k].index, members[k].offset,
                    members[k - 1].index, members[k - 1].offset,
                    members[k - 1].offset + members[k - 1].size));
                ok = false;
                break;
            }
        }
    }
    return ok;
}

namespace {

// ── D-CSUBSET-ENUM-GLOBAL-CODEGEN: the MATERIAL kind a type ENCODES as ──
//
// C 6.7.2.2 / C23 6.7.2.2: an enumerated type has an implementation-defined
// COMPATIBLE integer type, and its OBJECT REPRESENTATION *is* that integer's.
// `TypeKind::Enum` is a NOMINAL-IDENTITY marker with no representation of its
// own — which is exactly why `scalarByteSize(Enum)` is nullopt BY CONSTRUCTION
// (the kind alone cannot know the width; only `scalars[0]` does). So every
// KIND-KEYED data-emission decision in this file — the width, the value decode,
// the natural alignment — must ask about the UNDERLYING integer, never about
// the Enum marker.
//
// ⚠ WHY THIS EXISTS AT ALL: it did not, and an utterly ordinary `enum E g = B;`
// at file scope therefore reached the "non-primitive global types" refusal
// (K_NoMatchingObjectFormat) instead of emitting four bytes — while gcc and
// clang both compile and run it. This tier was the ONE kind-keyed tier in the
// pipeline that never performed the projection; every other one already does:
//   * `enumUnderlyingOrSelf`   (analysis/semantic/type_rules.hpp) — arithmetic
//   * `resolveScalarIntKind`   (mir/lowering/hir_to_mir.cpp)      — MIR scalars
//   * `classifyKind`/`reprKind`(lir/lowering/mir_to_lir.cpp)      — LIR widths
//   * `computeLayout`'s Enum arm (core/types/…/type_layout.cpp)   — size/align
// This is therefore NOT a new verb: it reads the SAME `scalars[0]` slot
// `TypeInterner::enumType` writes and all four sites above read.
//
// ★ IT PROJECTS THE KIND ONLY — `ty` itself is NEVER rewritten, so the
// structural readers (`operands`/`scalars`/`computeLayout`) keep seeing the
// DECLARED type and an aggregate's layout is untouched. `interner.kind()`
// already sees THROUGH a `VolatileQual` skin (c27), so `volatile enum E`
// projects too, and a typedef is not a type at all here (it interns to the same
// Enum id). Any non-enum passes through unchanged.
//
// ★ FAIL-LOUD ON A MALFORMED RECORD, never a guessed width: an Enum interned
// without its underlying scalar, or with one outside the core kind range,
// returns `Enum` UNCHANGED so the callers' existing refusals fire.
// ⓘ ✔MEASURED, and it corrects the obvious reading of that bound check: it is
// NOT there to make the cast well-defined. `TypeKind`'s underlying type is FIXED
// (`std::uint16_t`), so converting an out-of-range integer to it is already
// defined (it wraps). What the check buys is that a wrapped scalar cannot ALIAS
// a VALID kind and hand a malformed record a plausible width — the choice
// between REFUSING and GUESSING. The `AsmEnumGlobal` malformed-record pin is red
// exactly under the GUESSING mutant and green under both mutations that merely
// remove machinery; that pin's own comment names it and records why.
//
// ⛔ IT DELIBERATELY DOES **NOT** PROJECT `BitInt` → its container kind, even
// though the MIR/LIR twins of this projection do — and the REASON CHANGED when
// the multi-limb emitter landed, so read it fresh rather than by analogy.
// It USED to be that `decodeScalarLiteralBits` returns a `std::uint64_t` with no
// `_BitInt` gate, so a projected WIDE `_BitInt(N>64)` leaf would quietly encode
// its low 8 bytes as the whole value. That hazard is gone: both encode sites now
// route a `BitInt` kind to `encodeBitIntImage` BEFORE any `scalarByteSize` /
// `decodeScalarLiteralBits` pair is reached (D-CSUBSET-BITINT-DATA-GLOBAL).
// ★ THE PROJECTION IS STILL FORBIDDEN, FOR A SHARPER REASON: a `_BitInt(N)`'s
// image is not its container kind's image. The container carries N *value* bits
// plus padding, and this file emits the padding as the value's SIGN EXTENSION
// (matching DSS's `bitIntMask`/`maskTopLimb` runtime invariant). Projecting
// `_BitInt(17)` → `I32` would throw away N — the only thing that says where the
// value bits stop — and hand the image to an encoder that cannot ask. An enum
// projects soundly precisely because its underlying IS a whole core integer kind
// with no residual width parameter; `_BitInt` has one, so it keeps its own arm.
// The enum projection is sound because an enum's underlying is
// a core INTEGER kind (the semantic tier rejects anything else), and every arm
// below already handles those correctly INCLUDING their walls. ✔MEASURED on the
// widest case: `enum E : __int128 g = B;` reaches the dedicated 16-byte arm and
// EMITS; since P68 round 8 the same enum as a struct MEMBER emits too, through
// the same 16-byte producer — byte-for-byte what a plain `__int128` member gets
// (pinned by `AsmDataSection.Int128AggregateMemberEmitsItsSixteenBytesAtItsOffset`).
//
// ★ THE FILE-WIDE INVARIANT THIS ESTABLISHES, and it is the greppable form of
// the multi-site contract: in this file, EVERY `scalarByteSize(...)` and
// `decodeScalarLiteralBits(...)` argument that comes from an interner lookup
// goes through here first. A new kind-keyed encode site that reaches for
// `in.kind(x)` directly is the regression to look for.
[[nodiscard]] TypeKind
materialScalarKind(TypeInterner const& in, TypeId ty) noexcept {
    TypeKind const k = in.kind(ty);
    if (k != TypeKind::Enum) return k;
    auto const sc = in.scalars(ty);
    if (sc.empty() || sc[0] < 0
        || sc[0] >= static_cast<std::int64_t>(TypeKind::Count_))
        return k;
    return static_cast<TypeKind>(sc[0]);
}

// Byte-width of a primitive TypeKind. Returns nullopt for non-primitive
// kinds (Array / Struct / Ptr / FnSig / ...). Aggregate globals do NOT pass
// through here — they take the `MirAggregateValue` arm + `encodeAggregateValue`
// (the interner-side recursive layout walk, D-LK4-RODATA-PRODUCER-AGGREGATE-GLOBAL);
// this gate only widths the SCALAR-global fast path.
[[nodiscard]] std::optional<std::size_t>
primitiveByteSize(TypeKind k) noexcept {
    switch (k) {
        case TypeKind::Bool:
        case TypeKind::I8: case TypeKind::U8:
        case TypeKind::Char: case TypeKind::Byte:
            return 1u;
        case TypeKind::I16: case TypeKind::U16: case TypeKind::F16:
            return 2u;
        case TypeKind::I32: case TypeKind::U32: case TypeKind::F32:
            return 4u;
        case TypeKind::I64: case TypeKind::U64: case TypeKind::F64:
            return 8u;
        // F80 (D-CSUBSET-LONG-DOUBLE): 16-byte storage like binary128 — the
        // x87 format pads to 16/16. Sized here for LAYOUT; a VALUE of any of
        // these four kinds is encoded by `appendSixteenByteScalarImage`, never
        // by the u64 `decodeScalarLiteralBits` (which refuses all four).
        case TypeKind::I128: case TypeKind::U128: case TypeKind::F80:
        case TypeKind::F128:
            return 16u;
        default:
            return std::nullopt;
    }
}

// Little-endian encode `value` into `bytes` (appended). Width=`width`
// bytes. The integer's high bytes are dropped silently when `value`
// exceeds the type's range (caller invariant: HIR/MIR const-eval clamps
// to the type's range before reaching the literal pool).
//
// ⚠ CALLER INVARIANT: `width` MUST be ≤ 8. `value` is a `std::uint64_t`, so
// `value >> (j*8)` is UNDEFINED BEHAVIOUR for j ≥ 8 — it is NOT a zero fill.
// (A previous version of this comment claimed "trailing zeros are appended
// verbatim", which is false for width > 8 and was actively misleading: on both
// shipped host arches the masked shift count REPEATS the low 8 bytes into the
// high 8, so an over-wide call writes plausible-looking WRONG bytes rather
// than crashing. Corrected in TF-C94 — D-CSUBSET-INT128-DATA-GLOBAL.)
// Every 16-byte scalar kind is routed BEFORE reaching here — by the scalar-global
// arm and the aggregate-member leaf alike — to `appendSixteenByteScalarImage`,
// which calls this only at width 8, once per limb; so the only widths that
// arrive are the 1/2/4/8-byte ones this loop can encode.
//
// ★ THE LOOP ITSELF MOVED TO `asm.hpp::appendLittleEndianBytes` when the
// assembly-text data directives needed the same append
// (D-ASM-NO-DATA-DEFINING-DIRECTIVE, 2026-08-13). This name stays as the
// in-file spelling its ~15 call sites already use; what must not exist is a
// SECOND byte-order loop, since a divergence between two of them is a green
// build emitting reversed words.
void appendLE(std::vector<std::uint8_t>& bytes,
              std::uint64_t value,
              std::size_t width) noexcept {
    appendLittleEndianBytes(bytes, value, width);
}

// D-CSUBSET-LONG-DOUBLE-X87-ARITH (LD-1): widen a host `double` (IEEE-754
// binary64) LOSSLESSLY into the x87 80-bit extended format and append its 16
// on-disk bytes (10 significant + 6 zero pad — the SysV/darwin 16-byte,
// 16-aligned slot `scalarByteSize(F80)` reserves). This is 80-bit, WIDER than
// the u64 `decodeScalarLiteralBits` yields, so it is reached only through
// `appendSixteenByteScalarImage` — the one producer the scalar-global arm AND
// the aggregate-member leaf both call (D-CSUBSET-LONG-DOUBLE-AGGREGATE-GLOBAL,
// closed P68 round 8: a `long double` MEMBER was refused while the same value
// as a whole global was emitted, because only the scalar arm had a path).
// The x87 extended memory layout is little-endian:
//   bytes 0-7  = the 64-bit significand with an EXPLICIT integer bit (bit 63),
//   bytes 8-9  = sign (bit 15) | 15-bit exponent,
//   bytes 10-15 = zero pad.
// The widen from binary64 (11-bit exponent bias 1023, 52-bit fraction with an
// IMPLICIT leading 1): copy the sign; rebias the exponent to bias 16383; move
// the 52-bit fraction up by 11 into the significand and SET bit 63 (extended's
// integer bit is explicit). Zero, subnormal, infinity and NaN are handled
// specially — a binary64 subnormal is a NORMAL extended value (the wider
// exponent range absorbs it), so it is renormalized rather than emitted as a
// (never-produced-by-widening) extended subnormal.  Verified by hand: 20.0L →
// exponent 0x4003, significand 0xA000000000000000 → LE bytes
// 00 00 00 00 00 00 00 A0 03 40 00 00 00 00 00 00.
void appendF80Extended(std::vector<std::uint8_t>& bytes, double dv) noexcept {
    std::uint64_t d = 0;
    std::memcpy(&d, &dv, sizeof(double));
    std::uint64_t const sign   = (d >> 63) & 0x1ull;
    std::uint64_t const exp11  = (d >> 52) & 0x7FFull;
    std::uint64_t const frac52 = d & 0x000F'FFFF'FFFF'FFFFull;

    std::uint16_t signExp = 0;
    std::uint64_t mant64  = 0;
    constexpr std::uint64_t kIntegerBit = 0x8000'0000'0000'0000ull;
    if (exp11 == 0x7FFull) {
        // Infinity (frac52 == 0) / NaN (frac52 != 0): max exponent 0x7FFF,
        // integer bit set, the 52-bit payload shifted up 11 (quiet bit
        // preserved) — infinity's zero fraction stays zero.
        signExp = static_cast<std::uint16_t>((sign << 15) | 0x7FFFu);
        mant64  = kIntegerBit | (frac52 << 11);
    } else if (exp11 == 0) {
        if (frac52 == 0) {
            signExp = static_cast<std::uint16_t>(sign << 15);   // signed zero
            mant64  = 0;
        } else {
            // binary64 subnormal (value = frac52 * 2^-1074) → renormalize into
            // an extended NORMAL: shift the highest set bit up to bit 63 and
            // set the exponent E so that (63 - shift) + 15309 == the unbiased
            // exponent + 16383. frac52 < 2^52 ⇒ countl_zero ≥ 12.
            int const shift = std::countl_zero(frac52);
            int const e     = (63 - shift) + 15309;
            signExp = static_cast<std::uint16_t>(
                (sign << 15) | static_cast<std::uint16_t>(e & 0x7FFF));
            mant64  = frac52 << shift;
        }
    } else {
        // Normal: rebias 1023 → 16383, set the explicit integer bit, and lift
        // the fraction into place.
        std::uint64_t const e80 = exp11 - 1023 + 16383;
        signExp = static_cast<std::uint16_t>(
            (sign << 15) | static_cast<std::uint16_t>(e80 & 0x7FFFu));
        mant64  = kIntegerBit | (frac52 << 11);
    }

    for (int i = 0; i < 8; ++i)
        bytes.push_back(static_cast<std::uint8_t>((mant64 >> (i * 8)) & 0xFFu));
    bytes.push_back(static_cast<std::uint8_t>(signExp & 0xFFu));
    bytes.push_back(static_cast<std::uint8_t>((signExp >> 8) & 0xFFu));
    for (int i = 0; i < 6; ++i) bytes.push_back(0u);
}

// D-CSUBSET-LONG-DOUBLE-IEEE128-ARITH (LD-2): widen a host `double` (IEEE-754
// binary64) LOSSLESSLY into IEEE-754 binary128 (quad precision) and append its
// 16 on-disk bytes. binary128 layout: 1 sign (bit 127) | 15-bit exponent (bits
// 126..112, bias 16383) | 112-bit fraction (bits 111..0) with an IMPLICIT
// leading 1 for normals — UNLIKE the x87 extended format (appendF80Extended),
// whose integer bit is EXPLICIT. 16 bytes EXACT, NO padding (scalarByteSize
// (F128) == 16). Little-endian: bytes 0..13 hold the 112-bit fraction low-
// justified, bytes 14..15 hold sign(bit 127) | exponent(bits 126..112).
//
// The widen from binary64 (11-bit exponent bias 1023, 52-bit fraction, implicit
// leading 1): rebias 1023 -> 16383 and move the 52-bit fraction up by 60 into
// the 112-bit field (lossless — the low 60 bits stay zero). Zero, infinity/NaN
// and subnormal are special-cased: a binary64 SUBNORMAL is a binary128 NORMAL
// (the wider exponent absorbs it), so it renormalizes — but because binary128's
// leading 1 is IMPLICIT, the renormalized integer bit is DROPPED from the
// stored fraction (the crucial difference from the F80 renormalize, which KEEPS
// its explicit integer bit). The subnormal arm is NOT exercised by the
// 20.0L/22.0L witness (both normals) — hand-verified. Verified: 20.0L -> exp
// 0x4003, fraction top nibble 0x4 -> the 16 LE bytes are 00 x13 then 40 03 40
// (bytes 13/14/15).
void appendF128(std::vector<std::uint8_t>& bytes, double dv) noexcept {
    std::uint64_t d = 0;
    std::memcpy(&d, &dv, sizeof(double));
    std::uint64_t const sign   = (d >> 63) & 0x1ull;
    std::uint64_t const exp11  = (d >> 52) & 0x7FFull;
    std::uint64_t const frac52 = d & 0x000F'FFFF'FFFF'FFFFull;

    std::uint16_t exp15    = 0;
    std::uint64_t fracLo64 = 0;   // binary128 fraction bits 0..63
    std::uint64_t fracHi48 = 0;   // binary128 fraction bits 64..111 (48 bits)
    if (exp11 == 0x7FFull) {
        // Infinity (frac52 == 0) / NaN (frac52 != 0): max exponent 0x7FFF, the
        // 52-bit payload shifted up 60 (its MSB -> fraction bit 111, so a quiet
        // NaN stays quiet); infinity's zero fraction stays zero.
        exp15    = 0x7FFFu;
        fracLo64 = (frac52 & 0xFull) << 60;
        fracHi48 = frac52 >> 4;
    } else if (exp11 == 0) {
        if (frac52 == 0) {
            exp15 = 0;   // signed zero (fraction all zero)
        } else {
            // binary64 subnormal (value = frac52 * 2^-1074) -> binary128 NORMAL:
            // shift the highest set bit up to bit 63, set the exponent so
            // (63 - shift) + 15309 == unbiased + 16383, then DROP the now-
            // implicit leading 1 and left-justify the remaining 63 fraction bits
            // into the 112-bit field (<< 49). frac52 < 2^52 => countl_zero >= 12.
            int const shift = std::countl_zero(frac52);
            std::uint64_t const mant = frac52 << shift;   // leading 1 now at bit 63
            exp15 = static_cast<std::uint16_t>(((63 - shift) + 15309) & 0x7FFF);
            std::uint64_t const fracBits =
                mant & 0x7FFF'FFFF'FFFF'FFFFull;           // drop the integer bit
            fracLo64 = fracBits << 49;                     // low 64 bits
            fracHi48 = fracBits >> 15;                     // high 48 bits
        }
    } else {
        // Normal: rebias 1023 -> 16383, move the 52-bit fraction up by 60 into
        // the 112-bit field (implicit leading 1 in BOTH formats — nothing to
        // set). exp11 in [1, 0x7FE] => exp15 in [0x3C01, 0x43FE], never 0/0x7FFF.
        exp15    = static_cast<std::uint16_t>((exp11 - 1023 + 16383) & 0x7FFF);
        fracLo64 = (frac52 & 0xFull) << 60;
        fracHi48 = frac52 >> 4;
    }

    std::uint64_t const hi64 =
        (sign << 63)
        | (static_cast<std::uint64_t>(exp15 & 0x7FFFu) << 48)
        | fracHi48;
    for (int i = 0; i < 8; ++i)
        bytes.push_back(static_cast<std::uint8_t>((fracLo64 >> (i * 8)) & 0xFFu));
    for (int i = 0; i < 8; ++i)
        bytes.push_back(static_cast<std::uint8_t>((hi64 >> (i * 8)) & 0xFFu));
}

// LD-3 (D-CSUBSET-LONG-DOUBLE-CONSTFOLD-PRECISION): append the 16 on-disk bytes
// of a CONST-FOLDED F80/F128 value carried in the `WideFloatValue` pool arm. The
// kernel's `pack()` already produces {lo, hi} in EXACTLY the byte layout
// `appendF80Extended`/`appendF128` emit (F80: 10 significant + 6 pad; F128: 16),
// so this is a pure lo-then-hi little-endian write — ADDITIVE to those two
// (unmodified) `double`-arm widen producers, chosen FIRST for the folded arm.
void appendWideFloatBits(std::vector<std::uint8_t>& bytes, WideFloatValue const& wf) noexcept {
    WideFloatValue::Packed const p = wf.pack();
    for (int i = 0; i < 8; ++i)
        bytes.push_back(static_cast<std::uint8_t>((p.lo >> (i * 8)) & 0xFFu));
    for (int i = 0; i < 8; ++i)
        bytes.push_back(static_cast<std::uint8_t>((p.hi >> (i * 8)) & 0xFFu));
}

// ── THE 16-BYTE SCALAR IMAGE: ONE PRODUCER, TWO CALL SITES ──────────────────────
// (D-CSUBSET-LONG-DOUBLE-AGGREGATE-GLOBAL, and its 128-bit integer twin
// D-CSUBSET-INT128-AGGREGATE-MEMBER-STATIC-INITIALIZER-REFUSED — both closed P68
// round 8.) An F80 / F128 `long double` and an I128 / U128 integer each occupy
// 16 bytes, WIDER than the `std::uint64_t` `decodeScalarLiteralBits` returns, so
// none of them can pass through that chokepoint. The SCALAR-global arm of
// `lowerMirGlobalsToDataItems` used to carry three private copies of this
// encoding (F80, F128, 128-bit) and the aggregate-member LEAF had none — so
// `struct S { char c; long double x; } g = {'a', 40.5L};`, `static long double
// t[3] = {…};` and `struct { __int128 v; } w = {-3};` were REFUSED at the leaf
// while the same values as whole globals were emitted. ✔MEASURED at the P68
// round-8 base: refused on ELF x86_64 and ELF aarch64 (the 128-bit members on
// all four shipped formats), debug and release; gcc 13.3.0 and clang 18.1.3,
// each separately, run the same source to 42 at -O0 and -O2 on both processors.
// ★ Both sites now ask HERE — the `encodeBitIntImage` shape — so a value and
// the same value as a member cannot be encoded two ways.
// Appends exactly 16 bytes and returns true, or appends NOTHING and returns
// false when `v` is in no arm kind `k` can be read from:
//   * F80 / F128 — a const-folded `WideFloatValue` OF THE SAME KIND (its `pack()`
//     IS the on-disk layout: x87 10 significant bytes + 6 zero pad, or
//     binary128), or a host `double` widened losslessly (`appendF80Extended` /
//     `appendF128`). A `WideFloatValue` of the OTHER wide kind is refused: an
//     F128 pattern in an F80 slot is a different number, not a rounding.
//   * I128 / U128 — the two little-endian 64-bit limbs of a `BitIntValue` (at
//     most two; a third would be bits the slot cannot hold), a plain
//     `std::uint64_t` (zero-extended), a `std::int64_t` (SIGN-extended — a
//     negative value's high limb is all ones) or a `bool`. Keyed on the
//     declared KIND, never on the variant: a fits-in-64 `__int128` folds into a
//     plain integer arm (D-CSUBSET-INT128-DATA-GLOBAL's recorded lesson).
[[nodiscard]] bool
appendSixteenByteScalarImage(std::vector<std::uint8_t>& bytes,
                             MirLiteralValue const& v, TypeKind k) {
    if (k == TypeKind::F80 || k == TypeKind::F128) {
        if (auto const* wf = std::get_if<WideFloatValue>(&v.value)) {
            if (wf->kind() != k) return false;
            appendWideFloatBits(bytes, *wf);
            return true;
        }
        if (auto const* dv = std::get_if<double>(&v.value)) {
            if (k == TypeKind::F80) appendF80Extended(bytes, *dv);
            else                    appendF128(bytes, *dv);
            return true;
        }
        return false;
    }
    if (k != TypeKind::I128 && k != TypeKind::U128) return false;
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
    if (auto const* bv = std::get_if<BitIntValue>(&v.value)) {
        auto const& limbs = bv->limbs();
        if (limbs.size() > 2) return false;
        lo = limbs.size() > 0 ? limbs[0] : 0ull;
        hi = limbs.size() > 1 ? limbs[1] : 0ull;
        // A 1-limb payload declared 128 bits wide still needs its high limb
        // materialized; `BitIntValue` keeps its limbs sign-clean, so the
        // extension is the sign of the declared value.
        if (limbs.size() < 2 && bv->isSigned() && (lo >> 63) != 0) hi = ~0ull;
    } else if (auto const* uv = std::get_if<std::uint64_t>(&v.value)) {
        lo = *uv;                       // zero-extends
    } else if (auto const* iv = std::get_if<std::int64_t>(&v.value)) {
        lo = static_cast<std::uint64_t>(*iv);
        if (*iv < 0) hi = ~0ull;        // sign-extends
    } else if (auto const* bo = std::get_if<bool>(&v.value)) {
        lo = *bo ? 1ull : 0ull;
    } else {
        return false;
    }
    appendLE(bytes, lo, 8);
    appendLE(bytes, hi, 8);
    return true;
}

// Decode a SCALAR literal to the little-endian bit pattern to emit (zero-
// extended into a u64; the writer takes the low `width` bytes). Handles
// bool / signed / unsigned integers and F32/F64 — a `double`-arm value is
// NARROWED to `float` for an F32 leaf (writing the low 4 bytes of the
// binary64 pattern would be garbage, not a valid binary32). Returns nullopt
// for kinds the pool cannot represent as plain bytes: F16/F80/F128 — any float
// wider than F64 or otherwise without a lossless host-`double` arm (F80 joined
// with FC17.9(e)) — or a non-scalar / monostate variant (string /
// MirAggregateValue / a LD-3 `WideFloatValue` folded leaf / unknown). The SOLE
// scalar-encode chokepoint for the widths a u64 can carry — the scalar-global
// arm and the aggregate-leaf recursion both route through it, so the int/float
// value semantics can never drift between the two encoders. The four 16-byte
// kinds (F80/F128/I128/U128) are sent by BOTH callers to
// `appendSixteenByteScalarImage` before this chokepoint; the nullopt it returns
// for them is the backstop that keeps a u64 from standing in for 16 bytes.
[[nodiscard]] std::optional<std::uint64_t>
decodeScalarLiteralBits(MirLiteralValue const& v, TypeKind k) noexcept {
    // D-CSUBSET-INT128-DATA-GLOBAL (TF-C94): a 128-bit integer is 16 bytes —
    // WIDER than the u64 this chokepoint returns — so it cannot pass through,
    // exactly like F80/F128. Checked FIRST, before the integer arms, because a
    // 128-bit value's folded literal IS a plain u64/i64 arm (it is the CONTAINER
    // that is too narrow, not the variant that is wrong): without this the u64
    // arm below would happily return the low 8 bytes and a caller would write
    // them as if they were the whole value. Both callers send these kinds to
    // `appendSixteenByteScalarImage` first; this is the backstop.
    if (k == TypeKind::I128 || k == TypeKind::U128) return std::nullopt;
    // Same argument for the >64-bit FLOAT kinds, and it closes a real mismatch
    // between this function's contract and its code: the header above has always
    // promised nullopt for F16/F80/F128, but that promise was honoured only on
    // the `double` arm below. A u64/i64/bool-variant literal carrying an F80/F128
    // kind — the "malformed pool entry" the two F80/F128 arms in
    // `lowerMirGlobalsToDataItems` say falls through to "the decode chokepoint's
    // fail-loud" — actually returned a VALUE, and then `appendLE` was called with
    // width 16 on a u64: the same >>64 UB described there. Not a live miscompile
    // (an int literal assigned to a long double routes through the cast fold into
    // the `WideFloatValue` arm, so the audit could not reach it), but the two
    // comments described a wall that did not exist. Now it does.
    if (k == TypeKind::F16 || k == TypeKind::F80 || k == TypeKind::F128)
        return std::nullopt;
    if (std::holds_alternative<std::uint64_t>(v.value))
        return std::get<std::uint64_t>(v.value);
    if (std::holds_alternative<std::int64_t>(v.value))
        return static_cast<std::uint64_t>(std::get<std::int64_t>(v.value));
    if (std::holds_alternative<bool>(v.value))
        return std::get<bool>(v.value) ? 1u : 0u;
    if (std::holds_alternative<double>(v.value)) {
        double const dv = std::get<double>(v.value);
        std::uint64_t bits = 0;
        if (k == TypeKind::F32) {
            float const fv = static_cast<float>(dv);
            std::memcpy(&bits, &fv, sizeof(float));
        } else if (k == TypeKind::F64) {
            std::memcpy(&bits, &dv, sizeof(double));
        } else {
            return std::nullopt;   // F16/F80/F128 — no lossless pool arm
        }
        return bits;
    }
    return std::nullopt;   // monostate / string / MirAggregateValue
}

// ── D-CSUBSET-BITINT-DATA-GLOBAL: the SOLE `_BitInt(N)` STATIC-IMAGE producer ──
//
// A `_BitInt(N)` object occupies `sizeOfScalarOrBitInt` bytes and carries only N
// value bits (C23 6.2.6.2). Its image is therefore WIDER than the `std::uint64_t`
// `decodeScalarLiteralBits` returns whenever N > 64, which is why that chokepoint
// cannot carry one and why this producer exists beside it rather than inside it.
// ★ ONE producer, TWO call sites — the SCALAR-global arm of
// `lowerMirGlobalsToDataItems` and the aggregate-LEAF recursion in
// `encodeAggregateValue`. That is deliberate and it is the whole shape of this
// deferral's close: a scalar-only emitter would leave the leaf refusing a width
// the compiler demonstrably knows how to encode, i.e. the half-shipped multi-site
// contract. Both sites ask HERE; neither owns a second copy of the rule.
//
// ★★ PADDING BITS — THE DECISION, AND WHAT IT WAS MEASURED AGAINST.
// C23 6.2.6.1p6 leaves the values of padding bits UNSPECIFIED, so more than one
// image conforms and the choice must be made on evidence rather than defaulted
// into. ✔MEASURED (2026-08-27, clang 18.1.3 `-std=c23 -c`, ELF x86_64): clang
// ZERO-fills — `_BitInt(17) p = -3wb;` emits `fd ff 01 00`, `_BitInt(65) w = -1wb;`
// emits eight `ff` then `01` and seven `00`. ✔MEASURED on the same clang at -O0, a
// RUNTIME-computed `_BitInt(17) = -3` read back `fd ff 01 a1` — GARBAGE in the
// padding — so "clang's padding" is not even a stable target to copy; only its
// STATIC image is deterministic.
// ✔MEASURED on DSS, by execution, through a `unsigned char *` read of the object:
// a runtime `_BitInt(17) = -3` has byte 2 == 0xff and a runtime `_BitInt(65) = -1`
// has byte 8 == 0xff — DSS SIGN-EXTENDS the padding of a negative signed value,
// at BOTH the narrow and the wide width. That is not incidental: it is the
// `bitIntMask` (Shl/AShr) and `maskTopLimb` invariant, the one wrap chokepoint the
// whole `_BitInt` tier is built on, and `BitIntValue`'s host `wrapTo` mirrors it.
// ⇒ THIS PRODUCER SIGN-EXTENDS, because the static image is READ BY DSS's OWN
// RUNTIME. Zero-filling would match clang's bytes and then make
// `_BitInt(17) g = -3wb; g < 0` answer FALSE — the loaded container would hold
// +131069 — a silent miscompile of the compiler's own initializer. Agreement with
// the runtime is a correctness constraint; agreement with clang's padding is a
// preference the standard does not impose and clang itself does not keep.
// ⚠ The divergence from clang IS observable (a `union { _BitInt(17) b; unsigned u; }`
// reads 0x0001fffd there and 0xfffffffd here) and is recorded on the anchor row —
// it is a property of DSS's chosen `_BitInt` REPRESENTATION, decided in
// `hir_to_mir`'s wrap chokepoint, not of this emitter.
//
// ★ LIMB ORDER. The limbs are appended through `appendLittleEndianBytes`, the file's
// ONE byte-order chokepoint (`appendLE`'s loop, shared with the assembly-text data
// directives). This file has NO target-keyed byte order anywhere — every scalar,
// F80, F128 and the 128-bit arm are little-endian by construction — so giving
// `_BitInt` a private limb order would MINT the file's second byte-order mechanism,
// exactly what `appendLE`'s own comment forbids ("what must not exist is a SECOND
// byte-order loop"). Routing here means the day that chokepoint becomes byte-order
// aware from `.target.json`, `_BitInt` follows for free, with every other kind, at
// one site. The arithmetic is host-endian-independent (shifts of a `std::uint64_t`),
// so nothing here is host-keyed.
//
// ── The specific-cause carrier threaded through the static-data encoder ──
//
// D-DIAG-OVERLAP-REFUSAL-CODE-NOT-DISCRIMINATING (P65). This was a bare
// `std::string& why`, and that is exactly why every cause this encoder knows
// reached the user under ONE `DiagnosticCode`: prose carries no identity a
// consumer can filter, triage or grep on, so the render arms in
// `lowerMirGlobalsToDataItems` had nothing to key `emit` on but a constant. The
// messages discriminated; the code did not. A cause now travels WITH the code
// it belongs to, and the render arms ask the cause rather than deciding for it.
//
// ⚠ WHY ONE STRUCT AND NOT A SECOND OUT-PARAM. A `DiagnosticCode&` beside the
// `std::string&` is two values that must move together through three functions
// and every refusal in them; the first arm that sets one without the other
// files a cause under the wrong code SILENTLY. That is a wrong-answer failure,
// the class this file walls everywhere else. `set` is the ONLY writer and it
// writes both, so the pair cannot drift apart.
//
// ⓘ FIRST WRITER WINS, AND IT IS THE INNERMOST ONE. Every refusal path returns
// `false`/`nullopt` immediately after recording, and no caller records over a
// failed callee — so the cause that reaches the user is the one raised closest
// to the leaf that could not be encoded, which is the specific one.
struct EncodeFailure {
    // `None` + empty text ⇔ no specific cause was recorded. That state is REAL
    // and is what the callers' generic enumerating text exists for: several
    // refusal paths here are structural `return false`s that genuinely share
    // one cause and would gain nothing from prose of their own.
    DiagnosticCode code = DiagnosticCode::None;
    std::string    text;

    void set(DiagnosticCode c, std::string t) {
        code = c;
        text = std::move(t);
    }
    [[nodiscard]] bool empty() const noexcept { return text.empty(); }
    // The code this cause renders under. Both halves of the caller's choice —
    // which text, which code — are answered from the SAME predicate, so a
    // future arm cannot render specific prose under the generic code or the
    // reverse.
    [[nodiscard]] DiagnosticCode codeOr(DiagnosticCode fallback) const noexcept {
        return empty() ? fallback : code;
    }
};

// ★ FAIL LOUD, NEVER TRUNCATE. Every refusal writes `why`. The load-bearing one is
// the extension check: the bytes ABOVE the container are re-derived and must equal
// the value's extension byte, so a value that genuinely does not fit its declared
// container is REFUSED rather than silently narrowed. On a well-formed record it
// cannot fire (the container is sized FROM N); it is the guard that makes "a width
// we cannot emit correctly walls" true by construction rather than by argument.
// ── The `_BitInt` VALUE normalizer — step one of the producer, and its own function
// because the BIT-FIELD packer needs the same step and must not grow a second copy.
// Reads any integer literal arm and returns the value WRAPPED to the type's declared
// (N, signedness); nullopt (with `why`) for a non-`_BitInt` type, a malformed width,
// or a literal in no integer arm.
[[nodiscard]] std::optional<BitIntValue>
bitIntLiteralValue(MirLiteralValue const& v, TypeInterner const& in, TypeId ty,
                   EncodeFailure& why) {
    // `bitIntWidth`/`bitIntIsSigned` ABORT on a non-BitInt (deliberately — that
    // abort is the backstop for a missed gate), so the kind check is the contract,
    // not a defensive nicety.
    if (!ty.valid() || in.kind(ty) != TypeKind::BitInt) {
        why.set(DiagnosticCode::K_StaticDataEncoderInvariantBreach,
                "the `_BitInt` value normalizer was reached with a non-`_BitInt` "
                "type (D-CSUBSET-BITINT-DATA-GLOBAL)");
        return std::nullopt;
    }
    std::int64_t const n     = in.bitIntWidth(ty);
    bool const         signd = in.bitIntIsSigned(ty);
    if (n <= 0 || n > static_cast<std::int64_t>(kBitIntMaxWidth)) {
        why.set(DiagnosticCode::K_StaticDataEncoderInvariantBreach,
                std::format("`_BitInt({})` has a width outside [1,{}] — a malformed "
                            "interned record; refusing rather than guessing a "
                            "container (D-CSUBSET-BITINT-DATA-GLOBAL)",
                            n, kBitIntMaxWidth));
        return std::nullopt;
    }
    // ⓘ THE FOUR INTEGER LITERAL ARMS ARE ALL REAL AND NONE IMPLIES ANOTHER — the
    // lesson [[D-CSUBSET-INT128-DATA-GLOBAL]] paid for. A `_BitInt` initializer can
    // fold into the `BitIntValue` pool arm, but an ordinary `std::int64_t` /
    // `std::uint64_t` / `bool` arm reaches here too (the narrowest arm that holds the
    // value wins). Keying on the declared KIND and then accepting every integer arm
    // catches all four; keying on the VARIANT would miss three of them.
    std::optional<BitIntValue> src;
    if (auto const* bv = std::get_if<BitIntValue>(&v.value))            src = *bv;
    else if (auto const* u = std::get_if<std::uint64_t>(&v.value))
        src = BitIntValue::fromU64(*u, 64u, /*isSigned=*/false);
    else if (auto const* i = std::get_if<std::int64_t>(&v.value))
        src = BitIntValue::fromI64(*i, 64u, /*isSigned=*/true);
    else if (auto const* b = std::get_if<bool>(&v.value))
        src = BitIntValue::fromU64(*b ? 1u : 0u, 1u, /*isSigned=*/false);
    if (!src.has_value()) {
        why.set(DiagnosticCode::K_StaticDataEncoderInvariantBreach,
                std::format("`_BitInt({})` has an initializer in no integer literal "
                            "arm — refusing rather than emitting a fabricated image "
                            "(D-CSUBSET-BITINT-DATA-GLOBAL)", n));
        return std::nullopt;
    }
    // C 6.3.1.3 conversion of the folded value to the DECLARED (N, signedness).
    // Routed through `BitIntValue`'s own conversion rule rather than re-implemented:
    // every binary op converts BOTH operands to the result type FIRST, so an ADD OF
    // ZERO at (N, signd) IS that conversion — identity when the value already
    // carries the declared type, and the correct sign/zero extension when a narrower
    // literal type (`-1wb` is `_BitInt(2)`) is initializing a wider object.
    return BitIntValue::add(*src, BitIntValue{}, static_cast<std::uint32_t>(n), signd);
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
encodeBitIntImage(MirLiteralValue const& v, TypeInterner const& in, TypeId ty,
                  DataModel dm, EncodeFailure& why) {
    auto const valOpt = bitIntLiteralValue(v, in, ty, why);
    if (!valOpt.has_value()) return std::nullopt;   // `why` already written
    BitIntValue const& val   = *valOpt;
    std::int64_t const n     = in.bitIntWidth(ty);
    // ⓘ THE DECLARED SIGNEDNESS IS NO LONGER READ HERE, and its absence is the
    // point (D-CSUBSET-BITINT-PADDING-POLICY-HAS-THREE-OWNERS): `bitIntLiteralValue`
    // already normalized the value to THIS type's (N, signedness), so `val` carries
    // it, and asking the interner a second time is how a producer grows a second
    // opinion about a value it was handed. `val.paddingByte()` below is the only
    // consumer that ever needed it.
    // The width comes from `sizeOfScalarOrBitInt` — the TypeId-aware companion to
    // `scalarByteSize`, which is the layout authority's OWN size ladder and the one
    // this file's callers are documented to use for a data-global leaf. Not
    // re-derived here: a second ladder is a second ABI.
    auto const wOpt = sizeOfScalarOrBitInt(in, ty, dm);
    if (!wOpt.has_value() || *wOpt == 0) {
        why.set(DiagnosticCode::K_StaticDataEncoderInvariantBreach,
                std::format("`_BitInt({})` has no computable container size "
                            "(D-CSUBSET-BITINT-DATA-GLOBAL)", n));
        return std::nullopt;
    }
    std::size_t const width     = static_cast<std::size_t>(*wOpt);
    auto const&       limbs     = val.limbs();
    std::size_t const limbBytes = limbs.size() * 8u;
    if (limbs.empty() || width > limbBytes) {
        why.set(DiagnosticCode::K_StaticDataEncoderInvariantBreach,
                std::format("`_BitInt({})` reserves {} container bytes but its wrapped "
                            "value carries only {} — refusing rather than emitting a "
                            "SHORT image (D-CSUBSET-BITINT-DATA-GLOBAL)",
                            n, width, limbBytes));
        return std::nullopt;
    }
    // ★ WALL, NEVER TRUNCATE. Bits at and above `width*8` are outside the container
    // and must therefore be pure extension of the value at width N. Re-derive them
    // and compare; a mismatch means emitting the container would DROP VALUE BITS, so
    // the image is refused.
    // D-CSUBSET-BITINT-PADDING-POLICY-HAS-THREE-OWNERS: this byte used to be spelled
    // `(signd && val.signBitSet()) ? 0xFFu : 0x00u` HERE — the THIRD independent
    // statement of a rule whose owner is `bitIntPadding` in `bit_int_value.hpp`, and
    // the one tier where a divergence from the other two is a SILENT MISCOMPILE
    // rather than a refusal (this image is read back by DSS's own runtime). It now
    // ASKS the value what its padding is.
    std::uint8_t const ext = val.paddingByte();
    for (std::size_t j = width; j < limbBytes; ++j) {
        auto const byte =
            static_cast<std::uint8_t>((limbs[j / 8u] >> ((j % 8u) * 8u)) & 0xFFu);
        if (byte != ext) {
            why.set(DiagnosticCode::K_StaticDataEncoderInvariantBreach,
                    std::format("`_BitInt({})` value byte {} lies above its {}-byte "
                                "container and is 0x{:02x}, not the 0x{:02x} "
                                "extension — emitting the container would DROP value "
                                "bits, so the image is refused rather than truncated "
                                "(D-CSUBSET-BITINT-DATA-GLOBAL)",
                                n, j, width, byte, ext));
            return std::nullopt;
        }
    }
    std::vector<std::uint8_t> out;
    out.reserve(width);
    std::size_t remaining = width;
    for (std::uint64_t limb : limbs) {
        if (remaining == 0u) break;
        std::size_t const take = remaining < 8u ? remaining : 8u;
        appendLittleEndianBytes(out, limb, take);   // ★ the ONE byte-order chokepoint
        remaining -= take;
    }
    if (out.size() != width) {
        why.set(DiagnosticCode::K_StaticDataEncoderInvariantBreach,
                std::format("`_BitInt({})` encoded to {} bytes but its container "
                            "reserves {} — the encoder and the layout disagree "
                            "(D-CSUBSET-BITINT-DATA-GLOBAL)", n, out.size(), width));
        return std::nullopt;
    }
    return out;
}

// D-MIR-OVERLAP-STRUCT-ZERO-INIT: is every leaf of this static initializer a ZERO
// whose object representation is all-zero BYTES? The static-data twin of the MIR
// lowering's `isAllZeroAggregateInit`, and deliberately the same admissions and the
// same CONSERVATIVE default — an unrecognized arm answers FALSE so the caller keeps
// its refusal. `-0.0` is rejected: numerically zero, but its sign bit is SET, so a
// pre-zeroed buffer does NOT carry its bytes. Symbol addresses / wide floats /
// strings each need their own representation proof and get none here.
// D-CSUBSET-BITINT-DATA-GLOBAL: `_BitInt` NOW HAS ONE, and this is the whole proof.
// `BitIntValue`'s post-`wrapTo` invariant is that the limbs are "clean" — bits above
// N are the sign extension or zero — so `isZero()` (every limb 0) holds exactly when
// the extension is zero too, i.e. when `encodeBitIntImage` would emit all-zero bytes
// at any container width. A NEGATIVE value can never report zero, so the sign-extended
// padding this file emits cannot be mistaken for a zero image.
[[nodiscard]] bool isAllZeroMirLiteral(MirLiteralValue const& v) {
    if (auto const* b = std::get_if<bool>(&v.value))          return !*b;
    if (auto const* i = std::get_if<std::int64_t>(&v.value))  return *i == 0;
    if (auto const* u = std::get_if<std::uint64_t>(&v.value)) return *u == 0;
    if (auto const* w = std::get_if<BitIntValue>(&v.value))   return w->isZero();
    if (auto const* d = std::get_if<double>(&v.value))
        return *d == 0.0 && !std::signbit(*d);
    if (auto const* a = std::get_if<MirAggregateValue>(&v.value)) {
        for (MirLiteralValue const& f : a->fields)
            if (!isAllZeroMirLiteral(f)) return false;
        return true;
    }
    return false;
}

// Recursively encode an aggregate (or scalar) literal `v` of type `ty` into
// `buf` at absolute byte offset `base`. `buf` is pre-sized to the TOP
// aggregate's layout `size` and zero-filled by the caller, so every padding
// byte, partial-init tail, and union slack stays zero by construction — only
// the provided leaves are written. Walks the TYPE tree and the init-VALUE
// tree in lockstep — the MIRROR of `collectLeaves` (aggregate_abi.cpp), but
// writing the VALUE bytes instead of collecting ABI leaves. Returns false
// (the fail-loud signal) on any un-computable layout, a type↔value shape
// mismatch, an over-long initializer, or an unencodable leaf. PURE
// type/value-driven — no target/format/language identity branch (the per-ABI
// layout enters ONLY through `lp`/`dm`).
//
// Field/element pairing (zero-fills already normalized at HIR lowering, see
// cst_to_hir.cpp ConstructAggregate):
//   * struct — one value field per type field (omitted slots are synthetic
//     zero-fills) → `agg.fields[i]` ↔ field `i` at `fieldOffsets[i]`.
//   * union  — a brace-init sets the FIRST member only → a 1-field value →
//     field 0 ↔ member 0 at offset 0; the union's remaining bytes stay zero.
//   * array  — `agg.fields[i]` ↔ element `i` at `base + i*elemStride`; a
//     short initializer leaves the trailing elements zero.
//
// c67 (D-CSUBSET-AGGREGATE-GLOBAL-SYMBOL-ADDRESS): a scalar leaf may be a
// LINK-TIME-CONSTANT symbol address (a fn/`&global`/string member of the
// aggregate — the F5 / D-CSUBSET-SYMBOL-ADDRESS-GLOBAL mechanism, extended from
// top-level scalars to aggregate MEMBERS). Such a leaf emits an abs64
// RELOCATION at its member offset (into `relocs`) over the pre-zeroed 8-byte
// pointer slot; `absPtrRelocKind` is the target's abs64 tag (nullopt ⇒ the
// target declares no abs64 reloc ⇒ fail loud, as the F5 scalar arm does).
//
// A1 (audit fold) — WHY the `why` out-param: this is a free function in the
// file's anonymous namespace, so it cannot reach the caller's `emit()` (a lambda
// closing over a `DiagnosticReporter` local to `lowerMirGlobalsToDataItems`).
// Every `return false` therefore surfaced through ONE generic caller message
// that enumerates the causes it knew about ("a type↔value shape mismatch or an
// unencodable leaf — e.g. an f16 leaf, or an address-relocated leaf…"). The
// overlapping-struct refusal below is NONE of those, so a user hitting it —
// MEASURED reachable today as `static ULARGE_INTEGER g = {1,0,0};` on pe64,
// `windows.json`'s explicit-offset OVERLAY — was pointed at the wrong thing. An
// arm with a SPECIFIC cause writes it into `why`; the recursion threads the SAME
// reference, so a nested member's cause reaches the top unchanged, and the
// caller prefers a non-empty `why` over its generic text. Left EMPTY the
// behaviour is bit-identical to before — no arm is forced to invent a reason it
// does not have, and the generic message keeps covering the arms that share it.
[[nodiscard]] bool
encodeAggregateValue(TypeId ty, MirLiteralValue const& v,
                     TypeInterner const& in, AggregateLayoutParams lp,
                     DataModel dm, std::vector<std::uint8_t>& buf,
                     std::uint64_t base, std::vector<Relocation>& relocs,
                     std::optional<RelocationKind> absPtrRelocKind,
                     EncodeFailure&           why) {
    // D-CSUBSET-ENUM-GLOBAL-CODEGEN: an ENUM-typed member/element is a scalar
    // leaf whose representation is its UNDERLYING integer's — the member of a
    // `struct S { enum E e; }`, an element of `enum E a[2] = {A,B}`, the first
    // member of a `union U { enum E e; }`. Projecting HERE (rather than at the
    // leaf) keeps the Struct/Union/Array dispatch below untouched (those
    // kinds project to themselves) while the leaf's `scalarByteSize` /
    // `decodeScalarLiteralBits` see a sized integer instead of the un-sized Enum
    // marker they used to reject. `ty` is unchanged, so `computeLayout` still
    // lays the aggregate out from the DECLARED types.
    TypeKind const k = materialScalarKind(in, ty);

    if (k == TypeKind::Struct || k == TypeKind::Union) {
        if (!std::holds_alternative<MirAggregateValue>(v.value)) return false;
        auto const& agg = std::get<MirAggregateValue>(v.value);
        // c107 (D-FFI-DESCRIPTOR-UNION-OVERLAY): a static initializer of a struct
        // whose members SHARE BYTES would positionally write fields whose byte
        // ranges overlap — a later field silently overwrites an earlier one in
        // `buf`. Refuse LOUD rather than emit wrong static bytes.
        //
        // D-MIR-OVERLAP-STRUCT-ZERO-INIT: except when the initializer is ALL ZERO —
        // `{}` / `{0}` denote a whole object of zero bytes, which is unambiguous no
        // matter how many members alias those bytes. `buf` is PRE-ZEROED to the
        // layout size by the caller (`d.bytes.assign(lay->size, 0u)`), so the correct
        // encoding is to write NOTHING. The runtime twin of this rule lives at the
        // MIR brace-init lowering; both ask the same two questions — does the field
        // set overlap, and is every supplied element zero — and both key on ACTUAL
        // overlap, never on the mere presence of explicit offsets.
        //
        // ★ THE FALL-THROUGH IS A THIRD OUTCOME, not a leftover: a DISJOINT
        // explicit-offset struct (a descriptor pinning a foreign layout that simply
        // is not the natural one) drops past this gate and encodes MEMBER-WISE at
        // `lay->fieldOffsets[i]` — which for such a struct are the DECLARED offsets,
        // not natural ones (type_layout.cpp's explicit-offset arm copies them
        // verbatim). Refusing it was a FALSE refusal; nothing about disjoint offsets
        // is ambiguous, so the ordinary positional walk below is exactly right.
        // Pinned byte-exactly by AsmAggregateGlobal.DisjointExplicitOffsetStruct*.
        //
        // A1: the non-zero half gets its OWN reason. The caller's generic text
        // names shape mismatches and unencodable leaves; this is neither, and the
        // remedy is specific and actionable, so it is stated in the SAME words the
        // MIR twin uses (hir_to_mir.cpp `lowerAggregateInitIntoSlot`) — one rule,
        // one wording, whichever tier the user's declaration happens to hit.
        //
        // ★ D-CORE-COMPOSITE-OVERLAP-CLAIM-BLIND-TO-BITFIELDS — BIT-FIELD-FIRST
        // ORDERING, THE SAME ONE `lowerAggregateInitIntoSlot` ALREADY HAS. The
        // layout is hoisted ABOVE the gate (it was computed two lines below anyway)
        // so the gate is asked ONLY of a composite with no bit-fields. That is not a
        // way around a truthful predicate — it is the routing this arm always owed:
        // a full-width positional write of ANY bit-field member clobbers its
        // co-resident neighbours whatever the overlap answer is, which is why the
        // packing loop below exists and why it must OWN every bit-field composite.
        // `bitFields` non-empty ⇔ the composite has a bit-field (the layout
        // authority's invariant), and it is the exact analogue of the MIR twin's
        // `hasBitfieldMember` test.
        //
        // ⓘ WHAT THE ORDER BUYS: with the gate asked only when `bitFields` is empty,
        // a `true` can only have come from the EXPLICIT-OFFSET channel — so the
        // refusal text below stays accurate now that `compositeFieldsOverlap` also
        // answers `true` for bit-field composites. Hoisting the layout also means an
        // UN-SIZEABLE aggregate fails loud on its own cause here rather than through
        // the gate's conservative `true`, which would have named the wrong reason.
        //
        // ★ D-CORE-COMPOSITE-OVERLAP-CLAIM-BLIND-TO-UNIONS: A UNION IS ROUTED PAST
        // THIS GATE, the exact twin of the route `hir_to_mir.cpp`'s
        // `lowerAggregateInitIntoSlot` takes, and for the same reason. Once
        // `compositeFieldsOverlap` tells the truth it answers `true` for every union
        // with two or more sizeable members — they all sit at byte 0. But this gate
        // asks whether a POSITIONAL member-wise write would clobber a sibling, and a
        // union initializer names exactly ONE member (C 6.7.9p17), so the walk below
        // performs exactly one encode at `fieldOffsets[0] == 0` into a `buf` the
        // caller pre-zeroed to the layout size. Nothing can be lost. Without this
        // route the truthful predicate would refuse `static union U u = {1};`, which
        // gcc, clang and MSVC all accept.
        //
        // ⚠ THE ONE-CHILD PREMISE IS ASSERTED, NOT ASSUMED — see the MIR twin's note
        // for the three-guarantee measurement (both HIR producers plus
        // `HirVerifier::checkConstructAggregate`). All three live upstream of the
        // literal pool this encoder reads, and none of them runs here, so the
        // refusal is expected never to fire and is kept anyway: skipping the gate for
        // unions would otherwise silently GRANT the clobber the gate exists to
        // refuse, in the DATA SECTION, where the second member's bytes overwrite the
        // first's at the same offset with nothing to observe it.
        auto const lay = computeLayout(ty, in, lp, dm);
        if (!lay.has_value()) return false;
        bool const isUnion = (k == TypeKind::Union);
        if (isUnion && agg.fields.size() > 1) {
            why.set(DiagnosticCode::K_OverlappingStaticInitUnsupported,
                    "static initialization of a union supplied more than one member — "
                    "a union initializer names exactly one member (C 6.7.9p17), and the "
                    "union route past the overlapping-members gate is valid only for "
                    "that single write");
            return false;
        }
        if (!isUnion && lay->bitFields.empty() && compositeFieldsOverlap(ty, in, lp, dm)) {
            if (!isAllZeroMirLiteral(v)) {
                why.set(DiagnosticCode::K_OverlappingStaticInitUnsupported,
                        "static initialization of an overlapping explicit-offset "
                        "struct is unsupported — its members share bytes; assign "
                        "the members individually (an ALL-ZERO initializer `{0}` / "
                        "`{}` IS supported — D-MIR-OVERLAP-STRUCT-ZERO-INIT)");
                return false;
            }
            return true;
        }
        auto const ops = in.operands(ty);
        if (ops.size() != lay->fieldOffsets.size()) return false;
        if (agg.fields.size() > ops.size()) return false;   // too many inits → fail loud
        // FC8 D-CSUBSET-BITFIELD-INIT: a struct/union WITH bit-fields packs each
        // bit-field's value into its allocation unit (`buf` is pre-zeroed, so the
        // OR is correct + leaves un-covered bits / omitted fields at 0). Fields
        // sharing a unit share `fieldOffsets[i]`, so OR-ing each one in at its
        // `bitOffset` accumulates into the same bytes. Ordinary fields among the
        // bit-fields (`unitBytes == 0`) recurse normally. `bitFields` non-empty
        // ⇔ the struct has a bit-field (the layout authority's invariant); the
        // byte path below is byte-identical for a bit-field-free composite.
        for (std::size_t i = 0; i < agg.fields.size(); ++i) {
            bool const isBitfield =
                i < lay->bitFields.size() && lay->bitFields[i].unitBytes != 0;
            if (!isBitfield) {
                // A zero-width bit-field marker (`unsigned : 0;`) has no storage
                // (`unitBytes == 0` AND `fieldBitWidth` present); its `fieldOffsets`
                // entry aliases the NEXT unit, so a full-width write here would
                // touch that neighbour unit. Skip it (its synthetic child is 0).
                if (in.fieldBitWidth(ty, i).has_value()) continue;
                if (!encodeAggregateValue(ops[i], agg.fields[i], in, lp, dm, buf,
                                          base + lay->fieldOffsets[i], relocs,
                                          absPtrRelocKind, why))
                    return false;
                continue;
            }
            // Pack one bit-field: read its scalar value, mask to width, shift to
            // bitOffset, OR into the unit at `base + fieldOffsets[i]`. The unit
            // load/store width is `unitBytes` (little-endian, matching the MIR
            // read-modify-write codegen + the layout's LSB-first packing).
            BitFieldPlacement const& p = lay->bitFields[i];
            // ⚠ THE u64 PACKING BELOW IS ONLY VALID UP TO 64 BITS AND 8 UNIT BYTES,
            // and past those bounds it is not merely approximate — it is the SAME
            // `>> (j*8)` UB the 128-bit arm was walled for: `placed` is a
            // `std::uint64_t`, so a `unitBytes > 8` unit shifts by 64..120 and, on
            // both shipped host arches, REPEATS the low 8 bytes into the high ones;
            // and `bitWidth > 64` saturates the mask to `~0ull`, silently keeping
            // bits the field cannot hold. Reachable through a wide-`_BitInt`-backed
            // bit-field, whose allocation unit is its 16-byte-or-wider container.
            // A width this packer cannot encode WALLS; it never emits.
            if (p.bitWidth > 64u || p.unitBytes > 8u) {
                why.set(DiagnosticCode::K_NoMatchingObjectFormat,
                        "a bit-field wider than 64 bits (or in an allocation unit "
                        "wider than 8 bytes) cannot be packed by the u64 static "
                        "initializer packer — refusing rather than emitting bits it "
                        "would silently drop or repeat "
                        "(D-CSUBSET-BITINT-DATA-GLOBAL)");
                return false;
            }
            // D-CSUBSET-ENUM-BITFIELD: an enum-typed bit-field decodes at its
            // UNDERLYING integer — the same projection the MIR bit-field
            // extract/insert already performs. (An `int`-backed enum decoded
            // identically before, because this chokepoint's integer arms ignore
            // the kind; a 128-bit-backed one did NOT, and silently yielded its
            // low 8 bytes instead of the nullopt that fails loud.)
            //
            // D-CSUBSET-BITINT-DATA-GLOBAL: a `_BitInt`-typed bit-field —
            // `struct B { unsigned _BitInt(17) a : 5; }` with a STATIC initializer —
            // takes the normalizer, not the u64 chokepoint. ✔MEASURED before this
            // arm existed: it refused with the generic aggregate text, because
            // `decodeScalarLiteralBits` has no `BitIntValue` arm at all, so the
            // const-folded `_BitInt` bit-field value returned nullopt. The RUNTIME
            // twin (`c23_bitint_bitfield`) has always worked; only the static
            // initializer was walled, and nothing named it. `low64()` is the whole
            // value here because the guard above bounds the field at 64 bits, and
            // the mask below takes the low `bitWidth` of it — the same low-bits rule
            // the MIR bit-field insert applies, so a signed negative field packs
            // identically whichever tier writes it.
            std::optional<std::uint64_t> bitsOpt;
            if (materialScalarKind(in, ops[i]) == TypeKind::BitInt) {
                auto const bv = bitIntLiteralValue(agg.fields[i], in, ops[i], why);
                if (!bv.has_value()) return false;    // `why` already written
                bitsOpt = bv->low64();
            } else {
                bitsOpt = decodeScalarLiteralBits(agg.fields[i],
                                                  materialScalarKind(in, ops[i]));
            }
            if (!bitsOpt.has_value()) return false;   // non-int bit-field leaf → fail loud
            std::uint64_t const mask =
                p.bitWidth >= 64 ? ~0ull : ((1ull << p.bitWidth) - 1);
            std::uint64_t const placed = (*bitsOpt & mask) << p.bitOffset;
            std::uint64_t const unitBase = base + lay->fieldOffsets[i];
            if (unitBase + p.unitBytes > buf.size()) return false;  // layout↔buf disagreement
            for (std::uint32_t j = 0; j < p.unitBytes; ++j)
                buf[unitBase + j] |= static_cast<std::uint8_t>((placed >> (j * 8)) & 0xFFu);
        }
        return true;
    }

    // ── D-CSUBSET-COMPLEX-STATIC-STORAGE-INITIALIZER-HAS-NO-CONSTANT-IMAGE ──
    // A C99 `_Complex` leaf — the whole of `double _Complex g = 1.0;` at file
    // scope, a `static _Complex` LOCAL (lowered as a global, which is why this is
    // not a file-scope-only shape), and equally a complex MEMBER of a struct or
    // ELEMENT of an array. C 6.2.5p13: a complex lays out EXACTLY like an array of
    // TWO element-float components, real first — so this arm is the Array arm with
    // the count FIXED at 2 and the component type taken from the interner.
    //
    // ★ THE OFFSETS ARE `elemLay->size`, NOT THE ALIGNED STRIDE, and the choice is
    // deliberate: `computeLayout`'s own Complex arm returns `StructLayout{es * 2,
    // elem->align, …}` — it lays the imaginary component at exactly `es`, where the
    // Array arm rounds `es` UP to the element's alignment first. For every element
    // the layout authority sizes today the two coincide — F32 and F64 have size ==
    // align, and an x87 F80 is STORED 16/16 (10 significant bytes + 6 pad; this note
    // used to call it a 10-byte element, a size the authority never answers) — but
    // this arm keeps the LAYOUT AUTHORITY's formula, exactly as the Array arm keeps
    // its own, so no element can land `im` off the offset every reader —
    // `complexParts`/`loadComplex` in hir_to_mir, `collectLeaves` in aggregate_abi —
    // expects.
    // ⓘ F80/F128 elements DO reach here since P68 round 8: the MIR classifier folds
    // their components as `WideFloatValue`s
    // (D-CSUBSET-COMPLEX-LONG-DOUBLE-STATIC-INITIALIZER-REFUSED), and each lands in
    // the 16-byte leaf arm below — the imaginary one at 16, the element's second
    // 16-byte slot.
    //
    // ⚠ A SHORT VALUE IS NOT A ZERO IMAGINARY PART BY ACCIDENT — it is one BY
    // CONSTRUCTION: `buf` is pre-zeroed to the layout size by the caller, so a
    // 1-field value writes `re` and leaves `im` as the zero C 6.3.1.7 requires for a
    // real→complex conversion. More than 2 fields is a shape the type cannot hold,
    // and it FAILS LOUD rather than writing the first two and dropping the rest.
    if (k == TypeKind::Complex) {
        if (!std::holds_alternative<MirAggregateValue>(v.value)) {
            why.set(DiagnosticCode::K_StaticDataEncoderInvariantBreach,
                    "a `_Complex` static initializer must arrive as a two-component "
                    "aggregate value (real, imaginary) — a scalar leaf cannot carry "
                    "both components "
                    "(D-CSUBSET-COMPLEX-STATIC-STORAGE-INITIALIZER-HAS-NO-CONSTANT-IMAGE)");
            return false;
        }
        auto const& agg = std::get<MirAggregateValue>(v.value);
        auto const  ops = in.operands(ty);
        if (ops.empty()) return false;                  // malformed interned record
        TypeId const elem    = ops[0];
        auto const   elemLay = computeLayout(elem, in, lp, dm);
        if (!elemLay.has_value()) return false;
        if (agg.fields.size() > 2) {
            why.set(DiagnosticCode::K_StaticDataEncoderInvariantBreach,
                    "a `_Complex` static initializer carries more than two components "
                    "— refusing rather than dropping the extras "
                    "(D-CSUBSET-COMPLEX-STATIC-STORAGE-INITIALIZER-HAS-NO-CONSTANT-IMAGE)");
            return false;
        }
        for (std::size_t i = 0; i < agg.fields.size(); ++i)
            if (!encodeAggregateValue(elem, agg.fields[i], in, lp, dm, buf,
                                      base + i * elemLay->size, relocs,
                                      absPtrRelocKind, why))
                return false;
        return true;
    }

    if (k == TypeKind::Array) {
        // c62 / C14 (C 6.7.9p14 + 6.2.5p15, D-CSUBSET-STRING-LITERAL-ARRAY-ZERO-FILL):
        // a CHARACTER-ARRAY field/element initialized by a STRING LITERAL
        // (`char zName[7] = "hour";` / `unsigned char u[7] = "hour";` inside a static
        // aggregate). The const-eval folds the string-literal leaf to a `std::string`
        // value (NOT a per-char `MirAggregateValue`), so the element here carries a
        // string arm. Write the string bytes + the implicit NUL at `base`; the caller
        // pre-zeroed `buf` to the full layout size, so the trailing N−(len+1) bytes
        // are already zero (the C 6.7.9p14 zero-fill) — the aggregate twin of the
        // standalone string-literal global's producer-side padding. Element must be a
        // 1-byte CHARACTER type — char / signed char (I8) / unsigned char (U8), the
        // three C 6.2.5p15 character types, all 1 byte with identical string bytes
        // (LOCKSTEP with type_rules.hpp isCharacterType / the coerce realize arm); the
        // NUL+bytes must fit the array's byte extent (a layout↔value disagreement
        // fails loud). A non-character array with a string value, or an over-long
        // string, falls through to the shape-mismatch `false` below.
        //
        // ⚠ D-CSUBSET-ENUM-GLOBAL-CODEGEN — THE ONE INTERNER-DERIVED KIND IN THIS
        // FILE DELIBERATELY *NOT* PROJECTED THROUGH `materialScalarKind`, and the
        // reason is the distinction the projection exists to respect. This asks
        // "is the element a CHARACTER TYPE?" — a C 6.2.5p15 TYPE-IDENTITY question,
        // not a representation-width one. `enum E : unsigned char` REPRESENTS as U8
        // but is NOT a character type, and C 6.7.9 does not admit a string literal
        // as its initializer; projecting here would silently accept
        // `enum E : unsigned char a[3] = "ab";`. Identity questions read the
        // DECLARED kind; representation questions read the material one.
        if (std::holds_alternative<std::string>(v.value)
            && !in.operands(ty).empty()
            && (in.kind(in.operands(ty)[0]) == TypeKind::Char
                || in.kind(in.operands(ty)[0]) == TypeKind::I8
                || in.kind(in.operands(ty)[0]) == TypeKind::U8)) {
            auto const& s   = std::get<std::string>(v.value);
            auto const  lay = computeLayout(ty, in, lp, dm);
            if (!lay.has_value()) return false;
            if (base + s.size() + 1 > buf.size()) return false;   // NUL must fit
            if (s.size() + 1 > lay->size)          return false;   // over-long → loud
            for (std::size_t j = 0; j < s.size(); ++j)
                buf[base + j] = static_cast<std::uint8_t>(s[j]);
            // buf[base + s.size()] (the NUL) and the remaining bytes stay 0
            // (caller pre-zeroed) — the trailing zero-fill.
            return true;
        }
        if (!std::holds_alternative<MirAggregateValue>(v.value)) return false;
        auto const& agg   = std::get<MirAggregateValue>(v.value);
        auto const  ops   = in.operands(ty);
        auto const  scals = in.scalars(ty);
        // `scals[0]` is the element count (signed in the pool); a negative count
        // is malformed — reject it (mirrors computeLayout's array guard) before
        // the unsigned cast, so it can't become a huge `count`.
        if (ops.empty() || scals.empty() || scals[0] < 0) return false;
        TypeId const        elem  = ops[0];
        std::uint64_t const count = static_cast<std::uint64_t>(scals[0]);
        auto const elemLay = computeLayout(elem, in, lp, dm);
        if (!elemLay.has_value()) return false;
        if (agg.fields.size() > count) return false;        // too many inits → fail loud
        // Stride EXACTLY as computeLayout sizes the array (`stride * len`, where
        // stride = align-rounded element size) — NOT bare `elemLay->size`. They
        // coincide for every complete C type (size is a multiple of align), but
        // matching the layout authority's formula keeps element placement
        // correct-by-construction rather than relying on that invariant.
        std::uint64_t const stride = elemLay->align.alignUp(elemLay->size);
        for (std::size_t i = 0; i < agg.fields.size(); ++i)
            if (!encodeAggregateValue(elem, agg.fields[i], in, lp, dm, buf,
                                      base + i * stride, relocs, absPtrRelocKind,
                                      why))
                return false;
        return true;
    }

    // c67 (D-CSUBSET-AGGREGATE-GLOBAL-SYMBOL-ADDRESS): a symbol-address pointer
    // leaf — an aggregate MEMBER that is a fn/`&global`/string-literal address
    // const-eval could not fold (the F5 scalar arm at lowerMirGlobalsToDataItems,
    // D-CSUBSET-SYMBOL-ADDRESS-GLOBAL, generalized to a member). The pointer slot
    // is 8 bytes the caller already pre-zeroed; emit an abs64 relocation at this
    // member's `base` and leave the slot zero (the linker writes the resolved,
    // and on a PIE image slid, target VA). No abs64 reloc declared ⇒ fail loud,
    // exactly as the F5 scalar arm does. MUST precede decodeScalarLiteralBits
    // (which would reject the MirSymbolAddrValue variant).
    if (std::holds_alternative<MirSymbolAddrValue>(v.value)) {
        if (!absPtrRelocKind.has_value()) return false;
        auto const& sa = std::get<MirSymbolAddrValue>(v.value);
        relocs.push_back(Relocation{static_cast<std::uint32_t>(base),
                                    SymbolId{sa.symbol}, *absPtrRelocKind,
                                    sa.addend});
        return true;
    }

    // D-CSUBSET-BITINT-DATA-GLOBAL: a `_BitInt(N)` MEMBER / ELEMENT / union-first-
    // member leaf — the member of `struct S { _BitInt(17) a; }`, an element of
    // `_BitInt(65) a[2]`, the first member of `union U { _BitInt(100) w; }`. Routed
    // to the SAME image producer the SCALAR-global arm uses, so the two encoders
    // cannot drift: one `_BitInt` representation, asked in one place.
    // ★ MUST PRECEDE the `scalarByteSize` / `decodeScalarLiteralBits` pair below,
    // and neither of them could serve this leaf anyway — `scalarByteSize` takes a
    // KIND and a `_BitInt`'s size lives in its interned WIDTH (hence
    // `sizeOfScalarOrBitInt` inside the producer), and the decode chokepoint returns
    // a `std::uint64_t` that structurally cannot carry an N>64 image. Before this
    // arm existed the leaf fell to `scalarByteSize(BitInt) == nullopt` and refused —
    // LOUD, but with the generic aggregate text that names f16/f80/f128 and not
    // `_BitInt`; the producer's `why` now reaches the caller with the real cause.
    if (k == TypeKind::BitInt) {
        auto const img = encodeBitIntImage(v, in, ty, dm, why);
        if (!img.has_value()) return false;              // `why` already written
        if (base + img->size() > buf.size()) {
            why.set(DiagnosticCode::K_StaticDataEncoderInvariantBreach,
                    "a `_BitInt` member's image overruns the aggregate's laid-out "
                    "extent — the encoder and the layout disagree "
                    "(D-CSUBSET-BITINT-DATA-GLOBAL)");
            return false;
        }
        for (std::size_t j = 0; j < img->size(); ++j) buf[base + j] = (*img)[j];
        return true;
    }

    // ── D-CSUBSET-LONG-DOUBLE-AGGREGATE-GLOBAL + its 128-bit integer twin ──
    // A 16-byte leaf — the `long double` member of `struct S { char c; long
    // double x; }`, an element of `static long double t[3]`, the first member of
    // `union U { long double x; int i; }`, an `__int128` member, the component
    // of a `_Complex long double` — takes the SAME producer as the scalar-global
    // arm. ★ MUST PRECEDE the `scalarByteSize` / `decodeScalarLiteralBits` pair
    // below: that chokepoint returns a u64 and refuses all four kinds, which is
    // exactly how these members came to be refused while the same values as
    // whole globals were emitted. The image fills the whole 16-byte slot
    // `computeLayout` reserves (an x87 F80 leaf: 10 significant bytes + 6 zero
    // pad — the Complex arm above places an F80 imaginary part at
    // `elemLay->size`, i.e. at 16, the same slot).
    if (k == TypeKind::F80 || k == TypeKind::F128
        || k == TypeKind::I128 || k == TypeKind::U128) {
        std::vector<std::uint8_t> img;
        if (!appendSixteenByteScalarImage(img, v, k)) {
            why.set(DiagnosticCode::K_StaticDataEncoderInvariantBreach,
                    std::format("a 16-byte member (TypeKind={}) has an initializer "
                                "in no literal arm that kind can be read from — "
                                "refusing rather than writing a fabricated image "
                                "(D-CSUBSET-LONG-DOUBLE-AGGREGATE-GLOBAL)",
                                static_cast<int>(k)));
            return false;
        }
        auto const w = scalarByteSize(k, dm);
        if (!w.has_value() || img.size() != *w || base + img.size() > buf.size()) {
            why.set(DiagnosticCode::K_StaticDataEncoderInvariantBreach,
                    "a 16-byte member's image does not fill exactly the slot the "
                    "layout reserves for it — the encoder and the layout disagree "
                    "(D-CSUBSET-LONG-DOUBLE-AGGREGATE-GLOBAL)");
            return false;
        }
        for (std::size_t j = 0; j < img.size(); ++j) buf[base + j] = img[j];
        return true;
    }

    // Scalar / pointer leaf: write the literal's LE bytes at `base`. Width
    // comes from `scalarByteSize` (the SAME sizing `computeLayout` used for
    // the offsets, so leaf width and field offset can never disagree).
    auto const wOpt = scalarByteSize(k, dm);
    if (!wOpt.has_value()) return false;             // FnSig/Slice/Void/... → fail loud
    auto const bits = decodeScalarLiteralBits(v, k);
    if (!bits.has_value()) return false;             // F16/non-scalar → fail loud
    if (base + *wOpt > buf.size()) return false;     // layout↔encoder disagreement → fail loud
    for (std::uint64_t j = 0; j < *wOpt; ++j)
        buf[base + j] = static_cast<std::uint8_t>((*bits >> (j * 8)) & 0xFFu);
    return true;
}

// D-LK-RELRO-CONST-DATA-RELOCATABLE (c145): the reloc-bearing section choice is
// the shared `relocBearingGlobalSection` chokepoint (core/types/section_kind.hpp)
// — hoisted there in c154 so the linker's cross-CU merge routes through the SAME
// rule as the F5 scalar symbol-address arm and the aggregate arm here.

} // namespace

std::vector<AssembledData>
lowerMirGlobalsToDataItems(Mir const&                           mir,
                           TypeInterner const&                  interner,
                           std::optional<AggregateLayoutParams> aggregateLayout,
                           DataModel                            dataModel,
                           DiagnosticReporter&                  reporter,
                           std::optional<RelocationKind>        absPtrRelocKind) {
    // ★ WHICH CODE AN ARM HERE GETS, AND THE AXIS THAT DECIDES IT
    // (D-DIAG-OVERLAP-REFUSAL-CODE-NOT-DISCRIMINATING, P65). Every arm in this
    // function used to pass `K_NoMatchingObjectFormat`, so the prose
    // discriminated and the CODE — the only stable surface a consumer can
    // triage, filter or grep on — did not. The axis is WHO MUST CHANGE
    // SOMETHING, because that is what a reader of the code alone needs to know:
    //   * `K_OverlappingStaticInitUnsupported` — the USER edits the
    //     initializer. Raised inside `encodeAggregateValue`, never here, and it
    //     reaches this lambda through `EncodeFailure::codeOr`.
    //   * `K_StaticDataEncoderInvariantBreach` — NOBODY can: two parts of this
    //     compiler that must agree have drifted. Passed directly by the arms
    //     that detect a byte-count or record disagreement, and raised inside
    //     the encoder for the rest.
    //   * `K_NoMatchingObjectFormat` — the residual, and a STATED set rather
    //     than a leftover: this producer has no byte encoding for the global
    //     because a declared capability or an implemented lowering is missing
    //     (no `aggregateLayout`, no abs64 reloc, a runtime initializer, an
    //     un-sizeable or non-primitive type, a bit-field the u64 packer does
    //     not implement). One rule, one remediation KIND — change the target,
    //     the config, or wait for the shape.
    // ⚠ AN ARM ADDED HERE MUST PICK ITS SIDE ON THAT AXIS, not copy its
    // neighbour: the neighbour's code is right for the neighbour's cause.
    auto emit = [&](DiagnosticCode code, std::string msg) {
        ParseDiagnostic d;
        d.code     = code;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::move(msg);
        reporter.report(std::move(d));
    };

    std::vector<AssembledData> out;
    out.reserve(mir.moduleGlobalCount());

    for (std::uint32_t i = 0; i < mir.moduleGlobalCount(); ++i) {
        MirGlobalId const  gid     = mir.globalAt(i);
        TypeId const       ty      = mir.globalType(gid);
        SymbolId const     sym     = mir.globalSymbol(gid);
        std::uint32_t const litIdx = mir.globalInitLiteralIndex(gid);
        MirFuncId const    initFn  = mir.globalInitFunc(gid);

        // C11/C23 6.7.5 (D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN): the source-declared
        // explicit `alignas(N)` alignment (0 = none), threaded onto MirGlobal. A
        // data section aligns to any power of two ≤ 256 (no slot-width bound the
        // way a stack local has), so a global alignment may legitimately EXCEED
        // the type's natural alignment with no gate. `raiseToExplicit` returns the
        // STRICTER of the type-derived alignment and this override — applied at
        // every `.alignment =` assignment below (a plain `alignas(32) int g;` is a
        // SCALAR, so the override must reach the primitive/scalar arms too, not
        // only the aggregate `lay->align` arms). The frontend already validated
        // the value (power-of-two, ≤256), so `ofRuntimePow2` is safe.
        std::uint32_t const explicitAlignBytes = mir.globalAlignmentBytes(gid);
        auto const raiseToExplicit = [&](Alignment natural) -> Alignment {
            return (explicitAlignBytes > natural.bytes())
                       ? Alignment::ofRuntimePow2(explicitAlignBytes)
                       : natural;
        };

        // D-CSUBSET-THREAD-LOCAL (TLS C1): a `thread_local` global routes to
        // the thread-local section pair (Tdata/Tbss) — its storage is the
        // PER-THREAD template the loader copies for every thread, never the
        // process-shared Data/Bss/Rodata. Read ONCE here; consulted FIRST at
        // every section decision below (including the F5/c67 overrides —
        // audit fold CRIT-2). String-POOL globals are minted
        // isThreadLocal=false at their HIR mint site (the pooled bytes are
        // process-shared rodata; only the named thread_local OBJECT is
        // per-thread), so this flag is exactly the declared storage class.
        bool const isThreadLocal = mir.globalIsThreadLocal(gid);

        // Runtime-init globals: their bytes land via the
        // `__module_init__` synthesized function at module-load
        // time. Today this cycle scope produces NO AssembledData
        // for runtime-init globals (zero-bytes-emit is anchored
        // under D-LK4-RODATA-PRODUCER-RUNTIME-INIT). A silent
        // skip would cause downstream `K_SymbolUndefined` at the
        // linker (the producer-emitted REL32 reloc against the
        // global's SymbolId would have no symbolVa entry). Raise
        // a loud actionable diagnostic naming the global so the
        // user sees the gap at the producer tier (silent-failure
        // audit HIGH-1 fold, 2026-06-02).
        if (initFn.valid()) {
            emit(DiagnosticCode::K_NoMatchingObjectFormat,
                 std::format("lowerMirGlobalsToDataItems: global "
                             "SymbolId={{ {} }} has a runtime "
                             "initializer (__module_init__-driven) "
                             "— anchored under "
                             "D-LK4-RODATA-PRODUCER-RUNTIME-INIT; today's cycle "
                             "scope emits no AssembledData for "
                             "this shape.",
                             sym.v));
            continue;
        }

        // Zero-init globals (neither initLiteralIndex nor initFunc set): a
        // tentative C global `int g;` — zero-fill, NO on-disk bytes. Emit a
        // `Bss` AssembledData with EMPTY bytes and the byte SIZE recorded in
        // `reservedSize` (the wire format reserves the size in the section
        // header without storing file bytes). A tentative global is ALWAYS
        // mutable — C requires an initializer for a `const` object — so `.bss`
        // is unconditionally writable; the const bit is not consulted here.
        // Closes D-LK4-RODATA-PRODUCER-BSS-EMIT (the former fail-loud anchor).
        if (litIdx == UINT32_MAX) {
            // D-CSUBSET-ENUM-GLOBAL-CODEGEN: project enum → its underlying
            // integer so a tentative `enum E g;` reserves its .bss span on the
            // PRIMITIVE fast path. It reserved the right size before, but only
            // via the `computeLayout` fallback — which needs the target to have
            // declared an `aggregateLayout` block, a dependency an integer-sized
            // object has no business having. Now the two arms of this function
            // agree on what an enum IS instead of arriving there by two routes.
            TypeKind const zk = materialScalarKind(interner, ty);
            // Type byte size: the scalar fast path widths primitives; an
            // aggregate routes through the target's layout engine (same
            // `computeLayout` the initialized aggregate arm uses). Absent a
            // layout for a non-primitive ⇒ fail loud (no sound size to reserve).
            std::optional<std::uint64_t> sizeOpt;
            if (auto const pw = primitiveByteSize(zk); pw.has_value()) {
                sizeOpt = static_cast<std::uint64_t>(*pw);
            } else if (aggregateLayout.has_value()) {
                if (auto const lay = computeLayout(ty, interner, *aggregateLayout,
                                                   dataModel);
                    lay.has_value()) {
                    sizeOpt = lay->size;
                }
            }
            if (!sizeOpt.has_value()) {
                emit(DiagnosticCode::K_NoMatchingObjectFormat,
                     std::format("lowerMirGlobalsToDataItems: zero-init global "
                                 "SymbolId={{ {} }} has TypeKind={} whose byte "
                                 "size cannot be computed (non-primitive with no "
                                 "`aggregateLayout` block, or an un-sizeable "
                                 "type) — cannot reserve a .bss span "
                                 "(D-LK4-DATA-PRODUCER).",
                                 sym.v, static_cast<int>(zk)));
                continue;
            }
            AssembledData z;
            z.symbol       = sym;
            // D-CSUBSET-THREAD-LOCAL: a zero-init `thread_local int g;` is the
            // ZERO-FILL THREAD-LOCAL TEMPLATE extent → Tbss (each thread's copy
            // is loader-zeroed; a Bss slot would be ONE process-shared object —
            // the silent-miscompile of the declared storage duration).
            z.section      = isThreadLocal ? DataSectionKind::Tbss
                                           : DataSectionKind::Bss;
            z.reservedSize = *sizeOpt;   // bytes stays EMPTY (zero-fill invariant)
            // Alignment: primitives align to their size (power-of-two in
            // [1,16]); aggregates carry the layout's align. The walker raises
            // the section alignment to cover the strictest item.
            if (auto const pw = primitiveByteSize(zk); pw.has_value()) {
                z.alignment = raiseToExplicit(Alignment::ofRuntimePow2(
                    static_cast<std::uint32_t>(*pw)));
            } else if (aggregateLayout.has_value()) {
                if (auto const lay = computeLayout(ty, interner, *aggregateLayout,
                                                   dataModel);
                    lay.has_value()) {
                    z.alignment = raiseToExplicit(lay->align);
                }
            } else {
                // No primitive size, no aggregateLayout to derive from — a global
                // whose only alignment signal is the explicit override. Honor it
                // (raiseToExplicit over the byte-aligned default preserves it).
                z.alignment = raiseToExplicit(z.alignment);
            }
            out.push_back(std::move(z));
            continue;
        }

        MirLiteralValue const& v = mir.literalValue(litIdx);
        // D-CSUBSET-ENUM-GLOBAL-CODEGEN: the kind every arm below dispatches on
        // is the MATERIAL one — an `enum E g = B;` widths and decodes as its
        // underlying integer. Before this projection it reached the scalar arm's
        // "non-primitive global types" refusal, because `scalarByteSize(Enum)` is
        // nullopt by construction. `ty` stays the DECLARED type for every
        // structural reader below (`operands`/`scalars`/`computeLayout`).
        TypeKind const k = materialScalarKind(interner, ty);

        AssembledData d;
        d.symbol  = sym;
        // Section selection for an INITIALIZED global (D-LK4-DATA-PRODUCER-MUTABLE-GLOBAL):
        // a `const` global is genuinely read-only → `.rodata`;
        // a mutable one is written at runtime → writable `.data` (a store into
        // `.rodata` faults — the bug this cycle fixes). Keyed on the config-
        // driven `MirGlobal.isConst` PROPERTY threaded from the source's
        // const-qualifier, NOT on any target/format identity. A string-literal
        // global's isConst is set at its MINT site (D-CSUBSET-MUTABLE-CHAR-ARRAY-RODATA):
        // a SYNTHETIC string-pool global — the immutable bytes a
        // `char *p = "hi"` / a function-body literal points to — is minted const
        // → `.rodata`; a NAMED `char arr[N] = "str"` honors its declared
        // const-ness. So the string-literal arm below no longer overrides the
        // section — `isConst` is the single authority.
        //
        // D-CSUBSET-THREAD-LOCAL (TLS C1): the isThreadLocal check runs FIRST,
        // BEFORE isConst — a `const thread_local int k = 3;` MUST go to .tdata,
        // NOT .rodata: C11 6.7.1 gives it THREAD storage duration, so its
        // ADDRESS (tp + tpoff) differs per thread; parking it at one shared
        // .rodata VA would collapse every thread onto one object (and the
        // access codegen — tlsbase + lea — would then read a garbage tpoff
        // against a non-TLS symbol). The per-thread copy is never written
        // through this `const` object, but per-thread IDENTITY requires the
        // TLS template section. Initialized thread_local → Tdata (the
        // template bytes each thread's copy starts from).
        // D-LK-RELRO-CONST-DATA-RELOCATABLE (c145): capture const-ness once —
        // the symbol-address / aggregate arms below reuse it to route a
        // reloc-BEARING const global to RelRoConst (relocated-read-only)
        // instead of writable `.data`.
        bool const isConstGlobal = mir.globalIsConst(gid);
        d.section = isThreadLocal
                        ? DataSectionKind::Tdata
                        : (isConstGlobal ? DataSectionKind::Rodata
                                         : DataSectionKind::Data);

        // String-literal arm: bytes are the literal's std::string
        // contents. The HIR convention is Array<Char,N+1> where
        // the +1 counts an implicit NUL terminator; the literal
        // pool stores the N raw bytes without NUL. Emit N+1 bytes
        // here (raw bytes + 1 NUL byte) so the on-disk layout
        // matches what C-style consumers expect when dereferencing
        // through the array.
        //
        // DISPATCH-ORDER INVARIANT (code-architect audit fold,
        // 2026-06-02 — D-LK4-RODATA-PRODUCER-STRING coupling):
        // this `std::string` variant check MUST fire BEFORE the
        // TypeKind-keyed `primitiveByteSize` gate below. String-
        // literal-promoted MirGlobals carry `TypeKind::Array` (the
        // HIR string-literal's `Array<Char,N+1>` type), which
        // `primitiveByteSize` does NOT handle (returns nullopt →
        // K_NoMatchingObjectFormat with a misleading "non-primitive
        // global types are anchored under D-LK4-RODATA-PRODUCER-AGGREGATE-GLOBAL"
        // message). The dispatch on the LITERAL-
        // POOL VARIANT (not the TypeKind) is the correct
        // discriminator for the string case. A future refactor
        // that reorders to "TypeKind check first" would silently
        // break the D-LK4-RODATA-PRODUCER-STRING closure path.
        if (std::holds_alternative<std::string>(v.value)) {
            auto const& s = std::get<std::string>(v.value);
            // SECTION (D-CSUBSET-MUTABLE-CHAR-ARRAY-RODATA): the section was
            // already chosen from `isConst` above — do NOT override it here. A
            // SYNTHETIC string-literal-pool global (the immutable bytes a
            // `char *p = "hi"` / a function-body literal points to) was minted
            // CONST → `.rodata` (read-only, as a string literal must be). A NAMED
            // user `char arr[N] = "str"` global is the array OBJECT itself (C
            // 6.7.9 mutable storage) → it honors its declared const-ness:
            // `const` → `.rodata`, MUTABLE → writable `.data` (a runtime
            // `arr[0]='J'` must not fault). The former unconditional `.rodata`
            // override here wrongly forced a mutable named array into read-only
            // memory (a SIGSEGV on write) — removed.
            // The literal `s` already holds the ELEMENT-WIDTH-encoded code units
            // (narrow `Array<Char,N>` = 1 byte/unit; C11/C23 6.4.5 wide/UTF =
            // 2/4-byte LE units produced by the HIR wide encoder), so `s.begin()..`
            // is the exact on-wire byte sequence sans terminator.
            d.bytes.assign(s.begin(), s.end());
            // c62 (C 6.7.9p14, D-CSUBSET-STRING-LITERAL-ARRAY-ZERO-FILL): the global's
            // TYPE is `Array<elem,N>` — MATERIALIZE the full N*sizeof(elem) bytes here
            // (the Option-A "pad at the producer" choice). This subsumes the trailing
            // NUL: an ordinary literal has N == codeUnits+1, so the padding IS the
            // element-wide terminator (`u"A"` → Array<U16,2> → 4 bytes: `41 00 00 00`);
            // a `char[N]`/`wchar_t[N]` initializer (the HIR coerce retyped the literal
            // to N > codeUnits+1) gets the remaining zero elements too. A consumer that
            // copies N*sizeof(elem) bytes then reads GUARANTEED zeros, never an OOB read
            // of adjacent rodata. Only GROW (never shrink): N*sizeof(elem) >= s.size()
            // by construction; clamp defensively so a smaller type can never truncate.
            // The byte size is ELEMENT-WIDTH-AWARE — a wide element's size is
            // count*sizeof(elem), NOT the length scalar (which counts UNITS).
            std::uint64_t elemBytes = 1;   // narrow default; wide = 2/4 (drives align)
            if (interner.kind(ty) == TypeKind::Array) {
                if (auto const ops = interner.operands(ty);
                    !ops.empty() && ops[0].valid()) {
                    // D-CSUBSET-ENUM-GLOBAL-CODEGEN: the file-wide invariant —
                    // an interner-derived kind reaching `scalarByteSize` is the
                    // MATERIAL one. No shipped source can give a string literal an
                    // enum element type, so this site changes nothing today; it is
                    // here so the invariant holds by inspection at EVERY site
                    // rather than at the two that happen to be reachable.
                    auto const elemKind = materialScalarKind(interner, ops[0]);
                    if (auto const eb = scalarByteSize(elemKind, dataModel);
                        eb.has_value() && *eb > 0) {
                        elemBytes = *eb;
                    }
                }
                std::optional<std::uint64_t> typeSize;
                if (auto const sc = interner.scalars(ty); !sc.empty()) {
                    // The layout engine (when present) computes the agnostic total
                    // size (count * element stride) for any element width.
                    if (aggregateLayout.has_value()) {
                        if (auto const lay = computeLayout(ty, interner,
                                                           *aggregateLayout, dataModel);
                            lay.has_value()) {
                            typeSize = lay->size;
                        }
                    }
                    // Fallback (no aggregateLayout): count * element byte width. For a
                    // narrow `Array<Char,N>` the element is 1 byte so this is N (the
                    // pre-wide behavior); for a wide element it is N*sizeof(elem).
                    if (!typeSize.has_value())
                        typeSize = static_cast<std::uint64_t>(sc[0]) * elemBytes;
                }
                // No terminator was appended above — the resize-to-typeSize IS the
                // terminator (and any char[N] padding). A non-array string literal
                // (should not occur; strings are always Array-typed) would fall
                // through with exactly `s` bytes; guard by appending one NUL only in
                // that defensive case so a bare string is still terminated.
                if (typeSize.has_value() && *typeSize > d.bytes.size())
                    d.bytes.resize(static_cast<std::size_t>(*typeSize), 0u);
            } else {
                d.bytes.push_back(0);   // defensive: non-array string literal (unexpected)
            }
            // Align to the element width (narrow=1 → byte-aligned as before; wide
            // U16=2 / U32=4) so a `(unsigned short*)u"…"` read is naturally aligned
            // — matters on strict-alignment targets (arm64).
            d.alignment = raiseToExplicit(Alignment::ofRuntimePow2(elemBytes));
            out.push_back(std::move(d));
            continue;
        }

        // F5 (D-CSUBSET-SYMBOL-ADDRESS-GLOBAL): a global initialized to the
        // LINK-TIME-CONSTANT address of another symbol — `char* g = "...";`,
        // `int* p = &x;`, a function-pointer table. Emit a pointer-width zero slot
        // + an ABSOLUTE-64 relocation against the target symbol; the linker writes
        // the target's VA into the slot. Dispatch on the literal VARIANT (the same
        // discriminator the string / aggregate arms use), BEFORE the TypeKind-keyed
        // primitive gate. The 8-byte width matches the abs64 reloc (widthBytes 8);
        // all shipped targets are 64-bit-pointer (a 32-bit-pointer target would
        // declare abs32 + a 4-byte slot — anchored future, no shipped consumer).
        if (std::holds_alternative<MirSymbolAddrValue>(v.value)) {
            auto const& sa = std::get<MirSymbolAddrValue>(v.value);
            if (!absPtrRelocKind.has_value()) {
                emit(DiagnosticCode::K_NoMatchingObjectFormat,
                     std::format("lowerMirGlobalsToDataItems: global SymbolId={{ {} "
                                 "}} is initialized to a symbol address, but the "
                                 "target declares no absolute-64 relocation "
                                 "(widthBytes==8 && !pcRelative) — cannot emit the "
                                 "pointer fixup (D-CSUBSET-SYMBOL-ADDRESS-GLOBAL).",
                                 sym.v));
                continue;
            }
            // A symbol-address pointer is INHERENTLY load-writable: the loader
            // writes the resolved (and, on a PIE image, slid) target address into
            // this slot via the relocation below. It therefore MUST live in a
            // section that is WRITABLE at load — never read-only rodata (a
            // Mach-O PIE image cannot rebase the sealed __TEXT,__const).
            //
            // D-LK-RELRO-CONST-DATA-RELOCATABLE (c145): a CONST such pointer now
            // routes to RelRoConst (relocated-read-only: ELF `.data.rel.ro` /
            // Mach-O `__DATA_CONST` / PE `.rdata`) — load-writable for the
            // relocation, then read-only, matching gcc's `.data.rel.ro`. Only a
            // MUTABLE pointer stays writable `.data`. (Before c145 both went to
            // `.data`, silently dropping const-ness + the relro hardening.)
            //
            // ★ TLS C1 audit fold CRIT-2 (D-CSUBSET-THREAD-LOCAL): the override
            // must PRESERVE thread-locality. A `thread_local char *msg = "hi";`
            // is a per-thread POINTER OBJECT whose initial VALUE is patched into
            // the .tdata TEMPLATE at link time (sound for the fixed-base ET_EXEC
            // this cycle ships — the target VA is final; every thread's copy
            // starts from the patched template). Demoting it to .data would
            // silently make the pointer ONE process-shared slot — the exact
            // storage-duration miscompile the section-select guards against.
            d.section = relocBearingGlobalSection(isThreadLocal, isConstGlobal);
            d.bytes.assign(8, 0);                       // pointer-width zero slot
            d.alignment = raiseToExplicit(Alignment::ofRuntimePow2(8));
            d.relocations.push_back(Relocation{
                /*offset=*/0u,
                /*target=*/SymbolId{sa.symbol},
                /*kind=*/*absPtrRelocKind,
                /*addend=*/sa.addend});
            out.push_back(std::move(d));
            continue;
        }

        // Aggregate arm (Struct / Union / Array, recursively + nested —
        // D-LK4-RODATA-PRODUCER-AGGREGATE-GLOBAL). Dispatch on the LITERAL-
        // POOL VARIANT (`MirAggregateValue`) — the same discriminator the
        // `std::string` arm above uses, and it MUST fire BEFORE the
        // TypeKind-keyed `primitiveByteSize` gate (which returns nullopt for
        // every aggregate kind). The recursive `encodeAggregateValue` needs
        // the target's per-ABI layout params; absent them (the target
        // declared no `aggregateLayout` block) there is no sound layout, so
        // fail loud rather than guess a wrong one.
        if (std::holds_alternative<MirAggregateValue>(v.value)) {
            if (!aggregateLayout.has_value()) {
                emit(DiagnosticCode::K_NoMatchingObjectFormat,
                     std::format("lowerMirGlobalsToDataItems: global "
                                 "SymbolId={{ {} }} is an aggregate "
                                 "(TypeKind={}) but the target declared no "
                                 "`aggregateLayout` block — cannot compute "
                                 "its byte layout ("
                                 "D-LK4-RODATA-PRODUCER-AGGREGATE-GLOBAL).",
                                 sym.v, static_cast<int>(k)));
                continue;
            }
            auto const lay =
                computeLayout(ty, interner, *aggregateLayout, dataModel);
            if (!lay.has_value()) {
                emit(DiagnosticCode::K_NoMatchingObjectFormat,
                     std::format("lowerMirGlobalsToDataItems: global "
                                 "SymbolId={{ {} }} has an un-sizeable "
                                 "aggregate type (TypeKind={}) — incomplete "
                                 "or out-of-scope ("
                                 "D-LK4-RODATA-PRODUCER-AGGREGATE-GLOBAL).",
                                 sym.v, static_cast<int>(k)));
                continue;
            }
            // Pre-size + zero-fill to the layout total: every padding byte,
            // partial-init tail, and union slack is then 0 by construction —
            // the recursion writes only the provided leaves.
            d.bytes.assign(static_cast<std::size_t>(lay->size), 0u);
            // A1 (audit fold): `why` carries the SPECIFIC cause when the encoder
            // knows one. Prefer it verbatim; fall back to the enumerating text
            // only for the arms that genuinely share it — a diagnostic that
            // names a cause the user does not have is worse than one that names
            // a set they do.
            // ⚠ THIS COMMENT USED TO SAY "today: the overlapping explicit-offset
            // refusal", SINGULAR, AND THAT HAD GONE FALSE: the encoder records a
            // cause on THIRTEEN paths now, across this function's recursion and
            // both `_BitInt` helpers. The count is not restated here — it would
            // rot the same way — but the shape is: `why` is a MANY-writer
            // channel, so the code it renders under must come from the writer
            // and not from this arm
            // (D-DIAG-OVERLAP-REFUSAL-CODE-NOT-DISCRIMINATING). `codeOr` is
            // that hand-off.
            EncodeFailure why;
            if (!encodeAggregateValue(ty, v, interner, *aggregateLayout,
                                      dataModel, d.bytes, 0, d.relocations,
                                      absPtrRelocKind, why)) {
                emit(why.codeOr(DiagnosticCode::K_NoMatchingObjectFormat),
                     why.empty()
                         ? std::format("lowerMirGlobalsToDataItems: global "
                                       "SymbolId={{ {} }} aggregate initializer "
                                       "could not be encoded (a type↔value shape "
                                       "mismatch or an unencodable leaf — e.g. an "
                                       "f16 leaf, or an address-relocated leaf when "
                                       "the target declares no abs64 reloc) "
                                       "(D-LK4-RODATA-PRODUCER-AGGREGATE-GLOBAL).",
                                       sym.v)
                         : std::format("lowerMirGlobalsToDataItems: global "
                                       "SymbolId={{ {} }} {} "
                                       "(D-LK4-RODATA-PRODUCER-AGGREGATE-GLOBAL).",
                                       sym.v, why.text));
                continue;
            }
            // c67 (D-CSUBSET-AGGREGATE-GLOBAL-SYMBOL-ADDRESS): an aggregate that
            // carries load-time relocations (a fn/`&global`/string member) is
            // INHERENTLY load-writable — the loader patches the resolved (and on
            // a PIE image slid) target VAs into the member slots. It MUST live in
            // a section writable at load, never read-only rodata (a Mach-O PIE
            // __TEXT,__const cannot be rebased). A reloc-free const aggregate
            // keeps the section chosen above (.rodata for a const global).
            //
            // D-LK-RELRO-CONST-DATA-RELOCATABLE (c145): a CONST reloc-bearing
            // aggregate (the sqlite VFS method-table / `aSyscall[]` shape — a
            // `const` array of function pointers) now routes to RelRoConst
            // (relocated-read-only), matching gcc's `.data.rel.ro`; only a
            // MUTABLE one stays writable `.data`. SAME rule as the F5 scalar arm.
            //
            // ★ TLS C1 audit fold CRIT-2 (D-CSUBSET-THREAD-LOCAL): PRESERVE
            // thread-locality — a reloc-bearing `thread_local` aggregate (e.g.
            // `thread_local char *tbl[2] = {"a","b"};`) keeps its member slots
            // in the .tdata TEMPLATE, patched at link time (fixed-base ET_EXEC),
            // NOT demoted to a process-shared .data slot. A reloc-free
            // thread_local aggregate already sits in Tdata from the initial
            // selection and is untouched by this override.
            if (!d.relocations.empty()) {
                d.section = relocBearingGlobalSection(isThreadLocal, isConstGlobal);
            }
            d.alignment = raiseToExplicit(lay->align);
            out.push_back(std::move(d));
            continue;
        }

        // ── THE FOUR 16-BYTE SCALAR KINDS: F80 / F128 `long double`, I128 / U128 ──
        // (D-CSUBSET-LONG-DOUBLE-X87-ARITH LD-1, -IEEE128-ARITH LD-2, LD-3's folded
        // `WideFloatValue`, D-CSUBSET-INT128-DATA-GLOBAL.) Each is 16 bytes, WIDER
        // than the u64 `decodeScalarLiteralBits` returns, so each is encoded by
        // `appendSixteenByteScalarImage` — the ONE producer the aggregate-member
        // leaf calls too (D-CSUBSET-LONG-DOUBLE-AGGREGATE-GLOBAL, P68 round 8), so a
        // global and the same value as a member cannot be encoded two ways. This
        // arm used to be three: a 128-bit one here and an F80 and an F128 one
        // after the width unwrap, each with its own copy of the encoding.
        // ⚠ KEYED ON THE DECLARED KIND `k`, NEVER ON THE VALUE'S VARIANT, and
        // placed FIRST: `__uint128_t g = 5;` folds into a PLAIN `std::uint64_t`,
        // so a variant-keyed gate would miss it and `appendLE` would be asked for
        // width 16 — a shift of 64..120 bits, UB that on both shipped host arches
        // REPEATS the low 8 bytes into the high 8 (TF-C94's recorded lesson).
        // BEFORE the `_BitInt` arm, so a 128-bit value folded into the
        // `BitIntValue` pool arm keeps its declared kind's treatment.
        // ⓘ A negative `__int128` folded into the int64 arm gets a SIGN-extended
        // high limb; a folded `WideFloatValue` of the OTHER wide kind is refused.
        if (k == TypeKind::F80 || k == TypeKind::F128
            || k == TypeKind::I128 || k == TypeKind::U128) {
            bool const isInt = (k == TypeKind::I128 || k == TypeKind::U128);
            std::size_t const before = d.bytes.size();
            if (!appendSixteenByteScalarImage(d.bytes, v, k)) {
                emit(DiagnosticCode::K_StaticDataEncoderInvariantBreach,
                     isInt
                         ? std::format("lowerMirGlobalsToDataItems: global SymbolId={{ {} }} "
                                       "has a 128-bit integer type (TypeKind={}) but its "
                                       "initializer is in no integer literal arm — refusing "
                                       "rather than emitting a fabricated 16-byte image "
                                       "(D-CSUBSET-INT128-DATA-GLOBAL).",
                                       sym.v, static_cast<int>(k))
                         : std::format("lowerMirGlobalsToDataItems: global SymbolId={{ {} }} "
                                       "has a wide-float type (TypeKind={}) but its "
                                       "initializer is neither a `double` nor a folded "
                                       "value of that kind — refusing rather than emitting "
                                       "a fabricated 16-byte image "
                                       "(D-CSUBSET-LONG-DOUBLE-AGGREGATE-GLOBAL).",
                                       sym.v, static_cast<int>(k)));
                continue;
            }
            // The layout and the encoder must agree; a disagreement is a wrong
            // image, so it fails loud rather than shipping a short/long record.
            auto const w16 = scalarByteSize(k, dataModel);
            if (!w16.has_value() || d.bytes.size() - before != *w16) {
                emit(DiagnosticCode::K_StaticDataEncoderInvariantBreach,
                     std::format("lowerMirGlobalsToDataItems: 16-byte global "
                                 "SymbolId={{ {} }} encoded to {} bytes but "
                                 "scalarByteSize reserves {} — the 16-byte encoder "
                                 "and the layout disagree.",
                                 sym.v, d.bytes.size() - before,
                                 w16.has_value() ? *w16 : 0));
                continue;
            }
            d.alignment = raiseToExplicit(Alignment::ofRuntimePow2(
                static_cast<std::uint32_t>(*w16)));
            out.push_back(std::move(d));
            continue;
        }
        // ── D-CSUBSET-BITINT-DATA-GLOBAL: a `_BitInt(N)` SCALAR data-global ──
        // ★ THIS ARM USED TO BE A DEFERRAL WALL AND IS NOW A PRODUCER. What made the
        // wall necessary was never the value — `_BitInt` has const-folded since C4b —
        // but the ENCODER: `appendLE` takes a `std::uint64_t`, so any width past 8
        // shifted by 64+ bits (UB, and on both shipped host arches a repeat of the
        // low word), and `scalarByteSize` takes a KIND, which cannot know N. Both are
        // answered by `encodeBitIntImage`: a wider SOURCE (the wrapped limbs) and the
        // TypeId-aware `sizeOfScalarOrBitInt` ladder. The producer is SHARED with the
        // aggregate-leaf recursion, so closing this does not leave a member of a
        // struct refusing a width the scalar arm emits.
        //
        // ⚠ IT KEYS ON THE DECLARED KIND, NOT ON THE VALUE'S VARIANT — the wall it
        // replaces keyed on `holds_alternative<BitIntValue>` and that is exactly the
        // dispatch [[D-CSUBSET-INT128-DATA-GLOBAL]] recorded as the one that misses:
        // a `_BitInt` initializer can land in ANY of four integer literal arms
        // depending on how narrow the folded value is, and only the KIND is present
        // in all four. Placed AFTER the 128-bit arm so a value that is genuinely
        // I128/U128 keeps its own (align-16) treatment, and BEFORE the scalar arm,
        // whose `scalarByteSize(BitInt)` is nullopt by construction.
        //
        // ★ THE LAYOUT AUTHORITY OWNS SIZE **AND** ALIGNMENT, and for `_BitInt` those
        // two do not track each other: `_BitInt(128)` is 16 bytes with align **8**
        // (x86-64 psABI, pinned by `examples/c/c23_bitint_wide`), so the
        // `Alignment::ofRuntimePow2(width)` rule the other scalar arms use — correct
        // for I128, which really is 16/16 — would over-align every wide `_BitInt`.
        // `computeLayout` is asked for both, and its size is cross-checked against
        // the image the producer actually built: a disagreement is a wrong record, so
        // it refuses rather than shipping a short or long item.
        if (k == TypeKind::BitInt) {
            EncodeFailure why;
            auto const    img = encodeBitIntImage(v, interner, ty, dataModel, why);
            if (!img.has_value()) {
                // `encodeBitIntImage` writes a cause on EVERY refusal path, so the
                // fallback here is unreachable-by-construction rather than a real
                // arm — it is spelled anyway because `codeOr` is the one place both
                // halves of the choice are made, and a future path that forgets to
                // record must land on the shared code, never on a specific one it
                // did not earn.
                emit(why.codeOr(DiagnosticCode::K_NoMatchingObjectFormat),
                     std::format("lowerMirGlobalsToDataItems: global SymbolId={{ {} }} "
                                 "— {}.", sym.v, why.text));
                continue;
            }
            if (!aggregateLayout.has_value()) {
                emit(DiagnosticCode::K_NoMatchingObjectFormat,
                     std::format("lowerMirGlobalsToDataItems: `_BitInt` global "
                                 "SymbolId={{ {} }} needs the target's "
                                 "`aggregateLayout` block to resolve its alignment "
                                 "and none is declared — refusing rather than "
                                 "guessing an ABI alignment "
                                 "(D-CSUBSET-BITINT-DATA-GLOBAL).",
                                 sym.v));
                continue;
            }
            auto const lay = computeLayout(ty, interner, *aggregateLayout, dataModel);
            if (!lay.has_value() || lay->size != img->size()) {
                emit(DiagnosticCode::K_StaticDataEncoderInvariantBreach,
                     std::format("lowerMirGlobalsToDataItems: `_BitInt` global "
                                 "SymbolId={{ {} }} encoded to {} bytes but the "
                                 "layout engine reserves {} — the `_BitInt` encoder "
                                 "and the layout disagree "
                                 "(D-CSUBSET-BITINT-DATA-GLOBAL).",
                                 sym.v, img->size(),
                                 lay.has_value() ? lay->size : 0));
                continue;
            }
            d.bytes     = std::move(*img);
            d.alignment = raiseToExplicit(lay->align);
            out.push_back(std::move(d));
            continue;
        }

        // Scalar arm: encode the variant's u64/i64/bool value as LE bytes
        // sized by the type. Width comes from `scalarByteSize` — the SAME
        // sizing chokepoint the aggregate-member leaf recursion uses (a strict
        // superset of the former `primitiveByteSize`: it adds the pointer-
        // class scalars, sized by the target's DataModel). c80: a TOP-LEVEL
        // POINTER-typed global whose initializer folded to a pointer-valued
        // integer constant (`T* g = 0;` — sqlite's `vfsList`/
        // `sqlite3_temp_directory`; `void* g = SQLITE_INT_TO_PTR(X)`) lands
        // here with TypeKind::Ptr — formerly nullopt → a spurious
        // "non-primitive" fail-loud on a perfectly encodable 8-byte slot.
        auto const widthOpt = scalarByteSize(k, dataModel);
        if (!widthOpt.has_value()) {
            emit(DiagnosticCode::K_NoMatchingObjectFormat,
                 std::format("lowerMirGlobalsToDataItems: global "
                             "SymbolId={{ {} }} has TypeKind={} "
                             "— non-primitive global types are "
                             "anchored under "
                             "D-LK4-RODATA-PRODUCER-AGGREGATE-GLOBAL.",
                             sym.v, static_cast<int>(k)));
            continue;
        }
        // Decode the scalar value through the shared chokepoint (the SAME
        // int/float semantics the aggregate-leaf recursion uses, incl. the
        // mandatory `double → float` narrow for an F32 global — writing the
        // low 4 bytes of the binary64 pattern would be garbage). A nullopt
        // means either an f16 `double` (the pool can't represent it; the four
        // 16-byte kinds were taken by their own arm above) or a non-scalar /
        // monostate variant — distinguished HERE for a precise
        // diagnostic (the `MirAggregateValue` arm already fired above, so a
        // non-scalar here is monostate). (Code-reviewer F1 audit fold — the
        // silent-miscompile guard for `float g = 1.0f;` at file scope.)
        auto const bits = decodeScalarLiteralBits(v, k);
        if (!bits.has_value()) {
            if (std::holds_alternative<double>(v.value)) {
                emit(DiagnosticCode::K_NoMatchingObjectFormat,
                     std::format("lowerMirGlobalsToDataItems: "
                                 "global SymbolId={{ {} }} has "
                                 "TypeKind={} with a `double` "
                                 "literal — the pool cannot "
                                 "represent f16 losslessly "
                                 "(D-LK4-RODATA-PRODUCER-EXOTIC-FLOAT).",
                                 sym.v, static_cast<int>(k)));
            } else {
                emit(DiagnosticCode::K_StaticDataEncoderInvariantBreach,
                     std::format("lowerMirGlobalsToDataItems: global "
                                 "SymbolId={{ {} }} has a literal "
                                 "value of an unhandled variant arm "
                                 "(monostate) — anchored under "
                                 "D-LK4-RODATA-PRODUCER-AGGREGATE-GLOBAL.",
                                 sym.v));
            }
            continue;
        }
        appendLE(d.bytes, *bits, *widthOpt);
        // `scalarByteSize()` returns ∈ {1,2,4,8,16} (pointer-class
        // scalars are the model's 4- or 8-byte pointer width) —
        // every value is a power-of-two in [1,256], so the
        // `optional` unwrap path is dead. Use the runtime-asserting
        // factory to express the invariant in the type (type-design
        // audit fold 2026-06-02 — dead `K_NoMatchingObjectFormat`
        // arm removed; the wrong-domain diagnostic that arm would
        // emit was a future-reader trap).
        d.alignment = raiseToExplicit(Alignment::ofRuntimePow2(
            static_cast<std::uint32_t>(*widthOpt)));
        out.push_back(std::move(d));
    }

    return out;
}

} // namespace dss
