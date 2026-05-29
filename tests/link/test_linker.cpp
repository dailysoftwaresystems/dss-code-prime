// Linker substrate tests — plan 14 LK4.
//
// Pins:
//   * Empty module → LinkedImage::ok() == false (expectedFuncCount
//     gate, parallel-index discipline borrowed from
//     AssembledModule::ok / LirAllocation::ok).
//   * Single-CU intra-module symbol resolution: a reloc whose
//     target matches an assembled function's symbol resolves, even
//     with no per-format walker plugged in (LK4 substrate).
//   * Reloc kind known to target schema but absent from format
//     schema → K_RelocationKindMismatch.
//   * Reloc kind absent from BOTH schemas → K_RelocationKindMismatch.
//   * Reloc target unknown to the module's symbol set →
//     K_SymbolUndefined.
//   * Multiple functions: parallel-index discipline preserves
//     resolvedFuncCount on success.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

using namespace dss;

namespace {

// Uses the shipped `x86_64.target.json` for the target side so the
// `relocations[]` rows the linker resolves against are the SAME tags
// the assembler actually stamps end-to-end. The linker substrate
// (LK4) never reads `opcodes[]` — it only consults
// `relocationInfo(kind)` — but loading the real schema beats a
// synthetic placeholder for readability + drift insurance.
//
// Object-format side is synthetic: no `*.format.json` ships yet (LK1
// onwards), and the test contract is the cross-side reloc-tag
// agreement — so we craft a format schema that declares the real
// `rel32` tag (kind=1) by name.

constexpr std::string_view kFormatMatchingX86_64 = R"({
  "dssObjectFormatVersion": 1,
  "format": {"name": "test-elf", "kind": "elf"},
  "relocations": [
    { "name": "R_X86_64_PC32",   "kind": 1 },
    { "name": "R_X86_64_64",     "kind": 2 },
    { "name": "R_X86_64_32",     "kind": 3 }
  ]
})";

// Same target side, but a format schema that DOESN'T declare
// the assembler-side reloc tag — isolates the format-side
// half of plan 13 §2.6's reloc-taxonomy unifier.
constexpr std::string_view kFormatMissingReloc = R"({
  "dssObjectFormatVersion": 1,
  "format": {"name": "test-elf-bare", "kind": "elf"}
})";

struct Loaded {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};

[[nodiscard]] Loaded loadMinimal(std::string_view formatText = kFormatMatchingX86_64) {
    Loaded out;
    auto t = TargetSchema::loadShipped("x86_64");
    if (!t.has_value()) {
        ADD_FAILURE() << "loader rejected shipped x86_64 target schema; diagnostics:";
        for (auto const& d : t.error()) ADD_FAILURE() << "  " << d.message;
    }
    out.target = std::move(t).value();
    auto f = ObjectFormatSchema::loadFromText(formatText);
    if (!f.has_value()) {
        ADD_FAILURE() << "loader rejected format schema; diagnostics:";
        for (auto const& d : f.error()) ADD_FAILURE() << "  " << d.message;
    }
    out.format = std::move(f).value();
    return out;
}

[[nodiscard]] std::size_t countCode(DiagnosticReporter const& rep,
                                     DiagnosticCode code) {
    std::size_t n = 0;
    for (auto const& d : rep.all()) {
        if (d.code == code) ++n;
    }
    return n;
}

} // namespace

TEST(Linker, EmptyModuleIsNotOk) {
    auto loaded = loadMinimal();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule empty{};
    DiagnosticReporter rep;
    auto image = link(empty, *loaded.target, *loaded.format, rep);
    EXPECT_FALSE(image.ok());
    EXPECT_EQ(image.expectedFuncCount, 0u);
    EXPECT_EQ(image.resolvedFuncCount, 0u);
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(Linker, NoRelocationsResolvesCleanly) {
    auto loaded = loadMinimal();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{42};
    mod.functions.push_back(std::move(fn));
    DiagnosticReporter rep;
    auto image = link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(image.ok());
    EXPECT_EQ(image.resolvedFuncCount, 1u);
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(image.format, ObjectFormatKind::Elf);
}

TEST(Linker, IntraModuleSymbolReferenceResolves) {
    auto loaded = loadMinimal();
    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction caller;
    caller.symbol = SymbolId{1};
    Relocation rel;
    rel.offset = 4;
    rel.target = SymbolId{2};       // matches `callee.symbol`
    rel.kind   = RelocationKind{1}; // matches both schemas
    caller.relocations.push_back(rel);
    AssembledFunction callee;
    callee.symbol = SymbolId{2};
    mod.functions.push_back(std::move(caller));
    mod.functions.push_back(std::move(callee));
    DiagnosticReporter rep;
    auto image = link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(image.ok());
    EXPECT_EQ(image.resolvedFuncCount, 2u);
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(Linker, UnknownSymbolEmitsK_SymbolUndefined) {
    auto loaded = loadMinimal();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    Relocation rel;
    rel.target = SymbolId{99}; // not declared by any function in module
    rel.kind   = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    DiagnosticReporter rep;
    auto image = link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_EQ(image.resolvedFuncCount, 0u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolUndefined), 1u);
}

TEST(Linker, RelocationKindMissingFromFormatEmitsMismatch) {
    auto loaded = loadMinimal(kFormatMissingReloc);
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    Relocation rel;
    rel.target = SymbolId{1};       // self-ref to isolate the kind check
    rel.kind   = RelocationKind{1}; // declared by target but NOT format
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    DiagnosticReporter rep;
    auto image = link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_EQ(image.resolvedFuncCount, 0u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_RelocationKindMismatch), 1u);
}

TEST(Linker, PartialResolutionAcrossMultipleFunctions) {
    // Pin the parallel-index `resolvedFuncCount` accounting: a
    // 3-function module where fn#1 has a bad reloc while fn#0 and
    // fn#2 are clean should resolve 2 of 3 — NOT zero (regression
    // that flips `funcResolved = false` to a module-wide latch) and
    // NOT three (regression that swallows the bad reloc silently).
    auto loaded = loadMinimal();
    AssembledModule mod;
    mod.expectedFuncCount = 3;
    // fn#0 — clean
    AssembledFunction fn0;
    fn0.symbol = SymbolId{10};
    mod.functions.push_back(std::move(fn0));
    // fn#1 — bad reloc kind
    AssembledFunction fn1;
    fn1.symbol = SymbolId{20};
    Relocation badRel;
    badRel.target = SymbolId{20};       // self-ref
    badRel.kind   = RelocationKind{99}; // not declared
    fn1.relocations.push_back(badRel);
    mod.functions.push_back(std::move(fn1));
    // fn#2 — clean
    AssembledFunction fn2;
    fn2.symbol = SymbolId{30};
    mod.functions.push_back(std::move(fn2));

    DiagnosticReporter rep;
    auto image = link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_FALSE(image.ok());
    EXPECT_EQ(image.expectedFuncCount, 3u);
    EXPECT_EQ(image.resolvedFuncCount, 2u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_RelocationKindMismatch), 1u);
}

TEST(Linker, KindMissingFromTargetEmitsMismatchWithBothSidesNamed) {
    // Pin the cross-reference unifier's symmetric half: a reloc kind
    // present on the format side but missing from the target side.
    // The diagnostic must name BOTH sides accurately (the
    // 3-agent-convergence fix that closed the "wrong-side message
    // when both miss" hole).
    auto loaded = loadMinimal();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    Relocation rel;
    rel.target = SymbolId{1};
    rel.kind   = RelocationKind{99};  // declared by NEITHER schema
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));

    DiagnosticReporter rep;
    auto image = link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_EQ(image.resolvedFuncCount, 0u);
    bool sawBothSidesNamed = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_RelocationKindMismatch) {
            bool hasTargetSide = d.actual.find(loaded.target->name()) != std::string::npos;
            bool hasFormatSide = d.actual.find(loaded.format->name()) != std::string::npos;
            if (hasTargetSide && hasFormatSide) sawBothSidesNamed = true;
        }
    }
    EXPECT_TRUE(sawBothSidesNamed)
        << "diagnostic must name BOTH target and format when both miss the kind";
}

TEST(Linker, MismatchAndUndefinedSymbolFireIndependentlyOnSameReloc) {
    // Pin the silent-failure-hunter C2 fix: if a reloc has BOTH a
    // bad kind AND an undefined target symbol, the linker should
    // surface BOTH diagnostics in ONE pass — not require two link
    // attempts (kind first, then symbol).
    auto loaded = loadMinimal();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    Relocation rel;
    rel.target = SymbolId{77};         // undefined
    rel.kind   = RelocationKind{99};   // unknown
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));

    DiagnosticReporter rep;
    auto image = link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_EQ(image.resolvedFuncCount, 0u);
    EXPECT_GE(countCode(rep, DiagnosticCode::K_RelocationKindMismatch), 1u);
    EXPECT_GE(countCode(rep, DiagnosticCode::K_SymbolUndefined), 1u);
}

TEST(Linker, UnknownRelocationKindEmitsMismatch) {
    auto loaded = loadMinimal();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    Relocation rel;
    rel.target = SymbolId{1};
    rel.kind   = RelocationKind{99}; // declared by NEITHER schema
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    DiagnosticReporter rep;
    auto image = link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_EQ(image.resolvedFuncCount, 0u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::K_RelocationKindMismatch), 1u);
}
