// tests/link/test_merge_block_symbols.cpp
// ─────────────────────────────────────────────────────────────────────────────
// D-LINK-MERGE-DOES-NOT-REMAP-BLOCK-SYMBOLS
//
// ★★★ THE DEFECT THIS PINS. `mergeModules` builds each merged function with
// `AssembledFunction out = fn;` and then remaps exactly two things: the function's
// own `symbol`, and every `Relocation::target` in its `relocations`. It did NOT
// remap `out.blockSymbols`, which the copy carried over verbatim.
//
// A synthetic block symbol is how a computed `goto` / switch JUMP TABLE names a
// basic block: the table is a DATA item whose relocations target those symbols, and
// `buildCompoundIndex` declares them from `fn.blockSymbols`. After the merge the
// DECLARATION therefore sat at the block symbol's ORIGINAL per-CU id while the
// REFERENCE had been retargeted to a freshly minted merged id. The two disagreed,
// so the target resolved against nothing and the link failed loud with
// `K_SymbolUndefined`.
//
// ⚠ IT CANNOT FIRE IN A SINGLE-CU BUILD, which is why it reached a 103-translation-
// unit program before any test saw it: with one module the mint is the identity, the
// two ids coincide, and every single-CU pin over computed goto stays green. THIS FILE
// THEREFORE LINKS TWO CUs — the second module is not decoration, it is the whole
// precondition.
//
// ✔MEASURED 2026-08-26, cycle P37, on the sqlite3 CLI for x86_64:pe64-x86_64-windows-exec:
// **1313** `K_SymbolUndefined`, every one a data-item relocation into an id that no
// `ModuleSymbol` and no `ExternImport` could name — which is exactly what a dangling
// block-symbol mint looks like, because a block symbol has neither. The same manifest
// through the compiler built at `5085664a` (the commit BEFORE the merge work landed)
// produced **0** errors, which is what attributed it to a range rather than a guess.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/alignment.hpp"
#include "core/types/target_schema.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace dss;

namespace {

struct Loaded {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};

[[nodiscard]] Loaded loadShippedPair(std::string const& targetName,
                                     std::string const& formatName) {
    Loaded out;
    auto t = TargetSchema::loadShipped(targetName);
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped(" << targetName << ") failed";
        return out;
    }
    out.target = std::move(t).value();
    auto f = ObjectFormatSchema::loadShipped(formatName);
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(" << formatName << ") failed";
        return out;
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

// CU #1 — the shape that breaks: ONE function carrying a synthetic block symbol,
// plus a DATA item (the jump table) whose abs64 relocation NAMES that block symbol.
// This is the `static void* tbl[] = {&&L0}; goto *tbl[i];` shape reduced to the two
// structures the merge actually moves.
[[nodiscard]] AssembledModule makeJumpTableOwner() {
    AssembledModule m;
    m.cuId = CompilationUnitId{1};
    m.expectedFuncCount = 1;

    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    // `mov eax,1; ret` then a second block `mov eax,2; ret` at offset 6.
    fn.bytes  = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3,
                 0xB8, 0x02, 0x00, 0x00, 0x00, 0xC3};
    // The block symbol the table points at: the SECOND block, at byte offset 6.
    fn.blockSymbols.push_back(SyntheticBlockSymbol{SymbolId{9}, 6u});
    m.functions.push_back(std::move(fn));

    AssembledData tbl;
    tbl.symbol  = SymbolId{4};
    tbl.section = DataSectionKind::Rodata;
    tbl.bytes.assign(8, 0);      // one 8-byte slot, filled by the relocation
    tbl.alignment = Alignment::of<8>();
    Relocation rel;
    rel.offset = 0;
    rel.target = SymbolId{9};    // ← the block symbol
    rel.kind   = RelocationKind{2};  // abs64 on the shipped x86_64 ELF schema
    rel.addend = 0;
    tbl.relocations.push_back(rel);
    m.dataItems.push_back(std::move(tbl));

    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "table_user", SymbolBinding::Global,
                                     SymbolVisibility::Default});
    return m;
}

// CU #2 — the second module. Its ONLY job is to make this a MERGE, which is the
// precondition the defect needs; without it the id mint is the identity and the
// declaration and the reference agree by accident.
[[nodiscard]] AssembledModule makeEntry() {
    AssembledModule m;
    m.cuId = CompilationUnitId{2};
    m.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xB8, 0x00, 0x00, 0x00, 0x00, 0xC3};   // mov eax,0 ; ret
    m.functions.push_back(std::move(fn));
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "_start", SymbolBinding::Global,
                                     SymbolVisibility::Default});
    m.userEntrySymbol = SymbolId{1};
    return m;
}

}  // namespace

// ── The pin ─────────────────────────────────────────────────────────────────
//
// Asserts the PROPERTY (the link resolves) rather than any particular id, because the
// id a block symbol receives is an implementation detail of the mint and pinning it
// would fail the next time the allocator changed for an unrelated reason. What must
// hold is that the declaration and the reference name the SAME thing — which is
// observable exactly as "no undefined symbol".
TEST(MergeBlockSymbols, DataRelocationToABlockSymbolSurvivesTheCrossCuMerge) {
    Loaded const L = loadShippedPair("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_NE(L.target, nullptr);
    ASSERT_NE(L.format, nullptr);

    std::vector<AssembledModule> mods{makeJumpTableOwner(), makeEntry()};

    DiagnosticReporter rep;
    LinkedImage const img = linker::link(std::span<AssembledModule const>{mods},
                                         *L.target, *L.format, rep);

    EXPECT_EQ(countCode(rep, DiagnosticCode::K_SymbolUndefined), 0u)
        << "a DATA relocation naming a synthetic block symbol did not resolve after "
           "the cross-CU merge. The merge remaps the function's own symbol and every "
           "relocation target; if it does not ALSO remap `blockSymbols`, the "
           "declaration keeps the per-CU id while the reference gets the merged one "
           "and the two can never meet. This is the sqlite pe64 link failure in one "
           "function and two modules.";
    EXPECT_TRUE(img.ok())
        << "the merged image must link: the only cross-module edge here is the jump "
           "table's, so a failure is that edge's.";
}
