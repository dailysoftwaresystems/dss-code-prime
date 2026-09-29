// POSIX `environ` — how each object format realizes the name, and the
// properties that make each realization correct rather than merely link-clean.
//
// WHY THIS TEST EXISTS
//
// ★★ POSIX GIVES `environ` ONE SPELLING, AND IT IS THE PROGRAM'S OWN
// DECLARATION: XBD 8.1 says the environment is "pointed to by the external
// variable environ", and no POSIX header declares it (<unistd.h> declares
// optarg, opterr, optind and optopt, and nothing else). So the shape portable
// code is written against is
//
//     extern char **environ;
//
// in the program, and every reference compiler measured builds and runs it:
// gcc and clang on Linux (x86_64 and aarch64), mingw-w64 and MSVC on Windows,
// Apple clang on both Mac arches. DSS refused it on EVERY pair until P68
// round 11 (K_SymbolUndefined `environ`, `_environ` on Mach-O), because no
// descriptor realized the NAME: elf had a macro onto `__environ` that acts only
// in a TU that includes <unistd.h>, and pe and Mach-O had nothing.
//
// ★★ THE REALIZATION, PER FORMAT — exactly one each, and the kind differs:
//   elf    an `environ` DATA-OBJECT row (unistd.json). glibc exports `environ`,
//          `_environ` and `__environ` as ONE object at ONE address on both run
//          legs (0x20ad58 @GLIBC_2.2.5 x86_64 / 0x1b7288 @GLIBC_2.17 aarch64,
//          `nm -D` and `objdump -T` concurring). A program's own declaration
//          binds it through the GOT (`dataImportBinding: got-indirect`: the
//          exec DEFINES NOTHING, so no spelling can split the object).
//          `__environ` stays a row as well — glibc declares it unconditionally.
//   macho  the same row: libSystem exports `_environ` (Mach-O's spelling of
//          the C name); DSS binds it as a `__got` non-lazy pointer from
//          libSystem, which is where Apple's own link binds it.
//   pe     a MACRO (stdlib.json): `environ` -> `_environ` ->
//          `(*__p__environ())`. Neither Windows CRT exports a bare `environ`
//          data symbol (ucrtbase exports no spelling at all; msvcrt only
//          `_environ`), and BOTH Windows references' <stdlib.h> say exactly
//          this, so a program's `extern char **environ;` after <stdlib.h> is
//          rewritten into a compatible redeclaration of the UCRT accessor and
//          every use reads the CRT's own table — as under mingw-w64 and MSVC.
//
// ⓘ WHY THE OLD ELF MACRO WENT. It existed for the COPY-RELOCATION binding: a
// copy relocation redirects libc's references only for the name the exec
// claims, so an `environ` row once read NULL while glibc's startup wrote
// `__environ` (MEASURED then: `R_X86_64_COPY … environ + 0`, NULL on all four
// legs). Copy relocation is DELETED; under got-indirect ld.so resolves every
// spelling to the one object, so the macro guarded nothing any more — and as a
// macro it could not serve the POSIX spelling in a TU without <unistd.h>, while
// silently renaming every `environ` identifier of a TU with it.
// The RUNTIME object-identity property is witnessed where it can only be
// witnessed — across an image boundary, by
// `examples/c/environ_alias_object_identity`, which loads a gcc-built `.so`
// reading the un-prefixed name. A descriptor-shape test cannot see identity.
//
// So this file pins, each of which fails silently if edited:
//
//   (1) THE ROWS' SHAPE: `environ` and `__environ` are external DATA objects
//       typed `char **` (`ptr<ptr<char>>`; any other depth reshapes every
//       environ[i]); `environ` on EXACTLY {elf, macho}, `__environ` on EXACTLY
//       {elf}. Widening either to pe plants a data import no Windows CRT
//       exports: the loader refuses the binary (0xC0000139
//       STATUS_ENTRYPOINT_NOT_FOUND) with no link error and no diagnostic
//       naming the JSON, for every binary that references the name.
//
//   (2) THE pe MACRO CHAIN: `environ` -> `_environ` -> `(*__p__environ())`,
//       object-like, pe-gated, onto a pe-only accessor returning `char ***`.
//
//   (3) EXACTLY ONE REALIZATION PER FORMAT, ACROSS THE WHOLE SHIPPED TREE: a
//       symbol on elf and macho, a macro on pe. ZERO is the defect this file
//       now guards against — the POSIX spelling refused — and TWO is the
//       silent one: a macro and a symbol of one name on one format, where the
//       macro wins at every use and, because these are LVALUES, both reads
//       and writes are redirected with no diagnostic.
//
//   (4) NO `__environ` MACRO anywhere (it would shadow the data-object row),
//       and NO environment DATA symbol of any spelling available on pe.
//
//       ⚠ SCOPE, DELIBERATELY NARROW: these are properties of THESE NAMES, not
//       the general macro/symbol-overlap rule, which cannot be "same name +
//       overlapping format = error": tgmath.json legitimately does exactly
//       that for 17 names (C 7.25: the type-generic macro carries the SAME
//       NAME as the <math.h> function it dispatches to, and C 6.10.3.4p2 keeps
//       the inner name unexpanded).
//
// It reads the REAL shipped descriptor tree (never a hand-copied snapshot) and
// is FAIL-CLOSED throughout: a missing repo root, a missing descriptor, an
// unparseable descriptor, an empty symbol array or an empty sweep all FAIL
// rather than vacuously pass.
//
// RED-ON-DISABLE: `WidenedAvailabilityIsCaught`, `AGapOrAnOverlapIsCaught` and
// `ShadowingMacroIsCaught` reproduce each unsafe shape on SYNTHETIC
// descriptors, so every predicate stays pinned while the real tree is correct —
// the test_pe_crt_costate_binding.cpp `SplitCoStateGroupIsCaught` idiom.

#include "repo_root.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

[[nodiscard]] fs::path shippedLibsRoot() {
    auto const cfg = dss::test::findConfigRoot();
    if (!cfg) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    return *cfg / "shippedLibs";
}

// Load one descriptor, FAIL-CLOSED: the file must exist and parse.
[[nodiscard]] json loadDescriptor(fs::path const& p) {
    std::ifstream in(p);
    if (!in) {
        ADD_FAILURE() << "shipped descriptor unreadable: " << p.generic_string();
        return json::object();
    }
    json doc = json::object();
    try {
        in >> doc;
    } catch (std::exception const& e) {
        ADD_FAILURE() << "shipped descriptor " << p.generic_string()
                      << " does not parse: " << e.what()
                      << " -- a malformed descriptor breaks every #include of "
                         "that header";
        return json::object();
    }
    return doc;
}

// Every shipped descriptor, keyed by its path under shippedLibs/.
using DescriptorSet = std::vector<std::pair<std::string, json>>;

[[nodiscard]] DescriptorSet loadShippedTree(fs::path const& root) {
    DescriptorSet out;
    for (auto const& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file() || e.path().extension() != ".json") continue;
        out.emplace_back(fs::relative(e.path(), root).generic_string(),
                         loadDescriptor(e.path()));
    }
    std::sort(out.begin(), out.end(),
              [](auto const& a, auto const& b) { return a.first < b.first; });
    return out;
}

// The availability set a SYMBOL row resolves to, mirroring the reader's
// two-level fallback (row key -> document key -> available everywhere).
// An EMPTY result means "every format", exactly as the reader encodes it.
[[nodiscard]] std::vector<std::string>
resolvedAvailability(json const& doc, json const& row) {
    std::vector<std::string> out;
    json const* src = nullptr;
    if (row.contains("availableObjectFormats")) {
        src = &row.at("availableObjectFormats");
    } else if (doc.contains("availableObjectFormats")) {
        src = &doc.at("availableObjectFormats");
    }
    if (src != nullptr && src->is_array()) {
        for (auto const& f : *src) {
            if (f.is_string()) out.push_back(f.get<std::string>());
        }
    }
    return out;
}

[[nodiscard]] bool availableOn(std::vector<std::string> const& set,
                               std::string_view fmt) {
    if (set.empty()) return true;  // empty == every format
    for (auto const& f : set) {
        if (f == fmt) return true;
    }
    return false;
}

[[nodiscard]] json const* findNamed(json const& doc, char const* section,
                                    std::string_view name) {
    if (!doc.contains(section) || !doc.at(section).is_array()) return nullptr;
    for (auto const& e : doc.at(section)) {
        if (e.is_object() && e.contains("name") && e.at("name").is_string()
            && e.at("name").get<std::string>() == name) {
            return &e;
        }
    }
    return nullptr;
}

// THE GUARD PREDICATES, factored so the synthetic red-on-disable tests below
// exercise the SAME code the real-tree assertions run.

// (1) A data-object row's resolved availability is EXACTLY `want` — the
//     measured-safe set, neither wider (a format whose runtime has no such
//     export: a LOAD failure) nor narrower (a format that refuses the POSIX
//     spelling). "Every format" (no key at either tier) is never exact.
[[nodiscard]] bool availabilityIsExactly(json const& doc, json const& row,
                                         std::vector<std::string> want) {
    auto have = resolvedAvailability(doc, row);
    if (have.empty()) return false;
    std::sort(have.begin(), have.end());
    have.erase(std::unique(have.begin(), have.end()), have.end());
    std::sort(want.begin(), want.end());
    return have == want;
}

// Is macro entry `m` injected on `fmt`? A FLAT macro is injected on every
// format; a `variants` macro only where an arm's `when` selects the format
// (an arm with no `format` key applies to every format) — the reader's rule.
[[nodiscard]] bool macroInjectedOn(json const& m, std::string_view fmt) {
    if (!m.contains("variants")) return true;
    if (!m.at("variants").is_array()) return false;
    for (auto const& v : m.at("variants")) {
        if (!v.is_object() || !v.contains("when") || !v.at("when").is_object()) continue;
        json const& w = v.at("when");
        if (!w.contains("format")) return true;
        if (w.at("format").is_string() && w.at("format").get<std::string>() == fmt) return true;
    }
    return false;
}

// (3) Every realization of `name` a format sees across a descriptor set.
struct Realizations {
    std::vector<std::string> symbols;  // "<descriptor>" per available symbol row
    std::vector<std::string> macros;   // "<descriptor>" per injected macro entry
};

[[nodiscard]] Realizations realizationsOf(DescriptorSet const& docs,
                                          std::string_view name,
                                          std::string_view fmt) {
    Realizations r;
    for (auto const& [where, doc] : docs) {
        if (doc.contains("symbols") && doc.at("symbols").is_array()) {
            for (auto const& row : doc.at("symbols")) {
                if (!row.is_object() || !row.contains("name") || !row.at("name").is_string()
                    || row.at("name").get<std::string>() != name) {
                    continue;
                }
                if (availableOn(resolvedAvailability(doc, row), fmt)) r.symbols.push_back(where);
            }
        }
        if (doc.contains("macros") && doc.at("macros").is_array()) {
            for (auto const& m : doc.at("macros")) {
                if (!m.is_object() || !m.contains("name") || !m.at("name").is_string()
                    || m.at("name").get<std::string>() != name) {
                    continue;
                }
                if (macroInjectedOn(m, fmt)) r.macros.push_back(where);
            }
        }
    }
    return r;
}

enum class Via { Symbol, Macro };

// `name` is realized on `fmt` EXACTLY ONCE, and by the expected mechanism.
[[nodiscard]] bool realizedExactlyOnceVia(DescriptorSet const& docs,
                                          std::string_view name,
                                          std::string_view fmt, Via via) {
    Realizations const r = realizationsOf(docs, name, fmt);
    if (r.symbols.size() + r.macros.size() != 1) return false;
    return via == Via::Symbol ? r.symbols.size() == 1 : r.macros.size() == 1;
}

[[nodiscard]] std::string describe(Realizations const& r) {
    std::string s = "symbols [";
    for (auto const& w : r.symbols) s += " " + w;
    s += " ], macros [";
    for (auto const& w : r.macros) s += " " + w;
    return s + " ]";
}

// (4) A name is macro-realized in this descriptor.
[[nodiscard]] bool hasMacroNamed(json const& doc, std::string_view name) {
    return findNamed(doc, "macros", name) != nullptr;
}

// The shape every environment DATA row must have: an external object typed
// `char **`.
void expectEnvironmentDataRow(json const& row, std::string_view name) {
    ASSERT_TRUE(row.contains("kind")) << name;
    EXPECT_EQ(row.at("kind").get<std::string>(), "object")
        << "`" << name << "` is an extern DATA object; declared as a function it "
           "would bind a callable import thunk and every read would return code "
           "bytes";
    ASSERT_TRUE(row.contains("linkage")) << name;
    EXPECT_EQ(row.at("linkage").get<std::string>(), "external") << name;
    ASSERT_TRUE(row.contains("signature")) << name;
    EXPECT_EQ(row.at("signature").get<std::string>(), "ptr<ptr<char>>")
        << "`" << name << "` is `char **`; any other pointer depth silently "
           "reshapes every environ[i] access";
}

}  // namespace

// (1) The real rows: `environ` on elf and macho, `__environ` on elf.
TEST(EnvironDataObjectBinding, RealUnistdJsonEnvironObjectOnElfAndMacho) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config/shippedLibs";
    fs::path const path = root / "unistd.json";
    ASSERT_TRUE(fs::exists(path)) << "unistd.json missing: " << path.generic_string();

    json const doc = loadDescriptor(path);
    ASSERT_TRUE(doc.contains("symbols")) << "unistd.json declares no symbols";
    ASSERT_TRUE(doc.at("symbols").is_array());
    ASSERT_FALSE(doc.at("symbols").empty())
        << "fail-closed: an empty symbols array would make every assertion "
           "below vacuous";

    // (Never a C++ variable named `environ`: mingw-w64's and the UCRT's <stdlib.h>
    // define `environ` as a MACRO onto the accessor — the very fact this file pins.)
    json const* envRow = findNamed(doc, "symbols", "environ");
    ASSERT_NE(envRow, nullptr)
        << "unistd.json must declare `environ` as a DATA object: it is the name "
           "POSIX gives the environment (XBD 8.1) and the one a program declares "
           "itself; glibc exports it (with `_environ` and `__environ`, one object) "
           "and libSystem exports `_environ`";
    expectEnvironmentDataRow(*envRow, "environ");
    EXPECT_TRUE(availabilityIsExactly(doc, *envRow, {"elf", "macho"}))
        << "`environ` must be available on EXACTLY elf and macho: pe has no such "
           "export (its realization is the <stdlib.h> macro onto the UCRT "
           "accessor), and dropping either of the two refuses the POSIX spelling "
           "there again";

    json const* dunder = findNamed(doc, "symbols", "__environ");
    ASSERT_NE(dunder, nullptr)
        << "unistd.json must keep `__environ` — glibc's UNCONDITIONALLY declared "
           "spelling of the same object";
    expectEnvironmentDataRow(*dunder, "__environ");
    EXPECT_TRUE(availabilityIsExactly(doc, *dunder, {"elf"}))
        << "`__environ` is glibc's name: elf ONLY";

    EXPECT_FALSE(hasMacroNamed(doc, "environ"))
        << "unistd.json must not realize `environ` as a MACRO beside the data "
           "row: the macro would win at every use in a TU that includes "
           "<unistd.h> and silently rename the program's own `environ`";

    // Positive control: an ungated symbol row really does read as "every
    // format", so the assertions above are discriminating rather than trivially
    // true. It names `stdio.json`'s `puts`, a row ungated for a REASON rather
    // than by omission (C's standard I/O exists on every format DSS emits), in a
    // document that declares no `availableObjectFormats` either — a row is only
    // ungated in effect if BOTH tiers leave it open.
    {
        fs::path const controlPath = root / "stdio.json";
        ASSERT_TRUE(fs::exists(controlPath))
            << "positive control: stdio.json missing: "
            << controlPath.generic_string();
        json const  controlDoc = loadDescriptor(controlPath);
        json const* control    = findNamed(controlDoc, "symbols", "puts");
        ASSERT_NE(control, nullptr)
            << "positive control: stdio.json must declare `puts`";
        EXPECT_FALSE(control->contains("availableObjectFormats"))
            << "positive control: `puts` ships with no per-symbol availability "
               "set — if this ever gains one, the control is no longer a control "
               "and must MOVE, never be relaxed";
        EXPECT_FALSE(controlDoc.contains("availableObjectFormats"))
            << "positive control: stdio.json declares no document-level "
               "availability, so `puts` is ungated at BOTH tiers";
        EXPECT_TRUE(availableOn(resolvedAvailability(controlDoc, *control), "pe"));
        EXPECT_FALSE(availabilityIsExactly(controlDoc, *control, {"elf", "macho"}))
            << "positive control: 'every format' is never EXACTLY {elf, macho}";
    }
}

// (2) The real pe chain, in the header both Windows references put it in.
TEST(EnvironDataObjectBinding, RealStdlibJsonEnvironMacroOntoTheUcrtAccessor) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config/shippedLibs";
    fs::path const path = root / "stdlib.json";
    ASSERT_TRUE(fs::exists(path)) << "stdlib.json missing: " << path.generic_string();
    json const doc = loadDescriptor(path);

    // `environ` -> `_environ`: object-like, exactly one arm, pe.
    json const* macro = findNamed(doc, "macros", "environ");
    ASSERT_NE(macro, nullptr)
        << "stdlib.json must realize `environ` on pe as a MACRO onto `_environ` "
           "— both Windows references' <stdlib.h> do exactly this, and it is what "
           "makes a program's own `extern char **environ;` a redeclaration of the "
           "accessor rather than an import no Windows CRT exports";
    ASSERT_TRUE(macro->contains("variants"))
        << "per-format `variants`, never a flat body: a flat macro is injected on "
           "EVERY format and would rename elf/macho's data object away";
    ASSERT_TRUE(macro->at("variants").is_array());
    ASSERT_EQ(macro->at("variants").size(), 1u) << "exactly one arm: pe";
    json const& arm = macro->at("variants").at(0);
    ASSERT_TRUE(arm.contains("when") && arm.at("when").contains("format"));
    EXPECT_EQ(arm.at("when").at("format").get<std::string>(), "pe");
    ASSERT_TRUE(arm.contains("replacement"));
    EXPECT_EQ(arm.at("replacement").get<std::string>(), "_environ")
        << "onto `_environ`, the references' own spelling, so the two names stay "
           "one chain with one accessor";
    EXPECT_FALSE(arm.contains("params")) << "object-like: `environ` is an LVALUE";
    ASSERT_TRUE(macro->contains("availableObjectFormats"));
    EXPECT_EQ(macro->at("availableObjectFormats"), json::array({"pe"}));

    // `_environ` -> `(*__p__environ())`.
    json const* under = findNamed(doc, "macros", "_environ");
    ASSERT_NE(under, nullptr) << "stdlib.json must realize `_environ` on pe";
    ASSERT_TRUE(under->contains("variants") && under->at("variants").is_array());
    ASSERT_EQ(under->at("variants").size(), 1u);
    json const& uarm = under->at("variants").at(0);
    EXPECT_EQ(uarm.at("when").at("format").get<std::string>(), "pe");
    EXPECT_EQ(uarm.at("replacement").get<std::string>(), "(*__p__environ())")
        << "the UCRT exposes the table ONLY through the accessor";
    EXPECT_FALSE(uarm.contains("params"));

    // The accessor: pe-only, `char ***(void)`.
    json const* acc = findNamed(doc, "symbols", "__p__environ");
    ASSERT_NE(acc, nullptr) << "stdlib.json must declare the UCRT accessor";
    EXPECT_EQ(acc->at("kind").get<std::string>(), "function");
    EXPECT_EQ(acc->at("signature").get<std::string>(), "fn() -> ptr<ptr<ptr<char>>>")
        << "`char ***`, so `(*__p__environ())` has `environ`'s own type";
    EXPECT_TRUE(availabilityIsExactly(doc, *acc, {"pe"}))
        << "glibc and libSystem have no `__p__environ`";
}

// (3) Exactly one realization per format, over the WHOLE shipped tree.
TEST(EnvironDataObjectBinding, EveryFormatRealizesEnvironExactlyOnce) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config/shippedLibs";
    DescriptorSet const docs = loadShippedTree(root);
    ASSERT_GT(docs.size(), 10u)
        << "only " << docs.size() << " descriptors found -- the sweep found "
           "almost nothing, so its silence means nothing";

    struct Want { char const* fmt; Via via; };
    for (Want const w : {Want{"elf", Via::Symbol}, Want{"macho", Via::Symbol},
                         Want{"pe", Via::Macro}}) {
        Realizations const r = realizationsOf(docs, "environ", w.fmt);
        EXPECT_TRUE(realizedExactlyOnceVia(docs, "environ", w.fmt, w.via))
            << "`environ` on " << w.fmt << " must be realized exactly once, by a "
            << (w.via == Via::Symbol ? "DATA-OBJECT row" : "MACRO")
            << "; found " << describe(r)
            << ". Zero refuses the POSIX spelling (a program's own `extern char "
               "**environ;`); two lets a macro silently rename the object at "
               "every use, reads and writes alike.";
    }
}

// (4) Neither shadowing direction, and no environment data symbol on pe.
TEST(EnvironDataObjectBinding, NoDescriptorShadowsTheEnvironBinding) {
    fs::path const root = shippedLibsRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config/shippedLibs";
    DescriptorSet const docs = loadShippedTree(root);
    ASSERT_GT(docs.size(), 10u)
        << "only " << docs.size() << " descriptors scanned -- an empty sweep "
           "must not read as a pass";

    for (auto const& [where, doc] : docs) {
        // A `__environ` MACRO would shadow the data-object row at every use:
        // compile rc=0, no diagnostic — and because this is an LVALUE, both
        // reads and writes are silently redirected.
        EXPECT_FALSE(hasMacroNamed(doc, "__environ"))
            << where << " declares a `macros` entry named `__environ`, which "
                        "would SHADOW the environment data-object row silently";
    }
    // No Windows CRT exports a bare environment DATA symbol DSS can bind:
    // ucrtbase none of the three spellings, msvcrt only `_environ`, and pe binds
    // ucrtbase. A pe-available row of any spelling is a LOAD failure waiting for
    // its first referencing binary.
    for (char const* name : {"environ", "_environ", "__environ"}) {
        Realizations const r = realizationsOf(docs, name, "pe");
        EXPECT_TRUE(r.symbols.empty())
            << "`" << name << "` has a SYMBOL row available on pe: " << describe(r);
    }
}

// ── RED-ON-DISABLE: each unsafe shape, on synthetic descriptors ─────────────
//
// These keep the predicates pinned while the real tree is correct. Each builds
// the exact shape the real assertions forbid and asserts the predicate REJECTS
// it — so weakening a predicate to wave the real tree through reds here.

TEST(EnvironDataObjectBinding, WidenedAvailabilityIsCaught) {
    // The one-token edit: pe joins the set. No Windows CRT exports it.
    json const doc = json::parse(R"JSON({
      "header": "unistd.h",
      "availableObjectFormats": ["elf", "macho", "pe"],
      "symbols": [
        { "name": "environ", "signature": "ptr<ptr<char>>", "kind": "object",
          "linkage": "external", "availableObjectFormats": ["elf", "macho", "pe"] }
      ]
    })JSON");
    json const* row = findNamed(doc, "symbols", "environ");
    ASSERT_NE(row, nullptr);
    EXPECT_TRUE(availableOn(resolvedAvailability(doc, *row), "pe"))
        << "the synthetic really does declare pe (the shape under test)";
    EXPECT_FALSE(availabilityIsExactly(doc, *row, {"elf", "macho"}))
        << "the guard must REJECT a pe-widened environment row";

    // The DOCUMENT-level fallback is the same hole through another door: no row
    // key at all inherits [elf, macho, pe].
    json const inherit = json::parse(R"JSON({
      "header": "unistd.h",
      "availableObjectFormats": ["elf", "macho", "pe"],
      "symbols": [
        { "name": "environ", "signature": "ptr<ptr<char>>", "kind": "object",
          "linkage": "external" }
      ]
    })JSON");
    json const* inheritRow = findNamed(inherit, "symbols", "environ");
    ASSERT_NE(inheritRow, nullptr);
    EXPECT_FALSE(availabilityIsExactly(inherit, *inheritRow, {"elf", "macho"}))
        << "a row with no availability key INHERITS the document's set";

    // An entirely absent availability set means EVERY format.
    json const wide = json::parse(R"JSON({
      "header": "unistd.h",
      "symbols": [
        { "name": "environ", "signature": "ptr<ptr<char>>", "kind": "object",
          "linkage": "external" }
      ]
    })JSON");
    json const* wideRow = findNamed(wide, "symbols", "environ");
    ASSERT_NE(wideRow, nullptr);
    EXPECT_TRUE(resolvedAvailability(wide, *wideRow).empty())
        << "no key anywhere == every format (the reader's encoding)";
    EXPECT_FALSE(availabilityIsExactly(wide, *wideRow, {"elf", "macho"}))
        << "the guard must REJECT an ungated environment row";

    // NARROWER is caught too: macho dropped refuses the POSIX spelling there.
    json const narrow = json::parse(R"JSON({
      "header": "unistd.h",
      "symbols": [
        { "name": "environ", "signature": "ptr<ptr<char>>", "kind": "object",
          "linkage": "external", "availableObjectFormats": ["elf"] }
      ]
    })JSON");
    json const* narrowRow = findNamed(narrow, "symbols", "environ");
    ASSERT_NE(narrowRow, nullptr);
    EXPECT_FALSE(availabilityIsExactly(narrow, *narrowRow, {"elf", "macho"}))
        << "the guard must REJECT a row that no longer reaches macho";

    // Positive control: the real shape is ACCEPTED, so the predicate is not
    // simply always-false.
    json const good = json::parse(R"JSON({
      "header": "unistd.h",
      "availableObjectFormats": ["elf", "macho", "pe"],
      "symbols": [
        { "name": "environ", "signature": "ptr<ptr<char>>", "kind": "object",
          "linkage": "external", "availableObjectFormats": ["macho", "elf"] }
      ]
    })JSON");
    json const* goodRow = findNamed(good, "symbols", "environ");
    ASSERT_NE(goodRow, nullptr);
    EXPECT_TRUE(availabilityIsExactly(good, *goodRow, {"elf", "macho"}))
        << "positive control: exactly {elf, macho}, in either order, is accepted";
}

TEST(EnvironDataObjectBinding, AGapOrAnOverlapIsCaught) {
    json const unistdGood = json::parse(R"JSON({
      "header": "unistd.h",
      "symbols": [
        { "name": "environ", "signature": "ptr<ptr<char>>", "kind": "object",
          "linkage": "external", "availableObjectFormats": ["elf", "macho"] }
      ]
    })JSON");
    json const stdlibGood = json::parse(R"JSON({
      "header": "stdlib.h",
      "macros": [
        { "name": "environ", "availableObjectFormats": ["pe"], "variants": [
          { "when": { "format": "pe" }, "replacement": "_environ" } ] }
      ],
      "symbols": []
    })JSON");

    // Positive control: the real shape passes on all three formats.
    DescriptorSet const good{{"stdlib.json", stdlibGood}, {"unistd.json", unistdGood}};
    EXPECT_TRUE(realizedExactlyOnceVia(good, "environ", "elf", Via::Symbol));
    EXPECT_TRUE(realizedExactlyOnceVia(good, "environ", "macho", Via::Symbol));
    EXPECT_TRUE(realizedExactlyOnceVia(good, "environ", "pe", Via::Macro));

    // GAP: the pe macro gone — the POSIX spelling refused on pe again.
    DescriptorSet const noPe{{"unistd.json", unistdGood}};
    EXPECT_FALSE(realizedExactlyOnceVia(noPe, "environ", "pe", Via::Macro))
        << "the guard must REJECT a format with NO realization";

    // OVERLAP: the old elf macro back beside the row — two realizations on elf,
    // the macro silently winning at every use.
    json unistdBoth = unistdGood;
    unistdBoth["macros"] = json::parse(R"JSON([
        { "name": "environ", "availableObjectFormats": ["elf"], "variants": [
          { "when": { "format": "elf" }, "replacement": "__environ" } ] } ])JSON");
    DescriptorSet const both{{"stdlib.json", stdlibGood}, {"unistd.json", unistdBoth}};
    EXPECT_FALSE(realizedExactlyOnceVia(both, "environ", "elf", Via::Symbol))
        << "the guard must REJECT a macro and a symbol of one name on one format";
    EXPECT_TRUE(realizedExactlyOnceVia(both, "environ", "macho", Via::Symbol))
        << "and must not smear the overlap onto a format it does not touch";

    // A FLAT macro is injected everywhere: it overlaps elf and macho at once.
    json stdlibFlat = json::parse(R"JSON({
      "header": "stdlib.h",
      "macros": [ { "name": "environ", "replacement": "_environ" } ],
      "symbols": []
    })JSON");
    DescriptorSet const flat{{"stdlib.json", stdlibFlat}, {"unistd.json", unistdGood}};
    EXPECT_FALSE(realizedExactlyOnceVia(flat, "environ", "elf", Via::Symbol));
    EXPECT_FALSE(realizedExactlyOnceVia(flat, "environ", "macho", Via::Symbol));

    // WRONG KIND: pe realized by a data row instead of the macro — the import
    // no Windows CRT exports.
    json const unistdPe = json::parse(R"JSON({
      "header": "unistd.h",
      "symbols": [
        { "name": "environ", "signature": "ptr<ptr<char>>", "kind": "object",
          "linkage": "external", "availableObjectFormats": ["elf", "macho", "pe"] }
      ]
    })JSON");
    DescriptorSet const peRow{{"unistd.json", unistdPe}};
    EXPECT_FALSE(realizedExactlyOnceVia(peRow, "environ", "pe", Via::Macro))
        << "the guard must REJECT a pe realization by the wrong mechanism";
}

TEST(EnvironDataObjectBinding, ShadowingMacroIsCaught) {
    json const doc = json::parse(R"JSON({
      "header": "unistd.h",
      "availableObjectFormats": ["elf", "macho"],
      "macros": [
        { "name": "__environ", "variants": [
          { "when": { "format": "elf" }, "replacement": "(*envAccessor())" }
        ] }
      ],
      "symbols": [
        { "name": "__environ", "signature": "ptr<ptr<char>>", "kind": "object",
          "linkage": "external", "availableObjectFormats": ["elf"] }
      ]
    })JSON");
    EXPECT_TRUE(hasMacroNamed(doc, "__environ"))
        << "the guard must DETECT a macro shadowing the data-object row";

    // And it must not fire on an unrelated accessor macro (no false positive).
    json const benign = json::parse(R"JSON({
      "header": "stdio.h",
      "macros": [ { "name": "stdin", "variants": [
        { "when": { "format": "pe" }, "replacement": "(__acrt_iob_func(0))" }
      ] } ],
      "symbols": []
    })JSON");
    EXPECT_FALSE(hasMacroNamed(benign, "__environ"))
        << "the guard must not fire on an unrelated accessor macro";
}
