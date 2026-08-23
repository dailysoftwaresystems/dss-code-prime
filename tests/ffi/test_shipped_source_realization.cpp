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
// `examples/c-subset/shipped_dirent_readdir` — it compiles and RUNS the pe64 arm,
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

#include "repo_root.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace {

// The real trees these invariants are measured over, resolved through the ONE
// test-side resolver ($DSS_CONFIG_ROOT → the CMake-baked repo root → the cwd
// ancestor walk). A private cwd-walk here would find nothing in an out-of-tree
// build, and an invariant with no tree to read is a hole, not a pass.
[[nodiscard]] fs::path configRoot() {
    auto const root = dss::test::findRepoRoot();
    if (!root) {
        ADD_FAILURE() << dss::test::repoRootDiagnostic();
        return {};
    }
    return *root / "src" / "dss-config";
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
}

// The same refusal through the ENGINE's own entry point, so the test cannot
// pass while the compiler's copy of the rule is broken. A green corpus with a
// broken checker is the shape of "the mutant was never read".
TEST(ShippedSourceRealization, EngineAcceptsTheShippedCorpus) {
    ASSERT_FALSE(configRoot().empty());
    dss::DiagnosticReporter rep{};
    EXPECT_TRUE(dss::ffi::validateShippedSourceTree(descriptorDir(), runtimeDir(),
                                                    rep))
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
               "map, so nothing can ever add it to a build graph. In particular "
               "a HEADER here would sit at an INCLUDE PATH and silently shadow "
               "the descriptor a unit exists to consume.";
    }
    }
}

// ── R3 ───────────────────────────────────────────────────────────────────────
// No format carries BOTH an image and a source, at the EFFECTIVE (merged) level
// — the per-symbol override merged over the descriptor's, exactly as the
// injector and the realization oracle merge them. Checking only the raw override
// would miss the commonest shape: a descriptor-level image plus a per-symbol
// source.
TEST(ShippedSourceRealization, NoFormatDeclaresBothAnImageAndASource) {
    forEachDescriptor([&](fs::path const& p, json const& doc) {
        auto const docLib  = formatKeysOf(doc, "library");
        auto const docReal = formatKeysOf(doc, "realization");
        auto check = [&](std::vector<std::string> const& lib,
                         std::vector<std::string> const& real,
                         std::string const& ctx) {
            for (auto const& f : real)
                EXPECT_EQ(std::find(lib.begin(), lib.end(), f), lib.end())
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
            auto lib  = docLib;
            auto real = docReal;
            for (auto const& f : formatKeysOf(sym, "library"))
                if (std::find(lib.begin(), lib.end(), f) == lib.end())
                    lib.push_back(f);
            for (auto const& f : formatKeysOf(sym, "realization"))
                if (std::find(real.begin(), real.end(), f) == real.end())
                    real.push_back(f);
            check(lib, real, ctx);
        }
    });
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
