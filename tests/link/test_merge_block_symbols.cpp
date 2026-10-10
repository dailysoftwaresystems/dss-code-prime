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
#include "core/types/cfi.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/alignment.hpp"
#include "core/types/target_schema.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "program/program.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
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

// ═════════════════════════════════════════════════════════════════════════════
// THE OTHER REFERENCES OF A UNIT THAT THE MERGE'S COPY CARRIED VERBATIM
// ═════════════════════════════════════════════════════════════════════════════
//
// `AssembledFunction out = fn;` copies EVERY member, and the merge then renumbers the
// members it knows of. A member that names a symbol and is NOT renumbered keeps a
// number of its own UNIT inside a module numbered afresh — where that number is
// whatever the merge happened to give it: nothing at all, a symbol of another kind,
// or ANOTHER FUNCTION. The block symbols above were the first such member found.
// Reading both structs for every member that names a symbol or a position found
// three more, and each is pinned below through `linker::link`, as the block symbols
// are:
//   * an exception scope's two ids (`SehScopeEntry::filterFuncletSymbol` and
//     `personalitySymbol`);
//   * a unit's image entry (`AssembledModule::imageEntryOverride`, an INDEX into
//     that unit's `functions`);
//   * a unit's null-address symbols (`AssembledModule::nullAddressSymbols`).
// (`AssembledFunction` holds no fourth: `sourceMap` carries instruction ids, `cfi`
// registers and offsets, `blockByteOffsets` positions inside the function's own
// bytes.)
//
// ★ EVERY PIN HERE READS THE IMAGE, NEVER AN ID. Each function below ends in
// `mov eax, <four letters> ; ret`, and the image is searched for the letters: a
// function is found by what it IS, so no assertion depends on where a writer put it
// or on the number the merge gave it.

namespace {

using Bytes = std::vector<std::uint8_t>;

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) {
        s += "\n      " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    }
    return s;
}

[[nodiscard]] std::string hex(std::uint64_t v) {
    static char const digits[] = "0123456789abcdef";
    std::string s;
    do {
        s.insert(s.begin(), digits[v & 0xFu]);
        v >>= 4;
    } while (v != 0);
    return "0x" + s;
}

// A little-endian field of the image, or nothing when it does not lie inside it.
// Every offset read below is DERIVED from the image's own tables, so an image that
// was written wrong reads as "unread" and never as a read past its end.
[[nodiscard]] std::optional<std::uint64_t> le(Bytes const& b, std::uint64_t off,
                                              std::size_t width) {
    if (off > b.size() || width > b.size() - off) return std::nullopt;
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < width; ++i) {
        v |= static_cast<std::uint64_t>(b[static_cast<std::size_t>(off) + i]) << (i * 8);
    }
    return v;
}

// Where `needle` lies in `hay`, when it lies there EXACTLY once: a marker found
// twice identifies nothing.
[[nodiscard]] std::optional<std::size_t> findOnce(Bytes const& hay, Bytes const& needle) {
    std::optional<std::size_t> at;
    if (needle.empty() || hay.size() < needle.size()) return std::nullopt;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        if (!std::equal(needle.begin(), needle.end(),
                        hay.begin() + static_cast<std::ptrdiff_t>(i))) {
            continue;
        }
        if (at.has_value()) return std::nullopt;
        at = i;
    }
    return at;
}

// `mov eax, <four letters> ; ret` — the tail of every function built below.
[[nodiscard]] Bytes tailOf(std::string const& fourLetters) {
    Bytes b{0xB8};
    for (char const c : fourLetters) b.push_back(static_cast<std::uint8_t>(c));
    b.push_back(0xC3);
    return b;
}

// `sub rsp, 0x20` (7 bytes, the whole prologue of every function below that states
// a frame) and then the tail: 13 bytes.
[[nodiscard]] Bytes framedBody(std::string const& fourLetters) {
    Bytes b{0x48, 0x81, 0xEC, 0x20, 0x00, 0x00, 0x00};
    Bytes const tail = tailOf(fourLetters);
    b.insert(b.end(), tail.begin(), tail.end());
    return b;
}

[[nodiscard]] CfiFunction frameOf(std::uint32_t codeLength) {
    CfiFunction cfi;
    cfi.codeLength    = codeLength;
    cfi.initial       = CfiInitialState{4, 8, -8, std::nullopt};
    cfi.prologueEndPc = 7;
    cfi.ops = {CfiOp{7, CfiOpKind::DefCfaOffset, CfiRegRef{}, CfiRegRef{}, 0x28}};
    return cfi;
}

// The guarded region of the one guarded function a unit below holds, as offsets in
// that function's bytes; and where each kind of body keeps its tail.
constexpr std::uint32_t kGuardedBegin = 0x08;
constexpr std::uint32_t kGuardedEnd   = 0x10;
constexpr std::uint32_t kExceptBody   = 0x18;
constexpr std::size_t   kGuardedTailAt = 26;
constexpr std::size_t   kFramedTailAt  = 7;

// A unit with ONE guarded function (its tail spells "PAR" + `which`), that
// function's filter funclet ("FIL" + `which`) and the personality import. The unit
// numbers its symbols as it likes: the guarded function is #1, the funclet
// `funcletId`, the import `personalityId`. Nothing but the scope names the funclet
// or the import — exactly what the pass that synthesizes them produces.
[[nodiscard]] AssembledModule makeGuardedUnit(std::uint32_t cu, char which,
                                              std::uint32_t funcletId,
                                              std::uint32_t personalityId) {
    AssembledModule m;
    m.cuId              = CompilationUnitId{cu};
    m.expectedFuncCount = 2;

    AssembledFunction guarded;
    guarded.symbol = SymbolId{1};
    guarded.bytes  = {0x48, 0x81, 0xEC, 0x20, 0x00, 0x00, 0x00, 0xC3,   //  0..7   sub rsp,0x20 ; ret
                      0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,   //  8..15  the guarded body
                      0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,   // 16..23
                      0x90, 0x90};                                      // 24..25  the `__except` body
    {
        Bytes const tail = tailOf(std::string{"PAR"} + which);          // 26..31
        guarded.bytes.insert(guarded.bytes.end(), tail.begin(), tail.end());
    }
    guarded.cfi = frameOf(32);
    SehScopeEntry scope;
    scope.beginByteOffset      = kGuardedBegin;
    scope.endByteOffset        = kGuardedEnd;
    scope.jumpTargetByteOffset = kExceptBody;
    scope.filterFuncletSymbol  = SymbolId{funcletId};
    scope.personalitySymbol    = SymbolId{personalityId};
    guarded.sehScopes.push_back(scope);
    m.functions.push_back(std::move(guarded));

    AssembledFunction funclet;
    funclet.symbol = SymbolId{funcletId};
    funclet.bytes  = framedBody(std::string{"FIL"} + which);
    funclet.cfi    = frameOf(13);
    m.functions.push_back(std::move(funclet));

    // Stated as the pass that synthesizes it states it: EAGER, because what
    // references it is the unwind data's handler field and never a relocation.
    ExternImport personality{SymbolId{personalityId}, "__C_specific_handler",
                             "ucrtbase.dll", /*isData=*/false};
    personality.isEagerImport = true;
    m.externImports.push_back(std::move(personality));

    m.symbols.push_back(ModuleSymbol{SymbolId{1}, std::string{"guarded_"} + which,
                                     SymbolBinding::Global, SymbolVisibility::Default});
    return m;
}

// The unit that makes the link a MERGE and gives the merged module what a stale
// number can land on: the program's entry, a second FUNCTION with a frame ("DCOY")
// and a second function IMPORT (`puts`).
[[nodiscard]] AssembledModule makeProgramEntryWithDecoys(std::uint32_t cu) {
    AssembledModule m;
    m.cuId              = CompilationUnitId{cu};
    m.expectedFuncCount = 2;

    AssembledFunction entry;
    entry.symbol = SymbolId{1};
    entry.bytes  = tailOf("MAIN");
    m.functions.push_back(std::move(entry));

    AssembledFunction decoy;
    decoy.symbol = SymbolId{2};
    decoy.bytes  = framedBody("DCOY");
    decoy.cfi    = frameOf(13);
    m.functions.push_back(std::move(decoy));

    ExternImport other{SymbolId{3}, "puts", "ucrtbase.dll", /*isData=*/false};
    other.isEagerImport = true;
    m.externImports.push_back(std::move(other));

    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "main", SymbolBinding::Global,
                                     SymbolVisibility::Default});
    m.symbols.push_back(ModuleSymbol{SymbolId{2}, "decoy", SymbolBinding::Global,
                                     SymbolVisibility::Default});
    m.userEntrySymbol = SymbolId{1};
    return m;
}

// What the readers below need of a PE32+ image: its sections, its import directory
// and its exception directory (the RUNTIME_FUNCTION table).
struct PeImage {
    struct Section {
        std::uint32_t rva = 0, fileOffset = 0, fileSize = 0;
    };
    std::vector<Section> sections;
    std::uint32_t        importRva = 0;
    std::uint32_t        exceptionRva = 0, exceptionSize = 0;

    [[nodiscard]] std::optional<std::uint64_t> offsetOfRva(std::uint64_t rva) const {
        for (auto const& s : sections) {
            if (rva >= s.rva && rva - s.rva < s.fileSize) return s.fileOffset + (rva - s.rva);
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<std::uint32_t> rvaOfOffset(std::uint64_t off) const {
        for (auto const& s : sections) {
            if (off >= s.fileOffset && off - s.fileOffset < s.fileSize) {
                return static_cast<std::uint32_t>(s.rva + (off - s.fileOffset));
            }
        }
        return std::nullopt;
    }
};

[[nodiscard]] std::optional<PeImage> viewPe(Bytes const& b) {
    auto const lfanew = le(b, 0x3C, 4);
    if (!lfanew.has_value()) return std::nullopt;
    auto const signature = le(b, *lfanew, 4);
    if (!signature.has_value() || *signature != 0x00004550u) return std::nullopt;   // "PE\0\0"
    std::uint64_t const coff = *lfanew + 4;
    auto const sectionCount = le(b, coff + 2, 2);
    auto const optionalSize = le(b, coff + 16, 2);
    std::uint64_t const opt = coff + 20;
    auto const magic       = le(b, opt, 2);
    auto const directories = le(b, opt + 108, 4);
    if (!sectionCount.has_value() || !optionalSize.has_value() || !magic.has_value()
        || *magic != 0x20Bu || !directories.has_value()) {
        return std::nullopt;                                                        // PE32+ only
    }
    PeImage v;
    auto const directory = [&](std::uint64_t index, std::uint32_t& rva, std::uint32_t& size) {
        if (index >= *directories) return;
        auto const r = le(b, opt + 112 + index * 8, 4);
        auto const s = le(b, opt + 112 + index * 8 + 4, 4);
        if (r.has_value() && s.has_value()) {
            rva  = static_cast<std::uint32_t>(*r);
            size = static_cast<std::uint32_t>(*s);
        }
    };
    std::uint32_t importSize = 0;
    directory(1, v.importRva, importSize);            // IMAGE_DIRECTORY_ENTRY_IMPORT
    directory(3, v.exceptionRva, v.exceptionSize);    // IMAGE_DIRECTORY_ENTRY_EXCEPTION
    std::uint64_t const table = opt + *optionalSize;
    for (std::uint64_t i = 0; i < *sectionCount; ++i) {
        std::uint64_t const s = table + i * 40;
        auto const rva      = le(b, s + 12, 4);
        auto const fileSize = le(b, s + 16, 4);
        auto const fileAt   = le(b, s + 20, 4);
        if (!rva.has_value() || !fileSize.has_value() || !fileAt.has_value()) return std::nullopt;
        v.sections.push_back(PeImage::Section{static_cast<std::uint32_t>(*rva),
                                              static_cast<std::uint32_t>(*fileAt),
                                              static_cast<std::uint32_t>(*fileSize)});
    }
    return v;
}

// The name the import-address slot at `slotRva` is bound to, read from the image's
// import directory. The walks are bounded: a table with no terminator must not loop.
[[nodiscard]] std::optional<std::string> importOfSlot(Bytes const& b, PeImage const& v,
                                                      std::uint64_t slotRva) {
    if (v.importRva == 0) return std::nullopt;
    for (std::uint64_t d = 0; d < 4096; ++d) {
        auto const at = v.offsetOfRva(std::uint64_t{v.importRva} + d * 20);
        if (!at.has_value()) return std::nullopt;
        auto const lookup = le(b, *at, 4);          // OriginalFirstThunk
        auto const name   = le(b, *at + 12, 4);
        auto const slots  = le(b, *at + 16, 4);     // FirstThunk
        if (!lookup.has_value() || !name.has_value() || !slots.has_value()) return std::nullopt;
        if (*lookup == 0 && *name == 0 && *slots == 0) return std::nullopt;   // the terminator
        std::uint64_t const names = *lookup != 0 ? *lookup : *slots;
        for (std::uint64_t k = 0; k < 65536; ++k) {
            auto const entryAt = v.offsetOfRva(names + k * 8);
            if (!entryAt.has_value()) break;
            auto const entry = le(b, *entryAt, 8);
            if (!entry.has_value() || *entry == 0) break;
            if (*slots + k * 8 != slotRva) continue;
            if ((*entry >> 63) != 0) return "#" + std::to_string(*entry & 0xFFFFu);   // by ordinal
            auto const nameAt = v.offsetOfRva((*entry & 0x7FFFFFFFu) + 2);            // past the hint
            if (!nameAt.has_value()) return std::nullopt;
            std::string s;
            for (std::uint64_t c = *nameAt; c < b.size() && b[static_cast<std::size_t>(c)] != 0; ++c) {
                s.push_back(static_cast<char>(b[static_cast<std::size_t>(c)]));
            }
            return s;
        }
    }
    return std::nullopt;
}

// The import an x86_64 PE import thunk (`jmp qword ptr [rip + disp32]`, FF 25)
// jumps through — or nothing, when `thunkRva` is no such thunk.
[[nodiscard]] std::optional<std::string> importOfThunk(Bytes const& b, PeImage const& v,
                                                       std::uint64_t thunkRva) {
    auto const at = v.offsetOfRva(thunkRva);
    if (!at.has_value()) return std::nullopt;
    auto const opcode = le(b, *at, 2);
    auto const disp   = le(b, *at + 2, 4);
    if (!opcode.has_value() || *opcode != 0x25FFu || !disp.has_value()) return std::nullopt;
    auto const signedDisp = static_cast<std::int64_t>(static_cast<std::int32_t>(
        static_cast<std::uint32_t>(*disp)));
    return importOfSlot(b, v, thunkRva + 6 + static_cast<std::uint64_t>(signedDisp));
}

// The RVA at which the function whose tail spells `fourLetters` begins; `tailAt` is
// where that tail lies in its body.
[[nodiscard]] std::optional<std::uint32_t> rvaOfFunction(Bytes const& b, PeImage const& v,
                                                         std::string const& fourLetters,
                                                         std::size_t tailAt) {
    auto const at = findOnce(b, tailOf(fourLetters));
    if (!at.has_value() || *at < tailAt) return std::nullopt;
    return v.rvaOfOffset(*at - tailAt);
}

// What an image says of ONE guarded function: which function its scope record names
// as the filter, and which import its handler field reaches.
struct ScopeReading {
    bool          read = false;
    std::string   why;                 // when it could not be read
    bool          rangeRight = false;
    bool          filterRight = false;
    bool          handlerRight = false;
    std::uint32_t handlerRva = 0;
    std::string   filterIs;            // what the filter field names, in words
    std::string   handlerIs;           // the import the handler field reaches, or that it reaches none
};

[[nodiscard]] ScopeReading readScope(Bytes const& b, PeImage const& v, char which,
                                     std::string const& guardedUnits) {
    ScopeReading r;
    auto const guardedRva = rvaOfFunction(b, v, std::string{"PAR"} + which, kGuardedTailAt);
    auto const funcletRva = rvaOfFunction(b, v, std::string{"FIL"} + which, kFramedTailAt);
    if (!guardedRva.has_value() || !funcletRva.has_value()) {
        r.why = "the guarded function or its funclet is not in the image exactly once";
        return r;
    }
    if (v.exceptionRva == 0) {
        r.why = "the image has no exception directory";
        return r;
    }
    for (std::uint64_t k = 0; (k + 1) * 12 <= v.exceptionSize; ++k) {
        auto const at = v.offsetOfRva(std::uint64_t{v.exceptionRva} + k * 12);
        if (!at.has_value()) break;
        auto const begin  = le(b, *at, 4);
        auto const unwind = le(b, *at + 8, 4);
        if (!begin.has_value() || !unwind.has_value()) break;
        if (*begin != *guardedRva) continue;
        auto const u = v.offsetOfRva(*unwind);
        if (!u.has_value()) {
            r.why = "the guarded function's UNWIND_INFO lies in no section";
            return r;
        }
        auto const versionAndFlags = le(b, *u, 1);
        auto const codes           = le(b, *u + 2, 1);
        if (!versionAndFlags.has_value() || !codes.has_value()) {
            r.why = "the guarded function's UNWIND_INFO is cut short";
            return r;
        }
        if (((*versionAndFlags >> 3) & 0x1u) == 0) {
            r.why = "the guarded function's UNWIND_INFO states no exception handler "
                    "(UNW_FLAG_EHANDLER)";
            return r;
        }
        // The header, then the unwind codes padded to an even count; then the handler
        // RVA, and the C scope table: a count and {Begin, End, Handler, JumpTarget}.
        std::uint64_t const after = *u + 4 + 2 * ((*codes + 1) & ~std::uint64_t{1});
        auto const handler = le(b, after, 4);
        auto const count   = le(b, after + 4, 4);
        auto const sBegin  = le(b, after + 8, 4);
        auto const sEnd    = le(b, after + 12, 4);
        auto const sFilter = le(b, after + 16, 4);
        auto const sTarget = le(b, after + 20, 4);
        if (!handler.has_value() || !count.has_value() || !sBegin.has_value()
            || !sEnd.has_value() || !sFilter.has_value() || !sTarget.has_value()) {
            r.why = "the guarded function's scope table is cut short";
            return r;
        }
        if (*count != 1) {
            r.why = "the scope table holds " + std::to_string(*count)
                    + " records where the unit states one";
            return r;
        }
        r.read       = true;
        r.rangeRight = *sBegin == *guardedRva + kGuardedBegin
                       && *sEnd == *guardedRva + kGuardedEnd
                       && *sTarget == *guardedRva + kExceptBody;
        r.filterRight = *sFilter == *funcletRva;
        r.filterIs    = "RVA " + hex(*sFilter) + ", where none of this test's functions begins";
        auto const named = [&](std::string const& letters, std::size_t tailAt, std::string words) {
            if (rvaOfFunction(b, v, letters, tailAt) == std::optional<std::uint32_t>{
                    static_cast<std::uint32_t>(*sFilter)}) {
                r.filterIs = std::move(words);
            }
        };
        for (char const unit : guardedUnits) {
            named(std::string{"PAR"} + unit, kGuardedTailAt,
                  std::string{"the guarded function `guarded_"} + unit + "` itself");
            named(std::string{"FIL"} + unit, kFramedTailAt,
                  std::string{"the filter funclet of `guarded_"} + unit + "`");
        }
        named("DCOY", kFramedTailAt, "`decoy`, a function of another unit");
        named("MAIN", 0, "`main`");
        r.handlerRva = static_cast<std::uint32_t>(*handler);
        auto const import = importOfThunk(b, v, *handler);
        r.handlerIs    = import.has_value() ? "the import `" + *import + "`"
                                            : std::string{"no import thunk at all"};
        r.handlerRight = import == std::optional<std::string>{"__C_specific_handler"};
        return r;
    }
    r.why = "no RUNTIME_FUNCTION begins at the guarded function";
    return r;
}

}  // namespace

// ── The exception scopes ─────────────────────────────────────────────────────
//
// THE PROPERTY: a guarded function's scope record names ITS OWN filter funclet, and
// its handler field reaches the PERSONALITY, whatever numbers the unit gave those
// two symbols.
//
// ★★ THE DEFECT. The merge renumbered the guarded function, its block symbols and
// every relocation target, and left the two ids of each `SehScopeEntry` as the unit
// wrote them. The PE writer then resolved those numbers in the MERGED module:
//   * a number no merged symbol has, or one that is not a function: REFUSED
//     ("names symbol #N, which is not a defined function in this image") — what a C
//     program with a `__try` met the moment it was linked beside any other unit
//     (✔MEASURED 2026-10-08: such a unit alone exits 42; beside one foreign object,
//     or with one archive member pulled, the link is refused);
//   * a number that IS another function's, with one that IS another import's: the
//     link SUCCEEDS and the scope table names the wrong filter and the wrong
//     handler. No diagnostic. The first fault inside the guarded region then runs
//     some other function as its filter.
//
// ★ WHY A SWEEP AND NOT ONE CHOICE OF NUMBERS. Whether a stale number is refused,
// lands on another function, or lands on the RIGHT one by accident depends on the
// order the merge mints in — which is not this test's to know, and moves when the
// allocator does. One fixed choice would be a pin that goes blind the day its
// numbers happen to coincide with the merge's. So the unit's two ids are swept over
// every pair of a range wider than the whole merged module, in both link orders: at
// most one funclet number and one personality number can coincide per order, every
// other labelling fails without the renumbering, and the line this test prints says
// how many of them failed LOUD and how many failed SILENT.
TEST(MergeSehScopes, AScopeNamesItsOwnFuncletAndThePersonalityWhateverNumbersItsUnitGaveThem) {
    Loaded const L = loadShippedPair("x86_64", "pe64-x86_64-windows-exec");
    ASSERT_NE(L.target, nullptr);
    ASSERT_NE(L.format, nullptr);

    std::size_t links = 0, right = 0, refused = 0, unread = 0;
    std::size_t wrongFilter = 0, wrongHandler = 0, wrongBoth = 0;
    std::vector<std::string> firstWrong;
    auto const note = [&](std::string line) {
        if (firstWrong.size() < 6) firstWrong.push_back(std::move(line));
    };

    for (bool const guardedFirst : {true, false}) {
        for (std::uint32_t funcletId = 2; funcletId <= 9; ++funcletId) {
            for (std::uint32_t personalityId = 2; personalityId <= 9; ++personalityId) {
                if (personalityId == funcletId) continue;
                std::vector<AssembledModule> mods;
                if (guardedFirst) {
                    mods.push_back(makeGuardedUnit(1, 'A', funcletId, personalityId));
                    mods.push_back(makeProgramEntryWithDecoys(2));
                } else {
                    mods.push_back(makeProgramEntryWithDecoys(1));
                    mods.push_back(makeGuardedUnit(2, 'A', funcletId, personalityId));
                }
                DiagnosticReporter rep;
                LinkedImage const img = linker::link(std::span<AssembledModule const>{mods},
                                                     *L.target, *L.format, rep);
                ++links;
                std::string const labelling =
                    std::string{guardedFirst ? "guarded unit first" : "guarded unit second"}
                    + ", funclet #" + std::to_string(funcletId) + ", personality #"
                    + std::to_string(personalityId);
                if (!img.ok()) {
                    ++refused;
                    note(labelling + ": REFUSED" + diagnosticsOf(rep));
                    continue;
                }
                auto const view = viewPe(img.bytes);
                if (!view.has_value()) {
                    ++unread;
                    note(labelling + ": linked, and the artifact is not a PE32+ image");
                    continue;
                }
                ScopeReading const r = readScope(img.bytes, *view, 'A', "A");
                if (!r.read || !r.rangeRight) {
                    ++unread;
                    note(labelling + ": linked, and "
                         + (r.read ? std::string{"the scope's own range is not the unit's"} : r.why));
                    continue;
                }
                if (r.filterRight && r.handlerRight) {
                    ++right;
                    continue;
                }
                if (!r.filterRight && !r.handlerRight) {
                    ++wrongBoth;
                } else if (!r.filterRight) {
                    ++wrongFilter;
                } else {
                    ++wrongHandler;
                }
                note(labelling + ": LINKED with no diagnostic, and the scope's filter is "
                     + r.filterIs + "; its handler field reaches " + r.handlerIs);
            }
        }
    }

    std::cout << "[seh-merge] " << links << " labellings of one guarded unit beside another unit: " << right
              << " name their own funclet and the personality, " << refused << " refused, "
              << (wrongFilter + wrongHandler + wrongBoth) << " linked SILENTLY wrong (" << wrongFilter
              << " the filter alone, " << wrongHandler << " the handler alone, " << wrongBoth << " both), "
              << unread << " unread\n";
    std::string firsts;
    for (auto const& line : firstWrong) firsts += "\n  " + line;
    EXPECT_EQ(right, links)
        << "a guarded function's scope must name its own filter funclet and the "
           "personality whatever numbers its unit gave them — the merge renumbers "
           "every symbol of the link, so an id it leaves as the unit wrote it names "
           "whatever the merge gave that number. Of "
        << links << " labellings: " << refused << " refused, "
        << (wrongFilter + wrongHandler + wrongBoth) << " linked silently wrong, " << unread
        << " unread. The first of them:" << firsts;
}

// Two guarded units, each numbering its own funclet #2 and its own row of the one
// personality #3. After the merge there is ONE import of that name: both scope
// tables must reach it, and each must still name its OWN funclet — the property one
// guarded unit cannot show, since with one there is no second funclet to confuse it
// with and no second row to fold.
TEST(MergeSehScopes, TwoGuardedUnitsShareThePersonalityAndKeepTheirOwnFunclets) {
    Loaded const L = loadShippedPair("x86_64", "pe64-x86_64-windows-exec");
    ASSERT_NE(L.target, nullptr);
    ASSERT_NE(L.format, nullptr);

    std::vector<AssembledModule> mods;
    mods.push_back(makeGuardedUnit(1, 'A', 2, 3));
    mods.push_back(makeGuardedUnit(2, 'B', 2, 3));
    mods.push_back(makeProgramEntryWithDecoys(3));
    DiagnosticReporter rep;
    LinkedImage const img = linker::link(std::span<AssembledModule const>{mods}, *L.target,
                                         *L.format, rep);
    ASSERT_TRUE(img.ok()) << "two units that each guard a region must link together:"
                          << diagnosticsOf(rep);
    auto const view = viewPe(img.bytes);
    ASSERT_TRUE(view.has_value()) << "the artifact is not a PE32+ image";

    ScopeReading const a = readScope(img.bytes, *view, 'A', "AB");
    ScopeReading const b = readScope(img.bytes, *view, 'B', "AB");
    ASSERT_TRUE(a.read) << "unit A's scope: " << a.why;
    ASSERT_TRUE(b.read) << "unit B's scope: " << b.why;
    EXPECT_TRUE(a.rangeRight);
    EXPECT_TRUE(b.rangeRight);
    EXPECT_TRUE(a.filterRight) << "unit A's scope names " << a.filterIs;
    EXPECT_TRUE(b.filterRight) << "unit B's scope names " << b.filterIs;
    EXPECT_TRUE(a.handlerRight) << "unit A's handler field reaches " << a.handlerIs;
    EXPECT_TRUE(b.handlerRight) << "unit B's handler field reaches " << b.handlerIs;
    EXPECT_EQ(a.handlerRva, b.handlerRva)
        << "the two units' rows of the personality are ONE import of the image, so both "
           "handler fields hold the one thunk's address";
}

// ── Through the PROGRAM: regions beside a member the archive search pulls ────
//
// The pins above hand `linker::link` units this file built. This one hands the
// PROGRAM a C source, because that is where the defect was met a second time (the
// probes of the `__try` lowering, 2026-10-08): a guarded region in a program that
// calls `printf`. On this format DSS ships `printf`'s body as source — a member of
// the runtime's own archive — so the call alone makes the archive search pull a
// member, and the link a merge of two units. ✔MEASURED 2026-10-08 on the bytes
// before the fix, baseline and release: one region and a `printf` REFUSED ("a SEH
// scope-table field at `.xdata` offset 24 names symbol #563, which is not a
// defined function in this image"); the same two regions in a program that prints
// nothing link and exit 42. Every `__try` program that prints.
//
// WHAT IS READ, on every host (the image is never run here: the example
// `seh_scopes_beside_a_foreign_object` runs the same shape on Windows):
//   * the program links, in both configurations;
//   * the printing program's image imports a `__stdio_common_v*` core, which the
//     source does not name: the witness that the runtime's member IS in the link,
//     so the arm measured a merge. The CONTROL — the same source without the call —
//     imports none: one unit, the shape the base already linked;
//   * each guarded function's handler field reaches the import
//     `__C_specific_handler`;
//   * each scope's filter is the funclet that compares ITS OWN status. The two
//     regions accept different faults and each `__except` body assigns a constant
//     of its own, so a guarded function is told from the other by the constant its
//     body holds, and a funclet by the status it compares.
namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t kAccessViolation = 0xC0000005u;   // what `guarded_read`'s filter accepts
constexpr std::uint32_t kDivideByZero    = 0xC0000094u;   // what `guarded_divide`'s accepts
constexpr std::uint32_t kReadCaught      = 0x5EA0AC01u;   // what `guarded_read`'s `__except` body assigns
constexpr std::uint32_t kDivideCaught    = 0x5EA0D1F1u;   // what `guarded_divide`'s assigns

// The program: two guarded functions whose filters accept different faults; with
// `prints`, a `printf` of what each caught.
[[nodiscard]] std::string guardedProgram(bool prints) {
    std::string s;
    if (prints) s += "#include <stdio.h>\n";
    s += "#include <windows.h>\n"
         "static int guarded_read(void *p) {\n"
         "    int rc = 0;\n"
         "    __try {\n"
         "        rc = *(volatile int *)p;\n"
         "    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) {\n"
         "        rc = 0x5EA0AC01;\n"
         "    }\n"
         "    return rc;\n"
         "}\n"
         "static int guarded_divide(int by) {\n"
         "    int rc = 0;\n"
         "    __try {\n"
         "        rc = 100 / by;\n"
         "    } __except (GetExceptionCode() == 0xC0000094u) {\n"
         "        rc = 0x5EA0D1F1;\n"
         "    }\n"
         "    return rc;\n"
         "}\n"
         "int main(void) {\n"
         "    void *p = VirtualAlloc(0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);\n"
         "    volatile int zero = 0;\n"
         "    int const read = guarded_read(p);\n"
         "    int const divided = guarded_divide(zero);\n";
    if (prints) s += "    printf(\"%d %d\\n\", read != 0, divided != 0);\n";
    s += "    return (read != 0) + (divided != 0) == 2 ? 42 : 1;\n"
         "}\n";
    return s;
}

[[nodiscard]] Bytes fileBytesOf(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Every name the image imports (an import by ordinal has none and is passed over).
// The walks are bounded, as `importOfSlot`'s are.
[[nodiscard]] std::vector<std::string> importNamesOf(Bytes const& b, PeImage const& v) {
    std::vector<std::string> names;
    if (v.importRva == 0) return names;
    for (std::uint64_t d = 0; d < 4096; ++d) {
        auto const at = v.offsetOfRva(std::uint64_t{v.importRva} + d * 20);
        if (!at.has_value()) break;
        auto const lookup = le(b, *at, 4);          // OriginalFirstThunk
        auto const name   = le(b, *at + 12, 4);
        auto const slots  = le(b, *at + 16, 4);     // FirstThunk
        if (!lookup.has_value() || !name.has_value() || !slots.has_value()) break;
        if (*lookup == 0 && *name == 0 && *slots == 0) break;   // the terminator
        std::uint64_t const table = *lookup != 0 ? *lookup : *slots;
        for (std::uint64_t k = 0; k < 65536; ++k) {
            auto const entryAt = v.offsetOfRva(table + k * 8);
            if (!entryAt.has_value()) break;
            auto const entry = le(b, *entryAt, 8);
            if (!entry.has_value() || *entry == 0) break;
            if ((*entry >> 63) != 0) continue;                                     // by ordinal
            auto const nameAt = v.offsetOfRva((*entry & 0x7FFFFFFFu) + 2);         // past the hint
            if (!nameAt.has_value()) break;
            std::string s;
            for (std::uint64_t c = *nameAt; c < b.size() && b[static_cast<std::size_t>(c)] != 0; ++c) {
                s.push_back(static_cast<char>(b[static_cast<std::size_t>(c)]));
            }
            names.push_back(std::move(s));
        }
    }
    return names;
}

// Whether the image's bytes at RVAs [from, to) hold the 32-bit constant `value`
// as an immediate — in either sign, since a compare against C may be written
// `cmp r, C`, `sub r, C` or `add r, -C`.
[[nodiscard]] bool holdsConstant(Bytes const& b, PeImage const& v, std::uint32_t from,
                                 std::uint32_t to, std::uint32_t value) {
    for (std::uint32_t const wanted : {value, static_cast<std::uint32_t>(0u - value)}) {
        for (std::uint64_t rva = from; rva + 4 <= to; ++rva) {
            auto const at = v.offsetOfRva(rva);
            if (!at.has_value()) break;
            if (le(b, *at, 4) == std::optional<std::uint64_t>{wanted}) return true;
        }
    }
    return false;
}

// What an image says of one function whose unwind data states an exception
// handler: its extent, its handler field, and each scope record's filter.
struct GuardedOfAProgram {
    std::uint32_t              begin = 0, end = 0;
    std::uint32_t              handlerRva = 0;
    std::vector<std::uint32_t> filters;
};

// Every such function of the image; `begins` receives where EVERY function the
// exception directory describes begins. Nothing, and `why`, when a table could not
// be read.
[[nodiscard]] std::optional<std::vector<GuardedOfAProgram>>
guardedFunctionsOf(Bytes const& b, PeImage const& v, std::vector<std::uint32_t>& begins,
                   std::string& why) {
    std::vector<GuardedOfAProgram> out;
    if (v.exceptionRva == 0) {
        why = "the image has no exception directory";
        return std::nullopt;
    }
    for (std::uint64_t k = 0; (k + 1) * 12 <= v.exceptionSize; ++k) {
        auto const at = v.offsetOfRva(std::uint64_t{v.exceptionRva} + k * 12);
        if (!at.has_value()) {
            why = "the exception directory lies in no section";
            return std::nullopt;
        }
        auto const begin  = le(b, *at, 4);
        auto const end    = le(b, *at + 4, 4);
        auto const unwind = le(b, *at + 8, 4);
        if (!begin.has_value() || !end.has_value() || !unwind.has_value()) {
            why = "a RUNTIME_FUNCTION is cut short";
            return std::nullopt;
        }
        begins.push_back(static_cast<std::uint32_t>(*begin));
        auto const u = v.offsetOfRva(*unwind);
        if (!u.has_value()) {
            why = "the UNWIND_INFO of the function at RVA " + hex(*begin) + " lies in no section";
            return std::nullopt;
        }
        auto const versionAndFlags = le(b, *u, 1);
        auto const codes           = le(b, *u + 2, 1);
        if (!versionAndFlags.has_value() || !codes.has_value()) {
            why = "the UNWIND_INFO of the function at RVA " + hex(*begin) + " is cut short";
            return std::nullopt;
        }
        if (((*versionAndFlags >> 3) & 0x1u) == 0) continue;   // no UNW_FLAG_EHANDLER
        // The header, the unwind codes padded to an even count, the handler RVA, and
        // the C scope table: a count and {Begin, End, Handler, JumpTarget} records.
        std::uint64_t const after = *u + 4 + 2 * ((*codes + 1) & ~std::uint64_t{1});
        auto const handler = le(b, after, 4);
        auto const count   = le(b, after + 4, 4);
        if (!handler.has_value() || !count.has_value() || *count > 64) {
            why = "the scope table of the function at RVA " + hex(*begin)
                  + " is cut short, or counts more than 64 records";
            return std::nullopt;
        }
        GuardedOfAProgram g;
        g.begin      = static_cast<std::uint32_t>(*begin);
        g.end        = static_cast<std::uint32_t>(*end);
        g.handlerRva = static_cast<std::uint32_t>(*handler);
        for (std::uint64_t r = 0; r < *count; ++r) {
            auto const filter = le(b, after + 8 + r * 16 + 8, 4);
            if (!filter.has_value()) {
                why = "a scope record of the function at RVA " + hex(*begin) + " is cut short";
                return std::nullopt;
            }
            g.filters.push_back(static_cast<std::uint32_t>(*filter));
        }
        out.push_back(std::move(g));
    }
    return out;
}

}  // namespace

TEST(MergeSehScopes, AProgramsRegionsBesideAPulledRuntimeMemberEachNameTheirOwnFilter) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "seh-merge-program"};
    fs::path const dir = scratch.path();
    struct Arm {
        char const*   name;
        char const*   directory;
        bool          prints;
        CompileConfig config;
    };
    for (Arm const& arm :
         {Arm{"baseline, printing: the runtime's stdio member is pulled", "printing_baseline", true,
              CompileConfig::Debug},
          Arm{"release, printing: the runtime's stdio member is pulled", "printing_release", true,
              CompileConfig::Release},
          Arm{"CONTROL, baseline: one unit, no merge", "alone_baseline", false, CompileConfig::Debug},
          Arm{"CONTROL, release: one unit, no merge", "alone_release", false, CompileConfig::Release}}) {
        SCOPED_TRACE(arm.name);
        fs::path const out = dir / arm.directory;
        fs::create_directories(out);
        fs::path const source = out / "main.c";
        { std::ofstream(source, std::ios::binary) << guardedProgram(arm.prints); }
        Program p;
        p.setOutputDir(out);
        p.setCompileConfig(arm.config);
        DiagnosticReporter rep;
        int const rc = p.compileFiles(std::vector<std::string>{source.string()}, "c",
                                      std::vector<std::string>{"x86_64:pe64-x86_64-windows-exec"}, rep);
        std::size_t errors = 0;
        std::string errorText;
        for (auto const& d : rep.all()) {
            if (d.severity != DiagnosticSeverity::Error) continue;
            ++errors;
            errorText += "\n      " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
        }
        ASSERT_EQ(rc, 0) << "a program that guards a region must link"
                         << (arm.prints ? " beside the runtime member its `printf` pulls" : "") << ":"
                         << errorText;
        ASSERT_EQ(errors, 0u) << errorText;
        fs::path image;
        for (auto const& e : fs::directory_iterator(out)) {
            if (e.is_regular_file() && e.path().extension() == ".exe") image = e.path();
        }
        ASSERT_FALSE(image.empty()) << "no image under " << out.string();
        Bytes const bytes = fileBytesOf(image);
        auto const view = viewPe(bytes);
        ASSERT_TRUE(view.has_value()) << "the artifact is not a PE32+ image";

        // The witness that the arm is what it says: a merge, or one unit.
        std::string stdioCore;
        for (auto const& name : importNamesOf(bytes, *view)) {
            if (name.rfind("__stdio_common_v", 0) == 0) stdioCore = name;
        }
        if (arm.prints) {
            EXPECT_FALSE(stdioCore.empty())
                << "the printing program's image imports no `__stdio_common_v*` core: the runtime's "
                   "stdio member is not in the link, so this arm measured no merge";
        } else {
            EXPECT_TRUE(stdioCore.empty())
                << "the CONTROL imports `" << stdioCore << "`: a second unit is in its link";
        }

        std::vector<std::uint32_t> begins;
        std::string                why;
        auto const guarded = guardedFunctionsOf(bytes, *view, begins, why);
        ASSERT_TRUE(guarded.has_value()) << why;
        ASSERT_EQ(guarded->size(), 2u) << "the program guards a region in each of two functions";
        std::vector<std::uint32_t> everyFilter;
        for (auto const& g : *guarded) {
            everyFilter.insert(everyFilter.end(), g.filters.begin(), g.filters.end());
        }
        bool        sawRead = false, sawDivide = false;
        std::string said;
        for (auto const& g : *guarded) {
            bool const isRead   = holdsConstant(bytes, *view, g.begin, g.end, kReadCaught);
            bool const isDivide = holdsConstant(bytes, *view, g.begin, g.end, kDivideCaught);
            ASSERT_NE(isRead, isDivide)
                << "the guarded function at RVA " << hex(g.begin) << " holds "
                << (isRead ? "BOTH" : "NEITHER")
                << " of the two `__except` constants, so it cannot be told which function it is";
            (isRead ? sawRead : sawDivide) = true;
            std::string const who = isRead ? "guarded_read" : "guarded_divide";
            auto const import = importOfThunk(bytes, *view, g.handlerRva);
            EXPECT_TRUE(import == std::optional<std::string>{"__C_specific_handler"})
                << who << "'s handler field (RVA " << hex(g.handlerRva) << ") reaches "
                << (import.has_value() ? "the import `" + *import + "`" : std::string{"no import thunk"});
            ASSERT_EQ(g.filters.size(), 1u) << who << " guards one region";
            std::uint32_t const filter = g.filters[0];
            // The funclet ends where the next function, or the next funclet, begins.
            std::uint32_t bound = filter + 512;
            for (std::uint32_t const next : begins) {
                if (next > filter && next < bound) bound = next;
            }
            for (std::uint32_t const next : everyFilter) {
                if (next > filter && next < bound) bound = next;
            }
            std::uint32_t const own   = isRead ? kAccessViolation : kDivideByZero;
            std::uint32_t const other = isRead ? kDivideByZero : kAccessViolation;
            EXPECT_TRUE(holdsConstant(bytes, *view, filter, bound, own))
                << who << "'s scope names a filter (RVA " << hex(filter) << ".." << hex(bound)
                << ") that does not compare the status its `__except` accepts, " << hex(own);
            EXPECT_FALSE(holdsConstant(bytes, *view, filter, bound, other))
                << who << "'s scope names a filter (RVA " << hex(filter) << ".." << hex(bound)
                << ") that compares the OTHER region's status, " << hex(other);
            said += " " + who + " at " + hex(g.begin) + " -> filter " + hex(filter) + ";";
        }
        EXPECT_TRUE(sawRead && sawDivide) << "one guarded function of each kind";
        std::cout << "[seh-merge-program] " << arm.name << ": linked"
                  << (stdioCore.empty() ? std::string{", no stdio core imported"}
                                        : ", imports `" + stdioCore + "`")
                  << ";" << said << "\n";
    }
}

// ── The image entry a unit states, and its null-address symbols ──────────────

namespace {

// A unit of plain functions, numbered 1.. in order: each `mov eax, <letters> ; ret`
// under a name of its own.
struct NamedBody {
    std::string   name;
    std::string   fourLetters;
    SymbolBinding binding = SymbolBinding::Global;
};

[[nodiscard]] AssembledModule makePlainUnit(std::uint32_t cu, std::vector<NamedBody> const& bodies) {
    AssembledModule m;
    m.cuId              = CompilationUnitId{cu};
    m.expectedFuncCount = bodies.size();
    std::uint32_t id = 0;
    for (auto const& body : bodies) {
        AssembledFunction fn;
        fn.symbol = SymbolId{++id};
        fn.bytes  = tailOf(body.fourLetters);
        m.functions.push_back(std::move(fn));
        m.symbols.push_back(ModuleSymbol{SymbolId{id}, body.name, body.binding,
                                         SymbolVisibility::Default});
    }
    return m;
}

// The virtual address an ELF64 image loads file offset `off` at: its PT_LOAD
// segment's.
[[nodiscard]] std::optional<std::uint64_t> elfVaOfOffset(Bytes const& b, std::uint64_t off) {
    auto const phoff     = le(b, 0x20, 8);
    auto const phentsize = le(b, 0x36, 2);
    auto const phnum     = le(b, 0x38, 2);
    if (!phoff.has_value() || !phentsize.has_value() || !phnum.has_value()) return std::nullopt;
    for (std::uint64_t i = 0; i < *phnum; ++i) {
        std::uint64_t const ph = *phoff + i * *phentsize;
        auto const type   = le(b, ph, 4);
        auto const offset = le(b, ph + 8, 8);
        auto const vaddr  = le(b, ph + 16, 8);
        auto const filesz = le(b, ph + 32, 8);
        if (!type.has_value() || !offset.has_value() || !vaddr.has_value() || !filesz.has_value()) {
            return std::nullopt;
        }
        if (*type != 1) continue;                                         // PT_LOAD
        if (off >= *offset && off - *offset < *filesz) return *vaddr + (off - *offset);
    }
    return std::nullopt;
}

// The address the function whose whole body is `mov eax, <letters> ; ret` is loaded at.
[[nodiscard]] std::optional<std::uint64_t> elfVaOfFunction(Bytes const& b,
                                                           std::string const& fourLetters) {
    auto const at = findOnce(b, tailOf(fourLetters));
    if (!at.has_value()) return std::nullopt;
    return elfVaOfOffset(b, *at);
}

struct LinkedElf {
    LinkedImage        image;
    DiagnosticReporter rep;
};

void linkElf(std::vector<AssembledModule> const& mods, LinkedElf& out) {
    Loaded const L = loadShippedPair("x86_64", "elf64-x86_64-linux-exec");
    ASSERT_NE(L.target, nullptr);
    ASSERT_NE(L.format, nullptr);
    out.image = linker::link(std::span<AssembledModule const>{mods}, *L.target, *L.format,
                             out.rep);
}

}  // namespace

// ★★ THE DEFECT. `imageEntryOverride` says which function of a unit IS the image's
// entry (and that no entry is to be synthesized in front of it). The merge never
// read it, so the merged module carried none: a link of several units DROPPED the
// caller's entry in silence. With no unit naming a user entry the link was then
// refused for a reason that names something else; with one naming it, the link
// SUCCEEDED and the image started at an entry the linker synthesized instead of
// where the caller said. One unit alone always kept its entry — the merge is not on
// that path — which is why no test saw it.
TEST(MergeUnitReferences, AUnitsImageEntryIsTheMergedImagesEntry) {
    // Four arms. The stating unit FIRST or SECOND in the link: second, its entry's
    // index in the unit (1) is not its position in the merged module (3), so an
    // index carried as it stands names another unit's function. And with or without
    // ANOTHER unit naming a user entry: with one, forgetting the entry LINKS and
    // starts at an entry the linker synthesizes.
    struct Arm {
        bool statingUnitIsSecond;
        bool anotherUnitNamesAUserEntry;
    };
    for (Arm const arm : {Arm{false, false}, Arm{false, true}, Arm{true, false}, Arm{true, true}}) {
        SCOPED_TRACE(std::string{arm.statingUnitIsSecond ? "the stating unit is second" : "the stating unit is first"}
                     + (arm.anotherUnitNamesAUserEntry ? ", beside a unit that names a user entry"
                                                       : ", and no unit names a user entry"));
        AssembledModule stating = makePlainUnit(arm.statingUnitIsSecond ? 2 : 1,
                                                {{"first", "FRST"}, {"chosen", "CHSN"}});
        stating.imageEntryOverride = 1;                       // `chosen`: this unit's SECOND function
        AssembledModule other = makePlainUnit(arm.statingUnitIsSecond ? 1 : 2,
                                              {{"helper", "HLPR"}, {"spare", "SPAR"}});
        if (arm.anotherUnitNamesAUserEntry) other.userEntrySymbol = SymbolId{1};
        std::vector<AssembledModule> mods;
        if (arm.statingUnitIsSecond) {
            mods.push_back(std::move(other));
            mods.push_back(std::move(stating));
        } else {
            mods.push_back(std::move(stating));
            mods.push_back(std::move(other));
        }

        LinkedElf l;
        ASSERT_NO_FATAL_FAILURE(linkElf(mods, l));
        ASSERT_TRUE(l.image.ok()) << "a unit that states the image's entry must link beside "
                                     "another unit:" << diagnosticsOf(l.rep);
        auto const entry  = le(l.image.bytes, 0x18, 8);       // e_entry
        auto const chosen = elfVaOfFunction(l.image.bytes, "CHSN");
        ASSERT_TRUE(entry.has_value());
        ASSERT_TRUE(chosen.has_value()) << "`chosen` is not in the image exactly once";
        std::string startsAt = "none of the four functions the units hold";
        struct Named {
            char const* name;
            char const* fourLetters;
        };
        for (Named const n : {Named{"`first`", "FRST"}, Named{"`helper`", "HLPR"}, Named{"`spare`", "SPAR"}}) {
            if (elfVaOfFunction(l.image.bytes, n.fourLetters) == entry) startsAt = n.name;
        }
        EXPECT_EQ(*entry, *chosen)
            << "the image must start at the function its unit named as the entry (" << hex(*chosen)
            << "), and it starts at " << hex(*entry) << ", which is " << startsAt;
    }
}

// The entry is the FUNCTION the unit named, by the name's rules: when that function
// is a weak definition another unit's strong one replaces, the body the unit pointed
// at is not in the image at all, and the entry is the definition that won its name —
// what a reference to it from anywhere else resolves to.
TEST(MergeUnitReferences, AnEntryNamingAReplacedWeakDefinitionIsTheDefinitionThatWon) {
    std::vector<AssembledModule> mods;
    mods.push_back(makePlainUnit(1, {{"first", "FRST"}, {"chosen", "WEAK", SymbolBinding::Weak}}));
    mods.push_back(makePlainUnit(2, {{"chosen", "STRG"}}));
    mods[0].imageEntryOverride = 1;

    LinkedElf l;
    ASSERT_NO_FATAL_FAILURE(linkElf(mods, l));
    ASSERT_TRUE(l.image.ok()) << diagnosticsOf(l.rep);
    auto const entry  = le(l.image.bytes, 0x18, 8);
    auto const strong = elfVaOfFunction(l.image.bytes, "STRG");
    ASSERT_TRUE(entry.has_value());
    ASSERT_TRUE(strong.has_value()) << "the strong `chosen` is not in the image exactly once";
    EXPECT_FALSE(findOnce(l.image.bytes, tailOf("WEAK")).has_value())
        << "the replaced weak body must not be in the image";
    EXPECT_EQ(*entry, *strong) << "the image starts at " << hex(*entry)
                               << "; the strong `chosen` is at " << hex(*strong);
}

// One image has one entry: two units that each state one are refused, by the code the
// two-user-entries refusal beside it uses. Before the merge read the member this
// linked, with neither entry.
TEST(MergeUnitReferences, TwoUnitsEachStatingTheImagesEntryAreRefused) {
    std::vector<AssembledModule> mods;
    mods.push_back(makePlainUnit(1, {{"one", "ONE1"}}));
    mods.push_back(makePlainUnit(2, {{"two", "TWO2"}}));
    mods[0].imageEntryOverride = 0;
    mods[1].imageEntryOverride = 0;
    mods[1].userEntrySymbol    = SymbolId{1};   // so the link has an entry to synthesize when it forgets both

    LinkedElf l;
    ASSERT_NO_FATAL_FAILURE(linkElf(mods, l));
    EXPECT_FALSE(l.image.ok()) << "two units each stated the image's entry and the link "
                                  "succeeded";
    EXPECT_EQ(countCode(l.rep, DiagnosticCode::K_SymbolRedefinedAcrossUnits), 1u)
        << diagnosticsOf(l.rep);
}

// An entry that names no function of its unit is refused by the merge, naming the
// unit — as the writers refuse it for one unit alone.
TEST(MergeUnitReferences, AnEntryOutsideItsUnitsFunctionsIsRefused) {
    std::vector<AssembledModule> mods;
    mods.push_back(makePlainUnit(1, {{"first", "FRST"}, {"second", "SCND"}}));
    mods.push_back(makePlainUnit(2, {{"helper", "HLPR"}}));
    mods[0].imageEntryOverride = 2;             // the unit holds functions 0 and 1
    mods[1].userEntrySymbol    = SymbolId{1};

    LinkedElf l;
    ASSERT_NO_FATAL_FAILURE(linkElf(mods, l));
    EXPECT_FALSE(l.image.ok()) << "an entry index outside its unit's functions linked";
    EXPECT_GE(countCode(l.rep, DiagnosticCode::K_SymbolUndefined), 1u) << diagnosticsOf(l.rep);
    // The refusal that names THIS: the unit, its index and how many functions it holds.
    EXPECT_NE(diagnosticsOf(l.rep).find("states function index 2 as the image's entry and holds 2 function(s)"),
              std::string::npos)
        << diagnosticsOf(l.rep);
}

// An entry naming a function whose NAME a definition that is not a function won has
// nothing to start at: refused, naming the unit. (Beside a unit that names a user
// entry, so that dropping the entry unsaid would link.)
TEST(MergeUnitReferences, AnEntryWhoseNameANonFunctionWonIsRefused) {
    std::vector<AssembledModule> mods;
    mods.push_back(makePlainUnit(1, {{"first", "FRST"}, {"chosen", "WEAK", SymbolBinding::Weak}}));
    mods[0].imageEntryOverride = 1;
    AssembledModule other = makePlainUnit(2, {{"helper", "HLPR"}});
    other.userEntrySymbol = SymbolId{1};
    AssembledData datum;                         // the STRONG `chosen`: a datum
    datum.symbol    = SymbolId{2};
    datum.section   = DataSectionKind::Data;
    datum.bytes     = {'D', 'A', 'T', 'M', 0, 0, 0, 0};
    datum.alignment = Alignment::of<8>();
    other.dataItems.push_back(std::move(datum));
    other.symbols.push_back(ModuleSymbol{SymbolId{2}, "chosen", SymbolBinding::Global,
                                         SymbolVisibility::Default});
    mods.push_back(std::move(other));

    LinkedElf l;
    ASSERT_NO_FATAL_FAILURE(linkElf(mods, l));
    EXPECT_FALSE(l.image.ok()) << "an entry whose name a datum won linked";
    EXPECT_NE(diagnosticsOf(l.rep).find("is not a function of the image"), std::string::npos)
        << diagnosticsOf(l.rep);
}

// ★★ THE DEFECT, the third member. A unit's null-address symbols — symbols whose
// ADDRESS IS 0 — were not carried either: every relocation naming one was renumbered
// to a merged id nothing declared. One unit alone links (the control arm below); the
// same unit beside any other was refused as an undefined symbol.
TEST(MergeUnitReferences, AUnitsNullAddressSymbolIsStillAddressZeroAfterTheMerge) {
    auto const makeUnit = [] {
        AssembledModule m = makePlainUnit(1, {{"_start", "STRT"}});
        m.userEntrySymbol = SymbolId{1};
        // Eight bytes the relocation fills, then eight that say where they are.
        AssembledData slot;
        slot.symbol    = SymbolId{2};
        slot.section   = DataSectionKind::Data;
        slot.bytes     = {0, 0, 0, 0, 0, 0, 0, 0, 'N', 'U', 'L', 'L', 'S', 'L', 'O', 'T'};
        slot.alignment = Alignment::of<8>();
        Relocation rel;
        rel.offset = 0;
        rel.target = SymbolId{7};                 // the null-address symbol
        rel.kind   = RelocationKind{2};           // abs64 on the shipped x86_64 ELF schema
        rel.addend = 0x1122;                      // so "resolved to address 0" is not "left as written"
        slot.relocations.push_back(rel);
        m.dataItems.push_back(std::move(slot));
        m.symbols.push_back(ModuleSymbol{SymbolId{2}, "slot", SymbolBinding::Global,
                                         SymbolVisibility::Default});
        m.nullAddressSymbols.push_back(SymbolId{7});
        return m;
    };
    auto const slotValue = [](Bytes const& b) -> std::optional<std::uint64_t> {
        auto const marker = findOnce(b, Bytes{'N', 'U', 'L', 'L', 'S', 'L', 'O', 'T'});
        if (!marker.has_value() || *marker < 8) return std::nullopt;
        return le(b, *marker - 8, 8);
    };

    for (bool const besideAnotherUnit : {false, true}) {
        SCOPED_TRACE(besideAnotherUnit ? "beside another unit (the merge)"
                                       : "the unit alone (the control: no merge)");
        std::vector<AssembledModule> mods;
        mods.push_back(makeUnit());
        if (besideAnotherUnit) mods.push_back(makePlainUnit(2, {{"helper", "HLPR"}}));
        LinkedElf l;
        ASSERT_NO_FATAL_FAILURE(linkElf(mods, l));
        ASSERT_TRUE(l.image.ok()) << diagnosticsOf(l.rep);
        EXPECT_EQ(countCode(l.rep, DiagnosticCode::K_SymbolUndefined), 0u) << diagnosticsOf(l.rep);
        auto const value = slotValue(l.image.bytes);
        ASSERT_TRUE(value.has_value()) << "the slot is not in the image exactly once";
        EXPECT_EQ(*value, 0x1122u) << "a relocation naming a null-address symbol resolves to "
                                      "0 + its addend; the slot holds " << hex(*value);
    }
}

// A personality that a unit OF THE LINK defines is that definition — a name one unit
// defines resolves to it for every other unit of the link — and the handler field
// holds ITS address: nothing is imported, so there is no thunk to name. Reachable
// only once the scope's personality id follows the merge: until then the id was the
// unit's own number for an import row the merge had already resolved away.
TEST(MergeSehScopes, APersonalityAUnitOfTheLinkDefinesIsNamedByItsAddress) {
    Loaded const L = loadShippedPair("x86_64", "pe64-x86_64-windows-exec");
    ASSERT_NE(L.target, nullptr);
    ASSERT_NE(L.format, nullptr);

    std::vector<AssembledModule> mods;
    mods.push_back(makeGuardedUnit(1, 'A', 2, 3));
    mods.push_back(makePlainUnit(2, {{"__C_specific_handler", "PERS"}}));
    mods.push_back(makeProgramEntryWithDecoys(3));
    DiagnosticReporter rep;
    LinkedImage const img = linker::link(std::span<AssembledModule const>{mods}, *L.target,
                                         *L.format, rep);
    ASSERT_TRUE(img.ok()) << "a guarded unit must link beside the unit that defines its "
                             "personality:" << diagnosticsOf(rep);
    auto const view = viewPe(img.bytes);
    ASSERT_TRUE(view.has_value()) << "the artifact is not a PE32+ image";
    ScopeReading const r = readScope(img.bytes, *view, 'A', "A");
    ASSERT_TRUE(r.read) << r.why;
    EXPECT_TRUE(r.rangeRight);
    EXPECT_TRUE(r.filterRight) << "the scope names " << r.filterIs;
    auto const personality = rvaOfFunction(img.bytes, *view, "PERS", 0);
    ASSERT_TRUE(personality.has_value()) << "the defined personality is not in the image exactly once";
    EXPECT_EQ(r.handlerRva, *personality)
        << "the handler field must hold the address of the personality the link defines ("
        << hex(*personality) << "); it holds " << hex(r.handlerRva) << ", which reaches "
        << r.handlerIs;
}

// A scope whose personality — or whose filter funclet — is NO symbol of the link is
// refused by the writer, in words that say so. The id the guarded function carries
// is the only statement of either: no relocation names them, so the gate that
// refuses an undefined relocation target never reads them, and the writer is the
// first to ask what they name.
//
// ★★ THE DEFECT THESE PIN (✔MEASURED 2026-10-08, the first time the first of them
// ran): the link SUCCEEDED. An id that only a scope names is counted by nobody, so
// the pass that mints "the next free id" for the entry it synthesizes handed that
// very number to its own new symbol, and the scope then named THAT — the handler
// field of a guarded function reached the import minted for the process exit. So
// the dangling number is SWEPT across the ones such a pass mints next, with the
// guarded unit alone (no merge) and beside another unit: every one must be refused.
TEST(MergeSehScopes, AScopeWhosePersonalityIsNoSymbolOfTheLinkIsRefused) {
    Loaded const L = loadShippedPair("x86_64", "pe64-x86_64-windows-exec");
    ASSERT_NE(L.target, nullptr);
    ASSERT_NE(L.format, nullptr);

    for (bool const besideAnotherUnit : {false, true}) {
        for (std::uint32_t dangling = 3; dangling <= 6; ++dangling) {
            SCOPED_TRACE(std::string{besideAnotherUnit ? "beside another unit" : "the unit alone"}
                         + ", personality #" + std::to_string(dangling));
            std::vector<AssembledModule> mods;
            mods.push_back(makeGuardedUnit(1, 'A', 2, dangling));
            mods[0].externImports.clear();           // the personality's row is gone; the scope still names it
            if (besideAnotherUnit) {
                mods.push_back(makeProgramEntryWithDecoys(2));
            } else {
                mods[0].userEntrySymbol = SymbolId{1};
            }
            DiagnosticReporter rep;
            LinkedImage const img = linker::link(std::span<AssembledModule const>{mods}, *L.target,
                                                 *L.format, rep);
            EXPECT_FALSE(img.ok())
                << "a guarded function whose personality is no symbol of the link linked";
            EXPECT_NE(diagnosticsOf(rep).find("has no import thunk and is no function this image defines"),
                      std::string::npos)
                << diagnosticsOf(rep);
        }
    }
}

TEST(MergeSehScopes, AScopeWhoseFilterFuncletIsNoFunctionOfTheLinkIsRefused) {
    Loaded const L = loadShippedPair("x86_64", "pe64-x86_64-windows-exec");
    ASSERT_NE(L.target, nullptr);
    ASSERT_NE(L.format, nullptr);

    for (bool const besideAnotherUnit : {false, true}) {
        for (std::uint32_t dangling = 4; dangling <= 7; ++dangling) {
            SCOPED_TRACE(std::string{besideAnotherUnit ? "beside another unit" : "the unit alone"}
                         + ", filter funclet #" + std::to_string(dangling));
            std::vector<AssembledModule> mods;
            mods.push_back(makeGuardedUnit(1, 'A', dangling, 3));
            mods[0].functions.pop_back();            // the funclet is gone; the scope still names it
            mods[0].expectedFuncCount = 1;
            if (besideAnotherUnit) {
                mods.push_back(makeProgramEntryWithDecoys(2));
            } else {
                mods[0].userEntrySymbol = SymbolId{1};
            }
            DiagnosticReporter rep;
            LinkedImage const img = linker::link(std::span<AssembledModule const>{mods}, *L.target,
                                                 *L.format, rep);
            EXPECT_FALSE(img.ok())
                << "a guarded function whose filter funclet is no function of the link linked";
            EXPECT_NE(diagnosticsOf(rep).find("which is not a defined function in this image"),
                      std::string::npos)
                << diagnosticsOf(rep);
        }
    }
}
