#include "analysis/compilation_unit/compilation_unit.hpp"

#include "analysis/compilation_unit/import_resolver.hpp"
#include "ffi/shipped_lib_descriptor.hpp"   // readShippedLibTypedefNames (c43 follow-up: the cast-vs-call oracle)
#include "analysis/preprocess/preprocessor.hpp"
#include "analysis/syntactic/parser.hpp"
#include "core/substrate/mint_monotonic_id.hpp"
#include "core/substrate/phase_timers.hpp"   // c97: per-phase --time accumulation
#include "core/types/config_path_walk.hpp"   // resolveSystemDirs — THE owner of the shippedLibDirs walk
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "tokenizer/tokenizer.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dss {

namespace {

// Release-mode-fatal guard. Mirrors `treeFatal` in tree.cpp and `tbFatal`
// in tree_builder.cpp — same style, same exit posture.
[[noreturn]] void cuFatal(char const* what) {
    std::fputs("dss::CompilationUnit fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

// Build + report a driver-level diagnostic (no source span — driver errors
// reference a path/label, not a byte range). The renderer handles an
// invalid/absent buffer gracefully.
void reportDriver(DiagnosticReporter& rep,
                  DiagnosticCode      code,
                  DiagnosticSeverity  severity,
                  BufferId            buffer,
                  std::string         actual) {
    ParseDiagnostic d;
    d.code     = code;
    d.severity = severity;
    d.buffer   = buffer;
    d.span     = SourceSpan::empty(0);
    d.actual   = std::move(actual);
    rep.report(std::move(d));
}

// Build the ParserConfig for parsing `schema`'s sources, applying the
// language's config-driven knobs. Today the only such knob is the
// expression-nesting cap (`parser.maxExpressionDepth` in the `.lang.json`):
// when the config declares it, it overrides the `ParserConfig` C++ fallback
// default; when omitted, the fallback (256) stands. This is THE single
// chokepoint that makes the cap config-driven — every real parse in this file
// routes through it. (`P_ExpressionTooDeep` remains the fail-loud backstop at
// whatever value results.) AGNOSTIC: reads the schema's own value; no
// language/target/format branch.
[[nodiscard]] ParserConfig parserConfigFor(GrammarSchema const& schema) {
    ParserConfig cfg;
    if (auto cap = schema.maxExpressionDepth()) {
        cfg.maxExpressionDepth = *cap;
    }
    return cfg;
}

} // namespace

// ── CompilationUnit::nextId ───────────────────────────────────────────────
// Process-global monotonic counter starting at 1; 0 is InvalidCompilationUnit.
CompilationUnitId CompilationUnit::nextId() noexcept {
    return substrate::mintMonotonicId<CompilationUnitId>();
}

// ── CompilationUnit lifecycle ─────────────────────────────────────────────
CompilationUnit::CompilationUnit(PrivateTag,
                                 CompilationUnitId                    id,
                                 std::shared_ptr<GrammarSchema const> schema,
                                 std::vector<Tree>                    trees,
                                 DiagnosticReporter                   driverDiagnostics,
                                 std::vector<CrossTreeRef>            crossRefs,
                                 std::vector<ShippedDescriptorRef>    shippedLibDescriptors,
                                 std::uint32_t                        typeNameReparseCount,
                                 std::vector<std::shared_ptr<SourceBuffer>> auxiliaryBuffers,
                                 std::vector<std::unordered_map<std::uint32_t, std::uint32_t>>
                                     pragmaPackMaps,
                                 std::vector<std::unordered_set<std::uint32_t>>
                                     pragmaNoOptimizeSets,
                                 std::vector<PreprocessedPositionMap>
                                     preprocessedPositionMaps)
    : id_(id)
    , schema_(std::move(schema))
    , trees_(std::move(trees))
    , driverDiagnostics_(std::move(driverDiagnostics))
    , crossRefs_(std::move(crossRefs))
    , shippedLibDescriptors_(std::move(shippedLibDescriptors))
    , typeNameReparseCount_(typeNameReparseCount)
    , auxiliaryBuffers_(std::move(auxiliaryBuffers))
    , pragmaPackMaps_(std::move(pragmaPackMaps))
    , pragmaNoOptimizeSets_(std::move(pragmaNoOptimizeSets))
    , preprocessedPositionMaps_(std::move(preprocessedPositionMaps)) {}

CompilationUnit::~CompilationUnit()                                            = default;
CompilationUnit::CompilationUnit(CompilationUnit&&) noexcept                   = default;
CompilationUnit& CompilationUnit::operator=(CompilationUnit&&) noexcept        = default;

// ── CompilationUnit accessors ─────────────────────────────────────────────
CompilationUnitId             CompilationUnit::id()                const noexcept { return id_; }
std::span<Tree const>         CompilationUnit::trees()             const noexcept { return trees_; }
DiagnosticReporter const&     CompilationUnit::driverDiagnostics() const noexcept { return driverDiagnostics_; }
std::span<CrossTreeRef const> CompilationUnit::crossRefs()         const noexcept { return crossRefs_; }
std::span<ShippedDescriptorRef const>
CompilationUnit::shippedLibDescriptors() const noexcept { return shippedLibDescriptors_; }
std::span<std::shared_ptr<SourceBuffer> const>
CompilationUnit::auxiliaryBuffers() const noexcept { return auxiliaryBuffers_; }

std::unordered_map<std::uint32_t, std::uint32_t> const&
CompilationUnit::pragmaPackFor(std::size_t treeIndex) const noexcept {
    // TF-C82: a tree with no stamps (not preprocessed, or preprocessed with no
    // `#pragma pack`) and an out-of-range index answer identically — the EMPTY
    // map, i.e. "no cap anywhere", which is exactly the pre-TF-C82 layout. A
    // shared static keeps the reference valid without a per-call allocation.
    static std::unordered_map<std::uint32_t, std::uint32_t> const kEmpty;
    if (treeIndex >= pragmaPackMaps_.size()) return kEmpty;
    return pragmaPackMaps_[treeIndex];
}

std::unordered_set<std::uint32_t> const&
CompilationUnit::pragmaNoOptimizeFor(std::size_t treeIndex) const noexcept {
    // TF-C85: the exact sibling of `pragmaPackFor` above — a tree with no stamps
    // and an out-of-range index both answer the EMPTY set, i.e. "optimize
    // everything", which is exactly the pre-TF-C85 behavior.
    static std::unordered_set<std::uint32_t> const kEmpty;
    if (treeIndex >= pragmaNoOptimizeSets_.size()) return kEmpty;
    return pragmaNoOptimizeSets_[treeIndex];
}

bool CompilationUnit::isSynthesizedPreprocessorBuffer(
        BufferId buffer) const noexcept {
    if (!buffer.valid()) return false;
    for (PreprocessedPositionMap const& m : preprocessedPositionMaps_) {
        if (m.synth == buffer) return true;
    }
    return false;
}

void CompilationUnit::remapPreprocessedPosition(BufferId&   buffer,
                                                SourceSpan& span) const {
    // The common case by a wide margin: a CU whose files were never
    // preprocessed (toy / tsql / assembly, and every hand-built test CU) owns no
    // maps at all, so the whole mechanism costs one empty-vector test.
    if (preprocessedPositionMaps_.empty()) return;
    // Every closure self-gates on its OWN synth buffer id, so the order is
    // irrelevant and a position belonging to none of them is untouched. Once one
    // closure has moved the position, the rest no longer match it.
    for (PreprocessedPositionMap const& m : preprocessedPositionMaps_) {
        if (m.remap) m.remap(buffer, span);
    }
    // ★ THE REFUSAL. Reaching here with the position STILL on a recorded synth
    // buffer means the line-map that was recorded as covering that buffer could
    // not resolve the offset — a compiler-internal contradiction. The
    // alternative is to hand the user a file:line that names real text at a line
    // it does not occupy, which is precisely the class this closes, and which
    // survived for months exactly because it looked plausible. A named refusal
    // cannot.
    if (isSynthesizedPreprocessorBuffer(buffer)) {
        cuFatal("CompilationUnit::remapPreprocessedPosition: a diagnostic "
                "position is still in SYNTHESIZED preprocessor coordinates "
                "after every line-map remap ran - the preprocessor recorded a "
                "line map for this buffer, so the offset must resolve to an "
                "origin file");
    }
}

// D-LSP-POSITIONS-RESOLVED-IN-SYNTHESIZED-PREPROCESSOR-COORDINATES — the
// inverse. Contract and the reason it does NOT mirror the forward refusal are
// on the declaration.
void CompilationUnit::inversePreprocessedPositions(
        BufferId originBuffer, ByteOffset originOffset,
        std::vector<ByteOffset>& out) const {
    out.clear();
    if (preprocessedPositionMaps_.empty()) return;
    std::vector<ByteOffset> perMap;
    for (PreprocessedPositionMap const& m : preprocessedPositionMaps_) {
        m.map.inverse(originBuffer, originOffset, perMap);
        out.insert(out.end(), perMap.begin(), perMap.end());
    }
}

BufferId CompilationUnit::mainOriginForSynth(BufferId synth) const {
    for (PreprocessedPositionMap const& m : preprocessedPositionMaps_) {
        if (m.synth == synth) return m.mainOrigin;
    }
    return BufferId{};
}

void CompilationUnit::remapPreprocessedPositions(
        DiagnosticReporter& reporter) const {
    if (preprocessedPositionMaps_.empty()) return;
    reporter.remapBuffers([this](BufferId& b, SourceSpan& s) {
        remapPreprocessedPosition(b, s);
    });
}

std::string CompilationUnit::compositeSourceLanguage() const {
    std::string out;
    std::unordered_set<std::string> seen;
    for (Tree const& t : trees_) {
        std::string name{t.schema().name()};
        if (seen.insert(name).second) {
            if (!out.empty()) out += '+';
            out += name;
        }
    }
    return out.empty() ? std::string{schema().name()} : out;
}

// D-STATICLIB-MEMBER-NAME-DERIVES-FROM-THE-FIRST-SOURCE — see the header for
// why this is DERIVED from the trees rather than threaded alongside them.
// `sourceShared()` can legitimately be null on a hand-built test tree
// (`BufferRegistry::add` throws on one, and the driver's own drain guards for
// it), so the empty answer covers "no trees" and "a tree with no buffer" alike
// — both mean the same thing to a caller: this unit cannot name its source.
std::string_view CompilationUnit::primarySourceName() const noexcept {
    if (trees_.empty()) return {};
    // `sourceShared()` and not `source()`: the latter DEREFERENCES and requires
    // non-null. The buffer outlives the temporary `shared_ptr` because the Tree
    // holds its own reference for the CU's lifetime, so the view stays valid.
    std::shared_ptr<SourceBuffer> const src = trees_.front().sourceShared();
    return src ? src->name() : std::string_view{};
}

GrammarSchema const& CompilationUnit::schema() const noexcept {
    // Mirrors Tree::schema (tree.cpp): a moved-from CU has a null schema_
    // (the shared_ptr was moved out). Dereferencing it is UB; abort loudly
    // instead — same fail-loud posture as the rest of the substrate. The
    // other accessors return empty spans / a valid reporter ref on a
    // moved-from CU and are intentionally safe to read.
    if (!schema_) {
        cuFatal("CompilationUnit::schema: no schema (moved-from CompilationUnit?)");
    }
    return *schema_;
}

// ── UnitBuilder ───────────────────────────────────────────────────────────
UnitBuilder::UnitBuilder(std::shared_ptr<GrammarSchema const> schema,
                         DiagnosticBudget                     budget)
    : id_(CompilationUnit::nextId())
    , schema_(std::move(schema))
    // Driver diagnostics are keyed by PATH, not by source span — every
    // D_FileNotFound / D_DuplicateFile shares (code, InvalidBuffer, empty
    // span), so the reporter's span-based dedup window would silently
    // collapse N distinct missing files into one message. Disable dedup on
    // this reporter so each bad path surfaces. (Per-tree reporters keep
    // their dedup; this disables it only for the CU's driver-level stream.)
    , budget_(budget)
    , driverDiagnostics_(budget.withoutDedup().asConfig()) {
    if (!schema_) {
        cuFatal("UnitBuilder: schema is null");
    }
    schemas_.push_back(schema_);   // primary is also the sole registry entry
}

UnitBuilder::UnitBuilder(std::vector<std::shared_ptr<GrammarSchema const>> schemas,
                         DiagnosticBudget                                   budget)
    : id_(CompilationUnit::nextId())
    , budget_(budget)
    , driverDiagnostics_(budget.withoutDedup().asConfig()) {
    if (schemas.empty()) {
        cuFatal("UnitBuilder: schema registry is empty");
    }
    for (auto const& s : schemas) {
        if (!s) cuFatal("UnitBuilder: a registry schema is null");
    }
    schema_  = schemas.front();    // primary = first registered
    schemas_ = std::move(schemas);
}

void UnitBuilder::registerSchema(std::shared_ptr<GrammarSchema const> schema) {
    if (finished_) {
        cuFatal("UnitBuilder::registerSchema called after finish()");
    }
    if (!schema) {
        cuFatal("UnitBuilder::registerSchema: schema is null");
    }
    schemas_.push_back(std::move(schema));
}

UnitBuilder::~UnitBuilder() = default;

CompilationUnitId UnitBuilder::id() const noexcept { return id_; }

void UnitBuilder::addTree(Tree&& tree) {
    if (finished_) {
        cuFatal("UnitBuilder::addTree called after finish()");
    }
    trees_.push_back(std::move(tree));
    // FC2: keep the parse-sidecar vector index-parallel by construction.
    // An externally-built tree has no parse sidecar (empty candidates,
    // no source/schema handle) — it is never oracle-reparsed.
    sidecars_.emplace_back();
}

TreeId UnitBuilder::parseAndAdd_(std::shared_ptr<SourceBuffer> src,
                                std::shared_ptr<GrammarSchema const> schema) {
    // Keep the registry the authoritative set of EVERY schema used in this CU
    // (an explicit-schema addInMemory may name a schema never registered): so
    // finish()'s per-schema import resolution covers this tree, and so a later
    // addFile can route to it. Dedup by SchemaId (idempotent re-registration).
    {
        bool known = false;
        for (auto const& s : schemas_) {
            if (s->schemaId() == schema->schemaId()) { known = true; break; }
        }
        if (!known) schemas_.push_back(schema);
    }
    // Empty translation unit is valid (consistent with "empty CU is valid"):
    // note it as Info but still parse + add the (empty) tree. Lives here so
    // both addFile and addInMemory get the check from one place; the buffer
    // name (path for files, label for in-memory) identifies it.
    if (src->size() == 0) {
        reportDriver(driverDiagnostics_, DiagnosticCode::D_EmptyInput,
                     DiagnosticSeverity::Info, src->id(),
                     std::string{src->name()});
    }
    // Canonical tokenize → parse → ingest sequence (mirrors the LSP parse
    // path), UNDER the file's resolved schema (HR11/CU5 — multi-language CUs
    // parse each file with its own language). The tokenizer's lexer
    // diagnostics are handed to the Parser, which folds them into the produced
    // Tree's reporter (§2.6 C2-L1), so the finished Tree owns lexer + parser
    // diagnostics in one stream — and the Tree carries `schema` for the
    // downstream per-tree semantic + lowering dispatch.
    // FC13: config-SELECTED C preprocessor. When the file's schema opts in
    // (preprocess().enabled), run the preprocessor BETWEEN tokenize and
    // parse: it builds ONE synthesized buffer (recursively splicing quote
    // #include'd headers' text), tokenizes it once, runs the object-macro
    // pass (define/undef/expand/rescan, directives removed), and hands the
    // resulting tokens to the parser. The synthesized buffer becomes the
    // parsed tree's source() (every token span is in its coordinates); the
    // line-map remaps diagnostics back onto the real header/main file. A
    // language WITHOUT a preprocess block (toy / tsql) takes the unchanged
    // tokenize->parse path below. These two are the ONLY .tokenize() sites
    // in src/ that consume C source.
    if (schema->preprocess().enabled) {
        // FC15c: thread the system-header search path so `__has_include(<h>)`
        // resolves a system descriptor (`<stem>.json`) exactly as the post-parse
        // import resolver does for `#include <h>` (one shared mapping).
        // c97: the preprocess phase covers the whole config-driven pass —
        // splice + tokenize-of-the-synth-buffer + macro expansion.
        std::optional<substrate::PhaseTimers::Scope> phase;
        phase.emplace(substrate::CompilePhase::Preprocess);
        PreprocessResult pp = preprocess(src, schema, includeDirs_,
                                         headerNameMatching_, budget_, systemDirs_,
                                         activeFormat_, userDefines_,
                                         targetPredefinedMacros_,
                                         formatPredefinedMacros_);
        phase.reset();
        auto remap = pp.makeRemap();
        std::shared_ptr<SourceBuffer> synth = pp.synthBuffer;
        // The parser consumes a stream built from a COPY of the preprocessed
        // tokens; the vector is retained in the sidecar for the FC2 oracle
        // reparse. The synthesized buffer is the parse source.
        // FC13 gate (D-PP-FATAL-HALTS-PARSE): a FATAL preprocessor backstop
        // (the >256 macro-expansion-nesting guard or the include-nesting
        // guard) TRUNCATES the synthesized stream at the failure point.
        // Feeding that truncated stream to the parser produces an
        // inscrutable secondary cascade (or, on a pathologically deep
        // partial expansion, drives the expression recursion to its depth
        // guard) on top of the real PP cause. So on a FATAL truncation we
        // do NOT parse the truncated tokens: we parse an EOF-ONLY stream,
        // which yields a minimal well-formed tree that still CARRIES the
        // PP diagnostics (the Parser ingests them into the produced tree
        // below), then remap + addTree exactly as the normal path does.
        // The PP error surfaces cleanly and the parse halts before the
        // cascade.
        //
        // The gate keys on `pp.fatal` (stream truncated), NOT on
        // `diagnostics->hasErrors()`: a RECOVERABLE PP error (missing
        // `#include` file, malformed directive, redefinition) or a folded
        // LEXER error (illegal char) leaves the stream INTACT, so the
        // parser MUST still run to surface the parse-level diagnostics
        // (gating those would SWALLOW the real frontend errors — e.g. an
        // unresolved `#include` must not suppress the rest of the file).
        const bool ppFatal = pp.fatal;
        // `pp.eofToken()` is the CHECKED read of the Eof terminator
        // ([[D-PP-RESULT-CONTRACT-SINGLE-EXIT]]). This used to be
        // `pp.tokens.back()` under a comment asserting the vector is
        // "Eof-terminated by contract" — and when a producer early-return
        // broke that contract (the predefined-macro-collision abort) this line
        // read past the end of an EMPTY vector and crashed the compiler with
        // no diagnostic. The accessor cannot do that: it aborts with a named
        // message. `fromTokens` takes its argument by value (copies), so
        // `pp.tokens` survives intact for the sidecar move below.
        TokenStream stream =
            ppFatal
                ? TokenStream::fromTokens({pp.eofToken()})
                : TokenStream::fromTokens(pp.tokens);
        phase.emplace(substrate::CompilePhase::Parse);
        // D-PERF-2-TYPEDEF-SEED-DISAMBIGUATION: seed the binder sketch's global
        // scope with the LIVE shipped-descriptor typedef NAMES (`size_t` from
        // <stddef.h>, the stdint widths, …) BEFORE the FIRST parse. Those
        // typedefs are injected SEMANTICALLY (post-parse), so without this the
        // includer parsed `(size_t)(expr)` as a CALL and recorded an
        // AmbiguousTypeNameCandidate -> UnitBuilder::finish() re-tokenized and
        // re-parsed the WHOLE TU (~0.75s/TU on the SQLite amalgamation) just to
        // learn the name is a type. Seeding it up front commits the cast on parse
        // 1 (parser.cpp NameKind::Type) -> no candidate -> no reparse. This reuses
        // the EXACT channel the finish() oracle reparse already uses
        // (ParserConfig::seedGlobalTypeNames -> BinderSketch::seedGlobalType); the
        // reparse is RETAINED as the residual net for in-buffer FORWARD references
        // (a typedef used before its own definition in the synthesized buffer),
        // which no descriptor seed can cover. Interner-free NAME harvest over the
        // descriptors the preprocessor actually resolved (parent + transitive
        // `includes` closure), deduped by name. A scratch reporter: a malformed
        // descriptor is reported ONCE by the semantic read, never here.
        // ORACLE-ALIGNED (D-PERF-2): `pp.resolvedShippedDescriptors` is now the
        // AUTHORITATIVELY-LIVE descriptor set (the preprocessor drops a splice whose
        // offset falls in an `#if 0` dead range), EQUAL to the finish() oracle's
        // `shippedLibDescriptors`. So seeding resolves EXACTLY the names the finish()
        // reparse would -- never a superset, never a name from a dead-branch include.
        // ONE-DIRECTIONAL (P0016): a real in-source binding still SHADOWS a seed
        // (`lookup` scans bindings newest-first; seeds precede every parse-time
        // record), so seeding only ever turns Unknown -> Type, never overrides a
        // Value -- it resolves MORE names, never suppresses a diagnostic.
        ParserConfig cfg = parserConfigFor(*schema);
        {
            std::unordered_set<std::string> seen;
            for (std::filesystem::path const& desc :
                 pp.resolvedShippedDescriptors) {
                DiagnosticReporter scratch{budget_.withoutDedup().asConfig()};
                if (auto names =
                        ffi::readShippedLibTypedefNames(desc, scratch)) {
                    for (auto& n : *names) {
                        if (seen.insert(n).second) {
                            cfg.seedGlobalTypeNames.push_back(std::move(n));
                        }
                    }
                }
            }
        }
        Parser p{synth, schema, std::move(stream), budget_, std::move(cfg),
                 std::move(pp.diagnostics)};
        ParseResult result = std::move(p).parse();
        phase.reset();
        // Remap the produced tree's diagnostics off the synth buffer onto the
        // origin file(s) before ingest, so a header-origin (and post-splice
        // main-origin) diagnostic is attributed to its real file.
        result.tree.remapDiagnostics(remap);
        // Retain the PP's origin buffers (original main + every spliced header)
        // so the driver can register them for diagnostic rendering -- a
        // remapped diagnostic now references one of these buffers, not the
        // tree's synth `source()`.
        for (auto& ob : pp.originBuffers) {
            if (ob) auxiliaryBuffers_.push_back(std::move(ob));
        }
        addTree(std::move(result.tree));
        auto& sidecar           = sidecars_.back();
        sidecar.candidates      = std::move(result.typeNameCandidates);
        sidecar.globalTypeNames = std::move(result.globalTypeNames);
        sidecar.globalTypeBindings = std::move(result.globalTypeBindings);
        sidecar.source          = std::move(synth);
        sidecar.schema          = std::move(schema);
        sidecar.ppTokens        = std::move(pp.tokens);
        sidecar.ppRemap         = std::move(remap);
        // [[D-PP-SEMANTIC-DIAGNOSTIC-POSITION-UNREMAPPED]]: record the SYNTH
        // buffer so the finished CU can answer "is this position in synthesized
        // coordinates" for EVERY later tier — the parse tier is simply the only
        // one that used to run while this builder was still alive.
        //
        // Recorded ONLY when the line map can actually move a position off the
        // synth buffer: a non-empty map, EVERY segment of which names a real
        // origin. `LineMap::resolve` picks a segment by offset and hands back
        // that segment's origin, so one null origin anywhere is one offset range
        // the remap legitimately cannot move — and the CU's refusal must fire on
        // a contradiction, never on a case the map never claimed to cover.
        bool remapCoversEverySegment = !pp.lineMap.empty();
        for (LineMapSegment const& seg : pp.lineMap.segments()) {
            if (!seg.origin) { remapCoversEverySegment = false; break; }
        }
        if (remapCoversEverySegment) {
            sidecar.ppSynthBuffer = sidecar.source->id();
            // D-LSP-POSITIONS-RESOLVED-IN-SYNTHESIZED-PREPROCESSOR-COORDINATES:
            // carry the MAP and the MAIN ORIGIN under the SAME condition as the
            // synth id, so all three are recorded together or not at all. A
            // half-populated bridge is the state where one direction silently
            // answers and the other silently does not.
            sidecar.ppMap        = pp.lineMap;
            sidecar.ppMainOrigin = pp.mainSourceId;
        }
        // TF-C82 (D-PP-PRAGMA-REGISTRY): carry the `#pragma pack` stamps to the
        // finished CU. Empty for a TU with no `#pragma pack`, which is every TU
        // that existed before this cycle.
        sidecar.pragmaPack      = std::move(pp.pragmaPackByOffset);
        // TF-C85: same hop for the `#pragma optimize` stamps.
        sidecar.pragmaNoOptimize = std::move(pp.pragmaNoOptimizeByOffset);
        return trees_.back().id();
    }
    // c105 (D-PP-USER-DEFINE): `--define` macros can ONLY be consumed by a
    // preprocess-enabled language. Reaching the plain tokenize→parse path with
    // user defines pending means they would be SILENTLY ignored — fail loud
    // (once per file taking this path; a mixed CU's preprocessed files still
    // consume them normally).
    if (!userDefines_.empty()) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::D_DefineRequiresPreprocess;
        d.severity = DiagnosticSeverity::Error;
        d.buffer   = src->id();
        d.actual   = "--define was passed but language '"
                   + std::string{schema->name()}
                   + "' declares no preprocess block; the macro(s) cannot "
                     "be consumed";
        driverDiagnostics_.report(std::move(d));
    }
    std::optional<substrate::PhaseTimers::Scope> phase;
    phase.emplace(substrate::CompilePhase::Tokenize);
    Tokenizer tk{src, schema, budget_};
    auto [stream, lexDiags] = std::move(tk).tokenize();
    phase.emplace(substrate::CompilePhase::Parse);
    Parser p{src, schema, std::move(stream), budget_, parserConfigFor(*schema),
             std::move(lexDiags)};
    ParseResult result = std::move(p).parse();
    phase.reset();
    addTree(std::move(result.tree));
    // FC2: fill the sidecar addTree just pushed — the parse's ambiguous
    // type-name candidates + exported global type names (the finish()-time
    // oracle's inputs) and the handles a one-shot reparse needs.
    auto& sidecar           = sidecars_.back();
    sidecar.candidates      = std::move(result.typeNameCandidates);
    sidecar.globalTypeNames = std::move(result.globalTypeNames);
    sidecar.globalTypeBindings = std::move(result.globalTypeBindings);
    sidecar.source          = std::move(src);
    sidecar.schema          = std::move(schema);
    return trees_.back().id();
}

namespace {
// ASCII lower-case a copy (file extensions are ASCII; case-insensitive match).
[[nodiscard]] std::string asciiLower(std::string_view in) {
    std::string out{in};
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
} // namespace

std::vector<std::shared_ptr<GrammarSchema const>>
UnitBuilder::schemasForPath_(std::filesystem::path const& path) const {
    // ★★ EVERY claimant, not the first (D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET).
    //
    // This used to `return s;` on the first registered schema whose
    // `fileExtensions` matched, which is a correct answer only while no two
    // registered languages can claim one extension. That stopped being true
    // the moment a second assembly dialect shipped: `asm-x86_64-att` and
    // `asm-arm64-gas` BOTH declare `.s`/`.S`, and they disagree about what a
    // `#` means (a comment in one, an immediate in the other) — so
    // first-registered-wins would not have produced a parse ERROR, it would
    // have produced a WRONG PARSE under a plausible grammar, silently, with
    // registration order as the deciding input.
    //
    // Collecting all claimants moves the decision to the caller, which is the
    // only layer that can have the missing information (`addFile` has none and
    // fails loud; the DRIVER has the target and answers). Deduplicated by
    // schema IDENTITY so `registerSchema(s); registerSchema(s);` — explicitly
    // allowed, and exercised by `test_hir_lowering_multi_lang` — reads as one
    // claimant rather than a self-ambiguity.
    std::vector<std::shared_ptr<GrammarSchema const>> claimants;
    std::string const ext = asciiLower(path.extension().string());
    if (ext.empty()) return claimants;
    for (auto const& s : schemas_) {
        bool matches = false;
        for (std::string_view declared : s->fileExtensions()) {
            if (asciiLower(declared) == ext) { matches = true; break; }
        }
        if (!matches) continue;
        bool already = false;
        for (auto const& c : claimants) {
            if (c.get() == s.get() || c->schemaId() == s->schemaId()) {
                already = true;
                break;
            }
        }
        if (!already) claimants.push_back(s);
    }
    return claimants;
}

void UnitBuilder::addIncludeDir(std::filesystem::path dir) {
    if (finished_) {
        cuFatal("UnitBuilder::addIncludeDir called after finish()");
    }
    includeDirs_.push_back(std::move(dir));
}

void UnitBuilder::addSystemDir(std::filesystem::path dir) {
    if (finished_) {
        cuFatal("UnitBuilder::addSystemDir called after finish()");
    }
    systemDirs_.push_back(std::move(dir));
}

// See the header. The WALK is `resolveSystemDirs`
// (`core/types/config_path_walk.hpp`); this is only the binding onto a builder,
// and it is a named function rather than an open-coded loop so that every
// channel that builds a CU — driver, editor, and anything after them — is
// visibly asking the SAME question.
void applySystemDirs(UnitBuilder& builder, GrammarSchema const& grammar) {
    for (auto const& d : resolveSystemDirs(grammar)) builder.addSystemDir(d);
}

void UnitBuilder::setActiveFormat(ObjectFormatKind fmt) {
    if (finished_) {
        cuFatal("UnitBuilder::setActiveFormat called after finish()");
    }
    activeFormat_ = fmt;
}

// ═══ THE UNCONDITIONAL PREDEFINED-MACRO IDENTITY VALIDATION ═══════════════════
//
// ★★★ WHY IT IS HERE AND NOT IN THE PREPROCESSOR. The predefined-macro rules —
// the cross-family NAME collision, the mutual-exclusion groups, and (new) the
// `requires` backing claim — are statements about a (language, target, object
// format) TRIPLE. They are true or false before a single byte of source is read.
// But the only place they were ever evaluated was inside the preprocess pass, so
// they were silently conditional on the pass RUNNING, and MEASURED there are
// three ways it does not:
//
//   1. a TU that includes NO shipped header — the pass runs, but nothing ever
//      touched the shipped corpus, so a claim ABOUT that corpus had no site;
//   2. a `.s` ASSEMBLY input — the assembly languages declare no `preprocess`
//      block, so `preprocess().enabled` is false and the pass is a strict
//      identity: NOTHING was validated, while the target and format families
//      still contributed their macros to that build's identity;
//   3. `#include`s inside `#if 0` — same shape as (1): the corpus is never
//      reached, so a corpus claim is never tested.
//
// A rule that only fires when an unrelated thing happens is not a rule. Hoisting
// it to `finish()` — which EVERY CompilationUnit passes through, in every
// language, whether or not anything was preprocessed and whether or not any
// header was included — makes it hold for the triple, which is what it was
// always about.
//
// ⚠ HONEST NOTE ON DOUBLE REPORTING, stated rather than left to be discovered.
// `preprocessRun` still performs its OWN merge and still reports a collision or
// an exclusion-group violation itself, because it must REFUSE to preprocess
// under an identity no real compiler can present, and its refusal is pinned by
// its own suite. So for a preprocess-enabled language with a BROKEN config, the
// collision message appears twice: once positioned in the TU by the pass, once
// here as a driver diagnostic. That is deliberate. The alternative — making this
// site conditional on "did the preprocessor run" — reintroduces exactly the
// conditionality this hoist exists to remove, and it would buy a tidier failure
// mode for a build that is already failing.
void UnitBuilder::validatePredefinedMacroIdentity_() {
    // The LANGUAGE family is per-schema: a multi-language CU has one predefine
    // list per registered schema, and each is a separate claim about the same
    // (target, format). `schemas_` holds every schema any tree was parsed with;
    // a CU with no trees at all has none, and then only the target and format
    // families are in scope — which is still a real pair to check.
    auto const validateFamily = [&](std::span<PredefinedMacroDef const> rows,
                                    std::string_view document) {
        (void)ffi::validateShippedSurfaceRequirements(
            rows, document, systemDirs_, activeFormat_, driverDiagnostics_);
    };

    std::unordered_set<std::uint32_t> seenSchemaIds;
    for (auto const& schema : schemas_) {
        if (!schema) continue;
        if (!seenSchemaIds.insert(schema->schemaId().v).second) continue;
        auto const& pp = schema->preprocess();
        // The COLLISION + MUTUAL-EXCLUSION rules, evaluated for this language
        // against the active target and format. `mergePredefinedMacros` is the
        // ONE owner of both; this site only reports what it returns, so the two
        // evaluation sites can never disagree about what a conflict IS.
        MergedPredefinedMacros const merged = mergePredefinedMacros(
            pp.predefinedMacros, targetPredefinedMacros_,
            formatPredefinedMacros_, activeFormat_,
            pp.mutuallyExclusivePredefinedMacros);
        for (std::string const& msg : merged.conflicts) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::C_ConflictingPredefinedMacro;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = msg;
            driverDiagnostics_.report(std::move(d));
        }
        validateFamily(pp.predefinedMacros, kPredefinedMacroFamilyPaths[0]);
    }
    // The TARGET and FORMAT families are per-CU, not per-schema, so they are
    // checked ONCE outside the schema loop — otherwise a two-language CU would
    // report the same target-row failure twice.
    validateFamily(targetPredefinedMacros_, kPredefinedMacroFamilyPaths[1]);
    validateFamily(formatPredefinedMacros_, kPredefinedMacroFamilyPaths[2]);
}

void UnitBuilder::setHeaderNameMatching(HeaderNameMatching matching) {
    if (finished_) {
        cuFatal("UnitBuilder::setHeaderNameMatching called after finish()");
    }
    headerNameMatching_ = matching;
}

void UnitBuilder::setUserDefines(std::vector<std::string> defines) {
    if (finished_) {
        cuFatal("UnitBuilder::setUserDefines called after finish()");
    }
    userDefines_ = std::move(defines);
}

void UnitBuilder::setTargetPredefinedMacros(
    std::vector<PredefinedMacroDef> macros) {
    if (finished_) {
        cuFatal("UnitBuilder::setTargetPredefinedMacros called after finish()");
    }
    targetPredefinedMacros_ = std::move(macros);
}

void UnitBuilder::setFormatPredefinedMacros(
    std::vector<PredefinedMacroDef> macros) {
    if (finished_) {
        cuFatal("UnitBuilder::setFormatPredefinedMacros called after finish()");
    }
    formatPredefinedMacros_ = std::move(macros);
}

TreeId UnitBuilder::loadAndAdd_(std::filesystem::path const& path, bool& ok,
                               std::shared_ptr<GrammarSchema const> schema) {
    auto const key = core::PathIdentity::of(path);

    // Already loaded (by addFile or a previous include) → reuse, no re-parse.
    if (auto it = pathToTreeIndex_.find(key); it != pathToTreeIndex_.end()) {
        ok = true;
        return trees_[it->second].id();
    }

    std::shared_ptr<SourceBuffer> src;
    try {
        src = SourceBuffer::fromFile(path);
    } catch (std::exception const&) {
        ok = false;
        return InvalidTree;  // caller (resolver) emits D_UnresolvedImport.
    }
    seenPaths_.insert(key);
    TreeId const id = parseAndAdd_(std::move(src), std::move(schema));
    pathToTreeIndex_[key] = trees_.size() - 1;
    ok = true;
    return id;
}

void UnitBuilder::addInMemory(std::string source, std::string label) {
    addInMemory(std::move(source), std::move(label), schema_);
}

void UnitBuilder::addInMemory(std::string source, std::string label,
                              std::shared_ptr<GrammarSchema const> schema) {
    if (finished_) {
        cuFatal("UnitBuilder::addInMemory called after finish()");
    }
    if (!schema) {
        cuFatal("UnitBuilder::addInMemory: schema is null");
    }
    // Key the label into the SAME weakly-canonical path space addFile /
    // loadAndAdd_ use BEFORE parsing, so an #include that later resolves to a
    // path equal to this in-memory label dedups against this tree instead of
    // re-loading the file from disk (GAP E). Computed before the move-out of
    // `label`. Non-path-like labels (e.g. "<mem0>") canonicalize to a stable
    // distinct key and simply never collide with a real file.
    //
    // NB: addInMemory deliberately does NOT dedup two explicit in-memory
    // sources against each other (labels may legitimately repeat) — we only
    // record the mapping for include-following to consult. A repeated label
    // overwrites the map entry (last wins) without skipping the second tree.
    auto const key = core::PathIdentity::of(std::filesystem::path{label});
    seenPaths_.insert(key);  // also block a later addFile re-loading this path.
    parseAndAdd_(SourceBuffer::fromString(std::move(source), std::move(label)), std::move(schema));
    pathToTreeIndex_[key] = trees_.size() - 1;
}

void UnitBuilder::addFile(std::filesystem::path path) {
    if (finished_) {
        cuFatal("UnitBuilder::addFile called after finish()");
    }

    // Dedup by weakly-canonical path (handles `.`/`..`/symlinks without
    // requiring the file to exist). On canonicalization failure fall back
    // to the lexically-normal form so a bad path is still keyed stably.
    auto const key = core::PathIdentity::of(path);
    if (!seenPaths_.insert(key).second) {
        reportDriver(driverDiagnostics_, DiagnosticCode::D_DuplicateFile,
                     DiagnosticSeverity::Warning, InvalidBuffer, path.string());
        return;  // already added — skip the re-parse.
    }

    std::shared_ptr<SourceBuffer> src;
    try {
        src = SourceBuffer::fromFile(path);
    } catch (std::exception const& e) {
        // Missing/unreadable file, mid-read IO error, or a read-time
        // allocation failure (bad_alloc/length_error) — all *expected*
        // runtime failures at this boundary. Continue-on-failure (§2.6
        // C2-L2): record + return, never propagate (a throw would abort the
        // whole CU build). The 4-GiB cap is a deliberate hard abort inside
        // fromFile and is intentionally not catchable here.
        reportDriver(driverDiagnostics_, DiagnosticCode::D_FileNotFound,
                     DiagnosticSeverity::Error, InvalidBuffer, e.what());
        return;
    }
    // Route to a source language by file extension (HR11/CU5). A single-schema
    // builder routes to its one schema regardless of extension (CU1-CU4
    // behavior — the registry has one entry and the caller chose the language).
    // A multi-language builder with an UNMATCHED extension fails loud rather
    // than silently parsing under the wrong grammar.
    auto claimants = schemasForPath_(path);
    std::shared_ptr<GrammarSchema const> schema;
    if (claimants.size() == 1) {
        schema = std::move(claimants.front());
    } else if (claimants.size() > 1) {
        // ★ TWO REGISTERED LANGUAGES CLAIM THIS EXTENSION — fail loud NAMING
        // THEM (D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET). The builder has no
        // basis whatever for choosing: both documents declared the extension,
        // both are legitimate, and the tie is broken only by knowledge this
        // layer does not have (which CPU the file is written for). Picking one
        // would be registration order deciding what the source MEANS.
        //
        // ⚠ THE PRIMARY DOES NOT WIN HERE, and that is deliberate. On the
        // driver's path the primary is the caller's `--language` and it wins
        // BEFORE this point — the driver resolves one grammar per target and
        // registers exactly that one. A builder that reaches here with two
        // claimants was assembled by a caller who registered two languages
        // over one extension and then declined to say which; there is no
        // "explicit" among them to prefer.
        std::string names;
        for (auto const& c : claimants) {
            if (!names.empty()) names += ", ";
            names += std::string{c->name()};
        }
        //
        // The CODE is `D_UnknownFileExtension` and that is not a compromise:
        // 0xD006 means "this path's extension did not resolve to EXACTLY ONE
        // registered source language", and zero claimants and two claimants
        // are both that. The two MESSAGES differ — one says nothing claims it,
        // this one names every language that does — which is the axis an
        // operator acts on. Minting a second code would split one predicate
        // across two entries of a closed set every suppression list and
        // severity table then has to carry twice.
        reportDriver(driverDiagnostics_,
                     DiagnosticCode::D_UnknownFileExtension,
                     DiagnosticSeverity::Error, InvalidBuffer,
                     path.string() + ": extension claimed by "
                     + std::to_string(claimants.size())
                     + " registered source languages (" + names
                     + ") — register only one of them, or name the language "
                       "for this file explicitly");
        return;  // do not parse under an arbitrarily-chosen grammar.
    } else if (schemas_.size() == 1) {
        schema = schema_;
    } else {
        reportDriver(driverDiagnostics_, DiagnosticCode::D_UnknownFileExtension,
                     DiagnosticSeverity::Error, InvalidBuffer, path.string());
        return;  // do not parse under an arbitrary grammar.
    }
    parseAndAdd_(std::move(src), std::move(schema));
    // Record the path→tree mapping so a later #include resolving to this same
    // file dedups against it instead of re-parsing.
    pathToTreeIndex_[key] = trees_.size() - 1;
}

CompilationUnit UnitBuilder::finish() && {
    if (finished_) {
        cuFatal("UnitBuilder::finish() called twice");
    }

    validatePredefinedMacroIdentity_();

    // Resolve imports BEFORE marking finished: a resolver may load additional
    // included files (include-following) via the loadFile callback, which routes
    // through addTree — and addTree aborts once finished_ is set.
    //
    // HR11/CU5: run ONE resolver per DISTINCT registered schema (deduped below),
    // each bound to its language (chooseResolver) and processing only the trees
    // built from that schema. The edges all land in the one CU-global crossRefs;
    // injection is language-blind downstream. A homogeneous CU has a one-entry
    // registry, so this is a single resolver pass — identical to CU1-CU4. The
    // loadFile callback carries the including tree's schema so an #include loads
    // its target under the same language.
    std::vector<CrossTreeRef> crossRefs;
    std::vector<ShippedDescriptorRef> shippedLibDescriptors;
    ResolutionContext context{
        trees_,
        driverDiagnostics_,
        includeDirs_,
        systemDirs_,
        headerNameMatching_,
        // D-FFI-DESCRIPTOR-INCLUDES-EDGE-GATE: the same value the preprocessor
        // tier already receives. Both tiers walk the shipped-descriptor closure;
        // handing only one of them the format is how the two sets drift.
        activeFormat_,
        [this](std::filesystem::path const& path, bool& ok,
               std::shared_ptr<GrammarSchema const> schema) {
            return loadAndAdd_(path, ok, std::move(schema));
        },
        crossRefs,
        shippedLibDescriptors,
    };
    // One resolver per DISTINCT schema (dedup by SchemaId — registerSchema does
    // not dedup, and a duplicate would double-run a resolver over the same trees,
    // double-appending cross-refs). `schemas_` now contains every schema any tree
    // was parsed with (auto-registered in parseAndAdd_), so every tree gets its
    // own language's import resolution.
    std::unordered_set<std::uint32_t> resolvedSchemaIds;
    {
        // c97: resolve-imports phase — NB the loadFile callback may PARSE
        // additional included files; those nested parses accumulate into the
        // parse/preprocess phases (parseAndAdd_ scopes), and steady-clock
        // wall time double-counts across nested scopes by design (each
        // phase's number answers "how long did this phase's code run").
        substrate::PhaseTimers::Scope resolvePhase{
            substrate::CompilePhase::ResolveImports};
        for (auto const& schema : schemas_) {
            if (!resolvedSchemaIds.insert(schema->schemaId().v).second) continue;
            chooseResolver(schema)->resolve(context);
        }
    }

    // ── FC2 type-name oracle + conditional reparse ────────────────────────
    //
    // Premise: an `#include`d header's typedefs are invisible to the
    // INCLUDER's parse (each file parses alone; trees merge post-parse via
    // crossRefs), so `(MyT)-x` with MyT from a header froze as the value
    // reading and recorded an AmbiguousTypeNameCandidate. Here — after the
    // resolvers loaded every include target, while `trees_` is still
    // mutable — the oracle resolves each candidate against the UNION of
    // every tree's exported global TYPE names (each parse's binder sketch
    // already harvested its own; nested includes are covered because the
    // union spans ALL trees in the CU). A tree with ≥1 resolved candidate
    // is re-tokenized + re-parsed ONCE with the resolved names seeded into
    // the binder sketch's global scope, and REPLACED in place. The second
    // parse's diagnostics replace the first's wholesale (the Tree owns its
    // diagnostic stream — no double-report). Candidates the oracle cannot
    // resolve keep the value reading (semantic diagnoses misuse — fail
    // loud, correct C behavior). Single round by design: candidates that
    // EMERGE on a reparse are not re-processed.
    //
    // Languages with no binder declarations record no candidates → this
    // whole block is a no-op scan (zero cost, zero behavior).
    std::uint32_t typeNameReparseCount = 0;
    {
        bool anyCandidates = false;
        for (auto const& sc : sidecars_) {
            if (!sc.candidates.empty()) { anyCandidates = true; break; }
        }
        if (anyCandidates) {
            std::unordered_set<std::string> oracle;
            for (auto const& sc : sidecars_) {
                for (auto const& n : sc.globalTypeNames) oracle.insert(n);
            }
            // D-CSUBSET-SHIPPED-TYPEDEF-CAST-PARSE: a SHIPPED header's typedefs
            // (`size_t` from <stddef.h>, `ptrdiff_t`, the stdint widths, …) are
            // injected SEMANTICALLY (post-parse), so they appear in NO tree's
            // `globalTypeNames` — the includer parsed `(size_t)(expr)` as a CALL
            // and recorded an AmbiguousTypeNameCandidate. Harvest each resolved
            // shipped descriptor's typedef NAMES into the oracle so the reparse
            // below seeds them as parse-time type names and COMMITS the cast (then
            // c43's offsetof fold applies → sqlite's `keyinfoSpace[offsetof(...)]`
            // dim is constant, S001C clears). Interner-free (names only); a scratch
            // reporter so a malformed descriptor is reported ONCE by the semantic
            // read (readShippedLibDescriptor), never double-reported here.
            for (auto const& ref : shippedLibDescriptors) {
                DiagnosticReporter scratch{budget_.withoutDedup().asConfig()};
                if (auto names = ffi::readShippedLibTypedefNames(ref.path, scratch)) {
                    for (auto& n : *names) oracle.insert(std::move(n));
                }
            }
            // `sidecars_` is index-parallel to `trees_` by construction
            // (addTree appends both) — fatal if that invariant ever broke.
            if (sidecars_.size() != trees_.size()) {
                cuFatal("UnitBuilder::finish: parse-sidecar vector out of "
                        "sync with trees");
            }
            for (std::size_t i = 0; i < trees_.size(); ++i) {
                auto& sc = sidecars_[i];
                if (sc.candidates.empty()) continue;
                // D-CSUBSET-FN-TYPE-TYPEDEF-PAREN-NAME: a typedef name is not
                // in scope within its OWN declarator (C 6.2.1p7). The oracle
                // seeds a name as a GLOBAL (position-independent) type for the
                // whole reparse, so seeding a typedef's OWN name would make its
                // own parenthesized-name declarator (`typedef int (F);`) reparse
                // as an abstract function-suffix param (F a param TYPE) instead
                // of the parenthesized declarator that NAMES F — S0017. A
                // candidate's span IS its self-defining occurrence exactly when
                // it equals one of THIS tree's own global-TYPE binding spans (a
                // byte range uniquely identifies one token in the buffer, so
                // span equality ⇒ same token ⇒ the definition site). A genuine
                // cross-file / in-buffer-FORWARD use has a DISTINCT span from
                // the definition (BothDirectionsResolveInlineInOneBuffer), and a
                // cross-file typedef's binding lives in ANOTHER tree — neither
                // is excluded here.
                auto spanKey = [](SourceSpan s) {
                    return (static_cast<std::uint64_t>(s.start()) << 32)
                           | s.end();
                };
                std::unordered_set<std::uint64_t> ownBindingSpans;
                ownBindingSpans.reserve(sc.globalTypeBindings.size());
                for (auto const& [name, span] : sc.globalTypeBindings) {
                    (void)name;
                    ownBindingSpans.insert(spanKey(span));
                }
                std::vector<std::string>        seeds;
                std::unordered_set<std::string> seen;
                for (auto const& cand : sc.candidates) {
                    if (!oracle.contains(cand.name)) continue;
                    if (ownBindingSpans.contains(spanKey(cand.span))) {
                        continue;   // self-defining occurrence — C 6.2.1p7
                    }
                    if (seen.insert(cand.name).second) {
                        seeds.push_back(cand.name);
                    }
                }
                if (seeds.empty()) continue;   // unresolved → value reading stands
                if (!sc.source || !sc.schema) {
                    // Candidates exist but the tree was injected via the
                    // raw addTree path (no source/schema handle) — we
                    // cannot reparse what we did not parse. Unreachable
                    // for builder-parsed trees; fail loud over silently
                    // dropping a resolvable cross-file type.
                    cuFatal("UnitBuilder::finish: type-name candidates on "
                            "a tree with no reparse handles");
                }
                // Same config-driven cap as the first parse — the reparse
                // re-walks the SAME (possibly deep) tree, so it must admit the
                // identical nesting depth or a clean first parse would fail the
                // oracle reparse.
                ParserConfig cfg = parserConfigFor(*sc.schema);
                cfg.seedGlobalTypeNames = std::move(seeds);
                // Build the reparse result. When this tree was preprocessed
                // (FC13), `sc.source` is the synthesized buffer and
                // re-tokenizing it would lose macro expansion + leave
                // directives in -- so rebuild an identical stream from the
                // retained preprocessed tokens, reparse with the type-name
                // seed, and re-apply the line-map remap so the reparsed tree's
                // diagnostics still attribute to the origin header/main file.
                // c97: the oracle reparse accumulates into its OWN phase
                // (Reparse, not Parse) so the ~2x front-end multiplier is
                // visible in the --time breakdown.
                substrate::PhaseTimers::Scope reparsePhase{
                    substrate::CompilePhase::Reparse};
                ParseResult result = [&] {
                    if (!sc.ppTokens.empty()) {
                        TokenStream stream =
                            TokenStream::fromTokens(sc.ppTokens);
                        Parser p{sc.source, sc.schema, std::move(stream),
                                 budget_, std::move(cfg), nullptr};
                        ParseResult r = std::move(p).parse();
                        if (sc.ppRemap) r.tree.remapDiagnostics(sc.ppRemap);
                        return r;
                    }
                    Tokenizer tk{sc.source, sc.schema, budget_};
                    auto [stream, lexDiags] = std::move(tk).tokenize();
                    Parser p{sc.source, sc.schema, std::move(stream),
                             budget_, std::move(cfg), std::move(lexDiags)};
                    return std::move(p).parse();
                }();
                trees_[i]          = std::move(result.tree);
                sc.candidates      = std::move(result.typeNameCandidates);
                sc.globalTypeNames = std::move(result.globalTypeNames);
                sc.globalTypeBindings = std::move(result.globalTypeBindings);
                ++typeNameReparseCount;
            }
            if (typeNameReparseCount > 0) {
                // A reparsed tree carries a NEW TreeId (and fresh NodeIds),
                // so every crossRefs edge built above is potentially stale.
                // Re-run the resolvers over the FINAL tree set into fresh
                // outputs. The first pass's driver diagnostics + descriptor
                // paths remain authoritative (they are path-keyed, not
                // tree-id-keyed); the re-resolve writes into scratch sinks
                // so nothing double-reports. Inputs are identical (every
                // include target is already loaded; loadAndAdd_ dedups by
                // canonical path), so the edge SET is the same — only the
                // ids are refreshed.
                crossRefs.clear();
                DiagnosticReporter scratchDiags{budget_.withoutDedup().asConfig()};
                std::vector<ShippedDescriptorRef> scratchDescriptors;
                ResolutionContext recontext{
                    trees_,
                    scratchDiags,
                    includeDirs_,
                    systemDirs_,
                    headerNameMatching_,
                    activeFormat_,   // identical inputs to the first pass
                    [this](std::filesystem::path const& path, bool& ok,
                           std::shared_ptr<GrammarSchema const> schema) {
                        return loadAndAdd_(path, ok, std::move(schema));
                    },
                    crossRefs,
                    scratchDescriptors,
                };
                resolvedSchemaIds.clear();
                // c97: the post-reparse re-resolve accumulates into the same
                // resolve-imports phase (its `runs` count shows the 2nd pass).
                substrate::PhaseTimers::Scope reresolvePhase{
                    substrate::CompilePhase::ResolveImports};
                for (auto const& schema : schemas_) {
                    if (!resolvedSchemaIds.insert(schema->schemaId().v).second) {
                        continue;
                    }
                    chooseResolver(schema)->resolve(recontext);
                }
            }
        }
    }

    // c105 (D-PP-USER-DEFINE fold): remap every shipped-descriptor ref's
    // carried `#include` span/buffer from SYNTH coordinates onto its ORIGIN
    // file, exactly as tree diagnostics are remapped. Pre-c105 this was
    // coincidentally correct — a no-splice TU's synth text was byte-identical
    // to the main source, so the c8 availability gate's F001D landed on the
    // right line by accident; the "<built-in>"/"<command-line>" prologues (and
    // equally any quote-splice BEFORE an angle include — a pre-existing latent
    // mis-attribution) shift the synth, so the ref must be remapped for real.
    // Each preprocessed tree's ppRemap closure self-gates on its own synth
    // buffer id, so running every ref through every remap is a no-op for
    // non-matching refs (and for non-preprocessed trees, which have none).
    for (auto& sc : sidecars_) {
        if (!sc.ppRemap) continue;
        for (auto& ref : shippedLibDescriptors) {
            sc.ppRemap(ref.buffer, ref.span);
        }
    }

    // ★★★ [[D-PP-SEMANTIC-DIAGNOSTIC-POSITION-UNREMAPPED]]: hand the line-map
    // remaps to the FINISHED CU, so they outlive this builder.
    //
    // Everything above this line is the builder's own use of them — the parse
    // tier's `remapDiagnostics`, the FC2 oracle reparse, the descriptor refs.
    // Every tier BELOW (semantic, HIR, MIR, LIR, the assembly engine, the
    // linker) re-derives its positions from the SAME trees and therefore re-mints
    // the same synthesized coordinates; until now it had nothing to convert them
    // with, and the mismatch was invisible because the synth buffer carries the
    // main source's NAME. Moving the closures onto the CU is what makes the
    // coordinate system a property of the compiled unit rather than of a builder
    // that has already been consumed.
    //
    // One entry per PREPROCESSED tree, in tree order. `sidecars_` is
    // index-parallel to `trees_` by construction; the ORDER carries no meaning
    // here (each closure self-gates on its own synth buffer), only the SET does.
    std::vector<PreprocessedPositionMap> preprocessedPositionMaps;
    for (auto& sc : sidecars_) {
        if (!sc.ppRemap) continue;
        preprocessedPositionMaps.push_back(
            PreprocessedPositionMap{sc.ppSynthBuffer, std::move(sc.ppRemap),
                                    std::move(sc.ppMap), sc.ppMainOrigin});
    }

    // TF-C82: flatten the per-tree `#pragma pack` stamps out of the sidecars, in
    // tree order. `sidecars_` is index-parallel to `trees_` by construction
    // (`addTree` appends exactly one sidecar), so this vector is too.
    std::vector<std::unordered_map<std::uint32_t, std::uint32_t>> pragmaPackMaps;
    pragmaPackMaps.reserve(sidecars_.size());
    for (auto& sc : sidecars_) pragmaPackMaps.push_back(std::move(sc.pragmaPack));
    // TF-C85: the same flatten for the `#pragma optimize` stamps, on the same
    // index-parallel-by-construction guarantee.
    std::vector<std::unordered_set<std::uint32_t>> pragmaNoOptimizeSets;
    pragmaNoOptimizeSets.reserve(sidecars_.size());
    for (auto& sc : sidecars_) {
        pragmaNoOptimizeSets.push_back(std::move(sc.pragmaNoOptimize));
    }

    finished_ = true;
    return CompilationUnit{
        CompilationUnit::PrivateTag{},
        id_,
        std::move(schema_),
        std::move(trees_),
        std::move(driverDiagnostics_),
        std::move(crossRefs),
        std::move(shippedLibDescriptors),
        typeNameReparseCount,
        std::move(auxiliaryBuffers_),
        std::move(pragmaPackMaps),
        std::move(pragmaNoOptimizeSets),
        std::move(preprocessedPositionMaps),
    };
}

} // namespace dss
