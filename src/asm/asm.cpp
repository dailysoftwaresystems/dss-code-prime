#include "asm/asm.hpp"

#include "asm/format/byte_emit.hpp"
#include "asm/format/fixed32.hpp"
#include "asm/format/walker_util.hpp"
#include "asm/format/x86_variable.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_layout.hpp"   // computeLayout, scalarByteSize
#include "lir/lir_pass_util.hpp"

#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <unordered_map>

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
            lirToMir[inst.v]
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
    // legitimately produces a zero-function module (e.g. a parse that
    // emitted no top-level functions). Callers that need a non-empty
    // result MUST check `ok()` — which returns false here.
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
        bool blockSymOk = true;
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

    // Invariant 1: Bss items must have empty bytes.
    for (std::size_t i = 0; i < items.size(); ++i) {
        auto const& d = items[i];
        if (d.section == DataSectionKind::Bss && !d.bytes.empty()) {
            emit(DiagnosticCode::K_BssDataHasBytes,
                 std::format("AssembledData[{}] has section=Bss "
                             "but bytes is non-empty ({} bytes). "
                             "Bss is zero-fill — the wire format "
                             "reserves the size without storing "
                             "bytes. Substrate-shape violation "
                             "(D-LK4-RODATA-BSS-INVARIANT).",
                             i, d.bytes.size()));
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

// Byte-width of a primitive TypeKind. Returns nullopt for non-primitive
// kinds (Array / Struct / Ptr / FnSig / ...). Aggregate globals do NOT pass
// through here — they take the `MirAggregateValue` arm + `encodeAggregateValue`
// (the interner-side recursive layout walk, D-LK4-RODATA-PRODUCER-AGGREGATE-
// GLOBAL); this gate only widths the SCALAR-global fast path.
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
        case TypeKind::I128: case TypeKind::U128: case TypeKind::F128:
            return 16u;
        default:
            return std::nullopt;
    }
}

// Little-endian encode `value` into `bytes` (appended). Width=`width`
// bytes. Trailing zeros are appended verbatim — the integer's high
// bytes are dropped silently when `value` exceeds the type's range
// (caller invariant: HIR/MIR const-eval clamps to the type's range
// before reaching the literal pool).
void appendLE(std::vector<std::uint8_t>& bytes,
              std::uint64_t value,
              std::size_t width) noexcept {
    for (std::size_t j = 0; j < width; ++j) {
        bytes.push_back(static_cast<std::uint8_t>(
            (value >> (j * 8)) & 0xFFu));
    }
}

// Decode a SCALAR literal to the little-endian bit pattern to emit (zero-
// extended into a u64; the writer takes the low `width` bytes). Handles
// bool / signed / unsigned integers and F32/F64 — a `double`-arm value is
// NARROWED to `float` for an F32 leaf (writing the low 4 bytes of the
// binary64 pattern would be garbage, not a valid binary32). Returns nullopt
// for kinds the pool cannot represent as plain bytes: F16/F128 (no lossless
// `double` arm) or a non-scalar / monostate variant (string /
// MirAggregateValue / unknown). The SOLE scalar-encode chokepoint — the
// scalar-global arm and the aggregate-leaf recursion both route through it,
// so the int/float value semantics can never drift between the two encoders.
[[nodiscard]] std::optional<std::uint64_t>
decodeScalarLiteralBits(MirLiteralValue const& v, TypeKind k) noexcept {
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
            return std::nullopt;   // F16/F128 — no lossless pool arm
        }
        return bits;
    }
    return std::nullopt;   // monostate / string / MirAggregateValue
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
[[nodiscard]] bool
encodeAggregateValue(TypeId ty, MirLiteralValue const& v,
                     TypeInterner const& in, AggregateLayoutParams lp,
                     DataModel dm, std::vector<std::uint8_t>& buf,
                     std::uint64_t base, std::vector<Relocation>& relocs,
                     std::optional<RelocationKind> absPtrRelocKind) {
    TypeKind const k = in.kind(ty);

    if (k == TypeKind::Struct || k == TypeKind::Union) {
        if (!std::holds_alternative<MirAggregateValue>(v.value)) return false;
        auto const& agg = std::get<MirAggregateValue>(v.value);
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
                                          absPtrRelocKind))
                    return false;
                continue;
            }
            // Pack one bit-field: read its scalar value, mask to width, shift to
            // bitOffset, OR into the unit at `base + fieldOffsets[i]`. The unit
            // load/store width is `unitBytes` (little-endian, matching the MIR
            // read-modify-write codegen + the layout's LSB-first packing).
            BitFieldPlacement const& p = lay->bitFields[i];
            auto const bitsOpt = decodeScalarLiteralBits(agg.fields[i], in.kind(ops[i]));
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

    if (k == TypeKind::Array) {
        // c62 (C 6.7.9p14, D-CSUBSET-STRING-LITERAL-ARRAY-ZERO-FILL): a CHAR-ARRAY
        // field/element initialized by a STRING LITERAL (`char zName[7] = "hour";`
        // inside a static const aggregate). The const-eval folds the string-literal
        // leaf to a `std::string` value (NOT a per-char `MirAggregateValue`), so a
        // char[N] element here carries a string arm. Write the string bytes + the
        // implicit NUL at `base`; the caller pre-zeroed `buf` to the full layout
        // size, so the trailing N−(len+1) bytes are already zero (the C 6.7.9p14
        // zero-fill) — the aggregate twin of the standalone string-literal global's
        // producer-side padding. Element must be `char`; the NUL+bytes must fit the
        // array's byte extent (a layout↔value disagreement fails loud). A non-char
        // array with a string value, or an over-long string, falls through to the
        // shape-mismatch `false` below.
        if (std::holds_alternative<std::string>(v.value)
            && !in.operands(ty).empty()
            && in.kind(in.operands(ty)[0]) == TypeKind::Char) {
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
                                      base + i * stride, relocs, absPtrRelocKind))
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

    // Scalar / pointer leaf: write the literal's LE bytes at `base`. Width
    // comes from `scalarByteSize` (the SAME sizing `computeLayout` used for
    // the offsets, so leaf width and field offset can never disagree).
    auto const wOpt = scalarByteSize(k, dm);
    if (!wOpt.has_value()) return false;             // FnSig/Slice/Void/... → fail loud
    auto const bits = decodeScalarLiteralBits(v, k);
    if (!bits.has_value()) return false;             // F16/F128/non-scalar → fail loud
    if (base + *wOpt > buf.size()) return false;     // layout↔encoder disagreement → fail loud
    for (std::uint64_t j = 0; j < *wOpt; ++j)
        buf[base + j] = static_cast<std::uint8_t>((*bits >> (j * 8)) & 0xFFu);
    return true;
}

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
            TypeKind const zk = interner.kind(ty);
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
            z.section      = DataSectionKind::Bss;
            z.reservedSize = *sizeOpt;        // bytes stays EMPTY (Bss invariant)
            // Alignment: primitives align to their size (power-of-two in
            // [1,16]); aggregates carry the layout's align. The walker raises
            // the section alignment to cover the strictest item.
            if (auto const pw = primitiveByteSize(zk); pw.has_value()) {
                z.alignment = Alignment::ofRuntimePow2(
                    static_cast<std::uint32_t>(*pw));
            } else if (aggregateLayout.has_value()) {
                if (auto const lay = computeLayout(ty, interner, *aggregateLayout,
                                                   dataModel);
                    lay.has_value()) {
                    z.alignment = lay->align;
                }
            }
            out.push_back(std::move(z));
            continue;
        }

        MirLiteralValue const& v = mir.literalValue(litIdx);
        TypeKind const k = interner.kind(ty);

        AssembledData d;
        d.symbol  = sym;
        // Section selection for an INITIALIZED global (D-LK4-DATA-PRODUCER-
        // MUTABLE-GLOBAL): a `const` global is genuinely read-only → `.rodata`;
        // a mutable one is written at runtime → writable `.data` (a store into
        // `.rodata` faults — the bug this cycle fixes). Keyed on the config-
        // driven `MirGlobal.isConst` PROPERTY threaded from the source's
        // const-qualifier, NOT on any target/format identity. A string-literal
        // global's isConst is set at its MINT site (D-CSUBSET-MUTABLE-CHAR-ARRAY-
        // RODATA): a SYNTHETIC string-pool global — the immutable bytes a
        // `char *p = "hi"` / a function-body literal points to — is minted const
        // → `.rodata`; a NAMED `char arr[N] = "str"` honors its declared
        // const-ness. So the string-literal arm below no longer overrides the
        // section — `isConst` is the single authority.
        d.section = mir.globalIsConst(gid) ? DataSectionKind::Rodata
                                           : DataSectionKind::Data;

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
        // global types are anchored under D-LK4-RODATA-PRODUCER-
        // AGGREGATE-GLOBAL" message). The dispatch on the LITERAL-
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
            d.bytes.assign(s.begin(), s.end());
            d.bytes.push_back(0);                 // implicit NUL → s.size()+1 bytes
            // c62 (C 6.7.9p14, D-CSUBSET-STRING-LITERAL-ARRAY-ZERO-FILL): when this
            // string-literal global is the PRODUCER for a `char[N]` initializer (the
            // HIR coerce retyped the literal node to `char[N]` with N > s.size()+1),
            // the global's TYPE is `Array<char,N>` — so MATERIALIZE the trailing
            // N−(s.size()+1) zero-padding bytes HERE (the Option-A "pad at the
            // producer" choice). A consumer that copies N bytes (the aggregate-copy
            // of the field / the array-local init) then reads GUARANTEED zeros, never
            // an OOB read of adjacent rodata. The ORDINARY string-literal global
            // (`char *p = "hi";`, a bare decayed `"abc"`) has type `Array<char,M>`
            // with M == s.size()+1, so the type size equals the byte count already
            // emitted and this padding is a NO-OP (behaviour unchanged). Only GROW
            // (never shrink): the type size is N >= s.size()+1 by the coerce/semantic
            // `N >= M` guards; clamp defensively so a hypothetical smaller type can
            // never truncate the literal bytes.
            if (interner.kind(ty) == TypeKind::Array) {
                std::optional<std::uint64_t> typeSize;
                if (auto const sc = interner.scalars(ty); !sc.empty()) {
                    // Fast path: char element ⇒ N bytes == the length scalar. Use the
                    // layout engine when present for a non-trivial element (agnostic
                    // total size), but a char[N] string global is the only shape that
                    // reaches here, so the scalar is exact.
                    if (aggregateLayout.has_value()) {
                        if (auto const lay = computeLayout(ty, interner,
                                                           *aggregateLayout, dataModel);
                            lay.has_value()) {
                            typeSize = lay->size;
                        }
                    }
                    if (!typeSize.has_value())
                        typeSize = static_cast<std::uint64_t>(sc[0]);
                }
                if (typeSize.has_value() && *typeSize > d.bytes.size())
                    d.bytes.resize(static_cast<std::size_t>(*typeSize), 0u);
            }
            d.alignment = Alignment::of<1>();
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
            // writable-at-load section — NEVER read-only rodata — even when the
            // source global is const-qualified (`const char* const p`, or a
            // `const char* p` whose pointer-OBJECT DSS marks const). ELF
            // (ET_EXEC, no slide) and PE (.rdata is load-writable) merely TOLERATE
            // rodata; a Mach-O PIE image cannot rebase the sealed __TEXT,__const.
            // Overriding the section-above choice to .data on every format is the
            // agnostic root-cause placement, not a per-format special case.
            d.section = DataSectionKind::Data;
            d.bytes.assign(8, 0);                       // pointer-width zero slot
            d.alignment = Alignment::ofRuntimePow2(8);
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
            if (!encodeAggregateValue(ty, v, interner, *aggregateLayout,
                                      dataModel, d.bytes, 0, d.relocations,
                                      absPtrRelocKind)) {
                emit(DiagnosticCode::K_NoMatchingObjectFormat,
                     std::format("lowerMirGlobalsToDataItems: global "
                                 "SymbolId={{ {} }} aggregate initializer "
                                 "could not be encoded (a type↔value shape "
                                 "mismatch or an unencodable leaf — e.g. "
                                 "f16/f128, or an address-relocated leaf when "
                                 "the target declares no abs64 reloc) "
                                 "(D-LK4-RODATA-PRODUCER-AGGREGATE-GLOBAL).",
                                 sym.v));
                continue;
            }
            // c67 (D-CSUBSET-AGGREGATE-GLOBAL-SYMBOL-ADDRESS): an aggregate that
            // carries load-time relocations (a fn/`&global`/string member) is
            // INHERENTLY load-writable — the loader patches the resolved (and on
            // a PIE image slid) target VAs into the member slots. It MUST live in
            // a writable-at-load section, never read-only rodata, even when the
            // source global is const: the SAME rationale as the F5 scalar arm at
            // 951-961 (a Mach-O PIE __TEXT,__const cannot be rebased; ELF/PE
            // merely tolerate rodata). A reloc-free const aggregate keeps the
            // section chosen above (.rodata for a const global).
            if (!d.relocations.empty()) d.section = DataSectionKind::Data;
            d.alignment = lay->align;
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
        // Decode the scalar value through the shared chokepoint (the SAME
        // int/float semantics the aggregate-leaf recursion uses, incl. the
        // mandatory `double → float` narrow for an F32 global — writing the
        // low 4 bytes of the binary64 pattern would be garbage). A nullopt
        // means either an f16/f128 `double` (the pool can't represent it) or
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
                                 "represent f16/f128 losslessly "
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
        d.alignment = Alignment::ofRuntimePow2(
            static_cast<std::uint32_t>(*widthOpt));
        out.push_back(std::move(d));
    }

    return out;
}

} // namespace dss
