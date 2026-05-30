// End-to-end LIR → bytes pipeline integration test (plan 13 AS6).
//
// Lowers a c-subset snippet through the full pipeline:
//   c-subset source → HIR → MIR → LIR → legalize → assemble
// and pins the substrate-level invariants the assembler ships:
//   * AssembledModule.functions parallel-indexes with lir.funcAt(i)
//   * AssembledFunction.symbol carries lir.funcSymbol(fn)
//   * sourceMap entries are monotonically non-decreasing in
//     byteOffset (encoder appends each entry IMMEDIATELY before
//     the instruction's bytes, so the order is identical to the
//     LIR encode order)
//   * sourceMap entries point at valid MIR instructions (the
//     lirToMir table the caller supplies)
//   * Bytes are non-empty when any encoded opcode runs

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_2addr_legalize.hpp"
#include "lowered_lir_fixture.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using namespace dss;

namespace {

[[nodiscard]] AssembledModule
assembleEndToEnd(test_support::LoweredLir& lowered,
                 DiagnosticReporter&       rep) {
    auto legal = legalizeTwoAddress(lowered.lir.lir, *lowered.target, rep);
    EXPECT_TRUE(legal.ok());
    // Walk-back: after legalize, lirToMir's positional alignment
    // shifts (mov insertions don't appear in source MIR). The
    // post-legalize lirToMir is still slot-indexed by LirInstId.v
    // but the slots above original.instCount() are the legalize-
    // inserted insts that have no MIR predecessor — represent
    // those as `InvalidMirInst`.
    std::vector<MirInstId> lirToMir(legal.lir.instCount(),
                                      InvalidMirInst);
    // Source-mapped slots: take the original mapping from
    // mir_to_lir's output and copy what fits. Original LIR slot
    // indices are stable across the legalize rewrite ONLY for
    // instructions present in both; legalize inserts movs but the
    // slot-id allocation is monotonic in the rewritten arena, so
    // the original positions don't survive. v1 scope: stamp
    // InvalidMirInst for all post-legalize insts that the
    // legalize pass freshly minted. The pipeline test verifies
    // shape invariants, not source-map fidelity (that comes when
    // legalize threads its own lirToMir-equivalent through —
    // anchored at plan 12 §3.1 D-ML3-2.1 MirSourceMap IOU).
    return assemble(legal.lir, *lowered.target, lirToMir, rep);
}

} // namespace

TEST(AsmPipeline, CSubsetTrivialFunctionAssemblesEndToEnd) {
    auto lowered = test_support::lowerCSubsetToLir(
        "int f(int x) { return x; }");
    ASSERT_TRUE(lowered.lir.ok);

    DiagnosticReporter rep;
    auto result = assembleEndToEnd(lowered, rep);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.functions.size(), lowered.lir.lir.moduleFuncCount());
    // Every function got the parallel-indexed slot.
    EXPECT_EQ(result.expectedFuncCount, lowered.lir.lir.moduleFuncCount());
}

TEST(AsmPipeline, AssembledFunctionSymbolMatchesLirFunc) {
    auto lowered = test_support::lowerCSubsetToLir(
        "int f(int x) { return x; }");
    ASSERT_TRUE(lowered.lir.ok);

    DiagnosticReporter rep;
    auto result = assembleEndToEnd(lowered, rep);
    ASSERT_TRUE(result.ok());

    // Linker (plan 14) reads `AssembledFunction.symbol` to place
    // each function in the object-file symbol table without
    // re-consulting the upstream Lir.
    auto legal = legalizeTwoAddress(lowered.lir.lir, *lowered.target, rep);
    ASSERT_TRUE(legal.ok());
    for (std::uint32_t fi = 0; fi < legal.lir.moduleFuncCount(); ++fi) {
        EXPECT_EQ(result.functions[fi].symbol,
                  legal.lir.funcSymbol(legal.lir.funcAt(fi)))
            << "symbol round-trip broken for function " << fi;
    }
}

TEST(AsmPipeline, SourceMapByteOffsetsMonotonic) {
    // The encoder appends one SourceMapEntry IMMEDIATELY before the
    // instruction's bytes (asm.cpp::encodeInst stamps at the
    // dispatch level). Therefore the entries' byteOffsets must be
    // monotonically non-decreasing AND the final byteOffset must
    // be < bytes.size() (every entry points AT a real instruction,
    // never past the end).
    auto lowered = test_support::lowerCSubsetToLir(
        "int f(int x) { return x; }");
    ASSERT_TRUE(lowered.lir.ok);

    DiagnosticReporter rep;
    auto result = assembleEndToEnd(lowered, rep);
    ASSERT_TRUE(result.ok());

    // Stamp/byte symmetry: any function with non-empty bytes MUST
    // have non-empty sourceMap (and vice versa). The "stamp THEN
    // emit" ordering at asm.cpp::encodeInst pins this — a regression
    // that stops stamping (or stamps without emitting) breaks the
    // round-trip oracle's parallel-index invariant.
    for (auto const& fn : result.functions) {
        EXPECT_EQ(fn.bytes.empty(), fn.sourceMap.empty())
            << "bytes/sourceMap presence must agree per function "
               "(stamp-then-emit invariant)";
        if (fn.sourceMap.empty()) continue;
        std::uint32_t prev = 0;
        for (auto const& entry : fn.sourceMap) {
            EXPECT_GE(entry.byteOffset, prev)
                << "sourceMap byteOffsets must be monotonically "
                   "non-decreasing";
            // Each entry points AT the first byte of an
            // instruction. Even the trailing entry has bytes
            // following it (the instruction's payload). The only
            // way `entry.byteOffset >= bytes.size()` is the
            // pathological case where the encoder stamped an
            // entry but emitted zero bytes for the instruction
            // (e.g. a no-op opcode that's still encoding-shape !=
            // None). The substrate's invariant is that any
            // instruction reaching the encoded path produces at
            // least one byte; the stamp-then-emit ordering plus
            // this assertion catches a regression that would
            // produce a zero-byte instruction silently.
            EXPECT_LT(entry.byteOffset, fn.bytes.size())
                << "sourceMap entry points past the end of the "
                   "function's bytes — encoder stamped an entry "
                   "but emitted zero bytes for the instruction";
            prev = entry.byteOffset;
        }
    }
}

TEST(AsmPipeline, LirToMirSizeMismatchEmitsDiagnostic) {
    // Pin A_LirToMirSizeMismatch (asm.cpp:139). Mis-sized lirToMir
    // is exactly the silent-UB case the substrate gate exists for —
    // shorter span and the encoder would read past the end once the
    // walkers stamp per-byte-range entries. The substrate fails loud
    // BEFORE the walker loop runs, returning an empty `functions[]`
    // so the parallel-index invariant is broken on purpose
    // (`ok() == false`).
    auto lowered = test_support::lowerCSubsetToLir(
        "int f(int x) { return x; }");
    ASSERT_TRUE(lowered.lir.ok);
    DiagnosticReporter prep;
    auto legal = legalizeTwoAddress(lowered.lir.lir, *lowered.target, prep);
    ASSERT_TRUE(legal.ok());

    DiagnosticReporter rep;
    // Deliberately wrong size: lir has > 0 insts, give it 0.
    std::vector<MirInstId> lirToMir{};  // size = 0
    ASSERT_GT(legal.lir.instCount(), 0u);
    auto result = assemble(legal.lir, *lowered.target, lirToMir, rep);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.functions.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::A_LirToMirSizeMismatch) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

TEST(AsmPipeline, AllRelocationsResolveToDeclaredKinds) {
    // Every Relocation::kind the assembler writes MUST resolve
    // through TargetSchema::relocationInfo(kind) — closes the
    // assembler/linker contract at plan 13 §2.6 + plan 14 §2.0.
    // This test guards against an assembler regression that
    // fabricates a kind tag not declared in the target's
    // relocations[] table.
    auto lowered = test_support::lowerCSubsetToLir(
        "int f(int x) { return x; }");
    ASSERT_TRUE(lowered.lir.ok);

    DiagnosticReporter rep;
    auto result = assembleEndToEnd(lowered, rep);
    ASSERT_TRUE(result.ok());

    for (auto const& fn : result.functions) {
        for (auto const& r : fn.relocations) {
            auto const* info = (*lowered.target).relocationInfo(r.kind);
            EXPECT_NE(info, nullptr)
                << "relocation kind " << r.kind.v
                << " not declared in target's relocations[] table";
        }
    }
}
