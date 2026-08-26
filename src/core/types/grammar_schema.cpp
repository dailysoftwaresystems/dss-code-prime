#include "core/types/grammar_schema.hpp"

#include "core/crypto/sha256.hpp"   // crypto::sha256Hex — the retained content digest
#include "core/substrate/checked_file_read.hpp"   // the ONE checked whole-file read
#include "core/substrate/phase_timers.hpp"        // the load-config / build-config phases
#include "core/types/config_path_walk.hpp"
#include "core/types/grammar_schema_json.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace dss {

namespace {

// ★ WHERE A LANGUAGE DOCUMENT LIVES, SPELLED ONCE. Three sites need the same
// two strings — `loadShipped` WALKS for `<sources>/<stem><.lang.json>`,
// `loadFromText` RECOVERS the stem back out of the label it was handed, and
// `configDocumentPath` NAMES the document again for the runtime cache key. Two
// copies of that pair can drift, and a drifted pair is a cache key naming a
// document nobody loaded — an entry keyed on a fiction. One owner, no drift.
constexpr std::string_view kLanguageConfigSubdir = "sources";
constexpr std::string_view kLanguageConfigSuffix = ".lang.json";

// c97 sealing helper: the depth-first RuleLeaf-branch enumeration over one
// rule's position graph — the SAME walk (same order, same first-occurrence
// dedup, same cycle guard) the former per-call `altRuleBranches` ran; now
// executed ONCE per AltChoice position at schema construction and stored on
// the Position. Pure config-data transform; names no token/rule/language.
[[nodiscard]] std::vector<RuleId> collectAltBranchRules(
    std::vector<detail::Position> const& positions, std::uint32_t startPos) {
    std::vector<RuleId> out;
    std::vector<std::uint32_t> visitedPos;
    auto walk = [&](auto&& self, std::uint32_t posId) -> void {
        if (posId >= positions.size()) return;
        for (auto const seen : visitedPos) {
            if (seen == posId) return;
        }
        visitedPos.push_back(posId);
        auto const& p = positions[posId];
        if (p.slotKind() == SlotKind::RuleLeaf) {
            const RuleId r = p.ruleId();
            for (auto const existing : out) {
                if (existing.v == r.v) return;   // first occurrence wins
            }
            out.push_back(r);
            return;
        }
        if (p.slotKind() == SlotKind::AltChoice) {
            for (auto bid : p.branches()) {
                // D-PARSE-SPECULATIVE-OPTIONAL: when enumerating a SPECULATIVE
                // optional's OWN candidate set (posId == startPos), its
                // skip/continuation branch is NOT one of its alternatives —
                // those rule-leaves belong to the enclosing sequence tail.
                // Excluding it makes a peek that matches only the continuation
                // yield candidates=[] so the parser's nullable skip fires and
                // the tail parses non-speculatively. Gated to the walk ROOT so
                // an ENCLOSING non-speculative optional still reaches those
                // rules through its own (deeper) traversal — the non-
                // speculative `altRuleBranches` route stays byte-identical.
                if (posId == startPos && p.speculative() && p.hasSkipBranch()
                    && bid == p.skipBranch()) {
                    continue;
                }
                self(self, bid);
            }
        }
        // TokenLeaf / End: not a rule branch — token routing goes
        // through `advance`.
    };
    walk(walk, startPos);
    return out;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
// Per-lead-byte declared-length index (see the header for the contract)
// ─────────────────────────────────────────────────────────────────────────

void detail::LexemeLengthIndex::build(LexemeTable const& table) {
    rowStart_.fill(0);
    lengths_.clear();

    // (lead byte, key length) for every declared key, sorted so rows come out
    // grouped by lead byte and LONGEST-FIRST within a row — the order the
    // scan walks them in, so the first hit is the longest match.
    //
    // ⓘ An empty key is skipped deliberately, and that is not a hole: a
    // longest-match scan probes lengths 1..N and never 0, so a zero-length
    // key was already unreachable before this index existed. Skipping it
    // keeps the two behaviours identical rather than inventing a length the
    // scanner cannot ask about.
    //
    // ★ THE SORT MAKES THE RESULT INDEPENDENT OF THE TABLE'S ITERATION ORDER,
    // which matters because that order is a hash-bucket accident. Two schemas
    // with the same declared keys get byte-identical indexes.
    std::vector<std::pair<unsigned char, std::uint32_t>> pairs;
    pairs.reserve(table.size());
    for (auto const& [lex, meanings] : table) {
        (void)meanings;
        if (lex.empty()) continue;
        pairs.emplace_back(static_cast<unsigned char>(lex.front()),
                           static_cast<std::uint32_t>(lex.size()));
    }
    std::sort(pairs.begin(), pairs.end(),
              [](auto const& a, auto const& b) noexcept {
                  if (a.first != b.first) return a.first < b.first;
                  return a.second > b.second;
              });
    // Two keys of the same length under the same lead byte are ONE probe.
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());

    lengths_.reserve(pairs.size());
    std::size_t next = 0;   // lowest lead byte whose row start is still unwritten
    for (auto const& [lead, len] : pairs) {
        while (next <= lead) {
            rowStart_[next++] = static_cast<std::uint32_t>(lengths_.size());
        }
        lengths_.push_back(len);
    }
    while (next <= 256) {
        rowStart_[next++] = static_cast<std::uint32_t>(lengths_.size());
    }
}

GrammarSchema::GrammarSchema(detail::GrammarSchemaData&& d) noexcept : d_(std::move(d)) {
#ifndef NDEBUG
    // Cross-check the loader-computed `maxLexemeLength` against a fresh
    // recomputation. Catches a loader-side bug that would otherwise
    // silently truncate the tokenizer's longest-match window. Debug-
    // only — the production builds trust the loader (single call site
    // for the field).
    std::size_t recomputed = 0;
    for (auto const& [lex, meanings] : d_.lexemeTable) {
        (void)meanings;
        if (lex.size() > recomputed) recomputed = lex.size();
    }
    for (auto const& [modeId, table] : d_.lexerModeTokens) {
        (void)modeId;
        for (auto const& [lex, meanings] : table) {
            (void)meanings;
            if (lex.size() > recomputed) recomputed = lex.size();
        }
    }
    assert(recomputed == d_.maxLexemeLength
           && "GrammarSchemaData::maxLexemeLength out of sync with lexemeTable");
#endif

    // ── c97 sealing pass ──────────────────────────────────────────────
    // Drain the loader's build-time `compiledRules` map into the dense
    // RuleId-indexed vector, then derive the O(1) query companions —
    // per-set membership bitsets + the precomputed AltChoice branch
    // enumerations. Load-time-only work (one pass over static config
    // data); every per-token query downstream becomes an array index /
    // bit test. RuleIds are dense interner ids (1..N), so the vector is
    // sized by the interner (belt-and-braces: also covers any key above
    // it). Runs for EVERY construction path — the JSON loader and tests
    // that build GrammarSchemaData directly.
    {
        std::size_t denseSize = d_.rules ? d_.rules->size() : 0;
        for (auto const& [rid, rule] : d_.compiledRules) {
            (void)rule;
            if (rid + 1 > denseSize) denseSize = rid + 1;
        }
        compiledDense_.resize(denseSize);
        for (auto& [rid, rule] : d_.compiledRules) {
            compiledDense_[rid] = std::move(rule);
        }
        d_.compiledRules.clear();

        std::size_t const universe =
            d_.schemaTokens ? d_.schemaTokens->size() : 0;
        for (auto& rule : compiledDense_) {
            rule.firstBits = detail::buildTokenBits(rule.firstSet, universe);
            rule.prefixBits.clear();
            rule.prefixBits.reserve(rule.predictivePrefix.size());
            for (auto const& offsetSet : rule.predictivePrefix) {
                rule.prefixBits.push_back(
                    detail::buildTokenBits(offsetSet, universe));
            }
            for (auto& pos : rule.positions) {
                pos.sealExpectedBits(universe);
                // Seal the branch enumeration for EVERY slot kind — the
                // walk itself resolves each correctly (AltChoice → DFS,
                // RuleLeaf → {rule}, TokenLeaf/End → empty), preserving
                // the former per-call function's full contract.
                pos.sealAltBranchRules(collectAltBranchRules(
                    rule.positions,
                    static_cast<std::uint32_t>(&pos - rule.positions.data())));
            }
        }
    }

    // ── longest-match probe index ─────────────────────────────────────
    // D-PERF-TOK-LONGEST-MATCH-PROBES-EVERY-DECLARED-LENGTH-AT-EVERY-POSITION.
    // Derived HERE, in the same sealing pass and for the same reason as the
    // dense rule table above: this is the single point where the loader's
    // build-time maps stop changing, and it runs for EVERY construction path
    // — the JSON loader and the tests that assemble a GrammarSchemaData by
    // hand. ★ THAT PLACEMENT IS THE FAIL-LOUD ARGUMENT. Building the index
    // in the loader would create a second source of truth that a later
    // insertion could leave stale, and a stale probe index does not degrade
    // — it silently stops matching a lexeme, which is a wrong parse rather
    // than a parse error. Derived from the sealed table, there is nothing
    // for it to be stale WITH.
    {
        lexemeLengths_.build(d_.lexemeTable);

        // Sized by the mode table, and belt-and-braces by any override id
        // ABOVE it — the same posture `compiledDense_` takes above, and for
        // the same reason: a hand-assembled GrammarSchemaData is a supported
        // construction path, and refusing one here would turn a schema that
        // merely never queries that mode into a load-time abort.
        std::size_t denseModes = d_.lexerModes.size();
        for (auto const& [modeV, table] : d_.lexerModeTokens) {
            (void)table;
            if (std::size_t{modeV} + 1 > denseModes) denseModes = modeV + 1;
        }
        modeLexemeLengths_.assign(denseModes, {});
        for (auto const& [modeV, table] : d_.lexerModeTokens) {
            modeLexemeLengths_[modeV].build(table);
        }

        // FAIL-LOUD, ALWAYS ON: every declared key must be reachable through
        // the row the scanner will actually walk. The global half goes through
        // the PUBLIC accessor and the per-mode half through the very row
        // object that accessor hands back, so this catches the refactor that
        // indexes one table and queries another — the failure mode that would
        // otherwise present as a lexeme which quietly stopped existing.
        // O(keys), once per schema load, against config data that is hundreds
        // of entries: unmeasurable next to the JSON parse it follows, and NOT
        // debug-only, because a Release build is exactly where a silent
        // miscompile would ship.
        for (auto const& [lex, meanings] : d_.lexemeTable) {
            (void)meanings;
            if (lex.empty()) continue;
            auto const lens =
                lexemeLengthsForLeadByte(static_cast<unsigned char>(lex.front()));
            if (std::find(lens.begin(), lens.end(),
                          static_cast<std::uint32_t>(lex.size())) == lens.end()) {
                std::fprintf(stderr,
                    "dss::GrammarSchema: global lexeme '%s' (%zu bytes) is "
                    "absent from the probe index for its lead byte — the "
                    "tokenizer would never match it\n",
                    lex.c_str(), lex.size());
                std::abort();
            }
        }
        for (auto const& [modeV, table] : d_.lexerModeTokens) {
            for (auto const& [lex, meanings] : table) {
                (void)meanings;
                if (lex.empty()) continue;
                auto const lens =
                    modeLexemeLengths_[modeV].lengthsFor(
                        static_cast<unsigned char>(lex.front()));
                if (std::find(lens.begin(), lens.end(),
                              static_cast<std::uint32_t>(lex.size())) == lens.end()) {
                    std::fprintf(stderr,
                        "dss::GrammarSchema: mode %u lexeme '%s' (%zu bytes) is "
                        "absent from that mode's probe index for its lead byte "
                        "— the tokenizer would never match it\n",
                        modeV, lex.c_str(), lex.size());
                    std::abort();
                }
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Loaders — thin shims over the JSON-aware loader.
// ─────────────────────────────────────────────────────────────────────────

LoadResult<std::shared_ptr<GrammarSchema>> GrammarSchema::loadFromFile(
    std::filesystem::path const& path) {

    // `load-config` — the FILE route's own row in the `--time` report, and the
    // outermost of the three config seams. It spans the read, the digest, the
    // memo lookup and (on a miss only) the nested `build-config`. A memo HIT
    // therefore costs exactly this row and nothing else, which is the whole
    // point of measuring them separately.
    // ⓘ Opened HERE and not in `loadFromText`: the two nest, and a scope nested
    // inside another scope of the SAME phase would report a peak concurrency of
    // 2 on a strictly serial load. Every file route reaches this function.
    substrate::PhaseTimers::Scope const loadScope{
        substrate::CompilePhase::LoadConfig};

    // THE ONE CHECKED READ (D-CORE-SHIPPED-CONFIG-LOADERS-DRAIN-A-STREAM-WITHOUT-CHECKING-IT).
    // A failed read is reported AS a read failure and never reaches
    // `loadFromText` -- a truncated document handed to the parser produces a
    // syntax error about the config's CONTENTS, which sends the reader to the
    // wrong file entirely.
    auto text = core::readFileChecked(path);
    if (!text) {
        return std::unexpected(std::vector<ConfigDiagnostic>{
            {DiagnosticCode::C_MissingField, DiagnosticSeverity::Error,
             path.string(),
             std::move(text).error().message}});
    }
    return loadFromText(*std::move(text), path.string());
}

LoadResult<std::shared_ptr<GrammarSchema>> GrammarSchema::loadShipped(std::string_view name) {
    auto path = findShippedConfig({name, kLanguageConfigSubdir,
                                   kLanguageConfigSuffix, "language",
                                   DiagnosticCode::C_InvalidLanguageName});
    if (!path) return std::unexpected(std::move(path).error());
    return loadFromFile(*path);
}

// The config-root-relative document path — see the header for the contract.
// TOTAL, and deliberately BRANCHLESS AT THE CALL SITE: every caller wants one
// string for the key line `doc=<label>:<path>:<digest>`, and a caller forced to
// test for emptiness is a caller free to substitute `name()` when the test
// fires, which is the exact substitution `configName()` exists to prevent.
std::string GrammarSchema::configDocumentPath() const {
    if (configName_.empty()) {
        // ⓘ NOT `sources/.lang.json`. A schema with no document must not name
        // one: that spelling is a real path shape (a file literally named
        // `.lang.json` would collide with it) and it reads, to anyone auditing
        // a key document, as a document that was loaded. This term is not a
        // path in any tree and says so, and the digest beside it in the key
        // line still carries the identity — `computeRuntimeObjectKey` refuses
        // an EMPTY digest outright, so an unidentifiable input is already a
        // refusal there rather than a silent key term here.
        return "<inline language: " + std::string{name()} + ">";
    }
    std::string out{kLanguageConfigSubdir};
    out += '/';
    out += configName_;
    out += kLanguageConfigSuffix;
    return out;
}

LoadResult<std::shared_ptr<GrammarSchema>> GrammarSchema::loadFromText(
    std::string_view jsonText,
    std::string_view sourceLabel) {

    // ── Content digest ────────────────────────────────────────────────
    // Digest the bytes AS RECEIVED, before the parser is allowed an
    // opinion about them. This is the one chokepoint where the document
    // bytes are already in memory (`loadFromFile` reads them, hands them
    // here, and drops them), so the digest costs zero extra I/O. Computed
    // BEFORE the parse so it is the digest of what was actually LOADED,
    // independent of what the parse made of it. See `contentDigest()` for
    // the full rationale and for why a non-`loadFromText` construction
    // leaves it EMPTY.
    //
    // ⚠ THE NUMBER THAT USED TO STAND HERE WAS FALSE AND IT POINTED THE NEXT
    // READER AT THE WRONG HALF OF THE PROBLEM. It read "versus ~165 ms per
    // invocation to re-walk and re-read `src/dss-config/` (MEASURED
    // 2026-08-17, I/O-dominated)", which says the expensive part of a config
    // load is the FILESYSTEM. ✔RE-MEASURED 2026-08-25 (cycle P35) by
    // instrumenting the three `loadShipped` entry points and
    // `buildSchemaFromJsonText` on a Windows Debug build of
    // `int main(void){return 0;}` — 13 grammar loads, 2,068,338 bytes of JSON:
    //     precedence walk           2 ms
    //     file read                 1 ms   <- the I/O is a rounding error
    //     sha256 of the bytes      30 ms
    //     parse + construct       211 ms   <- 87% of the grammar cost
    // So the load is PARSE-dominated, not I/O-dominated, and the old figure
    // was ~55x the measured filesystem cost. What actually justifies digesting
    // every load is not an avoided re-read at all: it is that the digest is
    // the memo KEY immediately below, which turns those 211 ms into one build
    // per distinct document per process. The three `--time` rows
    // (`locate-config` / `load-config` / `build-config`) now carry this
    // decomposition continuously, so the next reader measures instead of
    // trusting a comment.
    std::string digest = crypto::sha256Hex(jsonText);

    // ── THE CONTENT-ADDRESSED MEMO ────────────────────────────────────
    // The digest above is the key, so consulting the memo costs nothing that
    // was not already spent. A hit hands back the SAME `GrammarSchema` a
    // previous load of these exact bytes under this exact label produced —
    // which is sound because the type has no non-const member function at all:
    // it is sealed by its constructor and read-only from then on, so two
    // holders of one instance cannot observe each other. See
    // `config_document_memo.hpp` for why the key is the bytes and not the name,
    // and for the referenced-document half the digest cannot see.
    if (auto hit = detail::ConfigDocumentMemo<GrammarSchema>::lookup(sourceLabel,
                                                                    digest)) {
        return hit;
    }

    // `build-config` — DEFINED as the work a memo hit skips, which is why the
    // scope starts after the lookup above and covers nothing else. Its `runs`
    // IS this process's miss count for this family, so the `--time` report
    // answers "how many documents did we actually build" without a second
    // counter that could disagree with it.
    auto schema = [&] {
        substrate::PhaseTimers::Scope const buildScope{
            substrate::CompilePhase::BuildConfig};
        return detail::buildSchemaFromJsonText(jsonText, sourceLabel);
    }();
    if (schema) {
        (*schema)->contentDigest_ = digest;
        // ── THE CONFIG NAME, DERIVED FROM THE DOCUMENT'S OWN FILENAME ──────
        // ★ From the PATH and not from the entry point: `loadShipped(name)`
        // knows the stem but `loadFromFile(path)` does not go through it, and
        // both share the memo entry below — so deriving it in `loadShipped`
        // would make the answer depend on which call populated the memo first.
        // The filename is the one input both have and both agree on.
        // ⓘ Left EMPTY for `<inline>` and for any label that is not a
        // `.lang.json`: absent is honest, and `configName()`'s contract says a
        // caller needing a path must refuse on empty rather than substitute
        // `name()`.
        {
            std::string_view           label{sourceLabel};
            constexpr std::string_view kSuffix{kLanguageConfigSuffix};
            auto const slash = label.find_last_of("/\\");
            if (slash != std::string_view::npos) label.remove_prefix(slash + 1);
            // ★ THE LENGTH TEST IS AN UNDERFLOW GUARD FIRST AND A BOUNDARY
            // SECOND — ✔MEASURED 2026-08-25 by mutation. Dropping it entirely
            // makes `label.size() - kSuffix.size()` wrap for any label shorter
            // than the suffix: the DEFAULT `<inline>` label (8 bytes) computed
            // `__pos 18446744073709551614` and `compare` threw, taking 106
            // cases of `core/test_grammar_schema` down with it.
            // ⓘ `>` vs `>=` is NOT observable — at equality the stem would be
            // `substr(0, 0)`, empty either way — so `>` states the intent (a
            // leaf that IS the suffix names nothing) rather than deciding an
            // outcome. Do not read the pins below as pinning that choice.
            if (label.size() > kSuffix.size()
                && label.compare(label.size() - kSuffix.size(), kSuffix.size(),
                                 kSuffix)
                       == 0) {
                (*schema)->configName_ =
                    std::string{label.substr(0, label.size() - kSuffix.size())};
            }
        }
        // ⚠ Stored only on the SUCCESS path. A failed load produced diagnostics
        // and no schema; memoizing "these bytes are broken" would be a second,
        // parallel definition of failure, and the loader's own refusal is
        // already the one that reports it — every time, with the same message.
        detail::ConfigDocumentMemo<GrammarSchema>::store(
            std::string{sourceLabel}, std::move(digest),
            std::vector<detail::ConfigDocumentDependency>{
                (*schema)->referencedDocuments().begin(),
                (*schema)->referencedDocuments().end()},
            *schema);
    }

    return schema;
}

// ─────────────────────────────────────────────────────────────────────────
// Read-only queries
// ─────────────────────────────────────────────────────────────────────────

std::span<LexemeMeaning const> GrammarSchema::lookupLexeme(std::string_view lexeme) const noexcept {
    // Heterogeneous `find` — the probe bytes are queried AS a string_view.
    // This used to be `find(std::string{lexeme})`, which copied the probe on
    // every lookup and malloc'd on every probe past 15 bytes; see
    // `detail::LexemeHash`.
    auto it = d_.lexemeTable.find(lexeme);
    if (it == d_.lexemeTable.end()) return {};
    return it->second;
}

bool GrammarSchema::isEmptySpace(SchemaTokenId id) const noexcept {
    return d_.emptySpaceTokens.contains(id.v);
}

bool GrammarSchema::declaresLexemeToken(SchemaTokenId id) const noexcept {
    return d_.declaredLexemeTokens.contains(id.v);
}

NodeFlags GrammarSchema::flagsForKind(SchemaTokenId id) const noexcept {
    // No schema field populates per-kind flags today, so every kind
    // returns None. The accessor is the structural channel the
    // numeric-literal emit site reads — a future `literalFlags`
    // schema field plugs in here without a tokenizer change. The
    // valid-id check bakes the contract NOW, while there are zero
    // data-bearing callers: future schemas wiring real data in must
    // not silently accept an invalid id.
    if (!id.valid()) return NodeFlags::None;
    return NodeFlags::None;
}

std::span<LexerMode const> GrammarSchema::lexerModes() const noexcept {
    // Slot 0 is the InvalidLexerMode sentinel — internal indexing
    // detail. Real modes start at index 1; subspan(1) hides the
    // sentinel from every iterating consumer.
    if (d_.lexerModes.size() <= 1) return {};
    return std::span<LexerMode const>(d_.lexerModes).subspan(1);
}

LexerModeId GrammarSchema::findLexerMode(std::string_view name) const noexcept {
    auto it = d_.lexerModeIds.find(std::string{name});
    return (it == d_.lexerModeIds.end()) ? InvalidLexerMode : it->second;
}

LexerMode const& GrammarSchema::lexerMode(LexerModeId id) const noexcept {
    if (!id.valid() || id.v >= d_.lexerModes.size()) {
        std::fprintf(stderr,
            "dss::GrammarSchema::lexerMode: invalid LexerModeId (v=%u, "
            "table_size=%zu)\n",
            id.v, d_.lexerModes.size());
        std::abort();
    }
    return d_.lexerModes[id.v];
}

StringStyle const* GrammarSchema::stringStyle(LexemeMeaning const& m) const noexcept {
    if (m.schemaId.v != d_.id.v) {
        std::fprintf(stderr,
            "dss::GrammarSchema::stringStyle: LexemeMeaning belongs to a "
            "different schema (meaning.schemaId=%u, this.schemaId=%u)\n",
            m.schemaId.v, d_.id.v);
        std::abort();
    }
    if (!m.stringStyleId.valid()) return nullptr;
    if (m.stringStyleId.v >= d_.stringStyles.size()) {
        std::fprintf(stderr,
            "dss::GrammarSchema::stringStyle: out-of-range stringStyleId "
            "(v=%u, pool_size=%zu)\n",
            m.stringStyleId.v, d_.stringStyles.size());
        std::abort();
    }
    return &d_.stringStyles[m.stringStyleId.v];
}

std::span<LexemeMeaning const>
GrammarSchema::lookupLexemeInMode(LexerModeId mode, std::string_view lexeme) const noexcept {
    if (!mode.valid() || mode.v >= d_.lexerModes.size()) {
        std::fprintf(stderr,
            "dss::GrammarSchema::lookupLexemeInMode: invalid LexerModeId "
            "(v=%u, table_size=%zu)\n",
            mode.v, d_.lexerModes.size());
        std::abort();
    }
    auto modeIt = d_.lexerModeTokens.find(mode.v);
    if (modeIt == d_.lexerModeTokens.end()) return {};
    auto lexIt = modeIt->second.find(lexeme);   // heterogeneous — see lookupLexeme
    if (lexIt == modeIt->second.end()) return {};
    return lexIt->second;
}

// The per-mode probe row. Holds `lookupLexemeInMode`'s strong-id contract
// verbatim — an invalid or out-of-range id ABORTS rather than answering
// "nothing declared", so a caller can never read a mis-typed mode id as a
// mode with no overrides and silently lose every token that mode defines.
std::span<std::uint32_t const>
GrammarSchema::lexemeLengthsForLeadByteInMode(LexerModeId mode,
                                              unsigned char lead) const noexcept {
    if (!mode.valid() || mode.v >= d_.lexerModes.size()) {
        std::fprintf(stderr,
            "dss::GrammarSchema::lexemeLengthsForLeadByteInMode: invalid "
            "LexerModeId (v=%u, table_size=%zu)\n",
            mode.v, d_.lexerModes.size());
        std::abort();
    }
    if (mode.v >= modeLexemeLengths_.size()) return {};
    return modeLexemeLengths_[mode.v].lengthsFor(lead);
}

namespace {

// Locate the position record for a cursor, or nullptr if the cursor is
// invalid / out of range. The cursor's `posId == 0` sentinel matches the
// loader's `positions[0]` placeholder, so this also blocks default-
// constructed cursors before they touch real data. (c97: reads the dense
// table — an id with no compiled body has an EMPTY positions vector, so
// every posId is out of range, exactly the former map-miss result.)
[[nodiscard]] detail::Position const* lookupPos(
    detail::CompiledRule const* rule, SchemaCursor cur) noexcept {
    if (!cur.valid() || rule == nullptr) return nullptr;
    auto const& positions = rule->positions;
    if (cur.posId() >= positions.size()) return nullptr;
    return &positions[cur.posId()];
}

} // namespace

SchemaCursor GrammarSchema::rootCursor() const noexcept {
    if (!d_.rootRule.valid()) return SchemaCursor{};
    auto const* r = ruleRow(d_.rootRule.v);
    // entryPos == 0 ⇒ no compiled body (see CompiledRule) — the former
    // map-miss result. A real rule's entry is never the 0 sentinel.
    if (r == nullptr || r->entryPos == 0) return SchemaCursor{};
    return SchemaCursor{d_.rootRule, r->entryPos};
}

SchemaCursor GrammarSchema::enterRule(RuleId rule) const noexcept {
    auto const* r = ruleRow(rule.v);
    if (r == nullptr || r->entryPos == 0) return SchemaCursor{};
    return SchemaCursor{rule, r->entryPos};
}

SchemaCursor GrammarSchema::leaveRule(SchemaCursor parentCur) const noexcept {
    auto const* p = lookupPos(ruleRow(parentCur.rule().v), parentCur);
    if (p == nullptr) return SchemaCursor{};
    if (p->slotKind() != SlotKind::RuleLeaf) return SchemaCursor{};
    return SchemaCursor{parentCur.rule(), p->nextPos()};
}

SchemaCursor GrammarSchema::routeToRuleLeaf(SchemaCursor parentCur,
                                            RuleId rule) const noexcept {
    auto const* p = lookupPos(ruleRow(parentCur.rule().v), parentCur);
    if (p == nullptr) return SchemaCursor{};
    if (p->slotKind() == SlotKind::RuleLeaf && p->ruleId().v == rule.v) {
        return parentCur;
    }
    if (p->slotKind() == SlotKind::AltChoice) {
        // Try each branch; first match wins (the loader already rejects
        // ambiguous alts at load time via C_AmbiguousAlternatives, so
        // any two RuleLeaf branches in the same AltChoice would have
        // been flagged before reaching here).
        for (auto bid : p->branches()) {
            SchemaCursor probe{parentCur.rule(), bid};
            auto found = routeToRuleLeaf(probe, rule);
            if (found.valid()) return found;
        }
    }
    return SchemaCursor{};
}

std::span<RuleId const> GrammarSchema::altRuleBranches(SchemaCursor cur) const noexcept {
    // c97: the DFS over AltChoice branch edges (declared JSON-array order,
    // first occurrence wins, positional-cycle guard — see
    // `collectAltBranchRules`) is precomputed per position at schema
    // construction; this is now a span read. Same contract as the former
    // per-call walk, including `{rule}` at a RuleLeaf position and empty
    // for invalid cursors / token positions.
    auto const* p = lookupPos(ruleRow(cur.rule().v), cur);
    if (p == nullptr) return {};
    return p->altBranchRules();
}

SchemaCursor GrammarSchema::advance(SchemaCursor cur, SchemaTokenId tok) const noexcept {
    auto const* r = ruleRow(cur.rule().v);
    if (r == nullptr) return SchemaCursor{};
    auto const& positions = r->positions;
    if (cur.posId() >= positions.size()) return SchemaCursor{};

    // Walk through AltChoice positions until we hit a TokenLeaf or fail.
    // Each AltChoice routes into the first branch whose precomputed
    // expectedSet contains `tok` (c97: an O(1) bit test on the sealed
    // bitset — same first-match-wins semantics as the former linear scan).
    // The loader rejects ambiguous alts at load time
    // (C_AmbiguousAlternatives), so the "first match wins" behaviour is
    // unambiguous in any schema that loaded successfully.
    std::uint32_t curPosId = cur.posId();
    while (true) {
        auto const& p = positions[curPosId];
        if (p.slotKind() == SlotKind::TokenLeaf) {
            // A token leaf admits the kinds in its `expectedSet` — exactly one
            // for the ordinary single-token form, a config-declared SET for a
            // `{"tokenClass": …}` leaf. The bitset is the same O(1) membership
            // test the AltChoice arm below already uses, and for a single-token
            // leaf it is `{tokenId()}`, so the two forms answer identically.
            if (detail::tokenBitsContain(p.expectedBits(), tok.v)) {
                return SchemaCursor{cur.rule(), p.nextPos()};
            }
            return SchemaCursor{};
        }
        if (p.slotKind() == SlotKind::AltChoice) {
            std::uint32_t matched = 0;
            bool found = false;
            for (auto bid : p.branches()) {
                if (detail::tokenBitsContain(positions[bid].expectedBits(),
                                             tok.v)) {
                    matched = bid;
                    found   = true;
                    break;
                }
            }
            if (!found) return SchemaCursor{};
            curPosId = matched;
            continue;
        }
        // RuleLeaf or End — not advanceable by a token. Caller must
        // descend via enterRule (RuleLeaf) or pop back to the parent
        // (End).
        return SchemaCursor{};
    }
}

std::span<SchemaTokenId const> GrammarSchema::expectedSet(SchemaCursor cur) const noexcept {
    auto const* p = lookupPos(ruleRow(cur.rule().v), cur);
    if (p == nullptr) return {};
    return p->expectedSet();
}

bool GrammarSchema::expectedSetContains(SchemaCursor cur,
                                        SchemaTokenId tok) const noexcept {
    auto const* p = lookupPos(ruleRow(cur.rule().v), cur);
    if (p == nullptr) return false;
    return detail::tokenBitsContain(p->expectedBits(), tok.v);
}

bool GrammarSchema::firstSetContains(RuleId rule,
                                     SchemaTokenId tok) const noexcept {
    auto const* r = ruleRow(rule.v);
    if (r == nullptr) return false;
    return detail::tokenBitsContain(r->firstBits, tok.v);
}

bool GrammarSchema::predictivePrefixExcludes(RuleId rule, std::size_t offset,
                                             SchemaTokenId tok) const noexcept {
    auto const* r = ruleRow(rule.v);
    if (r == nullptr) return false;                    // no rule → no constraint
    if (offset >= r->prefixBits.size()) return false;  // undefined → no constraint
    auto const& bits = r->prefixBits[offset];
    if (bits.empty()) return false;                    // empty set → no constraint
    return !detail::tokenBitsContain(bits, tok.v);
}

SlotKind GrammarSchema::slotKind(SchemaCursor cur) const noexcept {
    auto const* p = lookupPos(ruleRow(cur.rule().v), cur);
    return p == nullptr ? SlotKind::End : p->slotKind();
}

RuleId GrammarSchema::slotRuleRef(SchemaCursor cur) const noexcept {
    auto const* p = lookupPos(ruleRow(cur.rule().v), cur);
    if (p == nullptr || p->slotKind() != SlotKind::RuleLeaf) return InvalidRule;
    return p->ruleId();
}

bool GrammarSchema::isAtEndOfRule(SchemaCursor cur) const noexcept {
    auto const* p = lookupPos(ruleRow(cur.rule().v), cur);
    return p != nullptr && p->slotKind() == SlotKind::End;
}

bool GrammarSchema::isSpeculativeAlt(SchemaCursor cur) const noexcept {
    auto const* p = lookupPos(ruleRow(cur.rule().v), cur);
    return p != nullptr && p->slotKind() == SlotKind::AltChoice && p->speculative();
}

std::uint16_t GrammarSchema::lookahead(SchemaCursor cur) const noexcept {
    auto const* p = lookupPos(ruleRow(cur.rule().v), cur);
    return (p != nullptr && p->slotKind() == SlotKind::AltChoice) ? p->lookahead() : 0;
}

bool GrammarSchema::nullableTail(SchemaCursor cur) const noexcept {
    auto const* p = lookupPos(ruleRow(cur.rule().v), cur);
    return p != nullptr && p->nullableTail();
}

SchemaCursor GrammarSchema::nullableBranch(SchemaCursor cur) const noexcept {
    auto const* r = ruleRow(cur.rule().v);
    auto const* p = lookupPos(r, cur);
    if (p == nullptr || p->slotKind() != SlotKind::AltChoice) return SchemaCursor{};
    auto const& positions = r->positions;
    for (auto bid : p->branches()) {
        if (bid >= positions.size()) continue;
        if (positions[bid].nullableTail()) {
            return SchemaCursor{cur.rule(), bid};
        }
    }
    return SchemaCursor{};
}

std::span<SchemaTokenId const> GrammarSchema::firstSetOf(RuleId rule) const noexcept {
    auto const* r = ruleRow(rule.v);
    if (r == nullptr) return {};
    return r->firstSet;
}

std::span<SchemaTokenId const> GrammarSchema::followSetOf(RuleId rule) const noexcept {
    auto const* r = ruleRow(rule.v);
    if (r == nullptr) return {};
    return r->followSet;
}

std::size_t GrammarSchema::predictivePrefixLen(RuleId rule) const noexcept {
    auto const* r = ruleRow(rule.v);
    if (r == nullptr) return 0;
    return r->predictivePrefix.size();
}

std::span<SchemaTokenId const>
GrammarSchema::predictivePrefixAt(RuleId rule, std::size_t offset) const noexcept {
    auto const* r = ruleRow(rule.v);
    if (r == nullptr) return {};
    auto const& pfx = r->predictivePrefix;
    if (offset >= pfx.size()) return {};
    return pfx[offset];
}

bool GrammarSchema::isContextualKind(SchemaTokenId kind) const noexcept {
    // D-PARSE-PREDICTIVE-PRUNE-CONTEXTUAL-KEYWORD. O(1); the set is EMPTY for
    // every grammar with no contextual keyword (Strict policy + no per-keyword
    // `contextual: true`) — so the prune's deep-nest O(N) win is unaffected for
    // the non-contextual case (every shipped c speculative alt).
    return kind.valid() && d_.contextualKinds.contains(kind.v);
}

std::span<SchemaTokenId const> GrammarSchema::syncTokens() const noexcept {
    return d_.syncTokens;
}

std::span<TypeExtensionDescriptor const> GrammarSchema::typeExtensions() const noexcept {
    return d_.typeExtensions;
}

std::span<std::string const> GrammarSchema::artifactProfiles() const noexcept {
    return d_.artifactProfiles;
}

std::string_view GrammarSchema::isa() const noexcept {
    return d_.isa;
}

ImportConfig const& GrammarSchema::imports() const noexcept {
    return d_.imports;
}

PreprocessConfig const& GrammarSchema::preprocess() const noexcept {
    return d_.preprocess;
}

bool GrammarSchema::isNullable(RuleId rule) const noexcept {
    auto const* r = ruleRow(rule.v);
    return r != nullptr && r->nullable;
}

bool GrammarSchema::isExprRule(RuleId rule) const noexcept {
    auto const* r = ruleRow(rule.v);
    return r != nullptr && r->isExpr;
}

bool GrammarSchema::isAutoInternedWrapperRule(RuleId rule) const noexcept {
    return d_.wrapperRuleIds.contains(rule.v);
}

RuleId GrammarSchema::exprAtom(RuleId rule) const noexcept {
    auto const* r = ruleRow(rule.v);
    if (r == nullptr) return RuleId{};
    return r->exprAtom;
}

std::int32_t GrammarSchema::exprMinPrecedence(RuleId rule) const noexcept {
    auto const* r = ruleRow(rule.v);
    if (r == nullptr) return 0;
    return r->exprMinPrecedence;
}

RuleId GrammarSchema::typeNameCommitRule(RuleId rule) const noexcept {
    auto const* r = ruleRow(rule.v);
    if (r == nullptr) return RuleId{};
    return r->typeNameCommitRule;
}

TypeNameCommitPolarity
GrammarSchema::typeNameCommitPolarity(RuleId rule) const noexcept {
    auto const* r = ruleRow(rule.v);
    if (r == nullptr) return TypeNameCommitPolarity::PreferType;
    return r->typeNameCommitPolarity;
}

bool GrammarSchema::commitAfterPrefix(RuleId rule) const noexcept {
    auto const* r = ruleRow(rule.v);
    return r != nullptr && r->commitAfterPrefix;
}

ExprWrapperRules GrammarSchema::exprWrapperRules(RuleId rule) const noexcept {
    auto it = d_.exprWrapperRules.find(rule.v);
    if (it == d_.exprWrapperRules.end()) return ExprWrapperRules{};
    return it->second;
}

NumberStyle const* GrammarSchema::numberStyle() const noexcept {
    return d_.numberStyle.has_value() ? &(*d_.numberStyle) : nullptr;
}

IdentifierClass const& GrammarSchema::identifierClass() const noexcept {
    return d_.identifierClass;
}

SemanticConfig const& GrammarSchema::semantics() const noexcept {
    return d_.semantics;
}

HirLoweringConfig const& GrammarSchema::hirLowering() const noexcept {
    return d_.hirLowering;
}

PipelineEntryConfig const& GrammarSchema::pipelineEntry() const noexcept {
    return d_.pipelineEntry;
}

AssemblyConfig const& GrammarSchema::assembly() const noexcept {
    return d_.assembly;
}

std::span<ConfigDiagnostic const>
GrammarSchema::loadDiagnostics() const noexcept {
    return d_.loadDiagnostics;
}

bool GrammarSchema::isTokenValidInScope(SchemaTokenId tok,
                                       std::span<ScopeKind const> stack) const noexcept {
    // Walk the scope stack top-down so the innermost scope's rules win —
    // a `forbid` listed on the innermost frame applies even if an outer
    // frame allows the token.
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
        auto scopeIt = d_.scopeForbid.find(static_cast<std::uint16_t>(*it));
        if (scopeIt != d_.scopeForbid.end() && scopeIt->second.contains(tok.v)) {
            return false;
        }
    }
    return true;
}

bool GrammarSchema::canEndSource(SchemaCursor cur) const noexcept {
    if (cur.rule().v != d_.rootRule.v) return false;
    auto const* p = lookupPos(ruleRow(cur.rule().v), cur);
    return p != nullptr && p->nullableTail();
}

} // namespace dss
