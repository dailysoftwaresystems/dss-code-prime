// D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF — THE SHIPPED-SOURCE REALIZATION AXIS.
//
// WHY THIS TEST EXISTS
//
// DSS shipped the DECLARATION half of a toolchain (the `shippedLibs/*.json` FFI
// descriptors) and not the IMPLEMENTATION half, so a platform gap had no home.
// `opendir`/`readdir`/`closedir` on Windows is the canonical instance: no image
// exports them, because Windows has no POSIX directory API. The operator's
// ruling was that DSS ships the SOURCE — the same split every production
// toolchain makes, where the compiler synthesizes only stateless glue and a
// runtime library of compiled source provides everything with state, allocation
// or nontrivial control flow (libgcc / compiler-rt / libmingwex / newlib).
//
// A descriptor's per-format `realization` map is that mechanism: `library` says
// which IMAGE a symbol is imported FROM; `realization` says whether it is
// imported AT ALL, or provided by a file the compiler ships and compiles for the
// target.
//
// ★★ WHAT THIS FILE GUARDS THAT NOTHING ELSE CAN. The end-to-end proof lives in
// `examples/c/shipped_dirent_readdir` — it compiles and RUNS the pe64 arm,
// and it is the strongest evidence the mechanism works. But an example can only
// witness the configuration that EXISTS. The refusals here are about
// configurations that must never be accepted, and three of them are invisible to
// any example:
//
//   R1  a realization naming a source that is not there. Enforced at DESCRIPTOR
//       READ TIME, where it is one `is_regular_file` per declared entry. This is
//       the refusal that can otherwise produce a build silently missing a body.
//   R2  a source file NO descriptor names — inert config that nothing can ever
//       add to a build graph. ⚠ GATE-TEST ONLY, and that placement is a
//       deliberate departure from the ruling's "LOAD ERROR" wording rather than
//       verbatim compliance: without a unit manifest the check costs a directory
//       walk PLUS a corpus scan on EVERY compile, and an inert `.c` can only
//       waste disk while R1's failure can produce a wrong binary. Severity
//       matched to the failure.
//   R4  no HEADERS in the runtime tree. FOLDED INTO R2 rather than given its own
//       extension check, which is strictly stronger: a header is never a
//       translation unit, so no `realization` can name one, so the
//       unclaimed-file rule refuses it by construction — with no extension
//       vocabulary to enumerate or keep current. The hazard is real and silent:
//       the tree is a mirror of the include namespace, so a private header would
//       sit at an include path and shadow the descriptor a unit exists to
//       consume.
//   R3  one format carrying BOTH a `library` image and a `source`. Two owners
//       for one body. Silently preferring either is how a program links against
//       an image that does not export the symbol and dies at LOAD with rc=0 from
//       every compile stage — the exact D-FFI-DESCRIPTOR-EAGER-IMPORT class.
//
// ★ EVERY REFUSAL IS CHECKED FORMAT-INDEPENDENTLY. An arm no current target
// selects must not rot, so the sweeps read every declared format key rather than
// one active one — the bidirectional half of the bar.
//
// WHAT THIS FILE DELIBERATELY DOES NOT DO: it does not pin WHICH descriptor
// declares a realization, or how many do. Pinning "dirent.json, exactly one"
// would make the test a restatement of the config it guards, failing for the
// wrong reason the day a second unit lands and teaching maintainers to update it
// reflexively. The invariants are the four refusals plus the path/claim
// correspondence, and those hold at zero units, at one, and at fifty.

#include "ffi/shipped_lib_descriptor.hpp"

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"

#include "repo_root.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace {

// The real trees these invariants are measured over, resolved through the ONE
// test-side resolver ($DSS_CONFIG_ROOT → the CMake-baked repo root → the cwd
// ancestor walk). A private cwd-walk here would find nothing in an out-of-tree
// build, and an invariant with no tree to read is a hole, not a pass.
[[nodiscard]] fs::path configRoot() {
    auto const cfg = dss::test::findConfigRoot();
    if (!cfg) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    return *cfg;
}

[[nodiscard]] fs::path descriptorDir() { return configRoot() / "shippedLibs"; }
[[nodiscard]] fs::path runtimeDir()    { return configRoot() / "runtime"; }

// One realization claim, flattened out of the descriptor corpus: which format,
// which config-root-relative source, and which row said so.
struct Claim {
    std::string descriptor;
    std::string context;
    std::string format;
    std::string source;
};

// ★ EVERY LOOP OVER DESCRIPTORS RUNS ITS BODY THROUGH A `void` CALLABLE. A
// gtest ASSERT_* returns from the enclosing function, so an ASSERT inside a
// raw loop cancels every remaining iteration — the first bad descriptor would
// hide all the others, and the run would report one failure where there are
// five. Collecting first and asserting after (or asserting inside a void
// lambda) keeps every arm independent.
void forEachDescriptor(std::function<void(fs::path const&, json const&)> const& fn) {
    auto const dir = descriptorDir();
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        ADD_FAILURE() << "shippedLibs directory not found at " << dir;
        return;
    }
    for (fs::recursive_directory_iterator it{dir, ec}, end; it != end;
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        if (it->path().extension() != ".json") continue;
        std::ifstream in{it->path()};
        json doc = json::parse(in, nullptr, false);
        if (doc.is_discarded()) {
            ADD_FAILURE() << it->path() << ": not valid JSON";
            continue;
        }
        fn(it->path(), doc);
    }
}

[[nodiscard]] std::vector<Claim> allClaims() {
    std::vector<Claim> claims;
    forEachDescriptor([&](fs::path const& p, json const& doc) {
        auto harvest = [&](json const& node, std::string const& ctx) {
            if (!node.is_object()) return;
            for (auto const& kv : node.items()) {
                if (!kv.value().is_object()) continue;
                if (!kv.value().contains("source")) continue;
                if (!kv.value().at("source").is_string()) continue;
                claims.push_back(Claim{p.filename().generic_string(), ctx,
                                       kv.key(),
                                       kv.value().at("source").get<std::string>()});
            }
        };
        if (doc.contains("realization")) harvest(doc.at("realization"), "(root)");
        if (doc.contains("symbols") && doc.at("symbols").is_array()) {
            std::size_t i = 0;
            for (auto const& sym : doc.at("symbols")) {
                std::string const ctx = "symbols[" + std::to_string(i++) + "]";
                if (sym.is_object() && sym.contains("realization"))
                    harvest(sym.at("realization"), ctx);
            }
        }
    });
    return claims;
}

// The per-format image map and the per-format source map of ONE owner node, in
// the same shape so R3 can compare them.
[[nodiscard]] std::vector<std::string> formatKeysOf(json const& owner,
                                                    char const* key) {
    std::vector<std::string> out;
    if (!owner.is_object()) return out;
    if (!owner.contains(key) || !owner.at(key).is_object()) return out;
    for (auto const& kv : owner.at(key).items()) out.push_back(kv.key());
    return out;
}

}  // namespace

// ── THE SECOND KIND OF DECLARER ──────────────────────────────────────────────
//
// D-C-ATOMICS-RUNTIME-IS-OURS-ON-PE64: an object format's
// `runtimeLibraries[].source` names a file in this SAME tree, for a runtime role
// whose entry points the COMPILER MINTS — so there is no descriptor row to hang
// the realization off, and R1/R2 must see the format documents or the sweep
// refuses a file the config legitimately names.
//
// ⚠ THIS IS A RAW READ OF A KEY THE `link` TIER OWNS, and that is a real hazard
// rather than a shortcut: a second reader of a config key is how a sweep and a
// loader come to disagree. It is pinned shut in
// `tests/link/test_runtime_library_roles.cpp`, which asserts that every `source`
// this raw read finds is exactly what `ObjectFormatSchema::runtimeLibraries()
// .realizedSources()` reports for the same flavour. `tests/ffi` cannot load a
// format document without depending on the object-format schema, which is the
// layering inversion the split avoids.
//
// ★ EACH DECLARATION CARRIES ITS OWN LOCATOR (P54 lane `ar`). A bare list of
// paths made every refusal say `an object format's runtimeLibraries[].source`
// and name neither the document nor the role, so a broken path in ONE of the
// four pe64 flavours reported identically to a broken path in any other — and
// the reader had 26 documents to search. The document name and the role are
// known exactly here and nowhere downstream, so they travel with the claim.
//
// ⚠ THE SAME SOURCE NAMED BY SEVERAL DOCUMENTS IS **NOT** DEDUPLICATED, and the
// change is deliberate: `runtime/platform/src/atomic.c` is named by all FOUR
// pe64 flavours (✔MEASURED), and folding them to one entry would have made a
// refusal name one arbitrary document while the other three went unmentioned.
// R1 resolving the same path four times is four `is_regular_file` calls.
[[nodiscard]] std::vector<dss::ffi::ShippedSourceDeclaration>
formatRealizedSources() {
    std::vector<dss::ffi::ShippedSourceDeclaration> out;
    auto const root = configRoot();
    if (root.empty()) return out;
    std::error_code ec;
    fs::path const  dir = root / "object-formats";
    if (!fs::is_directory(dir, ec)) return out;
    for (fs::directory_iterator it{dir, ec}, end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::ifstream in{it->path()};
        if (!in) continue;
        json doc;
        try {
            in >> doc;
        } catch (...) {
            continue;   // a malformed document is the format loader's refusal
        }
        if (!doc.contains("runtimeLibraries")) continue;
        if (!doc.at("runtimeLibraries").is_array()) continue;
        std::string const document = it->path().filename().generic_string();
        for (auto const& row : doc.at("runtimeLibraries")) {
            if (!row.is_object() || !row.contains("source")) continue;
            if (!row.at("source").is_string()) continue;
            auto src = row.at("source").get<std::string>();
            if (src.empty()) continue;
            std::string role = "<no role>";
            if (row.contains("role") && row.at("role").is_string())
                role = row.at("role").get<std::string>();
            out.push_back(dss::ffi::ShippedSourceDeclaration{
                "object format document '" + document
                    + "' runtimeLibraries[" + role + "]",
                std::move(src)});
        }
    }
    std::sort(out.begin(), out.end(),
              [](auto const& a, auto const& b) {
                  return std::tie(a.source, a.declarer)
                       < std::tie(b.source, b.declarer);
              });
    return out;
}

// ── R1 ───────────────────────────────────────────────────────────────────────
// Every realization names a source that EXISTS. This is the corpus-wide half;
// the load-time half lives in `readShippedLibDescriptor` and fires on the
// descriptor actually being read.
TEST(ShippedSourceRealization, EveryRealizationNamesAnExistingSource) {
    auto const root = configRoot();
    ASSERT_FALSE(root.empty());
    for (auto const& c : allClaims()) {
        fs::path const p = (root / c.source).lexically_normal();
        std::error_code ec;
        EXPECT_TRUE(fs::is_regular_file(p, ec))
            << "R1: " << c.descriptor << ' ' << c.context << " realization."
            << c.format << ".source names '" << c.source
            << "', which resolves to '" << p.generic_string()
            << "' — no readable file is there, so object format '" << c.format
            << "' would carry a DECLARED symbol with no body.";
    }
    for (auto const& d : formatRealizedSources()) {
        fs::path const p = (root / d.source).lexically_normal();
        std::error_code ec;
        EXPECT_TRUE(fs::is_regular_file(p, ec))
            << "R1: " << d.declarer << " names '" << d.source
            << "', which resolves to '" << p.generic_string()
            << "' — no readable file is there, so the role would be DECLARED "
               "with no body and every compile that lowers to it would fail to "
               "link.";
    }
}

// The same refusal through the ENGINE's own entry point, so the test cannot
// pass while the compiler's copy of the rule is broken. A green corpus with a
// broken checker is the shape of "the mutant was never read".
TEST(ShippedSourceRealization, EngineAcceptsTheShippedCorpus) {
    ASSERT_FALSE(configRoot().empty());
    dss::DiagnosticReporter rep{};
    auto const formatSources = formatRealizedSources();
    EXPECT_TRUE(dss::ffi::validateShippedSourceTree(descriptorDir(), runtimeDir(),
                                                    formatSources, rep))
        << "the shipped corpus does not satisfy its own refusals";
    EXPECT_EQ(rep.errorCount(), 0u);
}

// ── R2 + R4 ──────────────────────────────────────────────────────────────────
// Every regular file under the runtime tree is NAMED by some descriptor. Read
// forwards this is R2 (inert config: nothing can ever add an unnamed file to a
// build graph). Read backwards it is R4 (no headers here): a header is never a
// translation unit, so no realization can name one, so this single rule refuses
// it by construction — which matters because the tree mirrors the include
// namespace, and a header here would sit at an include path and SILENTLY shadow
// the descriptor a unit exists to consume.
TEST(ShippedSourceRealization, EveryRuntimeFileIsNamedByADescriptor) {
    auto const root = configRoot();
    ASSERT_FALSE(root.empty());

    std::vector<std::string> claimed;
    for (auto const& c : allClaims())
        claimed.push_back((root / c.source).lexically_normal().generic_string());
    // The format documents are the SECOND declarer (see `formatRealizedSources`).
    for (auto const& d : formatRealizedSources())
        claimed.push_back((root / d.source).lexically_normal().generic_string());

    std::error_code ec;
    if (!fs::is_directory(runtimeDir(), ec)) return;  // no runtime tree yet is legal
    // ★ THE AUTHORED SURFACE IS `<root>/<tier>/src`, DERIVED HERE THE SAME WAY THE
    // ENGINE DERIVES IT — and derived rather than filtered on purpose. A tier also
    // holds `dist/`, the GENERATED object cache, so a walk of the tier root would
    // refuse every cached object and red every warm build. Excluding `dist/` by
    // NAME would work today and rot the moment the cache moves; asserting over the
    // authored half cannot.
    for (fs::directory_iterator tierIt{runtimeDir(), ec}, tierEnd; tierIt != tierEnd;
         tierIt.increment(ec)) {
        if (ec) break;
        if (!tierIt->is_directory(ec)) continue;
        fs::path const authored = tierIt->path() / "src";
        if (!fs::is_directory(authored, ec)) continue;
    for (fs::recursive_directory_iterator it{authored, ec}, end; it != end;
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string const p = it->path().lexically_normal().generic_string();
        EXPECT_NE(std::find(claimed.begin(), claimed.end(), p), claimed.end())
            << "R2/R4: '" << p << "' is named by NO descriptor's 'realization' "
               "map and by no object format's runtimeLibraries[].source, "
               "map, so nothing can ever add it to a build graph. In particular "
               "a HEADER here would sit at an INCLUDE PATH and silently shadow "
               "the descriptor a unit exists to consume.";
    }
    }
}

// ── R3 ───────────────────────────────────────────────────────────────────────
// No format carries BOTH an image and a source AT ONE LEVEL — the descriptor's own
// pair, or one symbol's own pair — and no symbol re-imports a format its
// descriptor realizes from source. ★ P69 (the R3 precedence): a symbol's OWN
// source over the image it INHERITS is ONE owner, not two — the row names its body,
// as a per-symbol `library` override names its image — and the reader records it by
// giving the symbol an empty image there (see `ShippedSymbol::library`), which the
// last arm below checks through the real reader.
TEST(ShippedSourceRealization, NoFormatDeclaresBothAnImageAndASource) {
    forEachDescriptor([&](fs::path const& p, json const& doc) {
        auto const docLib  = formatKeysOf(doc, "library");
        auto const docReal = formatKeysOf(doc, "realization");
        auto contains = [](std::vector<std::string> const& v, std::string const& f) {
            return std::find(v.begin(), v.end(), f) != v.end();
        };
        auto check = [&](std::vector<std::string> const& lib,
                         std::vector<std::string> const& real,
                         std::string const& ctx) {
            for (auto const& f : real)
                EXPECT_FALSE(contains(lib, f))
                    << "R3: " << p.filename().generic_string() << ' ' << ctx
                    << " declares BOTH 'library." << f << "' and 'realization."
                    << f << "' — two owners for one body. Preferring either "
                       "silently is how a program links against an image that "
                       "does not export the symbol and dies at LOAD.";
        };
        check(docLib, docReal, "(root)");
        if (!doc.contains("symbols") || !doc.at("symbols").is_array()) return;
        std::size_t i = 0;
        for (auto const& sym : doc.at("symbols")) {
            std::string const ctx = "symbols[" + std::to_string(i++) + "]";
            if (!sym.is_object()) continue;
            auto const symLib  = formatKeysOf(sym, "library");
            auto const symReal = formatKeysOf(sym, "realization");
            check(symLib, symReal, ctx);
            for (auto const& f : docReal)
                if (!contains(symReal, f))
                    EXPECT_FALSE(contains(symLib, f))
                        << "R3: " << p.filename().generic_string() << ' ' << ctx
                        << " imports on '" << f << "' where its descriptor realizes "
                           "every symbol from source — two owners for one body.";
        }
    });
}

// ★ The precedence, THROUGH THE READER (P69). A symbol's own source on a format
// whose import it would inherit reads clean, is realized from the source there,
// carries an EMPTY image there (what every binder fold routes unbound), and keeps
// the inherited image on every other format. The three refusals of the same rule —
// both at the root, both on the symbol, the symbol's import under the
// descriptor's source — each fail to read, naming the two owners and where.
TEST(ShippedSourceRealization, ASymbolsOwnSourceSupersedesTheImportItInherits) {
    auto const root = dss::test::findConfigRoot();
    ASSERT_TRUE(root.has_value()) << dss::test::configRootDiagnostic();
    fs::path const dir = fs::temp_directory_path() / "dss-r3-precedence";
    fs::create_directories(dir);
    auto write = [&](std::string const& name, std::string const& text) {
        fs::path const at = dir / name;
        std::ofstream(at, std::ios::binary) << text;
        return at;
    };
    auto read = [&](fs::path const& at, dss::DiagnosticReporter& rep) {
        dss::TypeInterner interner{dss::CompilationUnitId{1}};
        dss::TypeRegistry typeReg;
        return dss::ffi::readShippedLibDescriptor(at, interner, typeReg, rep);
    };
    {
        auto const at = write("precedence_ok.json", R"JSON({
            "header": "precedence_ok.h",
            "library": { "pe": "ucrtbase.dll", "elf": "libc.so.6", "macho": "/usr/lib/libSystem.B.dylib" },
            "symbols": [
              { "name": "shipped_fn", "signature": "fn() -> i32",
                "realization": { "pe": { "source": "runtime/platform/src/atomic.c" } } },
              { "name": "imported_fn", "signature": "fn() -> i32" }
            ]
        })JSON");
        dss::DiagnosticReporter rep;
        auto const desc = read(at, rep);
        ASSERT_TRUE(desc.has_value()) << (rep.all().empty() ? std::string{} : rep.all().front().actual);
        EXPECT_FALSE(rep.hasErrors());
        ASSERT_EQ(desc->symbols.size(), 2u);
        auto const& shipped = desc->symbols.at(0);
        ASSERT_EQ(shipped.name, "shipped_fn");
        ASSERT_EQ(shipped.library.count("pe"), 1u) << "the supersession must be recorded on the symbol";
        EXPECT_EQ(shipped.library.at("pe"), "") << "an EMPTY image: the source, not ucrtbase, owns the body";
        EXPECT_EQ(shipped.library.count("elf"), 0u) << "the other formats keep the inherited import";
        EXPECT_EQ(desc->symbols.at(1).library.size(), 0u) << "a row without its own source is untouched";
        ASSERT_EQ(shipped.realization.count("pe"), 1u);
        EXPECT_EQ(shipped.realization.at("pe"), "runtime/platform/src/atomic.c");
        EXPECT_EQ(desc->library.at("pe"), "ucrtbase.dll") << "the descriptor's own map is not rewritten";
    }
    struct Refusal {
        char const* file;
        char const* text;
        char const* names;
    };
    Refusal const refusals[] = {
        {"precedence_root_both.json", R"JSON({
            "header": "rb.h",
            "library": { "pe": "ucrtbase.dll" },
            "realization": { "pe": { "source": "runtime/platform/src/atomic.c" } },
            "symbols": [ { "name": "rb_fn", "signature": "fn() -> i32" } ]
        })JSON", "(root)"},
        {"precedence_symbol_both.json", R"JSON({
            "header": "sb.h",
            "symbols": [ { "name": "sb_fn", "signature": "fn() -> i32",
                           "library": { "pe": "ucrtbase.dll" },
                           "realization": { "pe": { "source": "runtime/platform/src/atomic.c" } } } ]
        })JSON", "symbols[0] ('sb_fn')"},
        {"precedence_symbol_import_under_source.json", R"JSON({
            "header": "su.h",
            "realization": { "pe": { "source": "runtime/platform/src/atomic.c" } },
            "symbols": [ { "name": "su_fn", "signature": "fn() -> i32",
                           "library": { "pe": "ucrtbase.dll" } } ]
        })JSON", "symbols[0] ('su_fn')"},
    };
    for (Refusal const& r : refusals) {
        SCOPED_TRACE(r.file);
        dss::DiagnosticReporter rep;
        EXPECT_FALSE(read(write(r.file, r.text), rep).has_value());
        // The refusal states R3's CONDITION — two owners for one body — and where; an
        // anchor id is bookkeeping for a comment, not compiler output (the
        // emitted-anchor-ids guard), so the message is matched by what it says.
        bool namedTwoOwners = false, namedWhere = false;
        for (auto const& d : rep.all()) {
            namedTwoOwners = namedTwoOwners || d.actual.find("two owners for one body") != std::string::npos;
            namedWhere     = namedWhere || d.actual.find(r.names) != std::string::npos;
        }
        EXPECT_TRUE(namedTwoOwners);
        EXPECT_TRUE(namedWhere) << "the refusal must name " << r.names;
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ── THE TREE'S OWN SHAPE ─────────────────────────────────────────────────────
// A realization's source lives under the shipped RUNTIME tree, in its `src/`
// half. This is the correspondence a convention alone cannot enforce, and a
// convention nothing checks is precisely what this mechanism exists to delete.
//
// ★ `src/` VS `dist/` IS WHAT MAKES THE TREE SELF-DESCRIBING: authored source in
// one, generated objects in the other, and nothing generated is ever interleaved
// with anything authored. A descriptor that named a path under `dist/` would be
// naming a build artifact as if it were source.
TEST(ShippedSourceRealization, EverySourceLivesUnderTheRuntimeSourceTree) {
    for (auto const& c : allClaims()) {
        EXPECT_EQ(c.source.rfind("runtime/platform/src/", 0), 0u)
            << c.descriptor << ' ' << c.context << " realization." << c.format
            << ".source is '" << c.source
            << "' — a shipped source file lives under 'runtime/platform/src/'. "
               "The sibling 'dist/' holds GENERATED objects and is gitignored; "
               "naming a path there would declare a build artifact as source.";
        EXPECT_EQ(c.source.find(".."), std::string::npos)
            << c.descriptor << ": a realization source may not escape the "
               "config root with a '..' component.";
    }
}

// ── THE FAST READER AND THE FULL READ AGREE ──────────────────────────────────
// The driver uses an interner-free fast reader to learn which sources a build
// needs; the semantic phase learns the same fact through the full typed read.
// Two readers of one fact is exactly the drift surface this cycle keeps
// deleting, so the agreement is pinned rather than assumed.
TEST(ShippedSourceRealization, FastReaderAgreesWithTheDeclaredClaims) {
    auto const root = configRoot();
    ASSERT_FALSE(root.empty());
    forEachDescriptor([&](fs::path const& p, json const& doc) {
        for (auto const& fmt : {"pe", "elf", "macho"}) {
            std::vector<std::string> expected;
            auto harvest = [&](json const& node) {
                if (!node.is_object()) return;
                auto const it = node.find(std::string{fmt});
                if (it == node.end() || !it->is_object()) return;
                if (!it->contains("source") || !it->at("source").is_string()) return;
                auto s = it->at("source").get<std::string>();
                if (std::find(expected.begin(), expected.end(), s) == expected.end())
                    expected.push_back(std::move(s));
            };
            if (doc.contains("realization")) harvest(doc.at("realization"));
            if (doc.contains("symbols") && doc.at("symbols").is_array())
                for (auto const& sym : doc.at("symbols"))
                    if (sym.is_object() && sym.contains("realization"))
                        harvest(sym.at("realization"));
            EXPECT_EQ(dss::ffi::readShippedSourcesForFormat(p, fmt), expected)
                << p.filename().generic_string() << " / format " << fmt
                << ": the driver's fast reader disagrees with the descriptor.";
        }
    });
}
