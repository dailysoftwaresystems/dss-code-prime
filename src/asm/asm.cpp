#include "asm/asm.hpp"

#include "asm/format/fixed32.hpp"
#include "asm/format/x86_variable.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "lir/lir_pass_util.hpp"

#include <format>

namespace dss {

namespace {

// Diagnostic-emit shorthand, same convention as ML6/ML7.
using lir_pass_util::report;

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
                                         reporter);

        case TargetEncodingShape::Fixed32:
            return fixed32::encode(lir, schema, inst, info, lirToMir,
                                    out, relocs, srcMap, reporter);
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
                         DiagnosticReporter&        reporter) {
    AssembledModule result;
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
        for (std::uint32_t bi = 0; bi < blockCount; ++bi) {
            LirBlockId const blk = lir.funcBlockAt(fn, bi);
            std::uint32_t const instCount = lir.blockInstCount(blk);
            for (std::uint32_t ii = 0; ii < instCount; ++ii) {
                LirInstId const inst = lir.blockInstAt(blk, ii);
                // Each inst's encoder either appends bytes / relocs /
                // source-map entries and returns true, or emits its
                // own diagnostic and returns false. The parallel-
                // index discipline requires we continue to the next
                // inst either way — the function slot exists for
                // every LIR function regardless of per-inst failure.
                // The reporter is the success channel.
                (void)encodeInst(lir, schema, inst,
                                 outFn.bytes, outFn.relocations,
                                 outFn.sourceMap, lirToMir, reporter);
            }
        }
    }

    return result;
}

} // namespace dss
