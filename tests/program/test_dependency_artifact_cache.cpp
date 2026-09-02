// THE CROSS-BUILD DEPENDENCY ARTIFACT CACHE —
// D-DEPS-NO-ARTIFACT-SHARING-ACROSS-BUILDS-AT-ONE-CONFIGURATION (C).
//
// The manifest surface (`core/types/project_config.{hpp,cpp}`), the key
// (`program/runtime_object_cache.{hpp,cpp}`'s second subject class), and the
// driver consult at the `buildCus` boundary (`program/program.cpp`).
//
// ── WHAT THIS SUBJECT BREAKS SILENTLY, WHICH IS WHY THE PINS LOOK PARANOID ───
//
//   * UNDER-INVALIDATION. A key that misses an input SERVES STALE BYTES, and
//     the build is green, fast and wrong. The whole reason (C) waited for
//     `CompilationUnit::inputDigest()` is that a C dependency's `#include`
//     closure is in NO manifest: ✔MEASURED in P46, a dependency whose header
//     alone is edited produces a different archive while every manifest-level
//     term — both manifests, both source lists, the target, the derived format,
//     the config — is byte-identical. `AHeaderNoManifestNamesMovesTheKey` is
//     that measurement turned into a pin, and it is the single most important
//     test in this file.
//   * A "WARM" ARM THAT PROVES NOTHING. "The second build also succeeded" is
//     equally true of a cache that never stored anything. Every warm pin below
//     therefore plants SENTINEL BYTES in the cache entry and asserts the OUTPUT
//     equals them — a recompile cannot produce a sentinel, so the assertion has
//     exactly one explanation.
//   * ORDER DEPENDENCE. A cold pin that ran after a warm one would read the
//     warm one's entries. Every build test below takes its OWN scratch tree AND
//     its OWN cache root through the manifest-declared override variable, so
//     the arms are independent by construction rather than by ordering.
//   * THE CONFIG KEY GOING DEAD. Every C++ mutant is blind to the difference
//     between a live manifest key and dead configuration, so
//     `NoPolicyStoresNothing` drives the REMOVE direction on the JSON itself:
//     with the `dependencyArtifactCache` object absent, the declared cache root
//     must stay EMPTY. Delete the key from `renderManifest`'s emission and the
//     positive pins go red.
//
// ⚠ THE PARSE PINS LIVE HERE RATHER THAN IN `test_project_config.cpp`
// DELIBERATELY. They are about THIS mechanism's manifest surface, and keeping
// the vocabulary pins beside the behaviour pins is what lets a reader see that
// the `eviction` tokens the loader accepts are the ones the store actually
// implements — two files would let those drift with nothing in either saying so.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/project_config.hpp"
#include "program/dependency_resolver.hpp"
#include "program/program.hpp"
#include "program/runtime_object_cache.hpp"

#include "diagnostic_count.hpp"
#include "scoped_env.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using dss::DependencyArtifactCacheEviction;
using dss::DiagnosticCode;
using dss::DiagnosticReporter;
using dss::Program;
using dss::test_support::countCode;
using dss::test_support::Location;
using dss::test_support::ScopedEnv;
using dss::test_support::ScratchDir;

namespace fs = std::filesystem;

namespace {

// ── the fixture vocabulary, spelled once ────────────────────────────────────
//
// The consumer CROSS-EMITS (nothing here spawns an artifact), so every pin
// stays live on every leg of the matrix rather than being `#if`-ed out on three
// of them. The dependency is a `staticlib`, whose derived format is the unique
// archive-writing one for the consumer's machine.
constexpr std::string_view kConsumerSpec  = "x86_64:elf64-x86_64-linux-exec";
constexpr std::string_view kDepFormatName = "elf64-x86_64-linux-staticlib";

// The environment variable the MANIFEST names. ⚠ Deliberately NOT
// `DSS_RUNTIME_CACHE_DIR`: if the driver sniffed a compiled-in variable instead
// of reading the declared one, every entry would land somewhere else and every
// positive pin below would fail. The name is therefore itself a pin on
// `rootOverrideVariable` being read rather than assumed.
constexpr char const* kCacheDirVar = "DSS_TEST_DEPENDENCY_ARTIFACT_CACHE_DIR";

constexpr std::string_view kDepHeader = R"(#ifndef DEP_IMPL_H
#define DEP_IMPL_H
#define DSS_DEP_BIAS 2
#endif
)";

constexpr std::string_view kDepSource = R"(#include "dep_impl.h"
int dep_value(int v) { return v + DSS_DEP_BIAS; }
)";

// ⚠ THE CONSUMER DELIBERATELY DOES NOT CALL INTO THE DEPENDENCY, and that is a
// requirement of the warm pins rather than laziness. A static archive is pulled
// DEMAND-DRIVEN, so a consumer that references nothing in it links successfully
// against ANY well-formed archive — which is what lets the warm arms plant a
// well-formed SENTINEL archive in the cache entry and still assert a GREEN
// build. With a reference here, the sentinel would fail the link and the pin
// would be reading a build failure instead of a served hit.
constexpr std::string_view kMainSource = R"(int main(void) { return 0; }
)";

// A minimal WELL-FORMED `ar` archive: the magic and no members. It is the
// sentinel the warm arms plant, and it is chosen for exactly two properties —
// no compiler can emit it as the archive for a translation unit that defines a
// function, and the linker's magic-byte dispatch accepts it, so serving it
// leaves the build green.
constexpr std::string_view kSentinelArchive = "!<arch>\n";

void writeText(fs::path const& p, std::string_view text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out{p, std::ios::binary | std::ios::trunc};
    out << text;
    out.flush();
    ASSERT_TRUE(out) << "could not write fixture file " << p.generic_string();
}

[[nodiscard]] std::string readBytes(fs::path const& p) {
    std::ifstream in{p, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in},
                       std::istreambuf_iterator<char>{}};
}

// JSON string quoting — `\` matters because Windows paths are interpolated into
// these manifests, and a raw one would be read as an escape introducer.
[[nodiscard]] std::string jsonQuote(std::string_view s) {
    std::string out = "\"";
    for (char const c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            default:   out += c;      break;
        }
    }
    out += '"';
    return out;
}

[[nodiscard]] std::string jsonStringArray(std::vector<std::string> const& raw) {
    std::string out = "[";
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (i != 0) out += ", ";
        out += jsonQuote(raw[i]);
    }
    out += "]";
    return out;
}

struct ManifestSpec {
    std::string              profile = "cli";
    std::vector<std::string> targets;
    std::vector<std::string> sources;
    std::string              dependsOnPath;   // empty ⇒ no `dependsOn`
    // 0 ⇒ the key is not emitted. Non-zero on a format that declares no
    // `stackReserveControl` is how this file reaches "the target wrote an
    // artifact AND reported an error" — see `AFailingBuildIsNotStored`.
    std::uint64_t            stackReserve = 0;
    // ⚠ EMPTY ⇒ THE `dependencyArtifactCache` KEY IS NOT EMITTED AT ALL. That
    // is the REMOVE-direction config mutant's shape, and it is a distinct input
    // from an emitted object whose `enabled` is false.
    std::string              cacheEviction;
};

[[nodiscard]] std::string renderManifest(ManifestSpec const& m) {
    std::string out = "{\n";
    out += "  \"language\": \"c\",\n";
    out += "  \"artifactProfile\": " + jsonQuote(m.profile) + ",\n";
    out += "  \"targets\": " + jsonStringArray(m.targets) + ",\n";
    out += "  \"sources\": " + jsonStringArray(m.sources);
    if (!m.dependsOnPath.empty()) {
        out += ",\n  \"dependsOn\": [{\"path\": " + jsonQuote(m.dependsOnPath)
             + "}]";
    }
    if (m.stackReserve != 0) {
        out += ",\n  \"stackReserve\": " + std::to_string(m.stackReserve);
    }
    if (!m.cacheEviction.empty()) {
        out += ",\n  \"dependencyArtifactCache\": {"
               "\"enabled\": true, \"rootOverrideVariable\": "
             + jsonQuote(kCacheDirVar) + ", \"eviction\": "
             + jsonQuote(m.cacheEviction) + "}";
    }
    out += "\n}\n";
    return out;
}

// Every `.key` sidecar under `root`, sorted. The sidecar rather than the
// artifact because it is the file whose EXISTENCE is what makes an entry
// servable: an artifact with no key document beside it is a REFUSAL, so
// counting artifacts would count entries the cache would never serve.
[[nodiscard]] std::vector<fs::path> keyDocumentsUnder(fs::path const& root) {
    std::vector<fs::path> found;
    std::error_code       ec;
    if (!fs::is_directory(root, ec)) return found;
    for (fs::recursive_directory_iterator it{root, ec}, end{};
         it != end && !ec; it.increment(ec)) {
        std::error_code typeEc;
        if (!it->is_regular_file(typeEc) || typeEc) continue;
        if (it->path().extension() == ".key") found.push_back(it->path());
    }
    std::sort(found.begin(), found.end());
    return found;
}

// The artifact beside a key document — `<stem>-<index>.key` → the sibling this
// store wrote first. Derived through the PRODUCTION spelling, so a test cannot
// disagree with the store about which two files form an entry.
[[nodiscard]] fs::path artifactBeside(fs::path const& keyDocument) {
    // `runtimeKeyDocumentPath` maps artifact → key; the inverse is the only
    // direction this file needs and it is one `replace_extension` away, but it
    // needs the ARTIFACT's extension, which is the dependency format's. The
    // fixture builds exactly one shape, so the answer is `.a`.
    return fs::path{keyDocument}.replace_extension(".a");
}

// U-9's layout: `<consumer output base>/deps/<name>/<formatName>/<file>`.
[[nodiscard]] fs::path depArtifactPath(fs::path const& outBase,
                                       std::string_view depName) {
    return outBase / std::string{dss::kDependencyOutputDirName}
         / std::string{depName} / std::string{kDepFormatName} / "lib.a";
}

// One complete fixture: a `staticlib` dependency (source + a header that NO
// manifest names) and a consumer that depends on it.
struct Fixture {
    fs::path dir;
    fs::path depDir;
    fs::path projectFile;
    fs::path outBase;

    explicit Fixture(fs::path root, std::string_view eviction,
                     std::uint64_t depStackReserve = 0,
                     std::string_view depSource = kDepSource)
        : dir(std::move(root)), depDir(dir / "leafutil"),
          projectFile(dir / "app.dss-project.json"), outBase(dir / "out") {
        writeText(depDir / "dep_impl.h", kDepHeader);
        writeText(depDir / "lib.c", depSource);
        writeText(depDir / std::string{dss::kDependencyManifestName},
                  renderManifest({.profile = "staticlib",
                                  .targets = {std::string{"x86_64:"}
                                              + std::string{kDepFormatName}},
                                  .sources = {"lib.c"},
                                  .stackReserve = depStackReserve}));
        writeText(dir / "main.c", kMainSource);
        writeText(projectFile,
                  renderManifest({.targets = {std::string{kConsumerSpec}},
                                  .sources = {(dir / "main.c").generic_string()},
                                  .dependsOnPath = depDir.generic_string(),
                                  .cacheEviction = std::string{eviction}}));
    }

    [[nodiscard]] fs::path depArtifact() const {
        return depArtifactPath(outBase, "leafutil");
    }

    // One full project build on a FRESH `Program` — the real input path, and
    // fresh because `compileProject` mutates persistent driver state (M3).
    [[nodiscard]] int build(DiagnosticReporter& rep) const {
        Program prog;
        prog.setOutputDir(outBase);
        return prog.compileProject(projectFile.string(), rep);
    }
};

// ═══ THE MANIFEST SURFACE ════════════════════════════════════════════════════

[[nodiscard]] std::optional<dss::ProjectConfig>
parseManifest(std::string_view json, DiagnosticReporter& rep) {
    return dss::parseProjectConfig(json, "<fixture>.dss-project.json", rep);
}

constexpr std::string_view kMinimalManifestHead =
    "{\"language\": \"c\", \"artifactProfile\": \"cli\", "
    "\"targets\": [\"x86_64:elf64-x86_64-linux-exec\"], "
    "\"sources\": [\"main.c\"]";

[[nodiscard]] std::string withCacheObject(std::string_view body) {
    return std::string{kMinimalManifestHead}
         + ", \"dependencyArtifactCache\": " + std::string{body} + "}";
}

TEST(DependencyArtifactCacheSurface, AbsentMemberIsNoPolicyAndNoDiagnostic) {
    DiagnosticReporter rep;
    auto const pc = parseManifest(std::string{kMinimalManifestHead} + "}", rep);
    ASSERT_TRUE(pc.has_value());
    EXPECT_FALSE(pc->dependencyArtifactCache.has_value())
        << "a manifest that names no cache must parse to NO POLICY — every "
           "manifest written before this member exists is in this state, and "
           "an invented default would start writing to a user's disk unasked.";
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(DependencyArtifactCacheSurface, AllThreeMembersAreAccepted) {
    DiagnosticReporter rep;
    auto const pc = parseManifest(
        withCacheObject("{\"enabled\": true, \"rootOverrideVariable\": "
                        "\"MY_CACHE\", \"eviction\": \"retain\"}"),
        rep);
    ASSERT_TRUE(pc.has_value()) << "unexpected reject";
    ASSERT_TRUE(pc->dependencyArtifactCache.has_value());
    EXPECT_TRUE(pc->dependencyArtifactCache->enabled);
    EXPECT_EQ(pc->dependencyArtifactCache->rootOverrideVariable, "MY_CACHE");
    EXPECT_EQ(pc->dependencyArtifactCache->eviction,
              DependencyArtifactCacheEviction::Retain);
}

// ⚠ EACH MEMBER, SEPARATELY. A single "one member missing" case would leave the
// other two untested, and the SILENT direction is a member the loader stopped
// requiring — which is a policy half-declared and half-defaulted.
TEST(DependencyArtifactCacheSurface, EveryMemberIsRequired) {
    struct Case {
        std::string_view missing;
        std::string_view body;
    };
    constexpr Case kCases[] = {
        {"enabled",
         "{\"rootOverrideVariable\": \"C\", \"eviction\": \"retain\"}"},
        {"rootOverrideVariable", "{\"enabled\": true, \"eviction\": \"retain\"}"},
        {"eviction",
         "{\"enabled\": true, \"rootOverrideVariable\": \"C\"}"},
    };
    for (Case const& c : kCases) {
        DiagnosticReporter rep;
        auto const pc = parseManifest(withCacheObject(c.body), rep);
        EXPECT_FALSE(pc.has_value())
            << "a 'dependencyArtifactCache' object missing '" << c.missing
            << "' must REJECT: a partial policy is a policy nobody declared.";
        EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u)
            << "missing member: " << c.missing;
    }
}

TEST(DependencyArtifactCacheSurface, AnUnknownMemberRejects) {
    DiagnosticReporter rep;
    auto const pc = parseManifest(
        withCacheObject("{\"enabled\": true, \"rootOverrideVariable\": \"C\", "
                        "\"eviction\": \"retain\", \"evicton\": \"retain\"}"),
        rep);
    EXPECT_FALSE(pc.has_value())
        << "a mistyped member must reject — silently dropping it is dropping "
           "the policy the entry exists to state.";
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
}

// ★ THE ACCEPTED SET IS DRIVEN FROM THE EXPORTED TABLE, never re-typed. A
// re-typed list in a test recreates exactly the drift the derivation removed.
TEST(DependencyArtifactCacheSurface, EveryExportedEvictionTokenIsAccepted) {
    auto const tokens = dss::dependencyArtifactCacheEvictionTokens();
    ASSERT_FALSE(tokens.empty())
        << "the eviction vocabulary must not be empty — an empty table would "
           "make every value reject and this whole loop vacuous.";
    for (std::string_view const token : tokens) {
        DiagnosticReporter rep;
        auto const pc = parseManifest(
            withCacheObject("{\"enabled\": true, \"rootOverrideVariable\": "
                            "\"C\", \"eviction\": \"" + std::string{token}
                            + "\"}"),
            rep);
        EXPECT_TRUE(pc.has_value())
            << "the loader must accept the token it publishes: " << token;
    }
}

TEST(DependencyArtifactCacheSurface, AnUnknownEvictionTokenRejectsNamingTheSet) {
    DiagnosticReporter rep;
    auto const pc = parseManifest(
        withCacheObject("{\"enabled\": true, \"rootOverrideVariable\": \"C\", "
                        "\"eviction\": \"lru\"}"),
        rep);
    EXPECT_FALSE(pc.has_value());
    ASSERT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    std::string message;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::C_MalformedJson) message = d.actual;
    }
    EXPECT_NE(message.find("'lru'"), std::string::npos)
        << "the reject must name the offending token: " << message;
    EXPECT_NE(message.find(dss::dependencyArtifactCacheEvictionTokenList()),
              std::string::npos)
        << "the reject must name the ACCEPTED set, derived from the table "
           "rather than re-typed: " << message;
}

// ═══ THE COLD ARM ════════════════════════════════════════════════════════════
//
// Its own scratch tree and its own cache root, so it can be run alone, first,
// last or under `ctest -j` and mean the same thing.

TEST(DependencyArtifactCacheCold, AFirstBuildWritesOneVerifiableEntry) {
    ScratchDir scratch{Location::Temp, "dep-artifact-cache-cold"};
    fs::path const cacheRoot = scratch.path() / "cache";
    ScopedEnv const cacheEnv{kCacheDirVar, cacheRoot.string()};

    Fixture const fx{scratch.path() / "tree", "prune-superseded"};
    DiagnosticReporter rep;
    ASSERT_EQ(fx.build(rep), 0) << "the cold build must succeed";
    ASSERT_TRUE(fs::exists(fx.depArtifact()))
        << "the dependency's artifact must land where U-9 says: "
        << fx.depArtifact().generic_string();

    auto const entries = keyDocumentsUnder(cacheRoot);
    ASSERT_EQ(entries.size(), 1u)
        << "exactly ONE dependency artifact was built, so exactly one entry "
           "must have been stored under the MANIFEST-DECLARED override root. "
           "Zero means the policy never reached the sub-build; more than one "
           "means the root's own artifact was cached too, which this mechanism "
           "deliberately does not do.";

    // The entry is SERVABLE, not merely present: an artifact beside its key
    // document, byte-identical to what the build produced.
    EXPECT_TRUE(fs::exists(artifactBeside(entries.front())));
    EXPECT_EQ(readBytes(artifactBeside(entries.front())),
              readBytes(fx.depArtifact()))
        << "the stored bytes must be the bytes the build produced";

    // And it lives under the `deps/` component that keeps it from sharing a
    // directory — and therefore a prune — with the shipped runtime objects.
    EXPECT_NE(entries.front().generic_string().find("/deps/"),
              std::string::npos)
        << "a dependency entry must live under its own 'deps/' component: "
        << entries.front().generic_string();
}

// ★★★ THE REMOVE-DIRECTION CONFIG MUTANT'S POSITIVE CONTROL. Every C++ mutant
// is blind to the difference between a live manifest key and dead
// configuration, so this arm drives the JSON: with `dependencyArtifactCache`
// ABSENT, the declared root must stay empty. Delete the key's emission from
// `renderManifest` and the cold/warm pins go red while this one stays green,
// which is what makes the pair a real control.
TEST(DependencyArtifactCacheCold, NoPolicyStoresNothing) {
    ScratchDir scratch{Location::Temp, "dep-artifact-cache-off"};
    fs::path const cacheRoot = scratch.path() / "cache";
    ScopedEnv const cacheEnv{kCacheDirVar, cacheRoot.string()};

    Fixture const fx{scratch.path() / "tree", /*eviction=*/""};
    DiagnosticReporter rep;
    ASSERT_EQ(fx.build(rep), 0);
    ASSERT_TRUE(fs::exists(fx.depArtifact()));

    EXPECT_TRUE(keyDocumentsUnder(cacheRoot).empty())
        << "a manifest that declares no cache must write NOTHING to the "
           "override root — the member's absence is the whole disable, and a "
           "build that cached anyway would be caching on a compiled-in default.";
}

// ═══ THE WARM ARM ════════════════════════════════════════════════════════════
//
// ★★★ THE HIT IS WITNESSED BY SENTINEL BYTES, NOT BY A SECOND GREEN BUILD.
// "It succeeded again" is equally true of a cache that stored nothing. Planting
// bytes no compiler would emit into the entry and then finding them AT THE
// OUTPUT PATH has exactly one explanation: the entry was looked up, verified
// against its key document, and served.

TEST(DependencyArtifactCacheWarm, ASecondBuildServesTheStoredBytes) {
    ScratchDir scratch{Location::Temp, "dep-artifact-cache-warm"};
    fs::path const cacheRoot = scratch.path() / "cache";
    ScopedEnv const cacheEnv{kCacheDirVar, cacheRoot.string()};

    Fixture const fx{scratch.path() / "tree", "prune-superseded"};
    DiagnosticReporter cold;
    ASSERT_EQ(fx.build(cold), 0);
    auto const entries = keyDocumentsUnder(cacheRoot);
    ASSERT_EQ(entries.size(), 1u);

    ASSERT_NE(readBytes(fx.depArtifact()), std::string{kSentinelArchive})
        << "the CONTROL: a real compile must not already produce the sentinel, "
           "or the assertion below would hold for both explanations.";

    writeText(artifactBeside(entries.front()), kSentinelArchive);

    // ⛔ THE PREVIOUS ARTIFACT IS DELIBERATELY LEFT IN PLACE, and that is a
    // REGRESSION PIN rather than an omission. ✔MEASURED 2026-08-31: serving
    // into a tree that still held the previous artifact FAILED on
    // Windows/MinGW — `copy_file(..., overwrite_existing)` reported *"File
    // exists"* — so the cache served exactly once per clean output tree and
    // refused on every rebuild after. That is the state EVERY real second build
    // is in. A warm pin that deleted the destination first could not see it,
    // and this one did not until a mutant made an unrelated case hit.
    ASSERT_TRUE(fs::exists(fx.depArtifact()))
        << "the cold build's artifact must still be here — serving OVER it is "
           "what this case exists to exercise";

    DiagnosticReporter warm;
    ASSERT_EQ(fx.build(warm), 0)
        << "the warm build must succeed — the served entry IS the artifact, "
           "and a well-formed empty archive the consumer references nothing in "
           "links exactly as the real one does.";
    ASSERT_TRUE(fs::exists(fx.depArtifact()))
        << "a served hit must still place the artifact where the build would "
           "have written it: a warm tree must look like a cold one.";
    EXPECT_EQ(readBytes(fx.depArtifact()), std::string{kSentinelArchive})
        << "the second build must have SERVED the cache entry. Real archive "
           "bytes here mean it recompiled and the cache is inert — and note "
           "that byte-EQUALITY with the cold artifact could never have "
           "distinguished the two, because a deterministic recompile produces "
           "it too. Only bytes no compile can emit can.";

    EXPECT_EQ(keyDocumentsUnder(cacheRoot).size(), 1u)
        << "a hit must not mint a second entry";
}

// The other half of the destination question: a hit into a tree where the
// artifact is ABSENT. Both arms matter — the case above is the one every real
// rebuild is in, and this one is what a fresh checkout with a warm per-user
// cache does, which is the workflow the whole mechanism exists to serve.
TEST(DependencyArtifactCacheWarm, AHitPlacesTheArtifactIntoACleanedTree) {
    ScratchDir scratch{Location::Temp, "dep-artifact-cache-clean"};
    fs::path const cacheRoot = scratch.path() / "cache";
    ScopedEnv const cacheEnv{kCacheDirVar, cacheRoot.string()};

    Fixture const fx{scratch.path() / "tree", "prune-superseded"};
    DiagnosticReporter cold;
    ASSERT_EQ(fx.build(cold), 0);
    auto const entries = keyDocumentsUnder(cacheRoot);
    ASSERT_EQ(entries.size(), 1u);

    writeText(artifactBeside(entries.front()), kSentinelArchive);
    std::error_code ec;
    fs::remove_all(fx.outBase, ec);   // the whole output tree, not just the file
    ASSERT_FALSE(fs::exists(fx.depArtifact()));

    DiagnosticReporter warm;
    ASSERT_EQ(fx.build(warm), 0);
    EXPECT_EQ(readBytes(fx.depArtifact()), std::string{kSentinelArchive})
        << "a hit must recreate the artifact in a cleaned tree";
}

// ★★★ THE PIN THIS WHOLE MECHANISM WAITED FOR
// `CompilationUnit::inputDigest()` TO MAKE POSSIBLE. The edited header is named
// by NO manifest: not by the dependency's `sources[]`, not by the consumer's,
// not by any config document. Every term a manifest-level key could reach is
// byte-identical across the two builds. If the key does not move, the sentinel
// is served and the build silently links an archive compiled against the old
// header — which is the exact silent-miscompile class this row exists to
// prevent.
TEST(DependencyArtifactCacheWarm, AHeaderNoManifestNamesMovesTheKey) {
    ScratchDir scratch{Location::Temp, "dep-artifact-cache-header"};
    fs::path const cacheRoot = scratch.path() / "cache";
    ScopedEnv const cacheEnv{kCacheDirVar, cacheRoot.string()};

    Fixture const fx{scratch.path() / "tree", "retain"};
    DiagnosticReporter cold;
    ASSERT_EQ(fx.build(cold), 0);
    auto const first = keyDocumentsUnder(cacheRoot);
    ASSERT_EQ(first.size(), 1u);

    // The stale entry is made UNMISTAKABLE. If the key does not move, this is
    // what the second build's artifact will contain.
    writeText(artifactBeside(first.front()), kSentinelArchive);

    // The ONE edit. `dep_impl.h` appears in no `sources[]` anywhere.
    writeText(fx.depDir / "dep_impl.h",
              "#ifndef DEP_IMPL_H\n#define DEP_IMPL_H\n"
              "#define DSS_DEP_BIAS 7\n#endif\n");

    DiagnosticReporter second;
    ASSERT_EQ(fx.build(second), 0);
    EXPECT_NE(readBytes(fx.depArtifact()), std::string{kSentinelArchive})
        << "EDITING A HEADER NO MANIFEST NAMES MUST MOVE THE KEY. Serving the "
           "sentinel here means the input closure is not in the key and the "
           "cache ships bytes compiled against a header that has changed.";

    auto const after = keyDocumentsUnder(cacheRoot);
    EXPECT_EQ(after.size(), 2u)
        << "under 'retain' the moved key must MINT a second entry and keep the "
           "first: " << after.size() << " entrie(s) found";
}

// ★ THE THIRD CONFIG MEMBER, DRIVEN. `retain` is pinned by the case above; this
// is its opposite arm on the identical edit, so the two together show the token
// DECIDES something rather than merely parsing.
TEST(DependencyArtifactCacheEvictionPolicy, PruneSupersededRemovesTheOldEntry) {
    ScratchDir scratch{Location::Temp, "dep-artifact-cache-prune"};
    fs::path const cacheRoot = scratch.path() / "cache";
    ScopedEnv const cacheEnv{kCacheDirVar, cacheRoot.string()};

    Fixture const fx{scratch.path() / "tree", "prune-superseded"};
    DiagnosticReporter cold;
    ASSERT_EQ(fx.build(cold), 0);
    ASSERT_EQ(keyDocumentsUnder(cacheRoot).size(), 1u);

    writeText(fx.depDir / "dep_impl.h",
              "#ifndef DEP_IMPL_H\n#define DEP_IMPL_H\n"
              "#define DSS_DEP_BIAS 11\n#endif\n");

    DiagnosticReporter second;
    ASSERT_EQ(fx.build(second), 0);
    EXPECT_EQ(keyDocumentsUnder(cacheRoot).size(), 1u)
        << "'prune-superseded' must leave ONE current entry per artifact stem. "
           "Two means the policy never reached the store; zero means it pruned "
           "the entry it had just written.";
}

// ⛔ A BUILD THAT REPORTED AN ERROR MUST NOT ENTER THE CACHE. Storing one would
// put a FAILING build's bytes in the cache; the next build would serve them,
// emit none of the errors (they belonged to the previous run) and go GREEN —
// the silent wrong answer this whole mechanism exists to make impossible,
// reached through the cache instead of through the key.
//
// ⚠⚠ AND THIS CASE DOES **NOT** WITNESS `compileOneTarget`'s
// `!reporter.hasErrors()` STORE GUARD — SAID HERE BECAUSE THE OBVIOUS READING
// IS THAT IT DOES. ✔MEASURED by REMOVE-direction mutation: delete that guard
// and this case stays GREEN (build rc 0, `program.cpp.obj` md5 moved and
// returned). It fails BEFORE the dependency's artifact is written, so nothing
// is stored whether a guard exists or not, and what it actually pins is the
// weaker "a failed graph leaves no entry behind".
//
// ✔TWO ROUTES TO THE STATE THE GUARD IS FOR — an artifact WRITTEN while the
// per-target reporter holds errors — WERE MEASURED AND BOTH FAIL EARLIER: a
// `stackReserve` the format cannot carry, and an `S_UnknownAttribute` warning
// promoted by `warningsAsErrors`. Both abort before the write. The guard is
// kept as defence in depth on `runCusToTargets`'s own stated conjunction
// (*"a scratch error alongside a written file means the build is failing"*),
// and it is recorded as UNWITNESSED rather than assumed — the same discipline
// `CompilationUnit::inputDigest()`'s `#pragma pack` term is held to. If a route
// is found, pin it here.
TEST(DependencyArtifactCacheCold, AGraphThatFailsBeforeItsArtifactStoresNothing) {
    ScratchDir scratch{Location::Temp, "dep-artifact-cache-failing"};
    fs::path const cacheRoot = scratch.path() / "cache";
    ScopedEnv const cacheEnv{kCacheDirVar, cacheRoot.string()};

    Fixture const fx{scratch.path() / "tree", "prune-superseded",
                     /*depStackReserve=*/8u * 1024u * 1024u};
    DiagnosticReporter rep;
    ASSERT_NE(fx.build(rep), 0)
        << "the fixture must actually FAIL, or this case asserts nothing";
    ASSERT_FALSE(fs::exists(fx.depArtifact()))
        << "this arm is ABOUT the pre-write failure — RECORDED as an assertion "
           "rather than left implicit, because the day the artifact IS written "
           "here is the day this case starts witnessing the store guard, and a "
           "reader must be told which of the two it is looking at";

    EXPECT_TRUE(keyDocumentsUnder(cacheRoot).empty())
        << "a graph that failed must leave no entry behind";
}

// ⚠ A SECOND ARM WAS WRITTEN AND DELETED, AND THAT IS RECORDED RATHER THAN
// QUIETLY DROPPED. It drove `S_UnknownAttribute` under `warningsAsErrors` to
// reach "artifact written WHILE the reporter holds errors". ✔MEASURED: the
// artifact is NOT written — the promoted error aborts before the write, exactly
// as the `stackReserve` route does. A pin whose precondition cannot hold is a
// permanent red, and a pin that drops the precondition is a vacuous green; the
// honest third option is to state that the guard is unwitnessed and say what
// was tried, which is what the note above does.

// ⛔ THE ONE ARM THAT STOPS A BUILD. An artifact whose key document is missing
// cannot be shown to be this key's, and treating that as a miss would restore
// the un-verified 80-bit behaviour by the back door — the check would be
// optional, hence not a check.
TEST(DependencyArtifactCacheWarm, AnEntryWithNoKeyDocumentFailsTheBuild) {
    ScratchDir scratch{Location::Temp, "dep-artifact-cache-refuse"};
    fs::path const cacheRoot = scratch.path() / "cache";
    ScopedEnv const cacheEnv{kCacheDirVar, cacheRoot.string()};

    Fixture const fx{scratch.path() / "tree", "prune-superseded"};
    DiagnosticReporter cold;
    ASSERT_EQ(fx.build(cold), 0);
    auto const entries = keyDocumentsUnder(cacheRoot);
    ASSERT_EQ(entries.size(), 1u);
    ASSERT_TRUE(fs::exists(artifactBeside(entries.front())));

    std::error_code ec;
    fs::remove(entries.front(), ec);   // the sidecar only; the artifact stays
    ASSERT_FALSE(ec);

    DiagnosticReporter refused;
    EXPECT_NE(fx.build(refused), 0)
        << "an artifact with no key document beside it must REFUSE, not be "
           "quietly recompiled around — a missing sidecar is exactly the state "
           "that makes the 16-character path index safe to truncate.";
    EXPECT_GE(countCode(refused, DiagnosticCode::D_FileReadFailed), 1u)
        << "the refusal must reach the operator as a diagnostic";
}

}  // namespace
