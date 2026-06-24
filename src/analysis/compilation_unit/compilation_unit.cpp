#include "analysis/compilation_unit/compilation_unit.hpp"

#include "analysis/compilation_unit/import_resolver.hpp"
#include "analysis/preprocess/preprocessor.hpp"
#include "analysis/syntactic/parser.hpp"
#include "core/substrate/mint_monotonic_id.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/source_buffer.hpp"
#include "tokenizer/tokenizer.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
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
                                 std::vector<std::filesystem::path>   shippedLibDescriptors,
                                 std::uint32_t                        typeNameReparseCount,
                                 std::vector<std::shared_ptr<SourceBuffer>> auxiliaryBuffers)
    : id_(id)
    , schema_(std::move(schema))
    , trees_(std::move(trees))
    , driverDiagnostics_(std::move(driverDiagnostics))
    , crossRefs_(std::move(crossRefs))
    , shippedLibDescriptors_(std::move(shippedLibDescriptors))
    , typeNameReparseCount_(typeNameReparseCount)
    , auxiliaryBuffers_(std::move(auxiliaryBuffers)) {}

CompilationUnit::~CompilationUnit()                                            = default;
CompilationUnit::CompilationUnit(CompilationUnit&&) noexcept                   = default;
CompilationUnit& CompilationUnit::operator=(CompilationUnit&&) noexcept        = default;

// ── CompilationUnit accessors ─────────────────────────────────────────────
CompilationUnitId             CompilationUnit::id()                const noexcept { return id_; }
std::span<Tree const>         CompilationUnit::trees()             const noexcept { return trees_; }
DiagnosticReporter const&     CompilationUnit::driverDiagnostics() const noexcept { return driverDiagnostics_; }
std::span<CrossTreeRef const> CompilationUnit::crossRefs()         const noexcept { return crossRefs_; }
std::span<std::filesystem::path const>
CompilationUnit::shippedLibDescriptors() const noexcept { return shippedLibDescriptors_; }
std::span<std::shared_ptr<SourceBuffer> const>
CompilationUnit::auxiliaryBuffers() const noexcept { return auxiliaryBuffers_; }

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
UnitBuilder::UnitBuilder(std::shared_ptr<GrammarSchema const> schema)
    : id_(CompilationUnit::nextId())
    , schema_(std::move(schema))
    // Driver diagnostics are keyed by PATH, not by source span — every
    // D_FileNotFound / D_DuplicateFile shares (code, InvalidBuffer, empty
    // span), so the reporter's span-based dedup window would silently
    // collapse N distinct missing files into one message. Disable dedup on
    // this reporter so each bad path surfaces. (Per-tree reporters keep
    // their dedup; this disables it only for the CU's driver-level stream.)
    , driverDiagnostics_(DiagnosticReporter::Config{.dedupWindow = 0}) {
    if (!schema_) {
        cuFatal("UnitBuilder: schema is null");
    }
    schemas_.push_back(schema_);   // primary is also the sole registry entry
}

UnitBuilder::UnitBuilder(std::vector<std::shared_ptr<GrammarSchema const>> schemas)
    : id_(CompilationUnit::nextId())
    , driverDiagnostics_(DiagnosticReporter::Config{.dedupWindow = 0}) {
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
        PreprocessResult pp = preprocess(src, schema, includeDirs_);
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
        // `pp.tokens` is Eof-terminated by contract; its last element is
        // that Eof. `fromTokens` takes its argument by value (copies),
        // so `pp.tokens` survives intact for the sidecar move below.
        TokenStream stream =
            ppFatal
                ? TokenStream::fromTokens({pp.tokens.back()})
                : TokenStream::fromTokens(pp.tokens);
        Parser p{synth, schema, std::move(stream), parserConfigFor(*schema),
                 std::move(pp.diagnostics)};
        ParseResult result = std::move(p).parse();
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
        sidecar.source          = std::move(synth);
        sidecar.schema          = std::move(schema);
        sidecar.ppTokens        = std::move(pp.tokens);
        sidecar.ppRemap         = std::move(remap);
        return trees_.back().id();
    }
    Tokenizer tk{src, schema};
    auto [stream, lexDiags] = std::move(tk).tokenize();
    Parser p{src, schema, std::move(stream), parserConfigFor(*schema),
             std::move(lexDiags)};
    ParseResult result = std::move(p).parse();
    addTree(std::move(result.tree));
    // FC2: fill the sidecar addTree just pushed — the parse's ambiguous
    // type-name candidates + exported global type names (the finish()-time
    // oracle's inputs) and the handles a one-shot reparse needs.
    auto& sidecar           = sidecars_.back();
    sidecar.candidates      = std::move(result.typeNameCandidates);
    sidecar.globalTypeNames = std::move(result.globalTypeNames);
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

std::shared_ptr<GrammarSchema const>
UnitBuilder::schemaForPath_(std::filesystem::path const& path) const {
    // Match the path's extension (case-insensitively, including the leading dot)
    // against each registered schema's declared `fileExtensions`; first
    // registered match wins. Returns null on no match — `addFile` decides
    // whether that is the single-schema fall-through or a multi-language error.
    std::string const ext = asciiLower(path.extension().string());
    if (ext.empty()) return nullptr;
    for (auto const& s : schemas_) {
        for (std::string_view declared : s->fileExtensions()) {
            if (asciiLower(declared) == ext) return s;
        }
    }
    return nullptr;
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

TreeId UnitBuilder::loadAndAdd_(std::filesystem::path const& path, bool& ok,
                               std::shared_ptr<GrammarSchema const> schema) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    std::string const key = (ec ? path.lexically_normal() : canonical).string();

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
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(label, ec);
    std::string const key =
        (ec ? std::filesystem::path(label).lexically_normal() : canonical).string();
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
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    std::string const key =
        (ec ? path.lexically_normal() : canonical).string();
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
    std::shared_ptr<GrammarSchema const> schema = schemaForPath_(path);
    if (!schema) {
        if (schemas_.size() == 1) {
            schema = schema_;
        } else {
            reportDriver(driverDiagnostics_, DiagnosticCode::D_UnknownFileExtension,
                         DiagnosticSeverity::Error, InvalidBuffer, path.string());
            return;  // do not parse under an arbitrary grammar.
        }
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
    std::vector<std::filesystem::path> shippedLibDescriptors;
    ResolutionContext context{
        trees_,
        driverDiagnostics_,
        includeDirs_,
        systemDirs_,
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
    for (auto const& schema : schemas_) {
        if (!resolvedSchemaIds.insert(schema->schemaId().v).second) continue;
        chooseResolver(schema)->resolve(context);
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
            // `sidecars_` is index-parallel to `trees_` by construction
            // (addTree appends both) — fatal if that invariant ever broke.
            if (sidecars_.size() != trees_.size()) {
                cuFatal("UnitBuilder::finish: parse-sidecar vector out of "
                        "sync with trees");
            }
            for (std::size_t i = 0; i < trees_.size(); ++i) {
                auto& sc = sidecars_[i];
                if (sc.candidates.empty()) continue;
                std::vector<std::string>        seeds;
                std::unordered_set<std::string> seen;
                for (auto const& cand : sc.candidates) {
                    if (!oracle.contains(cand.name)) continue;
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
                ParseResult result = [&] {
                    if (!sc.ppTokens.empty()) {
                        TokenStream stream =
                            TokenStream::fromTokens(sc.ppTokens);
                        Parser p{sc.source, sc.schema, std::move(stream),
                                 std::move(cfg), nullptr};
                        ParseResult r = std::move(p).parse();
                        if (sc.ppRemap) r.tree.remapDiagnostics(sc.ppRemap);
                        return r;
                    }
                    Tokenizer tk{sc.source, sc.schema};
                    auto [stream, lexDiags] = std::move(tk).tokenize();
                    Parser p{sc.source, sc.schema, std::move(stream),
                             std::move(cfg), std::move(lexDiags)};
                    return std::move(p).parse();
                }();
                trees_[i]          = std::move(result.tree);
                sc.candidates      = std::move(result.typeNameCandidates);
                sc.globalTypeNames = std::move(result.globalTypeNames);
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
                DiagnosticReporter scratchDiags{
                    DiagnosticReporter::Config{.dedupWindow = 0}};
                std::vector<std::filesystem::path> scratchDescriptors;
                ResolutionContext recontext{
                    trees_,
                    scratchDiags,
                    includeDirs_,
                    systemDirs_,
                    [this](std::filesystem::path const& path, bool& ok,
                           std::shared_ptr<GrammarSchema const> schema) {
                        return loadAndAdd_(path, ok, std::move(schema));
                    },
                    crossRefs,
                    scratchDescriptors,
                };
                resolvedSchemaIds.clear();
                for (auto const& schema : schemas_) {
                    if (!resolvedSchemaIds.insert(schema->schemaId().v).second) {
                        continue;
                    }
                    chooseResolver(schema)->resolve(recontext);
                }
            }
        }
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
    };
}

} // namespace dss
