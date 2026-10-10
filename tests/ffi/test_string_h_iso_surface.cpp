// <string.h>'s C23 7.26 surface, pinned PER PAIR (P69, lane `cs`).
//
// ═══ WHY THIS FILE EXISTS ═════════════════════════════════════════════════════
//
// C23 7.26 names 26 functions, and how each one is BOUND is a per-pair fact that DSS's
// <string.h> states in one document, `src/dss-config/shippedLibs/string.json`:
//
//   * 22 of them every C library exports under their own name;
//   * `strdup` and `memccpy` the pe C library exports ONLY under a leading underscore
//     (✔MEASURED 2026-10-07, `GetProcAddress` on ucrtbase.dll: `strdup` 0, `_strdup` 1;
//     `memccpy` 0, `_memccpy` 1), so on pe each row carries a `linkName` and on ELF and
//     Mach-O it carries none;
//   * `strndup` the pe C library does not export at all, so on pe the row is REALIZED from
//     DSS's own `runtime/platform/src/strndup.c`, while ELF and Mach-O import their library's;
//   * `memset_explicit` no shipped C library exports, so the row is realized from
//     `runtime/platform/src/memset_explicit.c` on EVERY format.
//
// Until this file the only thing that noticed one of those rows going wrong was a program
// that had to RUN on the pair in question (`examples/c/shipped_iso_string_and_ctype_functions`):
// a pe fact was invisible on every leg that cannot start a pe image. This is the same table
// read through the real reader with each pair's own facts, so it is checked on every host.
//
// WHAT IT DOES NOT SEE: whether the image a compile writes follows the table (the injection
// path), and what a realized body DOES when it runs. Both are the example's.
//
// RED-ON-DISABLE: rename or drop any of the 26 rows and the first case names the function on
// every pair; give `strdup` or `memccpy` its plain name on pe and the second case names the
// pair; drop a realization (or add one where the library has the function) and the third does.

#include "analysis/compilation_unit/compilation_unit.hpp"   // predefinedTypeFactsFor — the pair's own facts
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "link/object_format_schema.hpp"
#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::ffi;
namespace fs = std::filesystem;

namespace {

// One executable pair: the target schema's name and the object-format document.
struct Pair {
    char const* arch;
    char const* formatDoc;
};

constexpr Pair kPairs[] = {
    {"x86_64", "pe64-x86_64-windows-exec"},
    {"x86_64", "elf64-x86_64-linux-exec"},
    {"arm64", "elf64-aarch64-linux-exec"},
    {"arm64", "macho64-arm64-darwin-exec"},
    {"x86_64", "macho64-x86_64-darwin-exec"},
};

// C23 7.26, every function it names, in the order of its subclauses.
constexpr char const* kC23Functions[] = {
    "memcpy", "memccpy", "memmove", "strcpy", "strncpy", "strdup", "strndup",   // 7.26.2
    "strcat", "strncat",                                                        // 7.26.3
    "memcmp", "strcmp", "strcoll", "strncmp", "strxfrm",                        // 7.26.4
    "memchr", "strchr", "strcspn", "strpbrk", "strrchr", "strspn", "strstr",    // 7.26.5
    "strtok",
    "memset", "memset_explicit", "strerror", "strlen",                          // 7.26.6
};

[[nodiscard]] std::shared_ptr<GrammarSchema const> const& cLanguage() {
    static std::shared_ptr<GrammarSchema const> const schema =
        []() -> std::shared_ptr<GrammarSchema const> {
        auto loaded = GrammarSchema::loadShipped("c");
        if (!loaded.has_value()) {
            ADD_FAILURE() << "the shipped c language does not load";
            return std::shared_ptr<GrammarSchema const>{};
        }
        return *loaded;
    }();
    return schema;
}

// <string.h> as ONE pair reads it: the descriptor through the real reader with the pair's own
// facts, and the pair's format name (the key of every per-format map in the document).
struct StringH {
    TypeInterner                        interner{CompilationUnitId{1}};
    TypeRegistry                        typeReg;
    std::optional<ShippedLibDescriptor> desc;
    ObjectFormatKind                    kind{};
    std::string                         fmt;
    fs::path                            configRoot;

    // The ONE row a name has on this pair, or null. Two rows for one name on one pair is a
    // failure of its own: the reader would bind whichever it met first.
    [[nodiscard]] ShippedSymbol const* row(std::string_view name) const {
        ShippedSymbol const* found = nullptr;
        for (auto const& s : desc->symbols) {
            if (s.name != name) continue;
            if (!s.availableObjectFormats.empty()
                && std::find(s.availableObjectFormats.begin(), s.availableObjectFormats.end(), fmt)
                       == s.availableObjectFormats.end())
                continue;
            EXPECT_EQ(found, nullptr) << name << " has two rows on " << fmt;
            found = &s;
        }
        return found;
    }

    // The unit a row is realized from on this pair, or "" for an import.
    [[nodiscard]] std::string unitOf(ShippedSymbol const& s) const {
        auto const r = s.realization.find(fmt);
        return r == s.realization.end() ? std::string{} : r->second;
    }
};

void readStringH(Pair const& p, StringH& out) {
    ASSERT_NE(cLanguage(), nullptr);
    auto const cfg = dss::test::findConfigRoot();
    ASSERT_TRUE(cfg.has_value()) << dss::test::configRootDiagnostic();
    out.configRoot = *cfg;
    auto targetR = TargetSchema::loadShipped(p.arch);
    ASSERT_TRUE(targetR.has_value()) << p.arch;
    auto formatR = ObjectFormatSchema::loadShipped(p.formatDoc);
    ASSERT_TRUE(formatR.has_value()) << p.formatDoc;
    out.kind = (*formatR)->kind();
    out.fmt  = std::string{objectFormatKindName(out.kind)};
    PredefinedTypeFacts const typeFacts = predefinedTypeFactsFor(**targetR, **formatR);
    ShippedPairFacts const facts{cLanguage().get(), typeFacts.dataModel, typeFacts.charIsUnsigned,
                                 typeFacts.abiTypedefs, typeFacts.longDoubleFormat};
    DiagnosticReporter rep;
    out.desc = readShippedLibDescriptor(*cfg / "shippedLibs" / "string.json", out.interner, out.typeReg,
                                        rep, (*formatR)->dataModel(), std::string_view{p.arch},
                                        out.kind, {}, nullptr, &facts);
    ASSERT_TRUE(out.desc.has_value());
    ASSERT_FALSE(rep.hasErrors()) << (rep.all().empty() ? std::string{} : rep.all().front().actual);
}

}  // namespace

// ── Every function C23 7.26 names is declared on every executable pair ───────────
TEST(StringHIsoSurface, EveryC23FunctionIsDeclaredOnEveryExecutablePair) {
    for (Pair const& p : kPairs) {
        SCOPED_TRACE(p.formatDoc);
        StringH h;
        ASSERT_NO_FATAL_FAILURE(readStringH(p, h));
        for (char const* fn : kC23Functions) {
            ShippedSymbol const* s = h.row(fn);
            ASSERT_NE(s, nullptr) << "<string.h> declares no " << fn << " on " << h.fmt;
            EXPECT_EQ(h.interner.kind(s->signature), TypeKind::FnSig) << fn << " is not a function";
        }
    }
}

// ── The two names the pe C library exports only under a leading underscore ───────
//
// A row with no link name binds the C identifier itself. ucrtbase.dll exports neither
// `strdup` nor `memccpy`, so a pe image that imports the plain name is one the LOADER
// refuses; ELF and Mach-O export the plain name and must carry no link name at all.
TEST(StringHIsoSurface, StrdupAndMemccpyBindEachPairsOwnExport) {
    for (Pair const& p : kPairs) {
        SCOPED_TRACE(p.formatDoc);
        StringH h;
        ASSERT_NO_FATAL_FAILURE(readStringH(p, h));
        bool const pe = h.kind == ObjectFormatKind::Pe;
        for (char const* fn : {"strdup", "memccpy"}) {
            ShippedSymbol const* s = h.row(fn);
            ASSERT_NE(s, nullptr) << fn;
            EXPECT_EQ(s->linkName, pe ? std::string{"_"} + fn : std::string{})
                << fn << " on " << h.fmt << (pe ? ": the pe C library exports it only as _" : ": the library exports ")
                << fn;
            EXPECT_EQ(h.unitOf(*s), "") << fn << " is the library's own on every pair";
        }
        // Every OTHER imported name is bound under the identifier itself: a link name that
        // strayed onto a neighbouring row would import a name no library exports.
        for (char const* fn : kC23Functions) {
            std::string_view const name{fn};
            if (name == "strdup" || name == "memccpy") continue;
            ShippedSymbol const* s = h.row(fn);
            ASSERT_NE(s, nullptr) << fn;
            EXPECT_TRUE(s->linkName.empty()) << fn << " binds `" << s->linkName << "` on " << h.fmt;
        }
    }
}

// ── The two names DSS provides a body for, and where ─────────────────────────────
//
// `strndup`: the pe C library has none, so DSS's unit on pe and the library's own elsewhere
// (a realization on ELF or Mach-O would put a second definition beside the library's).
// `memset_explicit`: no shipped C library has one, so DSS's unit on every pair. No other
// name of the header is realized: each of those is the library's own.
TEST(StringHIsoSurface, StrndupAndMemsetExplicitAreRealizedWhereNoLibraryHasThem) {
    for (Pair const& p : kPairs) {
        SCOPED_TRACE(p.formatDoc);
        StringH h;
        ASSERT_NO_FATAL_FAILURE(readStringH(p, h));
        bool const pe = h.kind == ObjectFormatKind::Pe;
        for (char const* fn : kC23Functions) {
            std::string_view const name{fn};
            ShippedSymbol const* s = h.row(fn);
            ASSERT_NE(s, nullptr) << fn;
            std::string want;
            if (name == "strndup" && pe) want = "runtime/platform/src/strndup.c";
            if (name == "memset_explicit") want = "runtime/platform/src/memset_explicit.c";
            EXPECT_EQ(h.unitOf(*s), want) << fn << " on " << h.fmt;
            if (!want.empty())
                EXPECT_TRUE(fs::is_regular_file(h.configRoot / want)) << want << " is not there";
        }
    }
}
