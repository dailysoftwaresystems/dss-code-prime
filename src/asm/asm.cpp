#include "asm/asm.hpp"

#include "asm/format/byte_emit.hpp"
#include "asm/format/fixed32.hpp"
#include "asm/format/walker_util.hpp"
#include "asm/format/x86_variable.hpp"
#include "core/types/bit_int_value.hpp"   // the ONE `_BitInt` padding policy (BitIntValue::paddingByte)
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_layout.hpp"   // computeLayout, scalarByteSize
#include "lir/lir_pass_util.hpp"

#include <bit>
#include <cmath>     // D-MIR-OVERLAP-STRUCT-ZERO-INIT: std::signbit (rejects -0.0)
#include <cstring>
#include <format>
#include <limits>
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
                                    blockSymPatches, reporter);
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
                bool const ok = encodeInst(lir, schema, inst,
                                 outFn.bytes, outFn.relocations,
                                 outFn.sourceMap, blockPatches,
                                 blockSymPatches, lirToMir, reporter);
                if (!ok) {
                    outFn.bytes.resize(preInstByteCount);
                    funcEncodeOk = false;
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
                               "preceding diagnostic); D-ASM-ENCODE-"
                               "FAILURE-FUNCTION-ROLLBACK preserves "
                               "byte-offset integrity by aborting the "
                               "function on first per-inst failure",
                               outFn.symbol.v));
            // Clear the function's bytes/relocs entirely so the
            // partial output cannot leak past assemble().
            outFn.bytes.clear();
            outFn.relocations.clear();
            outFn.sourceMap.clear();
            continue;  // skip patch resolution for this function
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
                                   "block list — malformed LIR (D-CSUBSET-"
                                   "COMPUTED-GOTO)",
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
            continue;  // skip branch-patch resolution for this function
        }

        // Resolve intra-function block-relative branch patches now
        // that every block's byte offset is known. Each patch wrote
        // 4 zero placeholder bytes; we overwrite them with the
        // signed 32-bit displacement `target_offset - (patch_offset
        // + 4)` (the x86 convention: rel32 is relative to the byte
        // AFTER the displacement).
        //
        // D-ASM-PATCH-PARTIAL-OUTPUT-FAILLOUD (post-fold, silent-
        // failure-hunter HIGH #3): on ANY patch failure, abort the
        // whole function's emission rather than partially patching.
        // The previous shape `continue`-d past failures and shipped
        // a partial-patched binary — a missing-target patch left 4
        // zero bytes (rel32=0 → branch-to-self → infinite loop).
        // Dispatch via patch.kind so the shared resolver does NOT
        // bake in x86 rel32-after-disp arithmetic. Each ISA's
        // walker tags its patches with the appropriate kind; the
        // resolver dispatches accordingly. Architect FOLD-NOW post-
        // fold: pre-fix the `target - (patch + 4)` formula and
        // 4-byte LE write lived as raw arithmetic here — an
        // agnosticism break per the project's standing rules
        // (shared substrate, zero CPU-name branches).
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
            switch (patch.kind) {
                case walker_util::BlockRelPatchKind::X86Rel32: {
                    std::int64_t const disp =
                        static_cast<std::int64_t>(it->second)
                      - static_cast<std::int64_t>(patch.patchOffset + 4);
                    if (disp < std::numeric_limits<std::int32_t>::min()
                     || disp > std::numeric_limits<std::int32_t>::max()) {
                        report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                               DiagnosticSeverity::Error,
                               std::format("intra-function branch in fn '{}' "
                                           "needs displacement {} which exceeds "
                                           "rel32 range — function body too large "
                                           "for 32-bit branch reach (anchor "
                                           "D-CSUBSET-LONG-BRANCH for thunks)",
                                           outFn.symbol.v, disp));
                        patchOk = false;
                        break;
                    }
                    asm_byte_emit::writeU32LEAt(outFn.bytes, patch.patchOffset,
                        static_cast<std::uint32_t>(static_cast<std::int32_t>(disp)));
                    break;
                }
                case walker_util::BlockRelPatchKind::Arm64Imm19:
                case walker_util::BlockRelPatchKind::Arm64Imm26: {
                    // D-AS3-BLOCK-REL-IMM19/26: AArch64 intra-function
                    // branch resolution. The displacement is PC-relative
                    // TO THE INSTRUCTION ITSELF (no +4 bias, unlike x86's
                    // rel32-after-disp) and SCALED by 4 (branch targets
                    // are word-aligned). Imm19 (B.cond) occupies bits
                    // 5..23; Imm26 (B) occupies bits 0..25. We READ-
                    // MODIFY-WRITE only that bit-field so the opcode /
                    // cond-nibble / register bits already emitted into
                    // the word survive (writeU32LEAt over all 4 bytes
                    // would clobber them).
                    bool const isImm19 =
                        patch.kind == walker_util::BlockRelPatchKind::Arm64Imm19;
                    std::uint32_t const lsb   = isImm19 ? 5u : 0u;
                    std::uint32_t const width = isImm19 ? 19u : 26u;
                    std::int64_t const delta =
                        static_cast<std::int64_t>(it->second)
                      - static_cast<std::int64_t>(patch.patchOffset);
                    // 4-byte alignment is a hard invariant — every ARM64
                    // instruction (and thus every block boundary) is
                    // word-aligned. A non-multiple delta means the
                    // block-offset table or the patch offset is corrupt;
                    // fail loud rather than silently drop the low bits.
                    if ((delta & 0x3) != 0) {
                        report(reporter, DiagnosticCode::A_NoMatchingEncodingVariant,
                               DiagnosticSeverity::Error,
                               std::format("intra-function ARM64 branch in fn "
                                           "'{}' has unaligned displacement {} "
                                           "(not a multiple of 4) — block "
                                           "offsets must be word-aligned "
                                           "(D-AS3-BLOCK-REL-IMM19/26)",
                                           outFn.symbol.v, delta));
                        patchOk = false;
                        break;
                    }
                    std::int64_t const disp = delta >> 2;  // arithmetic, signed
                    // Signed range derived from the field WIDTH:
                    // Imm19 ∈ [-(1<<18), (1<<18)-1]; Imm26 ∈
                    // [-(1<<25), (1<<25)-1]. Out-of-range = the function
                    // body exceeds the branch's reach; fail loud (a long-
                    // branch thunk is the future generalization, anchored
                    // D-CSUBSET-LONG-BRANCH).
                    std::int64_t const lo = -(std::int64_t{1} << (width - 1));
                    std::int64_t const hi =  (std::int64_t{1} << (width - 1)) - 1;
                    if (disp < lo || disp > hi) {
                        report(reporter, DiagnosticCode::A_ImmediateOperandOutOfRange,
                               DiagnosticSeverity::Error,
                               std::format("intra-function ARM64 branch in fn "
                                           "'{}' needs scaled displacement {} "
                                           "which exceeds the signed {}-bit "
                                           "field range [{}..{}] — function "
                                           "body too large for branch reach "
                                           "(anchor D-CSUBSET-LONG-BRANCH for "
                                           "inverted-cond + long B thunks)",
                                           outFn.symbol.v, disp, width, lo, hi));
                        patchOk = false;
                        break;
                    }
                    // READ the existing 32-bit LE word at the patch site,
                    // OR in the masked displacement, write the whole word
                    // back. The mask clears only the [lsb, lsb+width) bits.
                    std::uint32_t const o = patch.patchOffset;
                    std::uint32_t word =
                        static_cast<std::uint32_t>(outFn.bytes[o])
                      | (static_cast<std::uint32_t>(outFn.bytes[o + 1]) << 8)
                      | (static_cast<std::uint32_t>(outFn.bytes[o + 2]) << 16)
                      | (static_cast<std::uint32_t>(outFn.bytes[o + 3]) << 24);
                    std::uint32_t const mask = (width >= 32u)
                        ? 0xFFFFFFFFu
                        : ((1u << width) - 1u);
                    word = (word & ~(mask << lsb))
                         | ((static_cast<std::uint32_t>(disp) & mask) << lsb);
                    asm_byte_emit::writeU32LEAt(outFn.bytes, o, word);
                    break;
                }
            }
            if (!patchOk) break;
        }
        if (!patchOk) {
            outFn.bytes.clear();
            outFn.relocations.clear();
            outFn.sourceMap.clear();
        }
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
// EMITS, while the same enum as a struct MEMBER hits the pre-existing aggregate
// refusal LOUD — byte-for-byte the behaviour a plain `__int128` already gets.
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
        // x87 format pads to 16/16. Sized here so LAYOUT-only uses work; a
        // VALUE encode still fails loud (decodeScalarLiteralBits has no
        // F80 arm — no lossless `double` backing).
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
// Every 16-byte scalar kind is walled BEFORE reaching here: F80/F128 have
// dedicated widen+append paths (appendF80Extended / the binary128 arm) and
// I128/U128 fail loud at the kind-keyed 128-bit gate, so the only widths that
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
// the u64 `decodeScalarLiteralBits` yields, so F80 has this dedicated
// widen+append path rather than routing through that chokepoint (which stays
// nullopt for F80, keeping the aggregate-member leaf recursion walled — a struct/
// array long-double MEMBER in a rodata global is a DISTINCT deferral,
// D-CSUBSET-LONG-DOUBLE-AGGREGATE-GLOBAL, NOT the scalar arithmetic const-fold
// this cycle's LD-3 closed: a 16-byte leaf cannot flow through the u64
// decodeScalarLiteralBits chokepoint the aggregate recursion uses).
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

// Decode a SCALAR literal to the little-endian bit pattern to emit (zero-
// extended into a u64; the writer takes the low `width` bytes). Handles
// bool / signed / unsigned integers and F32/F64 — a `double`-arm value is
// NARROWED to `float` for an F32 leaf (writing the low 4 bytes of the
// binary64 pattern would be garbage, not a valid binary32). Returns nullopt
// for kinds the pool cannot represent as plain bytes: F16/F80/F128 — any float
// wider than F64 or otherwise without a lossless host-`double` arm (F80 joined
// with FC17.9(e)) — or a non-scalar / monostate variant (string /
// MirAggregateValue / a LD-3 `WideFloatValue` folded leaf / unknown). The SOLE
// scalar-encode chokepoint — the scalar-global arm and the aggregate-leaf
// recursion both route through it, so the int/float value semantics can never
// drift between the two encoders. A folded F80/F128 SCALAR global is handled
// BEFORE this chokepoint (the dedicated appendWideFloatBits arm, LD-3); a folded
// F80/F128 leaf reaching HERE inside an AGGREGATE correctly stays nullopt → the
// aggregate recursion fails loud (D-CSUBSET-LONG-DOUBLE-AGGREGATE-GLOBAL, a
// 16-byte leaf cannot pass through this u64 chokepoint).
[[nodiscard]] std::optional<std::uint64_t>
decodeScalarLiteralBits(MirLiteralValue const& v, TypeKind k) noexcept {
    // D-CSUBSET-INT128-DATA-GLOBAL (TF-C94): a 128-bit integer is 16 bytes —
    // WIDER than the u64 this chokepoint returns — so it cannot pass through,
    // exactly like F80/F128. Checked FIRST, before the integer arms, because a
    // 128-bit value's folded literal IS a plain u64/i64 arm (it is the CONTAINER
    // that is too narrow, not the variant that is wrong): without this the u64
    // arm below would happily return the low 8 bytes and the aggregate-leaf
    // recursion would write them as if they were the whole value. Returning
    // nullopt makes a `struct { __uint128_t x; }` global fail loud at that
    // recursion; the scalar top-level global is walled by the dedicated
    // kind-keyed arm in `lowerMirGlobalsToDataItems`.
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
                   std::string& why) {
    // `bitIntWidth`/`bitIntIsSigned` ABORT on a non-BitInt (deliberately — that
    // abort is the backstop for a missed gate), so the kind check is the contract,
    // not a defensive nicety.
    if (!ty.valid() || in.kind(ty) != TypeKind::BitInt) {
        why = "the `_BitInt` value normalizer was reached with a non-`_BitInt` "
              "type (D-CSUBSET-BITINT-DATA-GLOBAL)";
        return std::nullopt;
    }
    std::int64_t const n     = in.bitIntWidth(ty);
    bool const         signd = in.bitIntIsSigned(ty);
    if (n <= 0 || n > static_cast<std::int64_t>(kBitIntMaxWidth)) {
        why = std::format("`_BitInt({})` has a width outside [1,{}] — a malformed "
                          "interned record; refusing rather than guessing a "
                          "container (D-CSUBSET-BITINT-DATA-GLOBAL)",
                          n, kBitIntMaxWidth);
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
        why = std::format("`_BitInt({})` has an initializer in no integer literal "
                          "arm — refusing rather than emitting a fabricated image "
                          "(D-CSUBSET-BITINT-DATA-GLOBAL)", n);
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
                  DataModel dm, std::string& why) {
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
        why = std::format("`_BitInt({})` has no computable container size "
                          "(D-CSUBSET-BITINT-DATA-GLOBAL)", n);
        return std::nullopt;
    }
    std::size_t const width     = static_cast<std::size_t>(*wOpt);
    auto const&       limbs     = val.limbs();
    std::size_t const limbBytes = limbs.size() * 8u;
    if (limbs.empty() || width > limbBytes) {
        why = std::format("`_BitInt({})` reserves {} container bytes but its wrapped "
                          "value carries only {} — refusing rather than emitting a "
                          "SHORT image (D-CSUBSET-BITINT-DATA-GLOBAL)",
                          n, width, limbBytes);
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
            why = std::format("`_BitInt({})` value byte {} lies above its {}-byte "
                              "container and is 0x{:02x}, not the 0x{:02x} "
                              "extension — emitting the container would DROP value "
                              "bits, so the image is refused rather than truncated "
                              "(D-CSUBSET-BITINT-DATA-GLOBAL)",
                              n, j, width, byte, ext);
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
        why = std::format("`_BitInt({})` encoded to {} bytes but its container "
                          "reserves {} — the encoder and the layout disagree "
                          "(D-CSUBSET-BITINT-DATA-GLOBAL)", n, out.size(), width);
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
// unencodable leaf — e.g. f16/f80/f128, or an address-relocated leaf…"). The
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
                     std::string& why) {
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
        if (compositeFieldsOverlap(ty, in, lp, dm)) {
            if (!isAllZeroMirLiteral(v)) {
                why = "static initialization of an overlapping explicit-offset "
                      "struct is unsupported — its members share bytes; assign "
                      "the members individually (an ALL-ZERO initializer `{0}` / "
                      "`{}` IS supported — D-MIR-OVERLAP-STRUCT-ZERO-INIT)";
                return false;
            }
            return true;
        }
        auto const  lay = computeLayout(ty, in, lp, dm);
        if (!lay.has_value()) return false;
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
                why = "a bit-field wider than 64 bits (or in an allocation unit "
                      "wider than 8 bytes) cannot be packed by the u64 static "
                      "initializer packer — refusing rather than emitting bits it "
                      "would silently drop or repeat "
                      "(D-CSUBSET-BITINT-DATA-GLOBAL)";
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
    // ★ THE OFFSETS ARE `elemLay->size`, NOT THE ALIGNED STRIDE, and the difference
    // is deliberate: `computeLayout`'s own Complex arm returns `StructLayout{es * 2,
    // elem->align, …}` — it lays the imaginary component at exactly `es`, where the
    // Array arm rounds `es` UP to the element's alignment first. They coincide for
    // F32 and F64 (size == align), and they DIVERGE for an x87 F80 element (10 bytes,
    // align 16), so copying the Array arm's stride would silently place `im` six
    // bytes past where every reader — `complexParts`/`loadComplex` in hir_to_mir,
    // `collectLeaves` in aggregate_abi — expects it. This arm matches the LAYOUT
    // AUTHORITY's formula, exactly as the Array arm matches its own.
    // ⓘ F80/F128 elements still cannot REACH here with a value: the MIR classifier
    // that mints this literal folds only F32/F64 components and refuses the rest
    // loud, matching the wall complex ARITHMETIC already hits at those widths. The
    // formula is written correctly anyway, because a layout rule copied wrong is
    // the kind of defect that surfaces one cycle after the gate it would have passed.
    //
    // ⚠ A SHORT VALUE IS NOT A ZERO IMAGINARY PART BY ACCIDENT — it is one BY
    // CONSTRUCTION: `buf` is pre-zeroed to the layout size by the caller, so a
    // 1-field value writes `re` and leaves `im` as the zero C 6.3.1.7 requires for a
    // real→complex conversion. More than 2 fields is a shape the type cannot hold,
    // and it FAILS LOUD rather than writing the first two and dropping the rest.
    if (k == TypeKind::Complex) {
        if (!std::holds_alternative<MirAggregateValue>(v.value)) {
            why = "a `_Complex` static initializer must arrive as a two-component "
                  "aggregate value (real, imaginary) — a scalar leaf cannot carry "
                  "both components "
                  "(D-CSUBSET-COMPLEX-STATIC-STORAGE-INITIALIZER-HAS-NO-CONSTANT-IMAGE)";
            return false;
        }
        auto const& agg = std::get<MirAggregateValue>(v.value);
        auto const  ops = in.operands(ty);
        if (ops.empty()) return false;                  // malformed interned record
        TypeId const elem    = ops[0];
        auto const   elemLay = computeLayout(elem, in, lp, dm);
        if (!elemLay.has_value()) return false;
        if (agg.fields.size() > 2) {
            why = "a `_Complex` static initializer carries more than two components "
                  "— refusing rather than dropping the extras "
                  "(D-CSUBSET-COMPLEX-STATIC-STORAGE-INITIALIZER-HAS-NO-CONSTANT-IMAGE)";
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
            why = "a `_BitInt` member's image overruns the aggregate's laid-out "
                  "extent — the encoder and the layout disagree "
                  "(D-CSUBSET-BITINT-DATA-GLOBAL)";
            return false;
        }
        for (std::size_t j = 0; j < img->size(); ++j) buf[base + j] = (*img)[j];
        return true;
    }

    // Scalar / pointer leaf: write the literal's LE bytes at `base`. Width
    // comes from `scalarByteSize` (the SAME sizing `computeLayout` used for
    // the offsets, so leaf width and field offset can never disagree).
    auto const wOpt = scalarByteSize(k, dm);
    if (!wOpt.has_value()) return false;             // FnSig/Slice/Void/... → fail loud
    auto const bits = decodeScalarLiteralBits(v, k);
    if (!bits.has_value()) return false;             // F16/F80/F128/non-scalar → fail loud
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
                             "— anchored under D-LK4-RODATA-"
                             "PRODUCER-RUNTIME-INIT; today's cycle "
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
                                 "its byte layout (D-LK4-RODATA-PRODUCER-"
                                 "AGGREGATE-GLOBAL).",
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
                                 "or out-of-scope (D-LK4-RODATA-PRODUCER-"
                                 "AGGREGATE-GLOBAL).",
                                 sym.v, static_cast<int>(k)));
                continue;
            }
            // Pre-size + zero-fill to the layout total: every padding byte,
            // partial-init tail, and union slack is then 0 by construction —
            // the recursion writes only the provided leaves.
            d.bytes.assign(static_cast<std::size_t>(lay->size), 0u);
            // A1 (audit fold): `why` carries the SPECIFIC cause when the encoder
            // knows one (today: the overlapping explicit-offset refusal). Prefer
            // it verbatim; fall back to the enumerating text only for the arms
            // that genuinely share it — a diagnostic that names a cause the user
            // does not have is worse than one that names a set they do.
            std::string why;
            if (!encodeAggregateValue(ty, v, interner, *aggregateLayout,
                                      dataModel, d.bytes, 0, d.relocations,
                                      absPtrRelocKind, why)) {
                emit(DiagnosticCode::K_NoMatchingObjectFormat,
                     why.empty()
                         ? std::format("lowerMirGlobalsToDataItems: global "
                                       "SymbolId={{ {} }} aggregate initializer "
                                       "could not be encoded (a type↔value shape "
                                       "mismatch or an unencodable leaf — e.g. "
                                       "f16/f80/f128, or an address-relocated leaf when "
                                       "the target declares no abs64 reloc) "
                                       "(D-LK4-RODATA-PRODUCER-AGGREGATE-GLOBAL).",
                                       sym.v)
                         : std::format("lowerMirGlobalsToDataItems: global "
                                       "SymbolId={{ {} }} {} "
                                       "(D-LK4-RODATA-PRODUCER-AGGREGATE-GLOBAL).",
                                       sym.v, why));
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

        // D-CSUBSET-INT128-DATA-GLOBAL (TF-C94): a 128-bit integer DATA-global is
        // a DEFERRAL boundary — its on-disk byte layout is not yet emitted. This
        // gate keys on the TYPE KIND, deliberately NOT on the value's variant arm,
        // and that distinction is the whole point: the neighbouring `_BitInt` wall
        // above keys on `holds_alternative<BitIntValue>`, and MIRRORING it here
        // would MISS the common case. For `__uint128_t g = 5;` the folded value is
        // a plain `std::uint64_t`, so a variant-keyed gate never fires and control
        // reaches `appendLE(d.bytes, *bits, *widthOpt)` below with width 16 — and
        // `appendLE` computes `(value >> (j*8))` on a `std::uint64_t` for
        // j ∈ [0,16), so every j ≥ 8 is a shift of 64..120 bits: UNDEFINED
        // BEHAVIOUR, which on both shipped host arches masks the shift count and
        // REPEATS the low 8 bytes into the high 8. Wrong bytes, silently.
        // Placed BEFORE the `widthOpt` unwrap so the wall stands whatever
        // `scalarByteSize` reports, and BEFORE the `_BitInt` VARIANT arm below so
        // the message matches the declared TYPE: once a >64-bit 128-bit fold
        // lands in the `BitIntValue` pool arm (it is the only arm wide enough to
        // carry one), a variant-keyed dispatch would claim a `__uint128_t` global
        // is a `_BitInt` one. Keying on `k` — the global's declared kind — and
        // going first keeps the two deferrals honestly labelled.
        // `decodeScalarLiteralBits` returns nullopt for
        // these kinds too, so a `struct { __uint128_t x; }` global walls at the
        // aggregate-leaf recursion rather than slipping through this scalar path.
        // ★ THE 16 BYTES ARE NOW EMITTED. This arm used to be a fail-loud
        // DEFERRAL wall; it is a producer. What made the wall necessary was that
        // `appendLE` takes a `std::uint64_t`, so a width-16 append computed
        // `(value >> (j*8))` for j in [0,16) -- a shift of 64..120 bits, UB,
        // which on both shipped host arches masks the count to 6 bits and
        // REPEATS the low 8 bytes into the high 8. The fix is not a wider shift
        // but a wider SOURCE: read the value as two little-endian 64-bit limbs
        // and append each at its own width, so no shift ever exceeds 56.
        //
        // ⓘ THE TWO PAYLOAD ARMS ARE BOTH REAL AND NEITHER IMPLIES THE OTHER --
        // this is the distinction the row recorded as the one a naive gate
        // misses. `__uint128_t g = 5;` folds into a PLAIN `std::uint64_t` (the
        // narrowest arm that holds it), while a value needing more than 64 bits
        // folds into the `BitIntValue` pool arm carrying an I128/U128 core. A
        // dispatch keyed on the VARIANT would silently miss the first; keying on
        // the declared KIND `k`, as this arm does, catches both.
        //
        // ⓘ SIGN MATTERS FOR THE HIGH LIMB. A negative `__int128` folded into
        // the int64 arm must fill its high limb with 1s, not zeros -- so the
        // extension is taken from the VALUE's signedness, not assumed.
        //
        // ⚠ THE AGGREGATE LEAF STAYS WALLED, DELIBERATELY. `decodeScalarLiteralBits`
        // still returns nullopt for these kinds, so `struct { __uint128_t x; }`
        // fails LOUD at the aggregate-leaf recursion rather than slipping
        // through: that chokepoint returns a `std::uint64_t` and structurally
        // cannot carry 16 bytes. This mirrors the shipped F80 arm exactly (the
        // sole F80 scalar-global producer, with F80 struct members walled the
        // same way). A remaining site that is LOUD is a deferral; a remaining
        // site that is SILENT would be the half-shipped multi-site contract this
        // cycle exists to stop.
        if (k == TypeKind::I128 || k == TypeKind::U128) {
            std::uint64_t lo = 0;
            std::uint64_t hi = 0;
            if (auto const* bv = std::get_if<BitIntValue>(&v.value)) {
                auto const& limbs = bv->limbs();
                lo = limbs.size() > 0 ? limbs[0] : 0ull;
                hi = limbs.size() > 1 ? limbs[1] : 0ull;
                // A 1-limb payload declared 128 bits wide still needs its high
                // limb materialized; `BitIntValue` keeps its limbs sign-clean, so
                // the extension is the sign of the declared value.
                if (limbs.size() < 2 && bv->isSigned()
                    && (lo >> 63) != 0) hi = ~0ull;
            } else if (auto const* uv = std::get_if<std::uint64_t>(&v.value)) {
                lo = *uv;                       // zero-extends
            } else if (auto const* iv = std::get_if<std::int64_t>(&v.value)) {
                lo = static_cast<std::uint64_t>(*iv);
                if (*iv < 0) hi = ~0ull;        // sign-extends
            } else if (auto const* bo = std::get_if<bool>(&v.value)) {
                lo = *bo ? 1ull : 0ull;
            } else {
                emit(DiagnosticCode::K_NoMatchingObjectFormat,
                     std::format("lowerMirGlobalsToDataItems: global SymbolId={{ {} }} "
                                 "has a 128-bit integer type (TypeKind={}) but its "
                                 "initializer is in no integer literal arm — refusing "
                                 "rather than emitting a fabricated 16-byte image "
                                 "(D-CSUBSET-INT128-DATA-GLOBAL).",
                                 sym.v, static_cast<int>(k)));
                continue;
            }
            std::size_t const before = d.bytes.size();
            appendLE(d.bytes, lo, 8);
            appendLE(d.bytes, hi, 8);
            // The layout and the encoder must agree; a disagreement is a wrong
            // image, so it fails loud rather than shipping a short/long record.
            auto const w128 = scalarByteSize(k, dataModel);
            if (!w128.has_value() || d.bytes.size() - before != *w128) {
                emit(DiagnosticCode::K_NoMatchingObjectFormat,
                     std::format("lowerMirGlobalsToDataItems: 128-bit global "
                                 "SymbolId={{ {} }} encoded to {} bytes but "
                                 "scalarByteSize reserves {} — the 128-bit encoder "
                                 "and the layout disagree.",
                                 sym.v, d.bytes.size() - before,
                                 w128.has_value() ? *w128 : 0));
                continue;
            }
            d.alignment = raiseToExplicit(Alignment::ofRuntimePow2(
                static_cast<std::uint32_t>(*w128)));
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
            std::string why;
            auto const  img = encodeBitIntImage(v, interner, ty, dataModel, why);
            if (!img.has_value()) {
                emit(DiagnosticCode::K_NoMatchingObjectFormat,
                     std::format("lowerMirGlobalsToDataItems: global SymbolId={{ {} }} "
                                 "— {}.", sym.v, why));
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
                emit(DiagnosticCode::K_NoMatchingObjectFormat,
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
                             "anchored under D-LK4-RODATA-PRODUCER-"
                             "AGGREGATE-GLOBAL.",
                             sym.v, static_cast<int>(k)));
            continue;
        }
        // D-CSUBSET-LONG-DOUBLE-X87-ARITH (LD-1): an x87 80-bit `long double`
        // global — the widened extended value is 10 significant + 6 pad = 16
        // bytes, WIDER than the u64 `decodeScalarLiteralBits` returns, so it
        // has this dedicated widen+append path (the SOLE F80 scalar-global
        // producer — F80 struct members stay walled at the aggregate-leaf
        // recursion, LD-3). The double→extended widen is lossless (the pool
        // carries the value as a host `double`, exactly representable for the
        // l-suffixed literals this slice exercises). `*widthOpt` is 16 by
        // construction (scalarByteSize(F80)); assert-guard the invariant.
        if (k == TypeKind::F80) {
            // LD-3 (D-CSUBSET-LONG-DOUBLE-CONSTFOLD-PRECISION): a CONST-FOLDED F80
            // global — the value lives in the `WideFloatValue` pool arm at true
            // 80-bit precision. Checked FIRST; `pack()` yields the identical 16-byte
            // x87 layout, so it shares the size-check + alignment path.
            if (auto const* wf = std::get_if<WideFloatValue>(&v.value)) {
                std::size_t const before = d.bytes.size();
                appendWideFloatBits(d.bytes, *wf);
                if (d.bytes.size() - before != *widthOpt) {
                    emit(DiagnosticCode::K_NoMatchingObjectFormat,
                         std::format("lowerMirGlobalsToDataItems: F80 folded global "
                                     "SymbolId={{ {} }} packed to {} bytes but "
                                     "scalarByteSize reserves {} — the wide-float "
                                     "encoder and the layout disagree.",
                                     sym.v, d.bytes.size() - before, *widthOpt));
                    continue;
                }
                d.alignment = raiseToExplicit(Alignment::ofRuntimePow2(
                    static_cast<std::uint32_t>(*widthOpt)));
                out.push_back(std::move(d));
                continue;
            }
            if (auto const* dv = std::get_if<double>(&v.value)) {
                std::size_t const before = d.bytes.size();
                appendF80Extended(d.bytes, *dv);
                if (d.bytes.size() - before != *widthOpt) {
                    emit(DiagnosticCode::K_NoMatchingObjectFormat,
                         std::format("lowerMirGlobalsToDataItems: F80 global "
                                     "SymbolId={{ {} }} widened to {} bytes but "
                                     "scalarByteSize reserves {} — the x87 "
                                     "extended encoder and the layout disagree.",
                                     sym.v, d.bytes.size() - before, *widthOpt));
                    continue;
                }
                d.alignment = raiseToExplicit(Alignment::ofRuntimePow2(
                    static_cast<std::uint32_t>(*widthOpt)));
                out.push_back(std::move(d));
                continue;
            }
            // A non-`double`, non-`WideFloatValue` F80 initializer is a malformed
            // pool entry — fall through to the decode chokepoint's fail-loud
            // below. (TF-C94: that fail-loud is now REAL. This comment used to
            // describe a wall the chokepoint did not provide — its u64/i64/bool
            // arms returned a value regardless of `k`, so a malformed F80 entry
            // reached `appendLE` with width 16 on a u64 and hit the >>64 UB.
            // `decodeScalarLiteralBits` now returns nullopt for F16/F80/F128 by
            // KIND, honouring the contract its own header always stated.)
        }
        // D-CSUBSET-LONG-DOUBLE-IEEE128-ARITH (LD-2): an IEEE binary128 `long
        // double` global — the widened quad value is 16 bytes, WIDER than the
        // u64 `decodeScalarLiteralBits` returns, so (like F80) it has this
        // dedicated widen+append path (the SOLE F128 scalar-global producer;
        // F128 struct members stay walled at the aggregate-leaf recursion). The
        // double->binary128 widen is lossless for the l-suffixed literals this
        // slice exercises. `*widthOpt` is 16 by construction (scalarByteSize
        // (F128)); assert-guard the invariant, exactly as the F80 arm does.
        if (k == TypeKind::F128) {
            // LD-3: a CONST-FOLDED F128 global — the value lives in the
            // `WideFloatValue` pool arm at true 113-bit precision. Checked FIRST;
            // `pack()` yields the identical 16-byte binary128 layout.
            if (auto const* wf = std::get_if<WideFloatValue>(&v.value)) {
                std::size_t const before = d.bytes.size();
                appendWideFloatBits(d.bytes, *wf);
                if (d.bytes.size() - before != *widthOpt) {
                    emit(DiagnosticCode::K_NoMatchingObjectFormat,
                         std::format("lowerMirGlobalsToDataItems: F128 folded global "
                                     "SymbolId={{ {} }} packed to {} bytes but "
                                     "scalarByteSize reserves {} — the wide-float "
                                     "encoder and the layout disagree.",
                                     sym.v, d.bytes.size() - before, *widthOpt));
                    continue;
                }
                d.alignment = raiseToExplicit(Alignment::ofRuntimePow2(
                    static_cast<std::uint32_t>(*widthOpt)));
                out.push_back(std::move(d));
                continue;
            }
            if (auto const* dv = std::get_if<double>(&v.value)) {
                std::size_t const before = d.bytes.size();
                appendF128(d.bytes, *dv);
                if (d.bytes.size() - before != *widthOpt) {
                    emit(DiagnosticCode::K_NoMatchingObjectFormat,
                         std::format("lowerMirGlobalsToDataItems: F128 global "
                                     "SymbolId={{ {} }} widened to {} bytes but "
                                     "scalarByteSize reserves {} — the binary128 "
                                     "encoder and the layout disagree.",
                                     sym.v, d.bytes.size() - before, *widthOpt));
                    continue;
                }
                d.alignment = raiseToExplicit(Alignment::ofRuntimePow2(
                    static_cast<std::uint32_t>(*widthOpt)));
                out.push_back(std::move(d));
                continue;
            }
            // A non-`double`, non-`WideFloatValue` F128 initializer is a malformed
            // pool entry — fall through to the decode chokepoint's fail-loud
            // below. (TF-C94: real as of this cycle — see the F80 arm's note;
            // `decodeScalarLiteralBits` walls F16/F80/F128 by KIND now, not only
            // on its `double` arm.)
        }
        // Decode the scalar value through the shared chokepoint (the SAME
        // int/float semantics the aggregate-leaf recursion uses, incl. the
        // mandatory `double → float` narrow for an F32 global — writing the
        // low 4 bytes of the binary64 pattern would be garbage). A nullopt
        // means either an f16/f80/f128 `double` (the pool can't represent it) or
        // a non-scalar / monostate variant — distinguished HERE for a precise
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
                                 "represent f16/f80/f128 losslessly "
                                 "(D-LK4-RODATA-PRODUCER-EXOTIC-"
                                 "FLOAT).",
                                 sym.v, static_cast<int>(k)));
            } else {
                emit(DiagnosticCode::K_NoMatchingObjectFormat,
                     std::format("lowerMirGlobalsToDataItems: global "
                                 "SymbolId={{ {} }} has a literal "
                                 "value of an unhandled variant arm "
                                 "(monostate) — anchored under "
                                 "D-LK4-RODATA-PRODUCER-AGGREGATE-"
                                 "GLOBAL.",
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
