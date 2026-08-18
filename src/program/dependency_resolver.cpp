#include "program/dependency_resolver.hpp"

#include "core/substrate/path_identity.hpp"
#include "core/types/artifact_profile.hpp"
#include "core/types/config_path_walk.hpp"   // findShippedConfigDir
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"
#include "program/build_scripts.hpp"
#include "program/cross_validate_language_target.hpp"
#include "program/cross_validate_target_format.hpp"
#include "program/dependency_cache.hpp"
#include "program/program.hpp"
#include "program/project_sources.hpp"
#include "program/target_spec.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

namespace dss {
namespace {

namespace fs = std::filesystem;

// ── small shared shapes ─────────────────────────────────────────────────────

void emitDriverError(DiagnosticReporter& rep, DiagnosticCode code,
                     std::string msg) {
    report(rep, code, DiagnosticSeverity::Error, std::move(msg));
}

// The three codes M5 keeps OUT of `kUnsuppressableCodes` and gives
// `DiagnosticDelivery::Guaranteed` instead (0xD019 / 0xD01B / 0xD01C), plus the
// two AP6 additions that share their shape. Delivery and suppression are
// DIFFERENT questions (`unsuppressable_codes.hpp` records the whole argument):
// these codes are each the ONLY statement of why the build stopped, so the
// reporter's cap must not be free to drop them on a diagnostic-heavy compile —
// but nothing distinguishes them from `D_FileNotFound` on the SUPPRESSION axis,
// so they do not join the closed table to obtain it.
void emitGuaranteed(DiagnosticReporter& rep, DiagnosticCode code,
                    std::string msg) {
    ParseDiagnostic d;
    d.code     = code;
    d.severity = DiagnosticSeverity::Error;
    d.delivery = DiagnosticDelivery::Guaranteed;
    d.actual   = std::move(msg);
    rep.report(std::move(d));
}

// `weakly_canonical` and never `canonical`: a `path` dependency is resolved
// before anything has confirmed the directory exists, and the cycle key must
// still be computable for the reject's own message. Falls back to the lexically
// normalized form when the filesystem refuses to answer — a degraded key is
// strictly better than an exception out of a resolver, and the only thing it
// costs is that two exotic spellings of one directory might read as two nodes
// (which produces a duplicate build, never a wrong one).
[[nodiscard]] fs::path canonicalize(fs::path const& p) {
    return core::PathIdentity::of(p).path();
}

// The de-dup key for a source path. Same rule `expandAndDedupProjectSources`
// uses, and it has to be the same one: a merged build draws sources from two
// manifests, one contributing ABSOLUTE paths and the other RELATIVE ones, so
// absolute-vs-relative spellings of ONE file are the NORMAL case here rather
// than an exotic edge — and its consequence is a duplicate CU and a
// duplicate-symbol link error no diagnostic can tie back to a manifest.
[[nodiscard]] core::PathIdentity sourceKey(std::string const& spelling) {
    return core::PathIdentity::of(fs::path{spelling});
}

// A shipped object-format document, remembered under THE NAME IT WAS LOADED BY.
//
// ★ THE LOAD NAME IS THE FILENAME STEM, AND IT IS NOT INTERCHANGEABLE WITH THE
// DOCUMENT'S OWN `name` FIELD. `ObjectFormatSchema::loadShipped` keys on the
// FILENAME (`<name>.format.json`), and so does every `<target>:<format>` spec a
// user or this resolver writes — so the load name is the only string that can
// be handed back to the driver OR shown to an operator who has to go and look
// at the file. ✔MEASURED: a candidate list printed from `schema.name()` reported
// two DIFFERENT documents under ONE name (a duplicated format file carries the
// original's `name` field), i.e. the ambiguity report named the same format
// twice and told the reader nothing about which two files to look at.
struct ShippedFormat {
    std::string                                loadName;
    std::shared_ptr<ObjectFormatSchema const>  schema;
};

// One built dependency artifact, travelling up the graph.
//
// `absorbable` is the format's declared CONTAINER, resolved to the ONE question
// U-8 asks of it — never the profile name, and never the format's identity.
struct BuiltArtifact {
    fs::path path;
    // `isStaticArchive()` of the format this artifact was built with.
    bool     isStaticArchive = false;
};

// U-8 as corrected (plan v2 §2): does a build with `consumerFormat` ABSORB
// `art`, i.e. does the artifact stop travelling here?
//
// ✔MEASURED at `program.cpp`'s static-archive arm: an archive build folds in
// static archives ONLY (`extractStaticArchiveMembers`, every member,
// deliberately). A dynamic library handed to the same build goes to the per-CU
// FFI path and is "resolved at the FINAL link" — a link this build never
// performs, and an `ar` archive records no import — so the root would never
// learn of it and the reference would be undefined TWO HOPS from its cause.
//
// The fork is on the format's declared container and on nothing else: an
// image-flavor link resolves both kinds, so it absorbs everything.
[[nodiscard]] bool absorbs(ObjectFormatSchema const& consumerFormat,
                           BuiltArtifact const&      art) noexcept {
    if (!consumerFormat.isStaticArchive()) return true;
    return art.isStaticArchive;
}

// ── the resolver ────────────────────────────────────────────────────────────

class Resolver {
public:
    Resolver(DependencyResolveRequest const& request, IGitRunner& git,
             DiagnosticReporter& rep)
        : req_(request), git_(git), rep_(rep) {}

    [[nodiscard]] std::optional<DependencyResolution>
    run(ProjectConfig const& rootConfig);

private:
    // One resolved manifest. Nodes are created once per CANONICAL manifest
    // path, so a diamond is ONE node with two parents — which is exactly why a
    // revisit is not a cycle and must not diagnose.
    struct Node {
        fs::path              manifestPath;   // canonical
        fs::path              dir;            // canonical manifest directory
        ProjectConfig         config;
        // Meaningless for the root (index 0), which is always a BUILD.
        DependencyComposition composition = DependencyComposition::NotConsumable;
        // U-9 / the output-name registry: the canonical directory's own name.
        std::string           outputName;
        std::vector<std::size_t> children;    // manifest order
        // Expanded against `dir` (B.3). EMPTY for the root: the driver expands
        // the root's own `sources[]` itself, after ITS pre-build hooks.
        std::vector<std::string> ownSources;
        // The dependency's own language document, already loaded by
        // `admitNode_` for the AP2 gate and KEPT rather than re-loaded: phase 2
        // needs it per (node × consumer target) for the ISA gate
        // (D-ISA-LANGUAGE-BOUND-TO-ARCHITECTURE), and re-loading per pair would
        // re-parse one document once per target of every consumer. NULL only
        // for the ROOT (index 0), whose grammar is `compileProject`'s to load
        // and whose ISA gate is the DRIVER's arm — see `gather_`.
        std::shared_ptr<GrammarSchema const> grammar;
    };

    // What one subtree contributes to the build that encloses it.
    struct Gathered {
        // Everything that build must link. Includes artifacts it cannot
        // absorb: they are still needed to resolve ITS OWN externs, and
        // withholding them would trade a link error for a compile error.
        std::vector<BuiltArtifact> forThisBuild;
    };

    // phase 1 — the graph walk
    [[nodiscard]] bool walkChildren_(std::size_t parent, std::size_t depth);
    [[nodiscard]] std::optional<std::size_t>
    admitNode_(std::size_t parent, fs::path const& manifestPath,
               std::size_t depth);
    [[nodiscard]] std::optional<fs::path>
    locateEntry_(Node const& parent, DependencyEntry const& entry,
                 std::optional<std::string> const& acquiredGitName);
    [[nodiscard]] bool prepareNodeSources_(std::size_t index);
    [[nodiscard]] bool registerOutputName_(Node& node);

    // phase 2 — per consumer target
    [[nodiscard]] std::optional<Gathered>
    gather_(std::size_t index, std::string const& consumerSpec,
            TargetSchema const& target, ObjectFormatSchema const& enclosingFmt);
    [[nodiscard]] std::optional<ShippedFormat>
    deriveFormat_(Node const& node, std::string const& consumerSpec,
                  TargetSchema const& target,
                  ObjectFormatSchema const& consumerFmt);
    [[nodiscard]] std::optional<fs::path>
    buildNode_(std::size_t index, std::string const& consumerSpec,
               TargetSchema const&                        target,
               ShippedFormat const&                       depFmt,
               std::vector<BuiltArtifact> const&          inputs);

    // shared
    [[nodiscard]] DependencyCache* cache_();
    [[nodiscard]] std::span<ShippedFormat const> shippedFormats_();
    void collectMergeSources_(std::size_t index, std::vector<std::string>& out,
                              std::set<core::PathIdentity>& seen) const;
    [[nodiscard]] std::vector<std::string> buildSourcesFor_(std::size_t index) const;

    DependencyResolveRequest const& req_;
    IGitRunner&                     git_;
    DiagnosticReporter&             rep_;

    std::vector<Node>                     nodes_;
    std::map<std::string, std::size_t>    nodeByManifest_;   // canonical → index
    std::vector<std::size_t>              stack_;            // DFS, cycle keys
    std::map<std::string, std::string>    outputNameOwner_;  // name → canonical dir

    // Lazily opened: a graph with no git entry must not create `.dss-deps`, and
    // that is also what makes `--force-git-cache` the silent no-op U-10 asks
    // for rather than a flag that materializes a cache directory.
    std::optional<DependencyCache> cache_slot_;
    bool                           cacheOpenFailed_ = false;
    bool                           cacheUsed_       = false;

    // Memoized, because a diamond must be BUILT once per (node, target) and not
    // once per path that reaches it.
    std::map<std::pair<std::size_t, std::string>, std::vector<BuiltArtifact>>
        gatherMemo_;
    std::map<std::pair<std::size_t, std::string>, fs::path> artifactMemo_;

    // Loaded once; the derivation runs over it per (kind, target, profile).
    std::vector<ShippedFormat> formats_;
    bool                       formatsLoaded_ = false;
    std::map<std::string, ShippedFormat> derivedMemo_;
};

// ── the shipped object-format inventory ─────────────────────────────────────
//
// The derivation searches the FORMATS THAT SHIP, because "which format produces
// this profile" is a question only the config directory can answer. It is read
// ONCE per resolve and reused: 24 documents × every (dependency × target) pair
// would otherwise be re-parsed for an answer that cannot change mid-build.
std::span<ShippedFormat const>
Resolver::shippedFormats_() {
    if (formatsLoaded_) return formats_;
    formatsLoaded_ = true;

    auto const dir = findShippedConfigDir("object-formats");
    if (!dir) {
        emitDriverError(rep_, DiagnosticCode::D_SchemaLoadFailed,
                        "the shipped object-format directory "
                        "('src/dss-config/object-formats') could not be "
                        "located, so no object format can be derived for a "
                        "dependency. Set DSS_CONFIG_ROOT to the directory that "
                        "contains 'src/dss-config', or run from inside the "
                        "compiler's source tree.");
        return formats_;
    }
    std::error_code ec;
    std::vector<std::string> names;
    for (fs::directory_iterator it{*dir, ec}, end; !ec && it != end;
         it.increment(ec)) {
        std::string const file = it->path().filename().string();
        constexpr std::string_view kSuffix = ".format.json";
        if (file.size() <= kSuffix.size()) continue;
        if (!std::string_view{file}.ends_with(kSuffix)) continue;
        names.push_back(file.substr(0, file.size() - kSuffix.size()));
    }
    // Sorted so the candidate ORDER — and therefore the wording of an
    // ambiguity report — is identical on every host, rather than inheriting
    // whatever order the filesystem enumerated in.
    std::sort(names.begin(), names.end());
    for (auto const& n : names) {
        auto loaded = ObjectFormatSchema::loadShipped(n);
        // A shipped document that will not load is NOT swallowed into a
        // narrower candidate set in silence: it simply cannot serve any
        // profile, and the zero-candidate arm (0xD022) reports the full search
        // key so the operator can check the directory themselves. Reporting
        // per-file here would put a load error for every unrelated format in
        // front of every dependency build.
        if (loaded.has_value()) formats_.push_back(ShippedFormat{n, *loaded});
    }
    return formats_;
}

DependencyCache* Resolver::cache_() {
    if (cache_slot_.has_value()) return &*cache_slot_;
    if (cacheOpenFailed_) return nullptr;
    fs::path const rootDir = nodes_.front().dir;
    auto opened = DependencyCache::open(rootDir, git_, req_.forceGitCache, rep_);
    if (!opened) {
        // `C_MalformedJson` is already on the reporter. Note what the API shape
        // makes structural: with no cache object there is no `acquire`, so a
        // corrupt lockfile CANNOT reach the network.
        cacheOpenFailed_ = true;
        return nullptr;
    }
    cache_slot_ = std::move(opened);
    return &*cache_slot_;
}

// ── PHASE 1: THE GRAPH WALK ─────────────────────────────────────────────────

// Locate the manifest a single `dependsOn` entry names.
//
// M6 — 0xD019 KEEPS THE DISTINCTION IT WAS MINTED FOR. Its allocation is
// remediation-distinct from `D_FileNotFound` BECAUSE the thing you named IS
// there but is not a DSS project — overwhelmingly a wrong-LEVEL path
// (`../lib/src` for `../lib`). So a `path` naming a directory that does not
// exist is `D_FileNotFound`; ONLY "the directory exists and has no
// `.dss-project.json` at its root" is 0xD019. Collapsing the two would send
// every reader of a typo'd path looking for a manifest they never had.
std::optional<fs::path>
Resolver::locateEntry_(Node const& parent, DependencyEntry const& entry,
                       std::optional<std::string> const& acquiredCheckout) {
    fs::path dir;
    std::string spelling;
    if (entry.path.has_value()) {
        spelling = *entry.path;
        fs::path const raw{spelling};
        // B.3: a DEPENDENCY path is relative to the CONSUMING manifest's own
        // directory. Anything else makes a dependency's meaning depend on
        // where the depender happened to be invoked from.
        dir = canonicalize(raw.is_absolute() ? raw : (parent.dir / raw));
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            emitDriverError(
                rep_, DiagnosticCode::D_FileNotFound,
                "project 'dependsOn': path dependency '" + spelling
                + "' declared by '" + parent.manifestPath.generic_string()
                + "' does not name a directory (looked at '"
                + dir.generic_string()
                + "'). A path entry names the dependency's project DIRECTORY, "
                  "resolved against the directory of the manifest that "
                  "declares it.");
            return std::nullopt;
        }
    } else {
        // A git entry: the checkout `DependencyCache::acquire` produced.
        spelling = entry.git.value_or(std::string{});
        dir      = canonicalize(fs::path{*acquiredCheckout});
    }

    fs::path const manifest = dir / std::string{kDependencyManifestName};
    std::error_code ec;
    if (!fs::is_regular_file(manifest, ec)) {
        emitGuaranteed(
            rep_, DiagnosticCode::D_DependencyManifestNotFound,
            "project 'dependsOn': '" + spelling + "' (declared by '"
            + parent.manifestPath.generic_string()
            + "') resolves to the directory '" + dir.generic_string()
            + "', but that directory holds no '"
            + std::string{kDependencyManifestName}
            + "'. The directory exists — it is simply not a DSS project. The "
              "usual cause is a path one level too deep (name the project "
              "directory, not its 'src'); the other is a dependency that has "
              "no manifest yet, in which case add '"
            + (manifest).generic_string() + "'.");
        return std::nullopt;
    }
    return canonicalize(manifest);
}

// The output/cache NAME a dependency's artifacts are filed under (U-9), derived
// UNIFORMLY for every node from its canonical manifest DIRECTORY's own name.
//
// ★ WHY UNIFORMLY, AND WHY IT NEEDS A DIAGNOSTIC AT ALL. U-9 calls
// `deps/<derived-dep-name>` "collision-free by construction", and for a GIT
// dependency it is: U-5 derives the name from the URL and 0xD020 rejects a
// collision between two distinct `(url, ref)` pairs BEFORE anything is fetched.
// For a `path` dependency nothing derives a name and nothing detects a
// collision — `../a/util` and `../b/util` would both want `deps/util`, and the
// second build would silently overwrite the first's artifact, so the consumer
// would link a library it never asked for with nothing indicating a
// substitution. That is 0xD020's own hazard with the detection removed.
//
// Deriving from the canonical DIRECTORY name makes the two agree by
// construction rather than by coincidence: a git dependency's directory IS
// `.dss-deps/<name>`, so this reproduces U-5's name exactly and those names
// stay 0xD020-checked on top.
bool Resolver::registerOutputName_(Node& node) {
    std::string name = node.dir.filename().string();
    // A canonical directory can still yield an unusable component — a
    // filesystem root has an EMPTY filename, and a name of `.` or `..` would
    // resolve on top of, or outside, the `deps/` tree. Fail CLOSED: the
    // alternative is writing an artifact somewhere the consumer will never look
    // for it.
    if (name.empty() || name == "." || name == "..") {
        emitGuaranteed(
            rep_, DiagnosticCode::D_DependencyOutputNameCollision,
            "project 'dependsOn': the dependency at '"
            + node.dir.generic_string()
            + "' derives no usable output name from its directory (got '" + name
            + "'). Dependency artifacts are filed under '"
            + std::string{kDependencyOutputDirName}
            + "/<name>/', and the name is the dependency directory's own name. "
              "Move the project into a named directory.");
        return false;
    }
    auto const [it, inserted] =
        outputNameOwner_.emplace(name, node.dir.generic_string());
    if (!inserted && it->second != node.dir.generic_string()) {
        emitGuaranteed(
            rep_, DiagnosticCode::D_DependencyOutputNameCollision,
            "project 'dependsOn': two different dependencies both derive the "
            "output name '" + name + "' — '" + it->second + "' and '"
            + node.dir.generic_string()
            + "'. Their artifacts would both be written to '"
            + std::string{kDependencyOutputDirName} + "/" + name
            + "/', so whichever is built second would silently overwrite the "
              "first and the consumer would link a library it did not ask for. "
              "Rename one of the two dependency directories.");
        return false;
    }
    node.outputName = std::move(name);
    return true;
}

// Run the node's own pre-build hooks and expand its `sources[]`.
//
// U-7: pre-build hooks run for EVERY resolved dependency — a `module` whose
// sources are generated is the motivating case, and it is why `runBuildScripts`
// took `cwd` as a PARAMETER from the day it landed. They run in THAT
// dependency's own directory, so a codegen step writes into the tree that
// declared it rather than into whichever tree the compiler was invoked from.
//
// ORDER: hooks BEFORE expansion, for the identical reason the root's do — the
// overwhelmingly common hook GENERATES sources a pattern then matches, and
// expansion is fail-loud on zero matches.
bool Resolver::prepareNodeSources_(std::size_t index) {
    Node& node = nodes_[index];
    if (!runBuildScripts(node.config.preBuildScripts, node.dir, rep_)) {
        return false;
    }
    auto expanded =
        expandAndDedupProjectSources(node.config.sources, node.dir, rep_);
    if (!expanded) return false;
    node.ownSources = *std::move(expanded);
    return true;
}

std::optional<std::size_t>
Resolver::admitNode_(std::size_t parent, fs::path const& manifestPath,
                     std::size_t depth) {
    std::string const key = manifestPath.generic_string();

    // ── CYCLE vs DIAMOND, and they are answered by two different tables ──────
    // ON THE STACK ⇒ a cycle: fail loud with the PATH as payload, because a
    // bare "cycle detected" on a deep graph is nearly unactionable. ALREADY
    // VISITED but NOT on the stack ⇒ a diamond, i.e. a legitimate shared
    // dependency, and it must NOT diagnose — the memo answers it.
    for (std::size_t i = 0; i < stack_.size(); ++i) {
        if (nodes_[stack_[i]].manifestPath.generic_string() != key) continue;
        std::string cycle;
        for (std::size_t j = i; j < stack_.size(); ++j) {
            cycle += nodes_[stack_[j]].manifestPath.generic_string();
            cycle += "\n    -> ";
        }
        cycle += key;
        emitDriverError(
            rep_, DiagnosticCode::D_DependencyCycle,
            "project 'dependsOn': the dependency graph contains a CYCLE:\n    "
            + cycle
            + "\nResolution is refused rather than breaking the back edge and "
              "continuing: a silently-broken cycle makes the resolved "
              "dependency set depend on where the walk started, so two targets "
              "of one build could legitimately see different source sets. "
              "Break the cycle by moving the shared code into a project both "
              "sides depend on.");
        return std::nullopt;
    }
    if (auto const seen = nodeByManifest_.find(key);
        seen != nodeByManifest_.end()) {
        return seen->second;  // DIAMOND — silent, deliberately
    }

    // The depth cap. `stack_` catches cycles; nothing catches a deep acyclic
    // graph, and on this project's own standing note that class is CI-invisible
    // (it passes the Release legs and surfaces on MSVC-Debug) — as a process
    // death with no diagnostic at all.
    if (depth > kMaxDependencyDepth) {
        std::string chain;
        for (std::size_t const s : stack_) {
            chain += "\n    " + nodes_[s].manifestPath.generic_string();
        }
        emitDriverError(
            rep_, DiagnosticCode::D_DependencyGraphTooDeep,
            "project 'dependsOn': the dependency graph nests deeper than "
            + std::to_string(kMaxDependencyDepth)
            + " levels, which this resolver refuses to walk. The chain so far "
              "is:" + chain + "\n    " + key
            + "\nThe limit is a budget, not a measurement: no hand-written "
              "project nests prerequisites this deep, so a graph that does is "
              "either generated or wrong. Flatten it, or depend on the shared "
              "projects directly.");
        return std::nullopt;
    }

    auto loaded = loadProjectConfig(manifestPath, rep_);
    if (!loaded) return std::nullopt;   // C_MalformedJson / C_MissingField etc.

    Node node;
    node.manifestPath = manifestPath;
    node.dir          = manifestPath.parent_path();
    node.config       = *std::move(loaded);

    // ── THE DEPENDENCY'S OWN AP2 LANGUAGE GATE (plan v2 §3 item 1) ───────────
    // ✔`toy` declares `["cli"]` only, so a `toy` manifest saying `"module"`
    // passes the LANGUAGE-BLIND composition lookup below and would be
    // source-merged with no gate anywhere. The root gets this gate in
    // `compileProject`; a dependency is never routed through that function
    // (M3), so it has to get it here or not at all.
    {
        auto grammarR = GrammarSchema::loadShipped(node.config.language);
        if (!grammarR.has_value()) {
            forwardConfigDiagnostics(grammarR.error(), rep_);
            emitDriverError(rep_, DiagnosticCode::D_SchemaLoadFailed,
                            "dependency '" + key + "' names language '"
                            + node.config.language
                            + "', whose schema could not be loaded — the "
                              "reason is in the configuration diagnostic(s) "
                              "above (config: src/dss-config/sources/"
                            + node.config.language + ".lang.json).");
            return std::nullopt;
        }
        if (!enforceArtifactProfile((*grammarR)->artifactProfiles(),
                                    node.config.artifactProfile,
                                    node.config.language, rep_)) {
            return std::nullopt;
        }
        // KEPT for phase 2's ISA gate
        // (D-ISA-LANGUAGE-BOUND-TO-ARCHITECTURE). The document is already
        // parsed here; dropping it and re-loading per (node × consumer target)
        // would re-parse one `.lang.json` once per target of every consumer,
        // and — worse — would put a SECOND load site on a path whose first one
        // owns the failure message.
        //
        // ⚠ The ISA verdict CANNOT be taken here. This function runs ONCE per
        // canonical manifest, in phase 1, where no consumer target exists yet;
        // the verdict is a relation between this language and a TARGET, so it
        // belongs where the target is known. Taking it here would mean
        // answering a per-target question with whatever target happened to be
        // first — the same class of mistake as `deriveFormat_`'s memo key.
        node.grammar = *grammarR;
    }

    // ── COMPOSITION: THE VERB, NEVER THE NAME (B.2) ──────────────────────────
    auto const verb = dependencyCompositionForProfile(node.config.artifactProfile);
    if (!verb.has_value()) {
        // NOT `NotConsumable`. Every enumerator is a valid INSTRUCTION, that
        // one included ("reject — the profile is terminal"), so an unregistered
        // name must produce an unknown-NAME complaint rather than a
        // confidently wrong claim about a profile that does not exist.
        emitDriverError(
            rep_, DiagnosticCode::C_UnknownArtifactProfile,
            "dependency '" + key + "' declares artifactProfile '"
            + node.config.artifactProfile
            + "', which is not a registered profile (registered: "
            + registeredArtifactProfileList() + ").");
        return std::nullopt;
    }
    node.composition = *verb;
    Node const& consumer = nodes_[parent];
    switch (node.composition) {
        case DependencyComposition::NotConsumable:
            // ★ THIS GATE RUNS BEFORE THE FORMAT DERIVATION, AND THE ORDER IS A
            // CORRECTNESS DEPENDENCY RATHER THAN A CONVENIENCE. ✔MEASURED: the
            // triple (elf, cli, x86_64) has TWO qualifying formats — the `exec`
            // and `pie` documents, both declaring `artifactProfiles: ["cli"]`
            // and the same machine — so uniqueness holds ONLY because `cli` is
            // `NotConsumable` and is rejected here first. Reorder these and
            // 0xD023 becomes genuinely reachable, with a real ambiguity, for
            // the commonest profile in the repo.
            emitGuaranteed(
                rep_, DiagnosticCode::D_DependencyArtifactProfileUnsupported,
                "project 'dependsOn': dependency '" + key
                + "' declares artifactProfile '" + node.config.artifactProfile
                + "', which produces a TERMINAL deliverable — something a "
                  "person or a platform runs, not something another build "
                  "consumes. Absorbing it as sources would splice its entry "
                  "point into the consumer, and there is no artifact to link "
                  "against. Depend on a project whose profile composes "
                  "(source-merge or artifact-link), or split the reusable half "
                  "of this one out.");
            return std::nullopt;
        case DependencyComposition::SourceMerge:
            // Merged sources are parsed by the CONSUMER's grammar, so a
            // language difference is not a preference mismatch but a guaranteed
            // parse failure — one that would otherwise surface as a pile of
            // `P_UnexpectedToken` pointing into a file the user never wrote and
            // may not know is in the build.
            if (node.config.language != consumer.config.language) {
                emitGuaranteed(
                    rep_, DiagnosticCode::D_DependencyLanguageMismatch,
                    "project 'dependsOn': source-merge dependency '" + key
                    + "' declares language '" + node.config.language
                    + "', but the consumer '"
                    + consumer.manifestPath.generic_string()
                    + "' compiles '" + consumer.config.language
                    + "'. A source-merge dependency's translation units join "
                      "the consumer's own set and are parsed by the consumer's "
                      "grammar, so this could only fail as a parse error inside "
                      "a file the consumer's author did not write. (A BUILT "
                      "artifact has no such constraint — cross-language linking "
                      "is exactly what the artifact-link profiles are for.)");
                return std::nullopt;
            }
            break;
        case DependencyComposition::ArtifactLink:
            // No language claim, deliberately: cross-language linking against a
            // built artifact is the entire point of the FFI surface.
            break;
    }

    nodes_.push_back(std::move(node));
    std::size_t const index = nodes_.size() - 1;
    nodeByManifest_.emplace(key, index);
    if (!registerOutputName_(nodes_[index])) return std::nullopt;

    stack_.push_back(index);
    bool const ok = walkChildren_(index, depth + 1);
    stack_.pop_back();
    if (!ok) return std::nullopt;

    // Children first, then this node's own hooks + expansion: a dependency's
    // codegen step may legitimately run a tool that one of ITS dependencies
    // produced, and nothing here can run before the thing it consumes exists.
    if (!prepareNodeSources_(index)) return std::nullopt;
    return index;
}

bool Resolver::walkChildren_(std::size_t parent, std::size_t depth) {
    // ★ BY VALUE, NOT BY REFERENCE, AND IT IS A CORRECTNESS BUG THE OTHER WAY.
    // `admitNode_` below `push_back`s into `nodes_`, which REALLOCATES the
    // vector and invalidates any reference into an element of it — so a
    // reference to `nodes_[parent].config.dependsOn` would dangle the moment
    // the FIRST child was admitted, and the loop would then read freed memory
    // for every entry after it. ✔MEASURED before this copy: a manifest with two
    // colliding `path` entries walked only the first and reported no collision,
    // i.e. the fail-loud guard was skipped by undefined behaviour rather than
    // by a wrong condition. The entry list is a handful of small strings; the
    // copy is not worth an invariant nobody can see.
    std::vector<DependencyEntry> const entries = nodes_[parent].config.dependsOn;
    if (entries.empty()) return true;

    // ── M7: REGISTER EVERY GIT ENTRY OF THIS NODE BEFORE ACQUIRING ANY ───────
    // 0xD020 and 0xD024 fire on the DERIVED NAMES, before a single byte is
    // fetched. Interleaving registration with acquisition makes a collision
    // detectable only after a clone has already run — and for the same-url /
    // different-ref shape it makes it undetectable at ALL, because the second
    // entry's checkout target already exists and reads as a cache hit.
    std::vector<std::optional<std::string>> gitNames(entries.size());
    bool const hasGit = std::any_of(
        entries.begin(), entries.end(),
        [](DependencyEntry const& e) { return e.git.has_value(); });
    if (hasGit) {
        DependencyCache* const cache = cache_();
        if (cache == nullptr) return false;
        cacheUsed_ = true;
        // U-3: no degraded git-less mode for a project that needs git. Emitted
        // once per build however many entries asked.
        if (!cache->requireGit(rep_)) return false;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (!entries[i].git.has_value()) continue;
            auto name = cache->registerGitDependency(*entries[i].git,
                                                     entries[i].ref, rep_);
            if (!name) return false;   // 0xD020 / 0xD024 already reported
            gitNames[i] = *std::move(name);
        }
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (!gitNames[i].has_value()) continue;
            auto const got = cache->acquire(*gitNames[i], rep_);
            if (got.outcome == CacheOutcome::AcquireFailed) {
                return false;          // 0xD01E already reported
            }
            // The checkout path replaces the name for the locate step below.
            gitNames[i] = got.checkout.generic_string();
        }
    }

    for (std::size_t i = 0; i < entries.size(); ++i) {
        auto const manifest = locateEntry_(nodes_[parent], entries[i], gitNames[i]);
        if (!manifest) return false;
        auto const child = admitNode_(parent, *manifest, depth);
        if (!child) return false;
        // A manifest may name the same dependency twice; the graph edge is a
        // SET, so the second mention is the diamond case at one level.
        auto& kids = nodes_[parent].children;
        if (std::find(kids.begin(), kids.end(), *child) == kids.end()) {
            kids.push_back(*child);
        }
    }
    return true;
}

// ── PHASE 2: THE PER-TARGET DERIVATION AND BUILD ────────────────────────────

// B.10's derivation, over DECLARED facts only.
std::optional<ShippedFormat>
Resolver::deriveFormat_(Node const& node, std::string const& consumerSpec,
                        TargetSchema const&       target,
                        ObjectFormatSchema const& consumerFmt) {
    std::string const memoKey = std::string{objectFormatKindName(consumerFmt.kind())}
                              + "\x1f" + std::string{target.name()} + "\x1f"
                              + node.config.artifactProfile;
    if (auto const hit = derivedMemo_.find(memoKey); hit != derivedMemo_.end()) {
        return hit->second;
    }

    std::vector<ShippedFormat> candidates;
    for (auto const& f : shippedFormats_()) {
        if (f.schema->kind() != consumerFmt.kind()) continue;
        if (!artifactProfileSupported(f.schema->artifactProfiles(),
                                      node.config.artifactProfile)) {
            continue;
        }
        // ★ THE PROBE RUNS ON A THROWAWAY REPORTER, AND THAT IS NOT TIDINESS.
        // `crossValidateTargetFormat` is a REPORTER, not a predicate: it emits
        // `D_TargetMachineCodeMismatch` / `D_TargetAbiModelMismatch` on the
        // failing path. Used as a candidate FILTER it is deliberately run
        // against candidates EXPECTED to fail — every other architecture's
        // format of the same kind — so filtering with the live reporter would
        // print a mismatch line per rejected candidate and bury the real
        // verdict in noise it manufactured itself. Same `DiagnosticReporter
        // scratch{…}` shape the driver's own pair pre-flight uses.
        auto scratchCfg           = rep_.config();
        scratchCfg.maxDiagnostics = std::numeric_limits<std::size_t>::max();
        scratchCfg.maxPerCode     = std::numeric_limits<std::size_t>::max();
        scratchCfg.dedupWindow    = 0;
        DiagnosticReporter scratch{scratchCfg};
        if (!crossValidateTargetFormat(target, *f.schema, scratch)) continue;
        candidates.push_back(f);
    }

    if (candidates.size() == 1) {
        derivedMemo_.emplace(memoKey, candidates.front());
        return candidates.front();
    }
    if (candidates.empty()) {
        emitDriverError(
            rep_, DiagnosticCode::D_DependencyTargetFormatUnresolvable,
            "project 'dependsOn': no shipped object format can build dependency "
            "'" + node.manifestPath.generic_string() + "' for the consumer "
            "target '" + consumerSpec + "'. The search was: format kind '"
            + std::string{objectFormatKindName(consumerFmt.kind())}
            + "' (taken from the consumer's own format '"
            + std::string{consumerFmt.name()} + "'), serving the DEPENDENCY's "
              "artifactProfile '" + node.config.artifactProfile
            + "', and passing machine/ABI cross-validation against target '"
            + std::string{target.name()}
            + "' — no format satisfies all three, so this dependency cannot be "
              "built for this target. Ship (or declare) a format of that kind "
              "serving that profile, or drop the target from the consumer.");
        return std::nullopt;
    }
    std::string list;
    for (auto const& c : candidates) {
        if (!list.empty()) list += ", ";
        list += c.loadName;
    }
    emitDriverError(
        rep_, DiagnosticCode::D_DependencyTargetFormatAmbiguous,
        "project 'dependsOn': " + std::to_string(candidates.size())
        + " shipped object formats can build dependency '"
        + node.manifestPath.generic_string() + "' for the consumer target '"
        + consumerSpec + "' — " + list
        + ". The derivation must be UNIQUE: with two producers the pick decides "
          "the dependency's container, and therefore what the consumer's link "
          "actually absorbs, so choosing one silently would make the emitted "
          "artifact depend on a policy nobody declared. Decide which of those "
          "formats should stop declaring artifactProfile '"
        + node.config.artifactProfile + "'.");
    return std::nullopt;
}

// M4(b) applies inside a dependency's own build too: the node's OWN sources
// lead, then each `SourceMerge` descendant's, because `sourceFiles.front()`'s
// stem names the artifact when the manifest states no `artifactName`.
void Resolver::collectMergeSources_(std::size_t index,
                                    std::vector<std::string>& out,
                                    std::set<core::PathIdentity>& seen) const {
    Node const& node = nodes_[index];
    for (auto const& s : node.ownSources) {
        if (seen.insert(sourceKey(s)).second) out.push_back(s);
    }
    for (std::size_t const child : node.children) {
        if (nodes_[child].composition != DependencyComposition::SourceMerge) {
            continue;
        }
        collectMergeSources_(child, out, seen);
    }
}

std::vector<std::string> Resolver::buildSourcesFor_(std::size_t index) const {
    std::vector<std::string>     out;
    std::set<core::PathIdentity> seen;
    collectMergeSources_(index, out, seen);
    return out;
}

// Build ONE dependency for ONE consumer target, on a FRESH `Program` (M3).
std::optional<fs::path>
Resolver::buildNode_(std::size_t index, std::string const& consumerSpec,
                     TargetSchema const&               target,
                     ShippedFormat const&              depFmt,
                     std::vector<BuiltArtifact> const& inputs) {
    Node& node = nodes_[index];
    std::string const depSpec =
        std::string{target.name()} + ":" + depFmt.loadName;

    Program prog;
    // M3's propagation list, and each entry is here because its ABSENCE is a
    // SILENT difference rather than a loud one: a dependency built Debug under
    // a Release root links fine and behaves differently, and a dependency built
    // on the process's own stack while the root uses an injected executor makes
    // one half of a build nondeterministic in a way nothing reports.
    prog.setCompileConfig(req_.compileConfig);
    prog.setJobs(req_.jobs);
    prog.setExecutor(req_.executor);
    prog.setGitRunner(&git_);
    // U-9: `<consumer output base>/deps/<name>/<formatName>/<file>`. NEVER
    // inside the dependency's own tree, which may be read-only — the same
    // argument that puts ONE `.dss-deps` at the root consumer.
    prog.setOutputDir(req_.artifactOutputBase
                      / std::string{kDependencyOutputDirName} / node.outputName);
    prog.setPerFormatOutputSubdir(true);
    // The dependency's OWN manifest knobs. Its `targets[]` and `output` are
    // deliberately NOT read: the first is B.10's whole ruling, and the second
    // is U-9's.
    prog.setArtifactName(node.config.artifactName);
    prog.setIncludeDirs(node.config.includes);
    prog.setUserDefines(node.config.defines);
    prog.setResolveLibraries(node.config.resolveLibraries);
    prog.setStackReserveBytes(node.config.stackReserveBytes);
    if (!inputs.empty()) {
        std::vector<ResolveLibrarySpec> libs;
        libs.reserve(inputs.size());
        for (auto const& a : inputs) libs.push_back(ResolveLibrarySpec{a.path, {}});
        prog.setResolveLibraryAdditionsByTarget({{depSpec, std::move(libs)}});
    }

    std::vector<std::string> const sources = buildSourcesFor_(index);
    if (sources.empty()) {
        emitDriverError(rep_, DiagnosticCode::D_EmptyInput,
                        "dependency '" + node.manifestPath.generic_string()
                        + "' resolved to no source files at all, so there is "
                          "nothing to build for consumer target '"
                        + consumerSpec + "'.");
        return std::nullopt;
    }
    std::vector<std::string> const targets{depSpec};

    // A SCRATCH reporter, merged back with a context prefix naming the
    // dependency. The sub-build renders its own diagnostics live (its delegate
    // drains on the way out), and the merged copies are what make
    // `rep.errorCount()` and a caller's inspection see them AT ALL — attributed
    // to the dependency they came from, which the bare rendering cannot say.
    auto subCfg           = rep_.config();
    subCfg.maxDiagnostics = std::numeric_limits<std::size_t>::max();
    subCfg.maxPerCode     = std::numeric_limits<std::size_t>::max();
    subCfg.dedupWindow    = 0;
    DiagnosticReporter sub{subCfg};
    int const rc = routesToMultiUnit(sources.size())
                       ? prog.compileUnits(sources, node.config.language,
                                           targets, sub)
                       : prog.compileFiles(sources, node.config.language,
                                           targets, sub);
    std::string const prefix = "[dependency=" + node.outputName + " target="
                             + depSpec + "] ";
    for (auto const& d : sub.all()) {
        ParseDiagnostic copy = d;
        copy.contextPrefix   = prefix;
        rep_.report(std::move(copy));
    }
    if (rc != 0) {
        // 0xD029 and NOT 0xD022, which this site used to reuse
        // (D-DEPS-BUILD-FAILURE-REUSES-THE-DERIVATION-UNRESOLVABLE-CODE).
        // 0xD022 is the ZERO-CANDIDATE outcome of `deriveFormat_` — the
        // dependency never got compiled. Reaching HERE means the derivation
        // succeeded, produced `depFmt`, and the compile then failed inside the
        // dependency: a source problem, not a manifest one. The message below
        // is unchanged — the attribution it carries, and the prefixed inner
        // diagnostics merged just above, are the load-bearing half.
        emitDriverError(rep_, DiagnosticCode::D_DependencyBuildFailed,
                        "dependency '" + node.manifestPath.generic_string()
                        + "' failed to build for consumer target '"
                        + consumerSpec + "' (built as '" + depSpec
                        + "'). The reason is in the diagnostic(s) above, "
                          "prefixed with the dependency's name.");
        return std::nullopt;
    }

    auto const& produced = prog.artifactPaths();
    if (produced.size() != 1 || !produced.front().has_value()) {
        emitDriverError(rep_, DiagnosticCode::D_CompileUnitNullNoDiagnostic,
                        "dependency '" + node.manifestPath.generic_string()
                        + "' reported a successful build for '" + depSpec
                        + "' but produced no artifact path, so there is nothing "
                          "to link into consumer target '" + consumerSpec
                        + "'.");
        return std::nullopt;
    }
    return *produced.front();
}

std::optional<Resolver::Gathered>
Resolver::gather_(std::size_t index, std::string const& consumerSpec,
                  TargetSchema const&       target,
                  ObjectFormatSchema const& enclosingFmt) {
    auto const memoKey = std::pair{index, consumerSpec};
    if (auto const hit = gatherMemo_.find(memoKey); hit != gatherMemo_.end()) {
        return Gathered{hit->second};
    }

    std::vector<BuiltArtifact>   incoming;
    std::set<core::PathIdentity> seen;
    auto const add = [&](BuiltArtifact const& a) {
        if (seen.insert(core::PathIdentity::of(a.path)).second) {
            incoming.push_back(a);
        }
    };

    for (std::size_t const child : nodes_[index].children) {
        // ★ THE LANGUAGE↔TARGET ARCHITECTURE GATE, DEPENDENCY ARM
        // (D-ISA-LANGUAGE-BOUND-TO-ARCHITECTURE) — ONE SITE, ABOVE THE
        // COMPOSITION FORK, DELIBERATELY.
        //
        // Placed here and not in either arm below because the question is the
        // same for both and the §A.5 multi-site rule says the way to guarantee
        // that is ONE chokepoint rather than two copies that can drift.
        // Putting it in `deriveFormat_` — the other candidate, and the one that
        // looks natural because that IS the per-target derivation — would have
        // been wrong TWICE OVER, and both faults are measured properties of
        // that function rather than guesses:
        //   * `deriveFormat_` NEVER RUNS FOR `SourceMerge` (the arm below
        //     `continue`s before reaching it), so half the feature would have
        //     been silently unguarded; and
        //   * its memo key is (format kind × target × artifactProfile) and
        //     OMITS NODE IDENTITY, so one dependency's verdict would be served
        //     to a different dependency that shares a profile — a wrong answer,
        //     not merely a missed one.
        // `gather_`'s own memo is keyed (node × consumer target spec), which is
        // exactly the granularity this verdict has, so a memo hit re-serves an
        // answer computed for the SAME pair.
        //
        // ⓘ THE ROOT (index 0) IS NOT CHECKED HERE, and that is not an
        // omission: this loop walks a node's CHILDREN, so every node except the
        // root is judged, and the root's own language is judged by the DRIVER
        // arm in `compileOneTarget` — which is also the only arm a project with
        // no `dependsOn` at all ever reaches.
        //
        // ⓘ A node whose grammar could not be loaded is SKIPPED rather than
        // guessed at: `admitNode_` already reported `D_SchemaLoadFailed` and
        // abandoned the walk, so this is unreachable defensively — the same
        // "let the one owner report it" discipline `run()` applies to an
        // unparseable target spec.
        if (nodes_[child].grammar
            && !crossValidateLanguageTarget(
                   *nodes_[child].grammar, nodes_[child].config.language,
                   target, consumerSpec,
                   "project 'dependsOn': dependency '"
                       + nodes_[child].manifestPath.generic_string() + "'",
                   rep_)) {
            return std::nullopt;
        }
        if (nodes_[child].composition == DependencyComposition::SourceMerge) {
            // A source-merge node is NOT a build, so nothing is absorbed here:
            // its subtree's artifacts pass straight THROUGH to the build that
            // encloses it, which is why the enclosing format is handed down
            // unchanged. Getting this wrong yields an undefined symbol whose
            // cause is two hops away.
            auto sub = gather_(child, consumerSpec, target, enclosingFmt);
            if (!sub) return std::nullopt;
            for (auto const& a : sub->forThisBuild) add(a);
            continue;
        }
        // ArtifactLink: its own build, its own derived format.
        auto depFmt = deriveFormat_(nodes_[child], consumerSpec, target,
                                    enclosingFmt);
        if (!depFmt) return std::nullopt;
        auto inner = gather_(child, consumerSpec, target, *depFmt->schema);
        if (!inner) return std::nullopt;

        fs::path artifact;
        auto const builtKey = std::pair{child, consumerSpec};
        if (auto const done = artifactMemo_.find(builtKey);
            done != artifactMemo_.end()) {
            artifact = done->second;
        } else {
            auto produced = buildNode_(child, consumerSpec, target, *depFmt,
                                       inner->forThisBuild);
            if (!produced) return std::nullopt;
            artifact = *produced;
            artifactMemo_.emplace(builtKey, artifact);
        }
        add(BuiltArtifact{artifact, depFmt->schema->isStaticArchive()});
        // U-8: whatever the child's OWN build could not absorb keeps
        // travelling. An archive absorbs archives; it cannot absorb a shared
        // library, so that artifact continues up to a build that can.
        for (auto const& a : inner->forThisBuild) {
            if (!absorbs(*depFmt->schema, a)) add(a);
        }
    }

    gatherMemo_.emplace(memoKey, incoming);
    return Gathered{std::move(incoming)};
}

// ── THE ENTRY POINT ─────────────────────────────────────────────────────────

std::optional<DependencyResolution> Resolver::run(ProjectConfig const& rootConfig) {
    Node root;
    root.manifestPath = canonicalize(req_.rootManifestPath);
    root.dir          = root.manifestPath.parent_path();
    root.config       = rootConfig;
    // The root's own `outputName` is never used (its artifacts are not filed
    // under `deps/`), but it OWNS its directory name in the collision registry:
    // a dependency directory named the same as the consumer's would otherwise
    // be able to claim `deps/<consumer name>/` with no complaint, which reads
    // as the consumer's own output tree.
    nodes_.push_back(std::move(root));
    nodeByManifest_.emplace(nodes_.front().manifestPath.generic_string(),
                            std::size_t{0});
    if (!registerOutputName_(nodes_.front())) return std::nullopt;

    stack_.push_back(0);
    bool const walked = walkChildren_(0, 1);
    stack_.pop_back();
    if (!walked) return std::nullopt;

    DependencyResolution out;

    // The `SourceMerge` half. Starts at the ROOT'S CHILDREN, never at the root
    // itself: the driver expands the root's own `sources[]` AFTER its own
    // pre-build hooks and puts them FIRST (M4b).
    {
        std::set<core::PathIdentity> seen;
        for (std::size_t const child : nodes_.front().children) {
            if (nodes_[child].composition != DependencyComposition::SourceMerge) {
                continue;
            }
            collectMergeSources_(child, out.mergedSources, seen);
        }
    }

    // The `ArtifactLink` half, once per CONSUMER target.
    for (auto const& spec : req_.targets) {
        auto parsed = TargetSpec::parse(spec);
        // A spec that will not parse, a target or format that will not load:
        // SKIPPED here, exactly as the driver's own AP3 gate skips them, so the
        // authoritative `D_InvalidTargetSpec` / `D_SchemaLoadFailed` comes from
        // the one place that owns it. Such a target still fails the whole build
        // downstream, so nothing slips past.
        if (!parsed) continue;
        auto targetR = TargetSchema::loadShipped(parsed->targetName);
        if (!targetR.has_value()) continue;
        auto formatR = ObjectFormatSchema::loadShipped(parsed->formatName);
        if (!formatR.has_value()) continue;

        auto gathered = gather_(0, spec, **targetR, **formatR);
        if (!gathered) return std::nullopt;
        if (gathered->forThisBuild.empty()) continue;
        std::vector<ResolveLibrarySpec> libs;
        libs.reserve(gathered->forThisBuild.size());
        for (auto const& a : gathered->forThisBuild) {
            libs.push_back(ResolveLibrarySpec{a.path, {}});
        }
        out.libraryAdditionsByTarget.emplace(spec, std::move(libs));
    }

    // U-7's post-build half: ONLY for an `ArtifactLink` dependency whose own
    // build returned 0, mirroring the root's rule. Run once per dependency
    // rather than once per consumer target — a hook is a statement about the
    // project, not about a platform — and only after every target of it
    // succeeded, which is exactly the state reached here.
    for (std::size_t i = 1; i < nodes_.size(); ++i) {
        Node const& node = nodes_[i];
        if (node.composition != DependencyComposition::ArtifactLink) continue;
        bool built = false;
        for (auto const& [key, path] : artifactMemo_) {
            if (key.first == i) { built = true; break; }
        }
        if (!built) continue;
        if (!runBuildScripts(node.config.postBuildScripts, node.dir, rep_)) {
            return std::nullopt;
        }
    }

    // The lockfile is written ONCE, after the walk — the file is rewritten
    // whole, so N writes would buy nothing. Only when a git dependency actually
    // opened the cache: a pure-`path` graph must not materialize `.dss-deps`.
    if (cacheUsed_ && cache_slot_.has_value()) {
        if (!cache_slot_->save(rep_)) return std::nullopt;
    }
    return out;
}

} // namespace

std::optional<DependencyResolution>
resolveProjectDependencies(ProjectConfig const&            rootConfig,
                           DependencyResolveRequest const& request,
                           IGitRunner&                     git,
                           DiagnosticReporter&             rep) {
    // The overwhelmingly common manifest pays NOTHING for this feature
    // existing: no canonicalization, no `.dss-deps`, no git probe, no
    // filesystem write. Checked here rather than at the caller so the property
    // belongs to the resolver rather than to whoever remembers to guard it.
    if (rootConfig.dependsOn.empty()) return DependencyResolution{};
    Resolver resolver{request, git, rep};
    return resolver.run(rootConfig);
}

} // namespace dss
