// D-LK-THREAD-STORAGE-DISAGREEMENT-REFUSED-ONLY-BY-THE-WRITER-BACKSTOP (P69 round 4) — a linked reference and the
// definition it binds to agree on storage duration, or the link is refused BY NAME, before the merge
// (`linker::reportThreadStorageDisagreements`, `link/thread_storage_agreement.hpp`).
//
// Synthetic two-unit links through `linker::link` — CU #1 holds `main`, which reads `shared`; CU #2 defines `shared`
// — on every shipped exec format that carries thread-locals:
//   * an ORDINARY reference to a THREAD-LOCAL definition, and a THREAD-LOCAL reference to an ORDINARY definition:
//     exactly one `K_ExternImportAttributeConflict`, naming the symbol, both units and the reading function, and no
//     `K_RelocationKindMismatch` — the image writers' CRIT-1 backstop, which until this round was the only refusal
//     and named SymbolIds;
//   * a reference that states its storage duration only by its relocation's kind (an object file's shape — it has no
//     declaration) is judged by that kind;
//   * CONTROLS: a reference and a definition that agree link clean, the thread-local pair with its `tls-tpoff32`
//     field patched to the x86_64 Variant II offset of a lone 4-byte template (-4) — which also shows the merge binds
//     a thread-local reference to a sibling's thread-local definition correctly when the row reads no slot;
//   * a reference no relocation names is not judged: it reaches no reference linker either (gcc writes no symbol for
//     an extern nothing uses), so refusing it would reject a program every reference accepts — UNLESS its object's own
//     symbol record states the storage duration: gcc writes a tentative definition nothing reads as an ELF symbol
//     typed an ordinary object, and GNU ld and ld.lld refuse it against a thread-local definition, while link.exe,
//     lld-link and Apple's ld link the same pair (a COFF record and a Mach-O nlist state none) and the program runs.
// The same refusal on gcc's own objects — a COMMON against a thread-local archive member — runs natively on the Linux
// legs (`CommonSymbolsNative.ACommonAgainstAThreadLocalMemberIsRefusedByName`, test_common_symbols.cpp); the two
// corpus examples `thread_local_definition_for_an_ordinary_reference_refused` and
// `ordinary_definition_for_a_thread_local_reference_refused` refuse it from a static library on four formats.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/section_kind.hpp"
#include "core/types/target_schema.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace dss;

namespace {

struct Pair {
    char const* target;
    char const* format;
};

// Every shipped exec format whose document carries thread-locals, on both ISAs.
constexpr Pair kExecs[] = {
    {"x86_64", "elf64-x86_64-linux-exec"},
    {"x86_64", "pe64-x86_64-windows-exec"},
    {"arm64", "elf64-aarch64-linux-exec"},
    {"arm64", "macho64-arm64-darwin-exec"},
};

struct Loaded {
    std::shared_ptr<TargetSchema const>       target;
    std::shared_ptr<ObjectFormatSchema const> format;
};

[[nodiscard]] Loaded load(Pair const& p) {
    Loaded l;
    auto t = TargetSchema::loadShipped(p.target);
    auto f = ObjectFormatSchema::loadShipped(p.format);
    EXPECT_TRUE(t.has_value() && f.has_value()) << p.target << " / " << p.format;
    if (t.has_value()) l.target = *t;
    if (f.has_value()) l.format = *f;
    return l;
}

[[nodiscard]] bool isArm64(Pair const& p) { return std::string{p.target} == "arm64"; }

// `main` reading `shared` (SymbolId 2 of its unit) as an ORDINARY object or as a THREAD-LOCAL one, with the
// relocations each ISA's code carries for it: x86_64 `mov eax, [rip+disp32]` (riprel32, kind 8) or
// `mov eax, fs:[disp32]` (tls-tpoff32, kind 4); arm64 `adrp x8` + `ldr w0, [x8]` (adr_prel_pg_hi21 2,
// ldst32_abs_lo12_nc 14) or `mrs x8, tpidr_el0` + `add x8, x8, #hi12, lsl 12` + `add x8, x8, #lo12` (tls-tprel-hi12 5,
// tls-tprel-lo12 6) + `ldr w0, [x8]`.
[[nodiscard]] AssembledFunction mainReading(bool arm64, bool threadLocal) {
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    auto reloc = [&](std::uint32_t offset, std::uint16_t kind) {
        Relocation r;
        r.offset = offset;
        r.target = SymbolId{2};
        r.kind   = RelocationKind{kind};
        r.addend = 0;
        fn.relocations.push_back(r);
    };
    if (!arm64 && !threadLocal) {
        fn.bytes = {0x8B, 0x05, 0, 0, 0, 0, 0xC3};
        reloc(2, 8);
    } else if (!arm64) {
        fn.bytes = {0x64, 0x8B, 0x04, 0x25, 0, 0, 0, 0, 0xC3};
        reloc(4, 4);
    } else if (!threadLocal) {
        fn.bytes = {0x08, 0x00, 0x00, 0x90,    // adrp x8, shared
                    0x00, 0x01, 0x40, 0xB9,    // ldr  w0, [x8, :lo12:shared]
                    0xC0, 0x03, 0x5F, 0xD6};   // ret
        reloc(0, 2);
        reloc(4, 14);
    } else {
        fn.bytes = {0x48, 0xD0, 0x3B, 0xD5,    // mrs  x8, tpidr_el0
                    0x08, 0x01, 0x40, 0x91,    // add  x8, x8, #:tprel_hi12:shared, lsl 12
                    0x08, 0x01, 0x00, 0x91,    // add  x8, x8, #:tprel_lo12_nc:shared
                    0x00, 0x01, 0x40, 0xB9,    // ldr  w0, [x8]
                    0xC0, 0x03, 0x5F, 0xD6};   // ret
        reloc(4, 5);
        reloc(8, 6);
    }
    return fn;
}

// CU #1: `main`, reading `shared` per `reads` (or not reading it at all, returning 0), with `shared`'s import row —
// `declaredThreadLocal` its `isThreadLocal`, the C declaration's claim.
enum class Reads { Ordinary, ThreadLocal, Nothing };
[[nodiscard]] AssembledModule referenceUnit(bool arm64, Reads reads, bool declaredThreadLocal) {
    AssembledModule m;
    m.cuId              = CompilationUnitId{1};
    m.expectedFuncCount = 1;
    if (reads == Reads::Nothing) {
        AssembledFunction fn;
        fn.symbol = SymbolId{1};
        fn.bytes  = arm64 ? std::vector<std::uint8_t>{0x00, 0x00, 0x80, 0x52, 0xC0, 0x03, 0x5F, 0xD6}   // mov w0,#0; ret
                          : std::vector<std::uint8_t>{0x31, 0xC0, 0xC3};                                // xor eax,eax; ret
        m.functions.push_back(std::move(fn));
    } else {
        m.functions.push_back(mainReading(arm64, reads == Reads::ThreadLocal));
    }
    ExternImport ext;
    ext.symbol         = SymbolId{2};
    ext.mangledName    = "shared";
    ext.isData         = true;
    ext.dataSizeBytes  = 4;
    ext.dataAlignBytes = 4;
    ext.isThreadLocal  = declaredThreadLocal;
    m.externImports.push_back(std::move(ext));
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "main", SymbolBinding::Global, SymbolVisibility::Default});
    m.userEntrySymbol = SymbolId{1};
    return m;
}

// CU #2: `int shared = 7;`, thread-local or ordinary.
[[nodiscard]] AssembledModule definitionUnit(bool threadLocal) {
    AssembledModule m;
    m.cuId              = CompilationUnitId{2};
    m.expectedFuncCount = 0;
    AssembledData d;
    d.symbol    = SymbolId{1};
    d.section   = threadLocal ? DataSectionKind::Tdata : DataSectionKind::Data;
    d.bytes     = {7, 0, 0, 0};
    d.alignment = Alignment::of<4>();
    m.dataItems.push_back(std::move(d));
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "shared", SymbolBinding::Global, SymbolVisibility::Default});
    return m;
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s;
}

[[nodiscard]] std::vector<std::string> textsOf(DiagnosticReporter const& rep, DiagnosticCode code) {
    std::vector<std::string> out;
    for (auto const& d : rep.all()) {
        if (d.code == code) out.push_back(d.actual);
    }
    return out;
}

struct Linked {
    LinkedImage       image;
    DiagnosticReporter rep;
};

void linkPair(Pair const& p, AssembledModule ref, AssembledModule def, Linked& out) {
    auto const L = load(p);
    ASSERT_TRUE(L.target && L.format);
    ASSERT_TRUE(L.format->acceptsDataSection(DataSectionKind::Tdata))
        << p.format << " no longer carries thread-locals -- drop it from kExecs";
    std::vector<AssembledModule> mods;
    mods.push_back(std::move(ref));
    mods.push_back(std::move(def));
    // The artifact's name: the Mach-O document's code-signature identity is a function of it, so a cell whose link
    // reaches that image's writer needs one (the refused cells never get that far).
    out.image = linker::link(std::span<AssembledModule const>{mods}, *L.target, *L.format, out.rep,
                             ImageRequest{.artifactFileName = "thread_storage_image"});
}

void expectRefusedByName(Linked const& l, char const* what, char const* how) {
    EXPECT_FALSE(l.image.ok());
    auto const named = textsOf(l.rep, DiagnosticCode::K_ExternImportAttributeConflict);
    ASSERT_EQ(named.size(), 1u) << diagnosticsOf(l.rep);
    EXPECT_NE(named[0].find("symbol 'shared'"), std::string::npos) << named[0];
    EXPECT_NE(named[0].find("CU #1 (in `main`)"), std::string::npos) << named[0];
    EXPECT_NE(named[0].find("in CU #2"), std::string::npos) << named[0];
    EXPECT_NE(named[0].find(what), std::string::npos) << named[0];
    EXPECT_NE(named[0].find(how), std::string::npos) << named[0];
    EXPECT_TRUE(textsOf(l.rep, DiagnosticCode::K_RelocationKindMismatch).empty())
        << "the writer's backstop must not be what refuses it:" << diagnosticsOf(l.rep);
}

[[nodiscard]] bool holds(std::vector<std::uint8_t> const& hay, std::vector<std::uint8_t> const& needle) {
    if (needle.empty() || hay.size() < needle.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size() && match; ++j) match = hay[i + j] == needle[j];
        if (match) return true;
    }
    return false;
}

} // namespace

// `extern int shared;` read by `main`, against `_Thread_local int shared = 7;`: GNU ld's "TLS definition in ...
// mismatches non-TLS reference in ...".
TEST(ThreadStorageAgreement, AnOrdinaryReferenceToAThreadLocalDefinitionIsRefusedByName) {
    for (auto const& p : kExecs) {
        SCOPED_TRACE(p.format);
        Linked l;
        linkPair(p, referenceUnit(isArm64(p), Reads::Ordinary, /*declaredThreadLocal=*/false),
                 definitionUnit(/*threadLocal=*/true), l);
        expectRefusedByName(l, "has THREAD STORAGE DURATION", "refers to it as an ORDINARY object");
    }
}

// `extern _Thread_local int shared;` read by `main`, against `int shared = 7;`: GNU ld's "TLS reference in ...
// mismatches non-TLS definition in ...".
TEST(ThreadStorageAgreement, AThreadLocalReferenceToAnOrdinaryDefinitionIsRefusedByName) {
    for (auto const& p : kExecs) {
        SCOPED_TRACE(p.format);
        Linked l;
        linkPair(p, referenceUnit(isArm64(p), Reads::ThreadLocal, /*declaredThreadLocal=*/true),
                 definitionUnit(/*threadLocal=*/false), l);
        expectRefusedByName(l, "is an ORDINARY object", "declares it `thread_local`");
    }
}

// An object file states no declaration: a reference its code reaches through a tls-flagged relocation is a
// thread-local reference, whatever its import row says.
TEST(ThreadStorageAgreement, AReferenceItsCodeReachesThroughAThreadLocalRelocationIsAThreadLocalOne) {
    for (auto const& p : kExecs) {
        SCOPED_TRACE(p.format);
        Linked l;
        linkPair(p, referenceUnit(isArm64(p), Reads::ThreadLocal, /*declaredThreadLocal=*/false),
                 definitionUnit(/*threadLocal=*/false), l);
        expectRefusedByName(l, "is an ORDINARY object",
                            isArm64(p) ? "through the 'tls-tprel-hi12' relocation"
                                       : "through the 'tls-tpoff32' relocation");
    }
}

// CONTROLS: the same two units when their storage durations agree. The thread-local pair's `tls-tpoff32` field holds
// the x86_64 Variant II offset of a lone 4-byte template, -4: the merge bound the reference to the definition itself.
TEST(ThreadStorageAgreement, AReferenceAndADefinitionThatAgreeLinkClean) {
    Pair const elf{"x86_64", "elf64-x86_64-linux-exec"};
    {
        SCOPED_TRACE("both ordinary");
        Linked l;
        linkPair(elf, referenceUnit(false, Reads::Ordinary, false), definitionUnit(false), l);
        EXPECT_TRUE(l.image.ok()) << diagnosticsOf(l.rep);
        EXPECT_EQ(l.rep.errorCount(), 0u) << diagnosticsOf(l.rep);
    }
    {
        SCOPED_TRACE("both thread-local");
        Linked l;
        linkPair(elf, referenceUnit(false, Reads::ThreadLocal, true), definitionUnit(true), l);
        ASSERT_TRUE(l.image.ok()) << diagnosticsOf(l.rep);
        EXPECT_EQ(l.rep.errorCount(), 0u) << diagnosticsOf(l.rep);
        EXPECT_TRUE(holds(l.image.bytes, {0x64, 0x8B, 0x04, 0x25, 0xFC, 0xFF, 0xFF, 0xFF, 0xC3}))
            << "main's fs-relative read must reach the definition's own thread-pointer offset, -4";
    }
}

// A reference no relocation of its unit names is not judged — `main` returns 0 and never reads `shared`.
TEST(ThreadStorageAgreement, AReferenceNoRelocationNamesIsNotJudged) {
    Pair const elf{"x86_64", "elf64-x86_64-linux-exec"};
    Linked     l;
    linkPair(elf, referenceUnit(false, Reads::Nothing, false), definitionUnit(true), l);
    EXPECT_TRUE(l.image.ok()) << diagnosticsOf(l.rep);
    EXPECT_TRUE(textsOf(l.rep, DiagnosticCode::K_ExternImportAttributeConflict).empty()) << diagnosticsOf(l.rep);
}

// A reference no relocation names IS judged where its object's own symbol record states the storage duration
// (`ExternImport::recordStatesStorageDuration`). ✔MEASURED 2026-10-08, a tentative `int c;` that nothing reads beside
// a thread-local definition of `c`, both object orders: gcc 13.3.0 writes the common as an ELF symbol typed an
// ordinary OBJECT, and GNU ld 2.42 ("TLS definition in ... mismatches non-TLS reference in ...") and ld.lld 18.1.3
// ("TLS attribute mismatch: c") REFUSE the pair; cl 19.44 and clang 19.1.5 write a COFF record, and Apple clang 21 a
// Mach-O nlist, that state no storage duration, and link.exe 14.44, lld-link 19.1.5 and Apple's ld LINK it to a
// program that runs. So the row carries what its record states: with it, refused by name; without it, linked and
// unjudged. The same pair as a COMMON row (`commonSize`), which the link's first pass turns into a reference of the
// thread-local definition it yields to, answers the same way.
TEST(ThreadStorageAgreement, AnUnreadReferenceIsJudgedWhereItsRecordStatesItsStorageDuration) {
    auto const unread = [](bool arm64, bool common, bool recordStates) {
        AssembledModule m = referenceUnit(arm64, Reads::Nothing, /*declaredThreadLocal=*/false);
        ExternImport&   row = m.externImports[0];
        row.recordStatesStorageDuration = recordStates;
        if (common) {
            row.dataSizeBytes   = 0;
            row.dataAlignBytes  = 0;
            row.commonSize      = 4;
            row.commonAlignment = 4;
        }
        return m;
    };
    for (auto const& p : kExecs) {
        for (bool const common : {false, true}) {
            SCOPED_TRACE(std::string{p.format} + (common ? ", a common" : ", a reference"));
            Linked l;
            linkPair(p, unread(isArm64(p), common, /*recordStates=*/true), definitionUnit(/*threadLocal=*/true), l);
            EXPECT_FALSE(l.image.ok());
            auto const named = textsOf(l.rep, DiagnosticCode::K_ExternImportAttributeConflict);
            ASSERT_EQ(named.size(), 1u) << diagnosticsOf(l.rep);
            EXPECT_NE(named[0].find("symbol 'shared': CU #1 holds it as an ORDINARY object"), std::string::npos)
                << named[0];
            EXPECT_NE(named[0].find("no code of the unit reads it"), std::string::npos) << named[0];
            EXPECT_NE(named[0].find("in CU #2, has THREAD STORAGE DURATION"), std::string::npos) << named[0];
            EXPECT_TRUE(textsOf(l.rep, DiagnosticCode::K_RelocationKindMismatch).empty()) << diagnosticsOf(l.rep);
        }
    }
    // CONTROLS. A record that states nothing is not judged, as a common or as a reference, on any format — the COFF
    // and the Mach-O shape on the formats whose reference linkers link it, and an in-memory unit's row everywhere.
    for (auto const& p : kExecs) {
        for (bool const common : {false, true}) {
            SCOPED_TRACE(std::string{"CONTROL, "} + p.format + (common ? ", a common" : ", a reference"));
            Linked l;
            linkPair(p, unread(isArm64(p), common, /*recordStates=*/false), definitionUnit(/*threadLocal=*/true), l);
            EXPECT_TRUE(l.image.ok()) << diagnosticsOf(l.rep);
            EXPECT_EQ(l.rep.errorCount(), 0u) << diagnosticsOf(l.rep);
            EXPECT_TRUE(textsOf(l.rep, DiagnosticCode::K_ExternImportAttributeConflict).empty())
                << diagnosticsOf(l.rep);
        }
    }
    // And a record that states an ordinary object agrees with an ordinary definition: linked.
    for (auto const& p : kExecs) {
        SCOPED_TRACE(std::string{"CONTROL, an ordinary definition, "} + p.format);
        Linked l;
        linkPair(p, unread(isArm64(p), /*common=*/true, /*recordStates=*/true), definitionUnit(/*threadLocal=*/false), l);
        EXPECT_TRUE(l.image.ok()) << diagnosticsOf(l.rep);
        EXPECT_EQ(l.rep.errorCount(), 0u) << diagnosticsOf(l.rep);
    }
}

// ══ A PE program with a thread-local, beside another unit ════════════════════
//
// D-LK-PE-THREAD-LOCAL-PROGRAM-REFUSED-BESIDE-ANY-OTHER-UNIT (P69 round 4). A `pe-indexed` thread-local access reads
// the image's `_tls_index`, a symbol the PE WRITER defines, which every unit names by ONE reserved id
// (`kTlsIndexReservedSymbolIdValue`). The multi-unit resolver did not exempt it as the single-unit unifier does, and
// the merge remapped it to a fresh id no writer binds: a PE program with a thread-local was refused beside ANY other
// unit (✔MEASURED 2026-10-07, `K_SymbolUndefined` naming symbol #4294967041, a static-library member beside it; alone
// it linked). Here `main` reads `_tls_index`, its own thread-local `t` and calls CU #2's `read_shared`, and a data slot
// of CU #1 holds `_tls_index`'s address (an abs64 data-item relocation naming the same reserved id — the resolver's
// data-item loop exempts it as its function loop does): the image links, `main`'s `_tls_index` read reaches the TLS
// directory's own AddressOfIndex, and the slot holds that address.
namespace {

[[nodiscard]] std::uint64_t rdLE(std::vector<std::uint8_t> const& b, std::size_t off, int width) {
    std::uint64_t v = 0;
    for (int i = 0; i < width; ++i) v |= static_cast<std::uint64_t>(b[off + i]) << (i * 8);
    return v;
}

[[nodiscard]] std::optional<std::size_t> findBytes(std::vector<std::uint8_t> const& hay,
                                                   std::vector<std::uint8_t> const& needle) {
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size() && match; ++j) match = hay[i + j] == needle[j];
        if (match) return i;
    }
    return std::nullopt;
}

// The file offset → VA map of a PE32+ image, and its TLS directory's AddressOfIndex.
struct PeView {
    std::uint64_t imageBase = 0;
    struct Section {
        std::uint32_t va = 0, size = 0, raw = 0, rawSize = 0;
    };
    std::vector<Section>         sections;
    std::optional<std::uint64_t> tlsAddressOfIndex;

    [[nodiscard]] std::optional<std::uint64_t> vaOfFileOffset(std::size_t off) const {
        for (auto const& s : sections) {
            if (off >= s.raw && off < s.raw + s.rawSize) return imageBase + s.va + (off - s.raw);
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<std::size_t> fileOffsetOfRva(std::uint64_t rva) const {
        for (auto const& s : sections) {
            if (rva >= s.va && rva < s.va + std::max(s.size, s.rawSize)) return s.raw + (rva - s.va);
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<std::size_t> fileOffsetOfVa(std::uint64_t va) const {
        if (va < imageBase) return std::nullopt;
        return fileOffsetOfRva(va - imageBase);
    }
};

[[nodiscard]] std::optional<PeView> viewPe(std::vector<std::uint8_t> const& b) {
    if (b.size() < 0x40) return std::nullopt;
    std::size_t const pe = static_cast<std::size_t>(rdLE(b, 0x3C, 4));
    if (pe + 24 > b.size() || rdLE(b, pe, 4) != 0x00004550u) return std::nullopt;
    std::size_t const coff     = pe + 4;
    auto const        nSect    = static_cast<std::size_t>(rdLE(b, coff + 2, 2));
    auto const        optSize  = static_cast<std::size_t>(rdLE(b, coff + 16, 2));
    std::size_t const opt      = coff + 20;
    if (rdLE(b, opt, 2) != 0x20Bu) return std::nullopt;   // PE32+
    PeView v;
    v.imageBase = rdLE(b, opt + 24, 8);
    std::size_t const sect = opt + optSize;
    for (std::size_t i = 0; i < nSect; ++i) {
        std::size_t const s = sect + i * 40;
        v.sections.push_back(PeView::Section{static_cast<std::uint32_t>(rdLE(b, s + 12, 4)),
                                             static_cast<std::uint32_t>(rdLE(b, s + 8, 4)),
                                             static_cast<std::uint32_t>(rdLE(b, s + 20, 4)),
                                             static_cast<std::uint32_t>(rdLE(b, s + 16, 4))});
    }
    // Data directory 9 (IMAGE_DIRECTORY_ENTRY_TLS); an IMAGE_TLS_DIRECTORY64's AddressOfIndex is its third quadword.
    std::uint64_t const tlsRva = rdLE(b, opt + 112 + 9 * 8, 4);
    if (tlsRva != 0) {
        if (auto const at = v.fileOffsetOfRva(tlsRva); at.has_value() && *at + 24 <= b.size()) {
            v.tlsAddressOfIndex = rdLE(b, *at + 16, 8);
        }
    }
    return v;
}

} // namespace

TEST(PeThreadLocalBesideAnotherUnit, TheTlsIndexReadSurvivesTheMergeAndTheWriterBindsIt) {
    Pair const pe{"x86_64", "pe64-x86_64-windows-exec"};
    auto const L = load(pe);
    ASSERT_TRUE(L.target && L.format);
    AssembledModule m1;
    m1.cuId              = CompilationUnitId{1};
    m1.expectedFuncCount = 1;
    {
        AssembledFunction fn;
        fn.symbol = SymbolId{1};
        fn.bytes  = {0x48, 0x8D, 0x15, 0, 0, 0, 0,                            // lea rdx, [rip + tls_index_slot]
                     0x8B, 0x05, 0, 0, 0, 0,                                  // mov eax, [rip + _tls_index]
                     0x65, 0x48, 0x8B, 0x0C, 0x25, 0x58, 0x00, 0x00, 0x00,    // mov rcx, gs:[0x58]
                     0x48, 0x8B, 0x0C, 0xC1,                                  // mov rcx, [rcx + rax*8]
                     0x8B, 0x81, 0, 0, 0, 0,                                  // mov eax, [rcx + secrel(t)]
                     0xE8, 0, 0, 0, 0,                                        // call read_shared
                     0xC3};
        auto reloc = [&](std::uint32_t offset, std::uint32_t target, std::uint16_t kind) {
            Relocation r;
            r.offset = offset;
            r.target = SymbolId{target};
            r.kind   = RelocationKind{kind};
            r.addend = 0;
            fn.relocations.push_back(r);
        };
        reloc(3, 4, 8);                                // riprel32 (the slot)
        reloc(9, kTlsIndexReservedSymbolIdValue, 8);   // riprel32
        reloc(28, 3, 4);                               // tls-tpoff32 (the PE writer's secrel)
        reloc(33, 2, 1);                               // rel32
        m1.functions.push_back(std::move(fn));
    }
    AssembledData t;
    t.symbol    = SymbolId{3};
    t.section   = DataSectionKind::Tdata;
    t.bytes     = {35, 0, 0, 0};
    t.alignment = Alignment::of<4>();
    m1.dataItems.push_back(std::move(t));
    AssembledData slot;   // `_tls_index`'s address, held in data: an abs64 (kind 2) naming the reserved id
    slot.symbol    = SymbolId{4};
    slot.section   = DataSectionKind::Data;
    slot.bytes     = std::vector<std::uint8_t>(8, 0);
    slot.alignment = Alignment::of<8>();
    {
        Relocation r;
        r.offset = 0;
        r.target = SymbolId{kTlsIndexReservedSymbolIdValue};
        r.kind   = RelocationKind{2};
        r.addend = 0;
        slot.relocations.push_back(r);
    }
    m1.dataItems.push_back(std::move(slot));
    ExternImport callee;
    callee.symbol      = SymbolId{2};
    callee.mangledName = "read_shared";
    m1.externImports.push_back(std::move(callee));
    m1.symbols.push_back(ModuleSymbol{SymbolId{1}, "main", SymbolBinding::Global, SymbolVisibility::Default});
    m1.symbols.push_back(ModuleSymbol{SymbolId{3}, "t", SymbolBinding::Global, SymbolVisibility::Default});
    m1.symbols.push_back(ModuleSymbol{SymbolId{4}, "tls_index_slot", SymbolBinding::Local, SymbolVisibility::Default});
    m1.userEntrySymbol = SymbolId{1};

    AssembledModule m2;
    m2.cuId              = CompilationUnitId{2};
    m2.expectedFuncCount = 1;
    AssembledFunction readShared;
    readShared.symbol = SymbolId{1};
    readShared.bytes  = {0xB8, 0x07, 0x00, 0x00, 0x00, 0xC3};   // mov eax, 7; ret
    m2.functions.push_back(std::move(readShared));
    m2.symbols.push_back(ModuleSymbol{SymbolId{1}, "read_shared", SymbolBinding::Global, SymbolVisibility::Default});

    std::vector<AssembledModule> mods;
    mods.push_back(std::move(m1));
    mods.push_back(std::move(m2));
    DiagnosticReporter rep;
    auto const image = linker::link(std::span<AssembledModule const>{mods}, *L.target, *L.format, rep);
    ASSERT_TRUE(image.ok()) << "a PE program with a thread-local must link beside another unit:" << diagnosticsOf(rep);
    EXPECT_TRUE(textsOf(rep, DiagnosticCode::K_SymbolUndefined).empty()) << diagnosticsOf(rep);

    auto const view = viewPe(image.bytes);
    ASSERT_TRUE(view.has_value()) << "not a PE32+ image";
    ASSERT_TRUE(view->tlsAddressOfIndex.has_value()) << "the image carries no TLS directory";
    auto const at = findBytes(image.bytes, {0x65, 0x48, 0x8B, 0x0C, 0x25, 0x58, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x0C, 0xC1,
                                       0x8B, 0x81});
    ASSERT_TRUE(at.has_value() && *at >= 13) << "main's access sequence is not in the image";
    std::size_t const mainAt = *at - 13;
    ASSERT_EQ(image.bytes[mainAt], 0x48);
    ASSERT_EQ(image.bytes[mainAt + 7], 0x8B);
    auto const mainVa = view->vaOfFileOffset(mainAt);
    ASSERT_TRUE(mainVa.has_value());
    auto const ripRel = [&](std::size_t field, std::size_t end) {
        auto const disp = static_cast<std::int32_t>(rdLE(image.bytes, mainAt + field, 4));
        return *mainVa + end + static_cast<std::uint64_t>(static_cast<std::int64_t>(disp));
    };
    EXPECT_EQ(ripRel(9, 13), *view->tlsAddressOfIndex)
        << "main's `_tls_index` read must reach the slot the TLS directory names (AddressOfIndex)";
    auto const slotAt = view->fileOffsetOfVa(ripRel(3, 7));
    ASSERT_TRUE(slotAt.has_value() && *slotAt + 8 <= image.bytes.size()) << "the data slot is not in the image";
    EXPECT_EQ(rdLE(image.bytes, *slotAt, 8), *view->tlsAddressOfIndex)
        << "the data slot's abs64 naming `_tls_index` must hold the address the TLS directory names";
}
