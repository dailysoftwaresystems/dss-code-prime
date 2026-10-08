// P69 (the PE half of D-LK-PE-DLL-EXPORTS-THE-SHIPPED-RUNTIME-IT-LINKS) — a COFF object's LINKER DIRECTIVES, both
// directions: what DSS writes into `.drectve` for a definition its visibility keeps out of an image's exports, and
// what it reads out of anyone's `.drectve`.
//
// COFF has no visibility field. ✔MEASURED 2026-10-01 (this host, work/xa/drectve_probe): clang
// `--target=x86_64-w64-windows-gnu -c` writes `visibility("hidden") int helper` as an ordinary EXTERNAL symbol plus
// ` -exclude-symbols:helper`; a DLL built from that object exports `lib_entry` ALONE under GNU ld 2.42 (MinGW-w64)
// and ld.lld's MinGW mode, and `helper` too without the attribute; link.exe 14.51 warns LNK4229 and ignores the
// directive (it exports nothing it is not told to). Every MSVC C object carries `/DEFAULTLIB:MSVCRT
// /DEFAULTLIB:OLDNAMES`, and `/EXPORT:<name>` for a dllexport (cl 19.51, clang-cl 19).
//
//   * The grammar and vocabulary (`pe::parseCoffLinkerDirectives`, `pe::coffLinkerDirectiveText`) on the shipped
//     relocatable document's `pe.linkerDirectives`: every text cl 19.51, clang-cl, clang and mingw-w64 gcc were
//     measured writing reads, each option by its declared meaning (P69 round 3: include, alternatename and
//     aligncomm have meanings; round 4: the image requests have theirs, `refused` and `ignored` rows state a
//     reason); a malformed include / alternatename / aligncomm / export / image request and an empty name are
//     refused, each by its own words, and an undeclared option is WARNED and handed on (P69 round 4: link.exe
//     LNK4229, GNU ld "unrecognized"); the writer states every request the reader reads back. The image requests'
//     own pins are `test_coff_directive_image_requests.cpp`.
//   * The WRITER (`pe::encode`, Obj arm): a hidden definition — canonical or alias, function or datum, strong or
//     weak — gets the directive, in a `.drectve` stamped from the document; a module with none writes no such
//     section; a required import and a fallback are handed on.
//   * The READER (`pe::readRelocatableObject`): the directive lifts the definition back to Hidden; an undeclared
//     option is warned, and a hide or an export of a symbol the object does not define goes to the LINK (round 4:
//     both are link-wide, as lld-link -lldmingw, link.exe and GNU ld take them).
//   * End to end through the production driver: a DSS DLL that links a DSS static-library member with a hidden
//     helper, a hidden datum and a hidden weak definition exports `lib_entry` alone — and the same member handed to
//     GNU ld (`gcc -shared`, Windows) does too.
//   * The loader refusals of the vocabulary itself, the `ignored` reason included.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/coff_linker_directives.hpp"
#include "link/format/coff_object_reader.hpp"
#include "link/format/pe.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "program/program.hpp"
#include "format_reject_support.hpp"   // countAtPath / countWithMessage / rejectSummary
#include "repo_root.hpp"               // findConfigRoot — the shipped documents the refusals mutate
#include "run_binary.hpp"
#include "scratch_dir.hpp"
#include "../core/native_c_probe.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;
using dss::link_format::test::countAtPath;
using dss::link_format::test::countWithMessage;
using dss::link_format::test::rejectSummary;

namespace {

namespace fs = std::filesystem;

struct Loaded {
    std::shared_ptr<TargetSchema const>       target;
    std::shared_ptr<ObjectFormatSchema const> format;
};

[[nodiscard]] Loaded loadPe(char const* stem) {
    Loaded l;
    auto t = TargetSchema::loadShipped("x86_64");
    auto f = ObjectFormatSchema::loadShipped(stem);
    EXPECT_TRUE(t.has_value() && f.has_value()) << stem;
    if (t.has_value()) l.target = *t;
    if (f.has_value()) l.format = *f;
    return l;
}

[[nodiscard]] PeLinkerDirectives const& shippedVocabulary() {
    static Loaded const l = loadPe("pe64-x86_64-windows");
    static PeLinkerDirectives const empty;
    if (!l.format || !l.format->pe().linkerDirectives.has_value()) {
        ADD_FAILURE() << "pe64-x86_64-windows declares pe.linkerDirectives";
        return empty;
    }
    return *l.format->pe().linkerDirectives;
}

[[nodiscard]] std::uint16_t rdU16(std::vector<std::uint8_t> const& b, std::size_t o) {
    return static_cast<std::uint16_t>(b[o] | (b[o + 1] << 8));
}
[[nodiscard]] std::uint32_t rdU32(std::vector<std::uint8_t> const& b, std::size_t o) {
    return static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1]) << 8)
         | (static_cast<std::uint32_t>(b[o + 2]) << 16) | (static_cast<std::uint32_t>(b[o + 3]) << 24);
}

struct CoffSection {
    std::string   name;
    std::uint32_t size = 0, raw = 0, chars = 0;
    std::size_t   headerOffset = 0;
};
[[nodiscard]] std::vector<CoffSection> coffSections(std::vector<std::uint8_t> const& b) {
    std::vector<CoffSection> out;
    std::uint16_t const n = rdU16(b, 2);
    for (std::uint16_t i = 0; i < n; ++i) {
        std::size_t const o = 20u + i * 40u;
        CoffSection s;
        for (std::size_t k = 0; k < 8 && b[o + k] != 0; ++k) s.name.push_back(static_cast<char>(b[o + k]));
        s.size  = rdU32(b, o + 16);
        s.raw   = rdU32(b, o + 20);
        s.chars = rdU32(b, o + 36);
        s.headerOffset = o;
        out.push_back(std::move(s));
    }
    return out;
}
[[nodiscard]] std::optional<CoffSection> sectionNamed(std::vector<std::uint8_t> const& b, std::string_view n) {
    for (auto const& s : coffSections(b)) {
        if (s.name == n) return s;
    }
    return std::nullopt;
}

// Two functions — `lib_entry` exported, `helper` hidden — and optionally an alias of `helper`.
[[nodiscard]] AssembledModule hiddenHelperModule(SymbolVisibility helperVisibility, bool withAlias) {
    AssembledModule m;
    m.expectedFuncCount = 2;
    AssembledFunction entry;
    entry.symbol = SymbolId{1};
    entry.bytes  = {0xB8, 0x2A, 0, 0, 0, 0xC3};   // mov eax, 42 ; ret
    AssembledFunction helper;
    helper.symbol = SymbolId{2};
    helper.bytes  = {0x8D, 0x41, 0x01, 0xC3};     // lea eax, [rcx+1] ; ret
    m.functions.push_back(entry);
    m.functions.push_back(helper);
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "lib_entry", SymbolBinding::Global, SymbolVisibility::Default});
    m.symbols.push_back(ModuleSymbol{SymbolId{2}, "helper", SymbolBinding::Global, helperVisibility});
    if (withAlias) {
        m.symbols.push_back(ModuleSymbol{SymbolId{2}, "helper_alias", SymbolBinding::Global, helperVisibility});
    }
    return m;
}

// Every SHAPE of a hidden definition, each decided at its own site on both sides (P69 round 3): the hidden
// function `helper` and a WEAK alias of it (`helper_weak`, which the writer spells as a weak external and the
// reader reads back through its weak-external arm), a hidden WEAK function (`hidden_weak`, a COMDAT) and a hidden
// DATUM (`hidden_datum`) — beside `lib_entry`, exported.
[[nodiscard]] AssembledModule everyHiddenShapeModule() {
    AssembledModule m = hiddenHelperModule(SymbolVisibility::Hidden, /*withAlias=*/false);
    m.expectedFuncCount = 3;
    AssembledFunction weakFn;
    weakFn.symbol = SymbolId{3};
    weakFn.bytes  = {0x31, 0xC0, 0xC3};   // xor eax, eax ; ret
    m.functions.push_back(weakFn);
    m.symbols.push_back(ModuleSymbol{SymbolId{2}, "helper_weak", SymbolBinding::Weak, SymbolVisibility::Hidden});
    m.symbols.push_back(ModuleSymbol{SymbolId{3}, "hidden_weak", SymbolBinding::Weak, SymbolVisibility::Hidden});
    AssembledData datum;
    datum.symbol    = SymbolId{4};
    datum.section   = DataSectionKind::Data;
    datum.bytes     = {7, 0, 0, 0};
    datum.alignment = Alignment::of<4>();
    m.dataItems.push_back(datum);
    m.symbols.push_back(ModuleSymbol{SymbolId{4}, "hidden_datum", SymbolBinding::Global, SymbolVisibility::Hidden});
    return m;
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string all;
    for (auto const& d : rep.all()) all += "\n  " + d.actual;
    return all;
}

// The exported names a directive text's exports state, in order.
[[nodiscard]] std::vector<std::string> exportedNamesOf(pe::CoffDirectiveRequests const& r) {
    std::vector<std::string> out;
    for (auto const& e : r.exports) out.push_back(e.exportedName);
    return out;
}

}  // namespace

// ══ The grammar and the vocabulary ══════════════════════════════════════════

TEST(CoffLinkerDirectives, TheReferencesOwnDirectiveTextsAreRead) {
    auto const& v = shippedVocabulary();
    // cl 19.51 /MD: quoted libraries, a dllexport, padding spaces around.
    auto const msvc = pe::parseCoffLinkerDirectives(
        "   /DEFAULTLIB:\"MSVCRT\" /DEFAULTLIB:\"OLDNAMES\" /EXPORT:lib_export ", v);
    ASSERT_TRUE(msvc.has_value()) << msvc.error();
    EXPECT_TRUE(msvc->hidden.empty());
    EXPECT_EQ(exportedNamesOf(*msvc), std::vector<std::string>{"lib_export"});
    // clang-cl 19: unquoted, `.lib` spelled, no leading pad.
    auto const clangCl = pe::parseCoffLinkerDirectives(
        " /DEFAULTLIB:msvcrt.lib /DEFAULTLIB:oldnames.lib /EXPORT:lib_export", v);
    ASSERT_TRUE(clangCl.has_value()) << clangCl.error();
    EXPECT_EQ(exportedNamesOf(*clangCl), std::vector<std::string>{"lib_export"});
    // clang --target=x86_64-w64-windows-gnu: the hide directive, a list, any case, a BOM, NUL padding, a quote.
    static constexpr char kGnuText[] =
        "\xEF\xBB\xBF -exclude-symbols:helper -EXCLUDE-SYMBOLS:a,b /exclude-symbols:\"q\"\0\0";
    auto const gnu = pe::parseCoffLinkerDirectives(std::string_view{kGnuText, sizeof(kGnuText) - 1}, v);
    ASSERT_TRUE(gnu.has_value()) << gnu.error();
    EXPECT_EQ(gnu->hidden, (std::vector<std::string>{"helper", "a", "b", "q"}));
    EXPECT_TRUE(gnu->exports.empty());
    // `name,DATA` and a rename `ext=int` (P69 round 4: both linkers export the rename under `ext` alone).
    auto const data = pe::parseCoffLinkerDirectives("/EXPORT:v,DATA /EXPORT:ext=int,PRIVATE", v);
    ASSERT_TRUE(data.has_value()) << data.error();
    EXPECT_EQ(exportedNamesOf(*data), (std::vector<std::string>{"v", "ext"}));
    ASSERT_EQ(data->exports.size(), 2u);
    EXPECT_TRUE(data->exports[0].isData);
    EXPECT_EQ(data->exports[1].internalName, "int");
    EXPECT_TRUE(data->exports[1].isPrivate);

    // ✔MEASURED 2026-10-06 (probe runs 20261006-212638-3ab25596, -212648-2547f2f2, -212645-25ddf9c5): the other
    // texts the references write. cl /fsanitize=address and /ZI:
    auto const asan = pe::parseCoffLinkerDirectives(
        " /InferAsanLibs /INCLUDE:__you_must_link_with_VCAsan_lib /DEFAULTLIB:VCASAN /EDITANDCONTINUE", v);
    ASSERT_TRUE(asan.has_value()) << asan.error();
    EXPECT_EQ(asan->included, std::vector<std::string>{"__you_must_link_with_VCAsan_lib"});
    // cl's C++ objects:
    auto const cpp = pe::parseCoffLinkerDirectives(
        " /FAILIFMISMATCH:\"annotate_string=0\" /FAILIFMISMATCH:\"RuntimeLibrary=MD_DynamicRelease\""
        " /alternatename:_Avx2WmemEnabled=_Avx2WmemEnabledWeakValue /DEFAULTLIB:\"msvcprt\"",
        v);
    ASSERT_TRUE(cpp.has_value()) << cpp.error();
    EXPECT_EQ(cpp->alternates,
              (std::vector<pe::CoffAlternateName>{{"_Avx2WmemEnabled", "_Avx2WmemEnabledWeakValue"}}));
    // `#pragma comment(linker, ...)` passes any text through — the TLS-callback idiom and every option the
    // pragma probe named:
    auto const pragmas = pe::parseCoffLinkerDirectives(
        " /INCLUDE:_tls_used /INCLUDE:dss_tls_cb /DEFAULTLIB:user32 /alternatename:alt_sym=alt_default"
        " /manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' language='*'\""
        " /subsystem:console /merge:.dssm=.data /section:.dsss,RWS /stack:0x300000 /heap:0x200000"
        " /nodefaultlib:libcmt /failifmismatch:dss_key=dss_value /export:exp_sym /release"
        " /entry:mainCRTStartup /GUARDSYM:dss_guard /THROWINGNEW",
        v);
    ASSERT_TRUE(pragmas.has_value()) << pragmas.error();
    EXPECT_EQ(pragmas->included, (std::vector<std::string>{"_tls_used", "dss_tls_cb"}));
    EXPECT_EQ(pragmas->alternates, (std::vector<pe::CoffAlternateName>{{"alt_sym", "alt_default"}}));
    EXPECT_EQ(exportedNamesOf(*pragmas), std::vector<std::string>{"exp_sym"});
    // Round 4: the image requests among them, in directive order (`/merge:` and `/manifestdependency:` are the two a
    // DSS image refuses, as the link that makes one will).
    EXPECT_EQ(pragmas->image.size(), 9u) << "manifestdependency, subsystem, merge, section, stack, heap, "
                                            "failifmismatch, release, entry";
    EXPECT_TRUE(pragmas->warnings.empty()) << "every option the probe named is in the vocabulary";
    EXPECT_TRUE(pragmas->hidden.empty());
    EXPECT_TRUE(pragmas->commonAlignments.empty()) << "only `aligncomm` states a common's alignment";
    // mingw-w64 gcc 13.2.0: a quoted export with `data`, and one `-aligncomm:` per -fcommon common.
    auto const gcc =
        pe::parseCoffLinkerDirectives(" -export:\"exp_data\",data -aligncomm:\"c32\",5 -aligncomm:\"c4\",2", v);
    ASSERT_TRUE(gcc.has_value()) << gcc.error();
    EXPECT_EQ(exportedNamesOf(*gcc), std::vector<std::string>{"exp_data"});
    EXPECT_EQ(gcc->commonAlignments, (std::vector<pe::CoffCommonAlignment>{{"c32", 5}, {"c4", 2}}));
}

// Every option the shipped vocabulary declares `ignored` or `refused` states WHY, and every applied meaning has
// exactly the option the references spell it with — so a row that loses its reason, or a meaning that loses its
// option, is a red here before it is a silently dropped request anywhere. P69 round 4: the five options link.exe
// and lld-link honour that a round-3 DSS link dropped (stack, subsystem, entry, section, and export in an EXE)
// have their meanings, and so does every other option link.exe takes in a directive (✔MEASURED 2026-10-07); the
// two a DSS image cannot carry are `refused`, naming their row.
TEST(CoffLinkerDirectives, TheShippedVocabularyGivesEveryOptionAMeaningOrAReason) {
    auto const& v = shippedVocabulary();
    std::size_t ignored = 0;
    std::size_t refused = 0;
    for (auto const& row : v.directives) {
        SCOPED_TRACE(row.option);
        if (row.meaning == LinkerDirectiveMeaning::Ignored || row.meaning == LinkerDirectiveMeaning::Refused) {
            (row.meaning == LinkerDirectiveMeaning::Ignored ? ignored : refused) += 1;
            EXPECT_FALSE(row.reason.empty()) << "an ignored or refused option states the reason that is right";
        } else {
            EXPECT_TRUE(row.reason.empty()) << "a reason belongs to an ignored or refused row only";
        }
        if (row.meaning == LinkerDirectiveMeaning::Refused) {
            EXPECT_NE(row.reason.find("D-LK-PE-IMAGE-CANNOT-HONOUR-EVERY-LINKER-DIRECTIVE"), std::string::npos)
                << "a refused option names the row that carries the work: " << row.reason;
        }
    }
    EXPECT_EQ(ignored, 9u);
    EXPECT_EQ(refused, 2u);
    auto const optionFor = [&](LinkerDirectiveMeaning m) {
        std::vector<std::string> out;
        for (auto const& row : v.directives) {
            if (row.meaning == m) out.push_back(row.option);
        }
        return out;
    };
    EXPECT_EQ(optionFor(LinkerDirectiveMeaning::HideSymbols), std::vector<std::string>{"exclude-symbols"});
    EXPECT_EQ(optionFor(LinkerDirectiveMeaning::ExportSymbol), std::vector<std::string>{"export"});
    EXPECT_EQ(optionFor(LinkerDirectiveMeaning::IncludeSymbol), std::vector<std::string>{"include"});
    EXPECT_EQ(optionFor(LinkerDirectiveMeaning::AlternateName), std::vector<std::string>{"alternatename"});
    EXPECT_EQ(optionFor(LinkerDirectiveMeaning::CommonAlignment), std::vector<std::string>{"aligncomm"});
    EXPECT_EQ(optionFor(LinkerDirectiveMeaning::StackSize), std::vector<std::string>{"stack"});
    EXPECT_EQ(optionFor(LinkerDirectiveMeaning::HeapSize), std::vector<std::string>{"heap"});
    EXPECT_EQ(optionFor(LinkerDirectiveMeaning::Subsystem), std::vector<std::string>{"subsystem"});
    EXPECT_EQ(optionFor(LinkerDirectiveMeaning::EntryPoint), std::vector<std::string>{"entry"});
    EXPECT_EQ(optionFor(LinkerDirectiveMeaning::SectionAttributes), std::vector<std::string>{"section"});
    EXPECT_EQ(optionFor(LinkerDirectiveMeaning::ImageChecksum), std::vector<std::string>{"release"});
    EXPECT_EQ(optionFor(LinkerDirectiveMeaning::ImageVersion), std::vector<std::string>{"version"});
    EXPECT_EQ(optionFor(LinkerDirectiveMeaning::ImageBase), std::vector<std::string>{"base"});
    EXPECT_EQ(optionFor(LinkerDirectiveMeaning::SectionAlignment), std::vector<std::string>{"align"});
    EXPECT_EQ(optionFor(LinkerDirectiveMeaning::MismatchCheck), std::vector<std::string>{"failifmismatch"});
    EXPECT_EQ(optionFor(LinkerDirectiveMeaning::DllImage), std::vector<std::string>{"dll"});
    EXPECT_EQ(optionFor(LinkerDirectiveMeaning::Refused), (std::vector<std::string>{"manifestdependency", "merge"}));
}

// P69 round 4 (MINOR 3): an option NO reference honours in a directive is WARNED and dropped, as link.exe does
// (LNK4229, ✔MEASURED 2026-10-07 over 45 options: `/LARGEADDRESSAWARE`, `/DELAYLOAD:` ...) and GNU ld does
// ("unrecognized"); the parse reads on past it, and a relocatable artifact hands the token on verbatim.
TEST(CoffLinkerDirectives, AnOptionNoReferenceHonoursIsWarnedAndHandedOn) {
    auto const& v = shippedVocabulary();
    auto const r = pe::parseCoffLinkerDirectives(
        " /LARGEADDRESSAWARE /DELAYLOAD:dss.dll /EXPORT:after_them \"/DSSNOSUCH:a b\"", v);
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_EQ(r->warnings.size(), 3u);
    EXPECT_NE(r->warnings[0].find("asks the final linker for 'LARGEADDRESSAWARE'"), std::string::npos)
        << r->warnings[0];
    EXPECT_NE(r->warnings[1].find("'DELAYLOAD'"), std::string::npos) << r->warnings[1];
    EXPECT_NE(r->warnings[0].find("LNK4229"), std::string::npos) << r->warnings[0];
    EXPECT_EQ(exportedNamesOf(*r), std::vector<std::string>{"after_them"}) << "the parse reads on past them";
    EXPECT_EQ(r->handOn, (std::vector<std::string>{"/LARGEADDRESSAWARE", "/DELAYLOAD:dss.dll",
                                                    "/EXPORT:after_them", "\"/DSSNOSUCH:a b\""}))
        << "each token handed on as it reads back, a quoted one requoted";
}

TEST(CoffLinkerDirectives, AMalformedDirectiveIsRefusedInItsOwnWords) {
    auto const& v = shippedVocabulary();
    struct Case {
        char const* label;
        char const* text;
        char const* words;
    };
    Case const kCases[] = {
        {"an include naming no symbol", " /INCLUDE:", "must name exactly one symbol ('/INCLUDE:<name>')"},
        {"an include naming two", " /INCLUDE:a,b", "must name exactly one symbol"},
        {"an alternate name with no fallback", " /alternatename:a",
         "must read '/alternatename:<name>=<fallback>'"},
        {"an alternate name with an empty fallback", " /alternatename:a=", "two non-empty names"},
        {"an alternate name with an empty name", " -alternatename:=b", "two non-empty names"},
        {"an alternate name with two fallbacks", " /alternatename:a=b=c", "two non-empty names"},
        {"a common alignment past 8192 bytes", " -aligncomm:c,14", "with n in [0, 13]"},
        {"a common alignment with no exponent", " -aligncomm:c", "with n in [0, 13]"},
        {"a common alignment that is not a number", " -aligncomm:c,5x", "with n in [0, 13]"},
        {"a token with no option prefix", " DEFAULTLIB:x", "does not begin with an option prefix"},
        {"an export attribute that is none", " /EXPORT:f,BOGUS", "('BOGUS' is none of those)"},
        {"an export of two empty names", " /EXPORT:=", "two non-empty names"},
        {"an empty name in a hide list", " -exclude-symbols:a,,b", "names an empty symbol"},
        // P69 round 4: the image requests, malformed as link.exe refuses them (✔MEASURED 2026-10-07).
        {"a stack size that is not a number", " /STACK:big", "must read '/STACK:<reserve>[,<commit>]'"},
        {"a heap commit past its reserve", " /HEAP:0x100000,0x200000", "LNK1229"},
        {"a subsystem nothing names", " /SUBSYSTEM:consol", "does not list (it lists console, windows)"},
        {"a subsystem version that is not one", " /SUBSYSTEM:console,6.x", "<name>[,<major>[.<minor>]]"},
        {"an entry naming two symbols", " /ENTRY:a,b", "must name exactly one symbol ('/ENTRY:<name>')"},
        {"section attributes with an ALIGN", " /SECTION:.data,ALIGN=16", "LNK1117 / LNK1137"},
        {"section attributes with a letter link.exe refuses", " /SECTION:.data,RWX", "E R W S D K P"},
        {"a section with no attributes", " /SECTION:.data", "<section>,<attributes>"},
        {"a checksum request with a value", " /RELEASE:yes", "takes no value"},
        {"a DLL request with a value", " /DLL:yes", "takes no value"},
        {"an image version past 65535", " /VERSION:65536", "must read '/VERSION:<major>[.<minor>]'"},
        {"an image version of three parts", " /VERSION:1.2.3", "must read '/VERSION:<major>[.<minor>]'"},
        {"a base that is not a multiple of 64 KiB", " /BASE:0x140001000", "LNK1224"},
        {"a mismatch check with no key", " /FAILIFMISMATCH:=v", "must read '/FAILIFMISMATCH:<key>=<value>'"},
    };
    for (auto const& c : kCases) {
        SCOPED_TRACE(c.label);
        auto const r = pe::parseCoffLinkerDirectives(c.text, v);
        ASSERT_FALSE(r.has_value());
        EXPECT_NE(r.error().find(c.words), std::string::npos) << r.error();
    }
}

TEST(CoffLinkerDirectives, TheWriterSpellsEachHiddenNameAsClangDoes) {
    auto const& v = shippedVocabulary();
    std::vector<std::string> const names{"helper", "helper_alias"};
    auto const text = pe::coffHideDirectiveText(names, v);
    ASSERT_TRUE(text.has_value()) << text.error();
    EXPECT_EQ(*text, " -exclude-symbols:helper -exclude-symbols:helper_alias");
    // ...and the reader reads exactly those names back.
    auto const back = pe::parseCoffLinkerDirectives(*text, v);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->hidden, names);
    // A name the list grammar would split or re-read is refused, not mangled.
    std::vector<std::string> const bad{"a,b"};
    auto const refused = pe::coffHideDirectiveText(bad, v);
    ASSERT_FALSE(refused.has_value());
    EXPECT_NE(refused.error().find("cannot be spelled"), std::string::npos) << refused.error();
    EXPECT_TRUE(pe::coffHideDirectiveText({}, v).value().empty()) << "no hidden name, no text";
}

// The writer states EVERY request a re-emitted object hands on — a hidden definition, an `/INCLUDE:` required of
// the object it was read from, a fallback, a common's alignment — and the reader reads each back as what it was.
TEST(CoffLinkerDirectives, TheWriterStatesEveryRequestTheReaderReadsBack) {
    auto const& v = shippedVocabulary();
    std::vector<std::string> const              hidden{"helper"};
    std::vector<std::string> const              included{"_tls_used", "puts"};
    std::vector<pe::CoffAlternateName> const    alternates{{"my_puts", "puts"}};
    std::vector<pe::CoffCommonAlignment> const  commons{{"c64", 6}};
    auto const text =
        pe::coffLinkerDirectiveText(pe::CoffDirectiveStatements{hidden, included, alternates, commons}, v);
    ASSERT_TRUE(text.has_value()) << text.error();
    EXPECT_EQ(*text, " -exclude-symbols:helper -include:_tls_used -include:puts -alternatename:my_puts=puts"
                     " -aligncomm:c64,6");
    auto const back = pe::parseCoffLinkerDirectives(*text, v);
    ASSERT_TRUE(back.has_value()) << back.error();
    EXPECT_EQ(back->hidden, hidden);
    EXPECT_EQ(back->included, included);
    EXPECT_EQ(back->alternates, alternates);
    EXPECT_EQ(back->commonAlignments, commons);
    // A name the `=` grammar would re-split is refused, not mangled; so is an alignment no section can state.
    std::vector<pe::CoffAlternateName> const resplit{{"a=b", "c"}};
    auto const refused = pe::coffLinkerDirectiveText(pe::CoffDirectiveStatements{{}, {}, resplit, {}}, v);
    ASSERT_FALSE(refused.has_value());
    EXPECT_NE(refused.error().find("cannot be spelled"), std::string::npos) << refused.error();
    std::vector<pe::CoffCommonAlignment> const tooWide{{"c", 14}};
    auto const wide = pe::coffLinkerDirectiveText(pe::CoffDirectiveStatements{{}, {}, {}, tooWide}, v);
    ASSERT_FALSE(wide.has_value());
    EXPECT_NE(wide.error().find("past the 2^13"), std::string::npos) << wide.error();
}

// link.exe's alignment for a common no `-aligncomm:` names (✔MEASURED 2026-10-06, run 20261006-225314-2e577758).
TEST(CoffLinkerDirectives, ACommonsNaturalAlignmentIsLinkExes) {
    std::pair<std::uint64_t, std::uint64_t> const kMeasured[] = {
        {1, 1}, {3, 4}, {5, 8}, {17, 32}, {40, 32}, {100, 32}, {5000, 32}};
    for (auto const& [size, alignment] : kMeasured) {
        EXPECT_EQ(pe::naturalCoffCommonAlignment(size), alignment) << "size " << size;
    }
}

// ══ The writer and the reader ════════════════════════════════════════════════

TEST(CoffLinkerDirectives, AHiddenDefinitionIsWrittenAsADirectiveAndReadBackHidden) {
    auto const l = loadPe("pe64-x86_64-windows");
    ASSERT_TRUE(l.target && l.format);
    for (bool const alias : {false, true}) {
        SCOPED_TRACE(alias ? "with a hidden alias" : "canonical only");
        DiagnosticReporter rep;
        auto const obj = pe::encode(hiddenHelperModule(SymbolVisibility::Hidden, alias), *l.target, *l.format, rep);
        ASSERT_FALSE(obj.empty()) << diagnosticsOf(rep);
        auto const sections = coffSections(obj);
        ASSERT_FALSE(sections.empty());
        auto const& d = sections.back();
        EXPECT_EQ(d.name, ".drectve") << "the directive section is the LAST ordinal";
        EXPECT_EQ(d.chars, 0x00100A00u) << "IMAGE_SCN_LNK_INFO | LNK_REMOVE | ALIGN_1BYTES, from the document";
        std::string const text(obj.begin() + d.raw, obj.begin() + d.raw + d.size);
        EXPECT_EQ(text, alias ? " -exclude-symbols:helper -exclude-symbols:helper_alias"
                              : " -exclude-symbols:helper");
        // Read back: the helper (and its alias) Hidden, the entry Default.
        DiagnosticReporter readRep;
        auto const m = pe::readRelocatableObject(obj, *l.target, *l.format, readRep);
        ASSERT_TRUE(m.has_value()) << diagnosticsOf(readRep);
        std::size_t seen = 0;
        for (auto const& s : m->symbols) {
            if (s.name == "lib_entry") {
                ++seen;
                EXPECT_EQ(s.visibility, SymbolVisibility::Default);
            }
            if (s.name == "helper" || s.name == "helper_alias") {
                ++seen;
                EXPECT_EQ(s.visibility, SymbolVisibility::Hidden) << s.name;
                EXPECT_EQ(s.binding, SymbolBinding::Global) << s.name << " stays a global name";
            }
        }
        EXPECT_EQ(seen, alias ? 3u : 2u);
    }
}

TEST(CoffLinkerDirectives, EveryHiddenShapeIsWrittenAsADirectiveAndReadBackHidden) {
    auto const l = loadPe("pe64-x86_64-windows");
    ASSERT_TRUE(l.target && l.format);
    DiagnosticReporter rep;
    auto const obj = pe::encode(everyHiddenShapeModule(), *l.target, *l.format, rep);
    ASSERT_FALSE(obj.empty()) << diagnosticsOf(rep);
    auto const d = sectionNamed(obj, ".drectve");
    ASSERT_TRUE(d.has_value());
    std::string const text(obj.begin() + d->raw, obj.begin() + d->raw + d->size);
    // Functions first (each with its aliases), then data: the writer's two loops, each pinned by its own name.
    EXPECT_EQ(text, " -exclude-symbols:helper -exclude-symbols:helper_weak -exclude-symbols:hidden_weak"
                    " -exclude-symbols:hidden_datum");
    DiagnosticReporter readRep;
    auto const m = pe::readRelocatableObject(obj, *l.target, *l.format, readRep);
    ASSERT_TRUE(m.has_value()) << diagnosticsOf(readRep);
    struct Want {
        char const*      name;
        SymbolBinding    binding;
        SymbolVisibility visibility;
    };
    Want const kWant[] = {
        {"lib_entry", SymbolBinding::Global, SymbolVisibility::Default},
        {"helper", SymbolBinding::Global, SymbolVisibility::Hidden},
        {"helper_weak", SymbolBinding::Weak, SymbolVisibility::Hidden},   // the reader's weak-external arm
        {"hidden_weak", SymbolBinding::Weak, SymbolVisibility::Hidden},   // its external arm, a COMDAT
        {"hidden_datum", SymbolBinding::Global, SymbolVisibility::Hidden},  // its external arm, a datum
    };
    for (auto const& w : kWant) {
        SCOPED_TRACE(w.name);
        auto const s = std::find_if(m->symbols.begin(), m->symbols.end(),
                                    [&](ModuleSymbol const& x) { return x.name == w.name; });
        ASSERT_NE(s, m->symbols.end());
        EXPECT_EQ(s->binding, w.binding);
        EXPECT_EQ(s->visibility, w.visibility);
    }
}

TEST(CoffLinkerDirectives, AnInternalDefinitionIsHiddenTooAndAProtectedOneIsNot) {
    auto const l = loadPe("pe64-x86_64-windows");
    ASSERT_TRUE(l.target && l.format);
    {
        DiagnosticReporter rep;
        auto const obj = pe::encode(hiddenHelperModule(SymbolVisibility::Internal, false), *l.target, *l.format, rep);
        ASSERT_FALSE(obj.empty()) << diagnosticsOf(rep);
        auto const d = sectionNamed(obj, ".drectve");
        ASSERT_TRUE(d.has_value()) << "internal visibility keeps a definition out of the exports as hidden does";
    }
    {
        // `protected` is still exported (isExternallyVisible): no directive, and no section at all.
        DiagnosticReporter rep;
        auto const obj = pe::encode(hiddenHelperModule(SymbolVisibility::Protected, false), *l.target, *l.format, rep);
        ASSERT_FALSE(obj.empty()) << diagnosticsOf(rep);
        EXPECT_FALSE(sectionNamed(obj, ".drectve").has_value());
    }
}

TEST(CoffLinkerDirectives, AModuleWithNoHiddenDefinitionWritesNoDirectiveSection) {
    auto const l = loadPe("pe64-x86_64-windows");
    ASSERT_TRUE(l.target && l.format);
    DiagnosticReporter rep;
    auto const obj = pe::encode(hiddenHelperModule(SymbolVisibility::Default, false), *l.target, *l.format, rep);
    ASSERT_FALSE(obj.empty()) << diagnosticsOf(rep);
    EXPECT_FALSE(sectionNamed(obj, ".drectve").has_value()) << "every other object is byte-identical";
}

// P69 round 4: what the reader no longer refuses, and why. An option no reference honours is the LINK's to warn
// about (MINOR 3: link.exe LNK4229, GNU ld "unrecognized"); a hide or an export naming a symbol this object does not
// define is LINK-WIDE (✔MEASURED 2026-10-07: link.exe and lld-link export a name another object defines, lld-link
// -lldmingw hides one), so the reader states each on the unit for the link to apply, and hands the token on.
TEST(CoffLinkerDirectives, TheReaderHandsAnUndeclaredDirectiveAndAForeignNameToTheLink) {
    auto const l = loadPe("pe64-x86_64-windows");
    ASSERT_TRUE(l.target && l.format);
    DiagnosticReporter rep;
    auto const obj = pe::encode(hiddenHelperModule(SymbolVisibility::Hidden, false), *l.target, *l.format, rep);
    ASSERT_FALSE(obj.empty()) << diagnosticsOf(rep);
    auto const d = sectionNamed(obj, ".drectve");
    ASSERT_TRUE(d.has_value());
    ASSERT_EQ(d->size, 24u);
    auto const readWith = [&](std::string_view text) {   // exactly 24 bytes, written over the section in place
        EXPECT_EQ(text.size(), 24u);
        auto patched = obj;
        std::copy(text.begin(), text.end(), patched.begin() + d->raw);
        DiagnosticReporter readRep;
        auto m = pe::readRelocatableObject(patched, *l.target, *l.format, readRep);
        EXPECT_EQ(readRep.errorCount(), 0u) << diagnosticsOf(readRep);
        EXPECT_TRUE(readRep.all().empty()) << "the reader reports nothing itself: " << diagnosticsOf(readRep);
        return m;
    };
    {
        SCOPED_TRACE("an option the vocabulary does not list");
        auto const m = readWith(" /DSSNOSUCHOPTION:forced");
        ASSERT_TRUE(m.has_value());
        ASSERT_EQ(m->linkerRequests.warnings.size(), 1u);
        EXPECT_NE(m->linkerRequests.warnings[0].find("asks the final linker for 'DSSNOSUCHOPTION'"),
                  std::string::npos)
            << m->linkerRequests.warnings[0];
        EXPECT_EQ(m->linkerRequests.handOn, std::vector<std::string>{"/DSSNOSUCHOPTION:forced"});
    }
    {
        SCOPED_TRACE("a hide directive naming no definition here");
        auto const m = readWith(" -exclude-symbols:absent");
        ASSERT_TRUE(m.has_value());
        EXPECT_EQ(m->linkerRequests.hides, std::vector<std::string>{"absent"});
        EXPECT_EQ(m->linkerRequests.handOn, std::vector<std::string>{"-exclude-symbols:absent"})
            << "restated in the vocabulary's own spelling for a relocatable artifact's final linker";
        for (auto const& ms : m->symbols) {
            EXPECT_EQ(ms.visibility, SymbolVisibility::Default) << ms.name << ": the hide named none of these";
        }
    }
    // An export is a REFERENCE of the object, as an include is: link.exe and lld-link pull the archive member that
    // defines an exported name nothing else names, and bind one only an import library defines (✔MEASURED
    // 2026-10-07, r4probe/m7) -- so a name the object does not define gets a required row, and one it does gets none.
    auto const requiredRow = [](AssembledModule const& m, std::string_view name) {
        std::size_t rows = 0;
        bool required = false;
        for (auto const& e : m.externImports) {
            if (e.mangledName != name) continue;
            ++rows;
            required = required || e.requiredByDirective;
        }
        return std::pair{rows, required};
    };
    {
        SCOPED_TRACE("an export of a name this object does not define");
        auto const m = readWith(" /EXPORT:absent_export  ");
        ASSERT_TRUE(m.has_value());
        ASSERT_EQ(m->linkerRequests.exports.size(), 1u);
        EXPECT_EQ(m->linkerRequests.exports[0].internalName, "absent_export");
        EXPECT_EQ(m->linkerRequests.exports[0].exportedName, "absent_export");
        EXPECT_EQ(m->linkerRequests.handOn, std::vector<std::string>{"/EXPORT:absent_export"});
        EXPECT_EQ(requiredRow(*m, "absent_export"), (std::pair<std::size_t, bool>{1u, true}))
            << "the link must find a definition of what the object exports";
    }
    {
        SCOPED_TRACE("a renamed export of a name this object defines");
        auto const m = readWith(" /EXPORT:alias=lib_entry");
        ASSERT_TRUE(m.has_value());
        ASSERT_EQ(m->linkerRequests.exports.size(), 1u);
        EXPECT_EQ(m->linkerRequests.exports[0].internalName, "lib_entry");
        EXPECT_EQ(requiredRow(*m, "lib_entry"), (std::pair<std::size_t, bool>{0u, false})) << "it is defined here";
        EXPECT_EQ(requiredRow(*m, "alias"), (std::pair<std::size_t, bool>{0u, false}))
            << "the exported name is no reference: the internal one is";
    }
}

// ══ End to end: a DSS DLL linking a DSS member, and GNU ld linking the same member ══

namespace {

// A hidden function, a hidden datum and a hidden WEAK function (P69 round 3): the three shapes a member's
// visibility hides, each reaching the writer's hide loops and the reader's arms by its own route.
constexpr char const* kMember =
    "__attribute__((visibility(\"hidden\"))) int helper(int x) { return x + 1; }\n"
    "__attribute__((visibility(\"hidden\"))) int hidden_datum = 1;\n"
    "__attribute__((visibility(\"hidden\"), weak)) int hidden_weak(void) { return 0; }\n"
    "int lib_entry(void) { return helper(41) + hidden_datum - 1 + hidden_weak(); }\n";
constexpr char const* kDllMain =
    "int lib_entry(void);\n"
    "int dll_api(void) { return lib_entry(); }\n";

void writeText(fs::path const& p, std::string_view t) { std::ofstream(p, std::ios::binary) << t; }

[[nodiscard]] std::vector<std::uint8_t> readFile(fs::path const& p) {
    std::ifstream in{p, std::ios::binary};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The names a PE image's export directory lists (data directory 0), read off the file.
[[nodiscard]] std::vector<std::string> exportedNames(std::vector<std::uint8_t> const& img) {
    std::vector<std::string> out;
    std::uint32_t const peOff = rdU32(img, 0x3C);
    std::size_t const coff = peOff + 4;
    std::uint16_t const nSections = rdU16(img, coff + 2);
    std::uint16_t const optSize = rdU16(img, coff + 16);
    std::size_t const opt = coff + 20;
    std::uint32_t const exportRva = rdU32(img, opt + 112);
    if (exportRva == 0) return out;
    std::size_t const secTable = opt + optSize;
    auto const fileOf = [&](std::uint32_t rva) -> std::size_t {
        for (std::uint16_t i = 0; i < nSections; ++i) {
            std::size_t const s = secTable + i * 40u;
            std::uint32_t const va = rdU32(img, s + 12), vs = rdU32(img, s + 8), raw = rdU32(img, s + 20);
            if (rva >= va && rva < va + std::max(vs, rdU32(img, s + 16))) return raw + (rva - va);
        }
        return 0;
    };
    std::size_t const dir = fileOf(exportRva);
    std::uint32_t const numNames = rdU32(img, dir + 24);
    std::size_t const namePtrs = fileOf(rdU32(img, dir + 32));
    for (std::uint32_t i = 0; i < numNames; ++i) {
        std::size_t p = fileOf(rdU32(img, namePtrs + i * 4u));
        std::string n;
        while (p < img.size() && img[p] != 0) n.push_back(static_cast<char>(img[p++]));
        out.push_back(n);
    }
    return out;
}

// Stage 1: the member, a DSS pe64 static library. Stage 2: a DSS DLL that links it.
[[nodiscard]] fs::path buildMemberLibrary(fs::path const& dir, DiagnosticReporter& rep) {
    writeText(dir / "member.c", kMember);
    auto const out = dir / "lib";
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    int const rc = p.compileFiles(std::vector<std::string>{(dir / "member.c").string()}, "c",
                                  std::vector<std::string>{"x86_64:pe64-x86_64-windows-staticlib"}, rep);
    if (rc != 0) return {};
    for (auto const& e : fs::directory_iterator(out)) {
        if (e.path().extension() == ".lib") return e.path();
    }
    return {};
}

}  // namespace

TEST(CoffLinkerDirectives, ADssDllLinkingAMemberWithAHiddenHelperExportsTheEntryAlone) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-directives"};
    auto const dir = scratch.path();
    DiagnosticReporter rep;
    auto const lib = buildMemberLibrary(dir, rep);
    ASSERT_FALSE(lib.empty()) << diagnosticsOf(rep);
    writeText(dir / "dllmain.c", kDllMain);
    auto const out = dir / "dll";
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    p.setResolveLibraries(std::vector<fs::path>{lib});
    DiagnosticReporter dllRep;
    int const rc = p.compileFiles(std::vector<std::string>{(dir / "dllmain.c").string()}, "c",
                                  std::vector<std::string>{"x86_64:pe64-x86_64-windows-dll"}, dllRep);
    ASSERT_EQ(rc, 0) << diagnosticsOf(dllRep);
    fs::path dll;
    for (auto const& e : fs::directory_iterator(out)) {
        if (e.path().extension() == ".dll") dll = e.path();
    }
    ASSERT_FALSE(dll.empty());
    auto names = exportedNames(readFile(dll));
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::string>{"dll_api", "lib_entry"}))
        << "the member's hidden `helper`, `hidden_datum` and `hidden_weak` must not be exported: its `.drectve` "
           "hides them, and the export gate honours the visibility the COFF reader reads back";
}

#if defined(_WIN32)
// A cl object's own requests, honoured by a DSS link (✔MEASURED 2026-10-06 that link.exe honours each, run
// 20261006-222410-df088936): `/alternatename:member_hook=member_hook_default` answers a reference nothing else
// defines (exit 42) and yields to a definition the program makes (exit 43); `/INCLUDE:member_kept` names a function
// the object defines; and the object's `/DEFAULTLIB:` options are read and dropped, each for its stated reason.
TEST(CoffLinkerDirectivesNative, ADssLinkHonoursAClObjectsAlternateNameAndInclude) {
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-directives-cl"};
    auto const dir = scratch.path();
    namespace np = test_support::native_probe;
    auto const msvc = np::locateMsvcToolchain(dir);
    if (msvc.toolAbsent()) GTEST_SKIP() << msvc.detail;
    ASSERT_TRUE(msvc.ok()) << msvc.describe();
    auto const tools = np::msvcToolsIn(msvc, dir);
    ASSERT_TRUE(tools.ready()) << tools.describe();
    writeText(dir / "lib.c",
              "int member_hook(void);\n"
              "int member_hook_default(void) { return 42; }\n"
              "#pragma comment(linker, \"/alternatename:member_hook=member_hook_default\")\n"
              "#pragma comment(linker, \"/INCLUDE:member_kept\")\n"
              "int member_kept(void) { return 7; }\n"
              "int lib_answer(void) { return member_hook(); }\n");
    std::string const cl = np::captureCmd("cd /d \"" + dir.string() + "\" && cl /nologo /c /O2 /MD lib.c",
                                          dir / "cl.txt");
    ASSERT_EQ(np::systemUnder(tools.env, cl), 0) << np::tailOf(dir / "cl.txt", 30, "cl");
    struct Arm {
        char const* stem;
        char const* main;
        unsigned    exit;
    };
    Arm const kArms[] = {
        {"fallback", "int lib_answer(void);\nint main(void) { return lib_answer(); }\n", 42u},
        {"defined", "int lib_answer(void);\nint member_hook(void) { return 43; }\n"
                    "int main(void) { return lib_answer(); }\n", 43u},
    };
    for (auto const& a : kArms) {
        SCOPED_TRACE(a.stem);
        writeText(dir / (std::string{a.stem} + ".c"), a.main);
        auto const out = dir / (std::string{a.stem} + ".out");
        fs::create_directories(out);
        Program p;
        p.setOutputDir(out);
        DiagnosticReporter rep;
        int const rc = p.compileFiles(
            std::vector<std::string>{(dir / (std::string{a.stem} + ".c")).string(), (dir / "lib.obj").string()},
            "c", std::vector<std::string>{"x86_64:pe64-x86_64-windows-exec"}, rep);
        ASSERT_EQ(rc, 0) << diagnosticsOf(rep);
        fs::path exe;
        for (auto const& e : fs::directory_iterator(out)) {
            if (e.path().extension() == ".exe") exe = e.path();
        }
        ASSERT_FALSE(exe.empty());
        auto const r = test_support::runBinary(exe);
        ASSERT_TRUE(r.spawned) << r.diagnostic;
        EXPECT_EQ(r.exitCode, a.exit);
    }
}

TEST(CoffLinkerDirectivesNative, GnuLdHonoursTheDirectiveDssWrites) {
    if (std::system("where gcc >nul 2>&1") != 0) {
        GTEST_SKIP() << "no MinGW `gcc` on PATH -- the GNU ld arm is inert on this host";
    }
    test_support::ScratchDir scratch{test_support::Location::InsideRepo, "coff-directives-gnu"};
    auto const dir = scratch.path();
    writeText(dir / "member.c", kMember);
    auto const out = dir / "obj";
    fs::create_directories(out);
    Program p;
    p.setOutputDir(out);
    DiagnosticReporter rep;
    ASSERT_EQ(p.compileFiles(std::vector<std::string>{(dir / "member.c").string()}, "c",
                             std::vector<std::string>{"x86_64:pe64-x86_64-windows"}, rep),
              0)
        << diagnosticsOf(rep);
    auto const obj = out / "member.obj";
    ASSERT_TRUE(fs::exists(obj));
    std::string const cmd = "cd /d \"" + dir.string() + "\" && gcc -shared -nostdlib -Wl,-e,lib_entry -o gnu.dll \""
                          + obj.string() + "\" >nul 2>&1";
    ASSERT_EQ(std::system(("\"" + cmd + "\"").c_str()), 0) << "GNU ld must link DSS's member into a DLL";
    auto names = exportedNames(readFile(dir / "gnu.dll"));
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, std::vector<std::string>{"lib_entry"})
        << "GNU ld auto-exports every external definition its member does not exclude";
}
#endif

// ══ The vocabulary's own refusals ═══════════════════════════════════════════

namespace {

[[nodiscard]] std::string shippedText(char const* stem) {
    auto const root = dss::test::findConfigRoot();
    if (!root.has_value()) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    std::ifstream in{*root / "object-formats" / (std::string{stem} + ".format.json"), std::ios::binary};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST(CoffLinkerDirectives, TheLoaderRefusesEveryMalformedOrMisplacedVocabulary) {
    struct Case {
        char const* label;
        char const* stem;
        void (*mutate)(nlohmann::json&);
        char const* path;
        char const* words;
    };
    Case const kCases[] = {
        {"declared on an image", "pe64-x86_64-windows-dll",
         [](nlohmann::json& d) {
             d["pe"]["linkerDirectives"] = nlohmann::json::parse(
                 R"({"section": ".drectve", "characteristics": 1051136, "optionPrefixes": "-/",
                     "directives": [{"option": "exclude-symbols", "meaning": "hideSymbols"}]})");
         },
         "/pe/linkerDirectives", "declared on a PE IMAGE document"},
        {"an unknown meaning", "pe64-x86_64-windows",
         [](nlohmann::json& d) { d["pe"]["linkerDirectives"]["directives"][1]["meaning"] = "export"; },
         "/pe/linkerDirectives/directives/1/meaning", "'meaning' must be"},
        {"an option declared twice, in two cases", "pe64-x86_64-windows",
         [](nlohmann::json& d) { d["pe"]["linkerDirectives"]["directives"][2]["option"] = "EXPORT"; },
         "/pe/linkerDirectives/directives/2/option", "is declared twice"},
        {"no hide option", "pe64-x86_64-windows",
         [](nlohmann::json& d) {
             d["pe"]["linkerDirectives"]["directives"][0]["meaning"] = "ignored";
             d["pe"]["linkerDirectives"]["directives"][0]["reason"]  = "the hide option dropped on purpose";
         },
         "/pe/linkerDirectives/directives", "declares no 'hideSymbols' option"},
        {"an ignored option that states no reason", "pe64-x86_64-windows",
         [](nlohmann::json& d) { d["pe"]["linkerDirectives"]["directives"][5].erase("reason"); },
         "/pe/linkerDirectives/directives/5/reason", "is 'ignored' and states no 'reason'"},
        {"an applied option that states a reason", "pe64-x86_64-windows",
         [](nlohmann::json& d) { d["pe"]["linkerDirectives"]["directives"][2]["reason"] = "stale"; },
         "/pe/linkerDirectives/directives/2/reason", "a 'reason' only an 'ignored' or 'refused' row states"},
        {"an empty reason", "pe64-x86_64-windows",
         [](nlohmann::json& d) { d["pe"]["linkerDirectives"]["directives"][5]["reason"] = ""; },
         "/pe/linkerDirectives/directives/5/reason", "'reason' must be a non-empty string"},
        // P69 round 4: the per-meaning tables, each read on its own meaning's row alone.
        {"a refused option that states no reason", "pe64-x86_64-windows",
         [](nlohmann::json& d) { d["pe"]["linkerDirectives"]["directives"][13].erase("reason"); },
         "/pe/linkerDirectives/directives/13/reason", "is 'refused' and states no 'reason'"},
        {"a subsystem row with no subsystems", "pe64-x86_64-windows",
         [](nlohmann::json& d) { d["pe"]["linkerDirectives"]["directives"][9].erase("subsystems"); },
         "/pe/linkerDirectives/directives/9/subsystems", "names no 'subsystems'"},
        {"a subsystem value past 0xFFFF", "pe64-x86_64-windows",
         [](nlohmann::json& d) { d["pe"]["linkerDirectives"]["directives"][9]["subsystems"]["console"] = 65536; },
         "/pe/linkerDirectives/directives/9/subsystems/console", "an integer value in [0, 0xFFFF]"},
        {"a minimum version that is not one", "pe64-x86_64-windows",
         [](nlohmann::json& d) { d["pe"]["linkerDirectives"]["directives"][9]["minimumVersion"] = "five"; },
         "/pe/linkerDirectives/directives/9/minimumVersion", "'minimumVersion' must be a string"},
        {"a subsystems table on another row", "pe64-x86_64-windows",
         [](nlohmann::json& d) {
             d["pe"]["linkerDirectives"]["directives"][10]["subsystems"] = nlohmann::json{{"console", 3}};
         },
         "/pe/linkerDirectives/directives/10/subsystems", "belongs to a 'subsystem' row"},
        {"an entry row with no runtime startups", "pe64-x86_64-windows",
         [](nlohmann::json& d) { d["pe"]["linkerDirectives"]["directives"][12].erase("runtimeStartups"); },
         "/pe/linkerDirectives/directives/12/runtimeStartups", "names no 'runtimeStartups'"},
        {"an unsupported startup with no reason", "pe64-x86_64-windows",
         [](nlohmann::json& d) {
             d["pe"]["linkerDirectives"]["directives"][12]["unsupportedStartups"]["wmainCRTStartup"] = "";
         },
         "/pe/linkerDirectives/directives/12/unsupportedStartups/wmainCRTStartup", "a non-empty reason"},
        {"runtime symbols on another row", "pe64-x86_64-windows",
         [](nlohmann::json& d) {
             d["pe"]["linkerDirectives"]["directives"][1]["runtimeSymbols"] = nlohmann::json{{"_tls_used", "x"}};
         },
         "/pe/linkerDirectives/directives/1/runtimeSymbols", "belongs to a 'includeSymbol' row"},
        {"a section name past eight bytes", "pe64-x86_64-windows",
         [](nlohmann::json& d) { d["pe"]["linkerDirectives"]["section"] = ".directives"; },
         "/pe/linkerDirectives/section", "exceeds the 8 bytes"},
        {"a prefix the grammar reads otherwise", "pe64-x86_64-windows",
         [](nlohmann::json& d) { d["pe"]["linkerDirectives"]["optionPrefixes"] = "-:"; },
         "/pe/linkerDirectives/optionPrefixes", "whitespace, a quote, a colon or repeated"},
        {"an empty vocabulary", "pe64-x86_64-windows",
         [](nlohmann::json& d) { d["pe"]["linkerDirectives"]["directives"] = nlohmann::json::array(); },
         "/pe/linkerDirectives/directives", "must be a non-empty array"},
        {"a typo in the block", "pe64-x86_64-windows",
         [](nlohmann::json& d) { d["pe"]["linkerDirectives"]["sectoin"] = ".drectve"; },
         "/pe/linkerDirectives/sectoin", "unknown key"},
        {"a typo in the pe block itself", "pe64-x86_64-windows",
         [](nlohmann::json& d) { d["pe"]["machnie"] = 34404; },
         "/pe/machnie", "unknown key"},
    };
    for (auto const& c : kCases) {
        SCOPED_TRACE(c.label);
        auto const text = shippedText(c.stem);
        ASSERT_FALSE(text.empty()) << "DSS_CONFIG_ROOT must name the shipped config";
        nlohmann::json doc = nlohmann::json::parse(text);
        c.mutate(doc);
        auto const r = ObjectFormatSchema::loadFromText(doc.dump(), c.stem);
        EXPECT_FALSE(r.has_value());
        EXPECT_GE(countAtPath(r, c.path), 1u) << rejectSummary(r);
        EXPECT_GE(countWithMessage(r, c.words), 1u) << rejectSummary(r);
    }
}

// ══ The link's reading of a required name and of a fallback ══════════════════
//
// The rows the COFF reader states (`ExternImport::requiredByDirective`, `ExternImport::fallbackName`) are decided by
// the link, which is format-blind — pinned here on an ELF executable, the one image whose `.symtab` and call sites
// a test can read back, with synthetic units.

namespace {

[[nodiscard]] std::uint64_t rdLE(std::vector<std::uint8_t> const& b, std::size_t off, int width) {
    std::uint64_t v = 0;
    for (int i = 0; i < width; ++i) v |= static_cast<std::uint64_t>(b[off + i]) << (i * 8);
    return v;
}
struct ElfText {
    std::uint64_t addr = 0, offset = 0, size = 0;
};
[[nodiscard]] std::optional<ElfText> elfText(std::vector<std::uint8_t> const& b) {
    std::uint64_t const shoff    = rdLE(b, 40, 8);
    auto const          shnum    = static_cast<std::uint16_t>(rdLE(b, 60, 2));
    auto const          shstrndx = static_cast<std::uint16_t>(rdLE(b, 62, 2));
    std::uint64_t const strOff   = rdLE(b, shoff + shstrndx * 64ull + 24, 8);
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::uint64_t const off = shoff + static_cast<std::uint64_t>(i) * 64;
        std::string name;
        for (std::uint64_t p = strOff + rdLE(b, off, 4); p < b.size() && b[p] != 0; ++p) {
            name.push_back(static_cast<char>(b[p]));
        }
        if (name == ".text") return ElfText{rdLE(b, off + 16, 8), rdLE(b, off + 24, 8), rdLE(b, off + 32, 8)};
    }
    return std::nullopt;
}
// The VA of the first occurrence of `body` in `.text`, or 0.
[[nodiscard]] std::uint64_t vaOfBody(std::vector<std::uint8_t> const& b, ElfText const& t,
                                     std::vector<std::uint8_t> const& body) {
    for (std::uint64_t i = t.offset; i + body.size() <= t.offset + t.size; ++i) {
        if (std::equal(body.begin(), body.end(), b.begin() + static_cast<std::ptrdiff_t>(i))) {
            return t.addr + (i - t.offset);
        }
    }
    return 0;
}

std::vector<std::uint8_t> const kTargetBody{0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};   // mov eax, 42 ; ret
std::vector<std::uint8_t> const kRivalBody{0xB8, 0x07, 0x00, 0x00, 0x00, 0xC3};    // mov eax, 7 ; ret
std::vector<std::uint8_t> const kCallerBody{0xE8, 0x00, 0x00, 0x00, 0x00, 0xC3};   // call rel32 ; ret

// The entry unit: `caller` calls the name `callee` (row symbol #2), which states `fallback` (empty: none) and is
// `required` or not; `extraRequired` adds a row no relocation names, required by a directive.
[[nodiscard]] AssembledModule callerUnit(std::string callee, std::string fallback, std::string extraRequired = {}) {
    AssembledModule m;
    m.cuId              = CompilationUnitId{1};
    m.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = kCallerBody;
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{2};
    rel.kind   = RelocationKind{1};   // rel32
    fn.relocations.push_back(rel);
    m.functions.push_back(fn);
    m.symbols.push_back(ModuleSymbol{SymbolId{1}, "caller", SymbolBinding::Global, SymbolVisibility::Default});
    ExternImport ext;
    ext.symbol       = SymbolId{2};
    ext.mangledName  = std::move(callee);
    ext.fallbackName = std::move(fallback);
    m.externImports.push_back(std::move(ext));
    if (!extraRequired.empty()) {
        ExternImport req;
        req.symbol              = SymbolId{3};
        req.mangledName         = std::move(extraRequired);
        req.requiredByDirective = true;
        req.kindOrigin          = ExternKindOrigin::Pending;
        m.externImports.push_back(std::move(req));
    }
    m.userEntrySymbol = SymbolId{1};
    return m;
}

// A unit defining each (name, body) pair as a Global function.
[[nodiscard]] AssembledModule definer(std::uint32_t cu,
                                      std::vector<std::pair<std::string, std::vector<std::uint8_t>>> const& defs) {
    AssembledModule m;
    m.cuId              = CompilationUnitId{cu};
    m.expectedFuncCount = defs.size();
    std::uint32_t id = 1;
    for (auto const& [name, body] : defs) {
        AssembledFunction fn;
        fn.symbol = SymbolId{id};
        fn.bytes  = body;
        m.functions.push_back(fn);
        m.symbols.push_back(ModuleSymbol{SymbolId{id}, name, SymbolBinding::Global, SymbolVisibility::Default});
        ++id;
    }
    return m;
}

struct Linked {
    LinkedImage        image;
    DiagnosticReporter rep;
};
[[nodiscard]] std::unique_ptr<Linked> linkElfExec(std::vector<AssembledModule> const& mods) {
    auto out = std::make_unique<Linked>();
    auto t = TargetSchema::loadShipped("x86_64");
    auto f = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    if (!t.has_value() || !f.has_value()) {
        ADD_FAILURE() << "x86_64 / elf64-x86_64-linux-exec must load";
        return out;
    }
    out->image = linker::link(std::span<AssembledModule const>{mods}, **t, **f, out->rep);
    return out;
}
// The VA `caller`'s call lands on: the first `call rel32 ; ret` in `.text` (`kCallerBody`, relocated).
[[nodiscard]] std::uint64_t callLanding(std::vector<std::uint8_t> const& b, ElfText const& t) {
    for (std::uint64_t i = t.offset; i + 6 <= t.offset + t.size; ++i) {
        if (b[i] == 0xE8 && b[i + 5] == 0xC3) {
            auto const disp = static_cast<std::int32_t>(rdLE(b, static_cast<std::size_t>(i + 1), 4));
            std::uint64_t const next = t.addr + (i + 5 - t.offset);
            return static_cast<std::uint64_t>(static_cast<std::int64_t>(next) + disp);
        }
    }
    return 0;
}

}  // namespace

// `/INCLUDE:X` with nothing defining X is refused BY NAME, as link.exe and lld-link refuse it (✔MEASURED 2026-10-06:
// lld-link "undefined symbol: nobody_defines_this"), in words that point at the directive, not at a prototype.
TEST(CoffLinkerDirectivesLink, ARequiredNameNothingDefinesIsRefusedByName) {
    auto const l = linkElfExec({callerUnit("target", "", "nobody_defines_this"),
                                definer(2, {{"target", kTargetBody}})});
    EXPECT_FALSE(l->image.ok());
    EXPECT_NE(diagnosticsOf(l->rep).find("undefined symbol 'nobody_defines_this'"), std::string::npos)
        << diagnosticsOf(l->rep);
    EXPECT_NE(diagnosticsOf(l->rep).find("linker directive (`/INCLUDE:` or `/EXPORT:`) requires"), std::string::npos)
        << diagnosticsOf(l->rep);
}

TEST(CoffLinkerDirectivesLink, ARequiredNameAUnitDefinesLinks) {
    auto const l = linkElfExec({callerUnit("target", "", "helper_two"),
                                definer(2, {{"target", kTargetBody}, {"helper_two", kRivalBody}})});
    EXPECT_TRUE(l->image.ok()) << diagnosticsOf(l->rep);
}

// A reference to a name nothing defines resolves to its FALLBACK, wherever the fallback is defined (✔MEASURED
// 2026-10-06, link.exe: `/alternatename:my_puts=puts` reaches the imported `puts`).
TEST(CoffLinkerDirectivesLink, AFallbackAnswersAReferenceNothingElseDefines) {
    auto const l = linkElfExec({callerUnit("my_alias", "real_target"), definer(2, {{"real_target", kTargetBody}})});
    ASSERT_TRUE(l->image.ok()) << diagnosticsOf(l->rep);
    auto const t = elfText(l->image.bytes);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(callLanding(l->image.bytes, *t), vaOfBody(l->image.bytes, *t, kTargetBody))
        << "the call through `my_alias` lands on `real_target`";
}

// A name the link DEFINES answers for itself: the fallback yields (✔MEASURED 2026-10-06, lld-link 18: with the
// alternate's name defined, the call reaches it).
TEST(CoffLinkerDirectivesLink, ADefinedNameWinsOverItsFallback) {
    auto const l = linkElfExec({callerUnit("my_alias", "real_target"),
                                definer(2, {{"real_target", kTargetBody}, {"my_alias", kRivalBody}})});
    ASSERT_TRUE(l->image.ok()) << diagnosticsOf(l->rep);
    auto const t = elfText(l->image.bytes);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(callLanding(l->image.bytes, *t), vaOfBody(l->image.bytes, *t, kRivalBody))
        << "the call reaches `my_alias` itself";
}

TEST(CoffLinkerDirectivesLink, AFallbackNothingDefinesIsRefusedAsTheFallback) {
    auto const l = linkElfExec({callerUnit("my_alias", "real_target"), definer(2, {{"unrelated", kRivalBody}})});
    EXPECT_FALSE(l->image.ok());
    EXPECT_NE(diagnosticsOf(l->rep).find("'real_target'"), std::string::npos) << diagnosticsOf(l->rep);
}

TEST(CoffLinkerDirectivesLink, TwoFallbacksForOneNameAcrossUnitsAreRefused) {
    AssembledModule other = callerUnit("my_alias", "second_target");
    other.cuId            = CompilationUnitId{3};
    other.userEntrySymbol.reset();
    other.symbols[0].name = "caller_two";
    auto const l = linkElfExec({callerUnit("my_alias", "real_target"), other,
                                definer(2, {{"real_target", kTargetBody}, {"second_target", kRivalBody}})});
    EXPECT_FALSE(l->image.ok());
    EXPECT_NE(diagnosticsOf(l->rep).find("two fallbacks"), std::string::npos) << diagnosticsOf(l->rep);
}

TEST(CoffLinkerDirectivesLink, AFallbackCycleNothingDefinesIsRefused) {
    AssembledModule loop = callerUnit("name_b", "name_a");
    loop.cuId            = CompilationUnitId{3};
    loop.userEntrySymbol.reset();
    loop.symbols[0].name = "caller_two";
    auto const l = linkElfExec({callerUnit("name_a", "name_b"), loop, definer(2, {{"unrelated", kRivalBody}})});
    EXPECT_FALSE(l->image.ok());
    EXPECT_NE(diagnosticsOf(l->rep).find("form a cycle"), std::string::npos) << diagnosticsOf(l->rep);
}
