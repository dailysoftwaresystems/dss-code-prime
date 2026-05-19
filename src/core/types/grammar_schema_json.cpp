#include "core/types/grammar_schema_json.hpp"

#include "core/types/grammar_schema.hpp"
#include "core/types/operator_table.hpp"
#include "core/types/rule_id.hpp"
#include "core/types/schema_token_interner.hpp"
#include "core/types/scope_kind.hpp"
#include "core/types/tree_node.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace dss::detail {

namespace {

using json = nlohmann::json;

// ── ScopeKind name lookup ─────────────────────────────────────────────────
constexpr std::pair<std::string_view, ScopeKind> kBuiltinScopes[] = {
    {"None",    ScopeKind::None},
    {"Root",    ScopeKind::Root},
    {"Block",   ScopeKind::Block},
    {"Paren",   ScopeKind::Paren},
    {"Bracket", ScopeKind::Bracket},
    {"Generic", ScopeKind::Generic},
    {"String",  ScopeKind::String},
    {"Comment", ScopeKind::Comment},
};

std::optional<ScopeKind> parseScopeName(std::string_view name) {
    for (auto const& [n, kind] : kBuiltinScopes) {
        if (n == name) return kind;
    }
    return std::nullopt;
}

// Built-in token-kind names — predeclared so shape references like
// "Identifier" resolve regardless of whether the user re-declared them
// under tokens/keywords. Mirrors CoreTokenKind on the lexer side.
constexpr std::string_view kBuiltinTokenKindNames[] = {
    "Identifier",
    "IntLiteral",
    "FloatLiteral",
    "StringLiteral",
    "CharLiteral",
    "BoolLiteral",
    "NullLiteral",
    "Eof",
    "Error",
};

struct Collector {
    std::vector<ConfigDiagnostic> diagnostics;

    void emit(DiagnosticCode code, std::string path, std::string message,
              DiagnosticSeverity sev = DiagnosticSeverity::Error) {
        diagnostics.push_back({code, sev, std::move(path), std::move(message)});
    }

    [[nodiscard]] bool hasErrors() const noexcept {
        return std::ranges::any_of(diagnostics,
            [](auto const& d) { return d.severity == DiagnosticSeverity::Error; });
    }
};

// Parse a JSON array of scope names into vector<ScopeKind>. Unknown
// names emit C_UnclosableScope and are dropped; the walk continues so
// one bad entry doesn't hide a subsequent one.
std::vector<ScopeKind> parseScopeArray(json const& arr,
                                       std::string const& fieldPath,
                                       Collector& c) {
    std::vector<ScopeKind> out;
    if (!arr.is_array()) return out;
    out.reserve(arr.size());
    for (auto const& sn : arr) {
        if (!sn.is_string()) continue;
        const auto sk = parseScopeName(sn.get<std::string>());
        if (!sk) {
            c.emit(DiagnosticCode::C_UnclosableScope, fieldPath,
                   std::format("unknown scope '{}'", sn.get<std::string>()));
            continue;
        }
        out.push_back(*sk);
    }
    return out;
}

bool present(json const& j, std::string_view key, Collector& c, std::string const& path) {
    if (!j.contains(key) || j.at(key).is_null()) {
        c.emit(DiagnosticCode::C_MissingField, path,
               std::format("required field '{}' missing", key));
        return false;
    }
    return true;
}

NodeFlags parseFlagList(json const& arr) {
    NodeFlags out = NodeFlags::None;
    if (!arr.is_array()) return out;
    for (auto const& f : arr) {
        if (!f.is_string()) continue;
        const auto s = f.get<std::string>();
        if      (s == "EmptySpace") out |= NodeFlags::EmptySpace;
        else if (s == "Missing")    out |= NodeFlags::Missing;
        else if (s == "Synthetic")  out |= NodeFlags::Synthetic;
        else if (s == "HasError")   out |= NodeFlags::HasError;
        // unknown → ignore (forward-compatibility with future flags)
    }
    return out;
}

// Recursively collect every string atom that appears in a shape body.
// These are *references* — names that must resolve to either a rule
// (RuleInterner) or a schema-token-kind (SchemaTokenInterner).
void collectReferences(json const& body,
                       std::string const& path,
                       std::vector<std::string>& outRefs,
                       Collector& c) {
    if (body.is_string()) {
        outRefs.push_back(body.get<std::string>());
        return;
    }
    if (!body.is_object()) {
        c.emit(DiagnosticCode::C_MissingField, path,
               "shape body must be a string or an object");
        return;
    }

    auto recurseArray = [&](json const& arr, std::string const& kind) {
        if (!arr.is_array()) {
            c.emit(DiagnosticCode::C_MissingField,
                   std::format("{}/{}", path, kind),
                   std::format("'{}' must be an array", kind));
            return;
        }
        if (arr.empty()) {
            c.emit(DiagnosticCode::C_MissingField,
                   std::format("{}/{}", path, kind),
                   std::format("'{}' must contain at least one element", kind));
            return;
        }
        for (std::size_t i = 0; i < arr.size(); ++i) {
            collectReferences(arr[i], std::format("{}/{}/{}", path, kind, i),
                              outRefs, c);
        }
    };

    // The five shape kinds are mutually exclusive — a body declares one of
    // sequence|alt|optional|repeat|expr, never two — and a body must declare
    // at least one. A body with zero kinds silently produces a no-op rule
    // (the position builder falls through to `return cont`); a body with
    // multiple kinds silently runs every matching branch. Both are surface
    // misfires that this guard turns into load errors.
    {
        constexpr std::string_view kShapeKinds[] = {
            "sequence", "alt", "optional", "repeat", "expr",
        };
        std::vector<std::string_view> present;
        for (auto k : kShapeKinds) {
            if (body.contains(k)) present.push_back(k);
        }
        if (present.empty()) {
            c.emit(DiagnosticCode::C_UnknownShape, path,
                   "shape body declares no kind — expected exactly one of sequence|alt|optional|repeat|expr");
        } else if (present.size() > 1) {
            std::string joined;
            for (std::size_t i = 0; i < present.size(); ++i) {
                if (i) joined += ", ";
                joined += present[i];
            }
            c.emit(DiagnosticCode::C_UnknownShape, path,
                   std::format("shape body declares multiple kinds ({}) — exactly one of sequence|alt|optional|repeat|expr must be present",
                               joined));
        }
    }

    if (body.contains("sequence")) recurseArray(body.at("sequence"), "sequence");
    if (body.contains("alt"))      recurseArray(body.at("alt"),      "alt");
    if (body.contains("optional")) {
        collectReferences(body.at("optional"),
                          std::format("{}/optional", path), outRefs, c);
    }
    if (body.contains("repeat")) {
        collectReferences(body.at("repeat"),
                          std::format("{}/repeat", path), outRefs, c);
    }
    // `expr` body is `{ atom: <rule>, minPrecedence?: <int> }`. The loader
    // validates the atom reference resolves and rejects unknown keys so a
    // typo (e.g. `minPrecdence`) doesn't silently use the default. Operator-
    // table consumption is the parser's responsibility, not the loader's.
    if (body.contains("expr")) {
        json const& exprBody = body.at("expr");
        const auto exprPath = std::format("{}/expr", path);
        if (!exprBody.is_object()) {
            c.emit(DiagnosticCode::C_MissingField, exprPath,
                   "'expr' body must be an object with an 'atom' field");
        } else if (!exprBody.contains("atom") || !exprBody.at("atom").is_string()) {
            c.emit(DiagnosticCode::C_MissingField, exprPath,
                   "'expr' requires a string 'atom' field naming the operand rule");
        } else {
            outRefs.push_back(exprBody.at("atom").get<std::string>());
            // Allowlist: anything other than atom/minPrecedence is a typo.
            for (auto const& [k, _] : exprBody.items()) {
                if (k != "atom" && k != "minPrecedence") {
                    c.emit(DiagnosticCode::C_UnknownShape,
                           std::format("{}/{}", exprPath, k),
                           std::format("unknown 'expr' field '{}' — expected 'atom' or 'minPrecedence'", k));
                }
            }
        }
    }
}

// ── Compile pass: FIRST/NULLABLE + position tables ──────────────────────
//
// The cursor walks a flat per-rule position table built here. Steps:
//
//   1. Iteratively compute per-rule FIRST set and NULLABLE flag until
//      stable. Cycles in the grammar (A → repeat[B] → A) terminate
//      because FIRST sets grow monotonically.
//   2. Build each rule's position table via a recursive walk over the
//      JSON body, threading a `cont` (continuation position-id) through
//      children. Repeat uses a tie-the-knot pattern so the loop body's
//      tail returns to the loop entry.
//   3. Iteratively compute `nullableTail` per position — true when a
//      position can complete to end-of-rule without consuming any
//      further token. canEndSource(cursor) reads this directly.

void insertSorted(std::vector<SchemaTokenId>& v, SchemaTokenId t) {
    auto it = std::lower_bound(v.begin(), v.end(), t,
        [](SchemaTokenId a, SchemaTokenId b) { return a.v < b.v; });
    if (it == v.end() || it->v != t.v) v.insert(it, t);
}

void mergeSorted(std::vector<SchemaTokenId>& into,
                 std::span<SchemaTokenId const> from) {
    for (auto t : from) insertSorted(into, t);
}

bool nullableOfBody(json const& body,
                    GrammarSchemaData const& data) {
    if (body.is_string()) {
        const auto& name = body.get<std::string>();
        if (data.rules->contains(name)) {
            const auto rid = data.rules->find(name);
            auto it = data.compiledRules.find(rid.v);
            return it != data.compiledRules.end() && it->second.nullable;
        }
        return false;  // token reference is never nullable
    }
    if (body.contains("sequence")) {
        for (auto const& child : body.at("sequence")) {
            if (!nullableOfBody(child, data)) return false;
        }
        return true;
    }
    if (body.contains("alt")) {
        for (auto const& child : body.at("alt")) {
            if (nullableOfBody(child, data)) return true;
        }
        return false;
    }
    if (body.contains("optional") || body.contains("repeat")) return true;
    if (body.contains("expr")) {
        return nullableOfBody(body.at("expr").at("atom"), data);
    }
    return false;
}

std::vector<SchemaTokenId> firstOfBody(json const& body,
                                       GrammarSchemaData const& data) {
    if (body.is_string()) {
        const auto& name = body.get<std::string>();
        if (data.rules->contains(name)) {
            const auto rid = data.rules->find(name);
            auto it = data.compiledRules.find(rid.v);
            return it != data.compiledRules.end()
                ? it->second.firstSet
                : std::vector<SchemaTokenId>{};
        }
        if (data.schemaTokens->contains(name)) {
            return {data.schemaTokens->find(name)};
        }
        return {};
    }
    if (body.contains("sequence")) {
        std::vector<SchemaTokenId> result;
        for (auto const& child : body.at("sequence")) {
            mergeSorted(result, firstOfBody(child, data));
            if (!nullableOfBody(child, data)) break;
        }
        return result;
    }
    if (body.contains("alt")) {
        std::vector<SchemaTokenId> result;
        for (auto const& child : body.at("alt")) {
            mergeSorted(result, firstOfBody(child, data));
        }
        return result;
    }
    if (body.contains("optional")) {
        return firstOfBody(body.at("optional"), data);
    }
    if (body.contains("repeat")) {
        return firstOfBody(body.at("repeat"), data);
    }
    if (body.contains("expr")) {
        return firstOfBody(body.at("expr").at("atom"), data);
    }
    return {};
}

void computeFirstAndNullable(GrammarSchemaData& data, json const& shapesJson) {
    // Initialise every rule's FIRST = {} and nullable = false. Iterate
    // until a full pass leaves every rule's values unchanged. FIRST sets
    // only grow (via insertSorted's dedup) and nullable only flips
    // false→true, so the loop is monotone over a finite lattice. The
    // iteration cap is a safety net — if a future refactor breaks
    // monotonicity, we abort rather than hanging silently.
    for (auto const& [shapeName, _] : shapesJson.items()) {
        const auto rid = data.rules->find(shapeName);
        data.compiledRules[rid.v] = detail::CompiledRule{};
    }

    constexpr int kMaxIters = 10000;
    bool changed = true;
    int iters = 0;
    while (changed && iters++ < kMaxIters) {
        changed = false;
        for (auto const& [shapeName, body] : shapesJson.items()) {
            const auto rid = data.rules->find(shapeName);
            auto& rule = data.compiledRules[rid.v];
            auto newFirst     = firstOfBody(body, data);
            const bool newNul = nullableOfBody(body, data);
            if (newFirst != rule.firstSet || newNul != rule.nullable) {
                rule.firstSet = std::move(newFirst);
                rule.nullable = newNul;
                changed = true;
            }
        }
    }
}

struct PositionBuilder {
    std::vector<detail::Position>& positions;
    GrammarSchemaData const&       data;

    [[nodiscard]] std::uint32_t emplace(detail::Position p) {
        positions.push_back(std::move(p));
        return static_cast<std::uint32_t>(positions.size() - 1);
    }

    // `cont` is the position-id the built body falls through to when it
    // completes. Threaded right-to-left through sequence children so each
    // child's `nextPos` points at its actual successor.
    [[nodiscard]] std::uint32_t build(json const& body, std::uint32_t cont) {
        if (body.is_string()) {
            const auto& name = body.get<std::string>();
            if (data.rules->contains(name)) {
                const auto rid = data.rules->find(name);
                std::vector<SchemaTokenId> firstSet;
                auto it = data.compiledRules.find(rid.v);
                if (it != data.compiledRules.end()) firstSet = it->second.firstSet;
                return emplace(detail::Position::makeRuleLeaf(rid, cont, std::move(firstSet)));
            }
            if (data.schemaTokens->contains(name)) {
                return emplace(detail::Position::makeTokenLeaf(
                    data.schemaTokens->find(name), cont));
            }
            return cont;  // unreachable; references validated earlier
        }
        if (body.contains("sequence")) {
            const auto& seq = body.at("sequence");
            std::uint32_t curCont = cont;
            for (auto it = seq.rbegin(); it != seq.rend(); ++it) {
                curCont = build(*it, curCont);
            }
            return curCont;
        }
        if (body.contains("alt")) {
            const auto& alt = body.at("alt");
            std::vector<std::uint32_t> branches;
            branches.reserve(alt.size());
            for (auto const& branch : alt) branches.push_back(build(branch, cont));
            std::vector<SchemaTokenId> ex;
            for (auto bid : branches) mergeSorted(ex, positions[bid].expectedSet());
            return emplace(detail::Position::makeAltChoice(std::move(branches), std::move(ex)));
        }
        if (body.contains("optional")) {
            const auto innerStart = build(body.at("optional"), cont);
            std::vector<SchemaTokenId> ex;
            mergeSorted(ex, positions[innerStart].expectedSet());
            mergeSorted(ex, positions[cont].expectedSet());
            return emplace(detail::Position::makeAltChoice({innerStart, cont}, std::move(ex)));
        }
        if (body.contains("repeat")) {
            // Tie-the-knot: reserve a slot for the loop entry first, build
            // the body with cont=loopEntry so the body's tail loops back,
            // then overwrite the reserved slot with a real AltChoice once
            // `innerStart` is known. Nothing reads `positions[loopEntry]`
            // mid-recursion — child positions only read sibling rules'
            // `firstSet` from `data.compiledRules`, which is pre-populated
            // by `computeFirstAndNullable`.
            const auto loopEntry  = emplace(detail::Position::makeEnd());
            const auto innerStart = build(body.at("repeat"), loopEntry);
            std::vector<SchemaTokenId> ex;
            mergeSorted(ex, positions[innerStart].expectedSet());
            mergeSorted(ex, positions[cont].expectedSet());
            positions[loopEntry] = detail::Position::makeAltChoice(
                {innerStart, cont}, std::move(ex));
            return loopEntry;
        }
        if (body.contains("expr")) {
            // Treat the `expr` shape as its `atom` rule reference — the
            // Pratt operator-climbing step is the parser's job, not the
            // cursor's.
            return build(body.at("expr").at("atom"), cont);
        }
        return cont;
    }
};

void buildPositionTables(GrammarSchemaData& data, json const& shapesJson) {
    for (auto const& [shapeName, body] : shapesJson.items()) {
        const auto rid = data.rules->find(shapeName);
        auto& rule = data.compiledRules[rid.v];
        rule.positions.clear();
        // positions[0] is the sentinel for cursor posId==0 (invalid).
        // positions[1] is the End that body completions fall through to.
        rule.positions.emplace_back();
        PositionBuilder builder{rule.positions, data};
        const auto endId   = builder.emplace(detail::Position::makeEnd());
        const auto entryId = builder.build(body, endId);
        rule.entryPos = entryId;
    }
}

// Scan every compiled AltChoice for branches whose precomputed FIRST sets
// overlap. The cursor's `advance` does silent first-branch-wins on overlap,
// which is the wrong default for a grammar that wants disjoint alts; this
// pass surfaces the conflict at load time so the config author resolves
// it by either restructuring the grammar OR (in PR4) marking the alt as
// `speculative: true` to opt in to backtracking semantics. Today the
// `speculative` field doesn't exist yet, so every ambiguous alt is a
// load error.
void detectAmbiguousAlternatives(GrammarSchemaData& data, Collector& coll,
                                  json const& shapesJson) {
    for (auto const& [shapeName, _] : shapesJson.items()) {
        const auto rid = data.rules->find(shapeName);
        auto it = data.compiledRules.find(rid.v);
        if (it == data.compiledRules.end()) continue;
        auto const& positions = it->second.positions;
        for (std::size_t posIdx = 0; posIdx < positions.size(); ++posIdx) {
            auto const& p = positions[posIdx];
            if (p.slotKind() != SlotKind::AltChoice) continue;
            auto branches = p.branches();
            for (std::size_t i = 0; i + 1 < branches.size(); ++i) {
                auto a = positions[branches[i]].expectedSet();
                for (std::size_t j = i + 1; j < branches.size(); ++j) {
                    auto b = positions[branches[j]].expectedSet();
                    // Both sets are sorted by SchemaTokenId.v; linear merge
                    // detects overlap in O(|a|+|b|).
                    std::size_t ai = 0, bi = 0;
                    SchemaTokenId overlap{};
                    bool found = false;
                    while (ai < a.size() && bi < b.size()) {
                        if (a[ai].v == b[bi].v) {
                            overlap = a[ai]; found = true; break;
                        }
                        if (a[ai].v < b[bi].v) ++ai; else ++bi;
                    }
                    if (found) {
                        coll.emit(DiagnosticCode::C_AmbiguousAlternatives,
                                  std::format("/shapes/{}", shapeName),
                                  std::format("alt branches share FIRST token '{}' — the cursor would silently take the first branch; restructure the alt or factor the shared prefix",
                                              data.schemaTokens->name(overlap)));
                    }
                }
            }
        }
    }
}

void computeNullableTails(GrammarSchemaData& data) {
    // Fixed point over a finite lattice (false ⇒ true is monotone). Cap
    // iterations as a safety net — if a future refactor accidentally
    // flips a flag back, we'd otherwise hang silently.
    constexpr int kMaxIters = 10000;
    for (auto& [_, rule] : data.compiledRules) {
        auto& positions = rule.positions;
        for (auto& p : positions) {
            p.setNullableTail(p.slotKind() == SlotKind::End);
        }
        bool changed = true;
        int iters = 0;
        while (changed && iters++ < kMaxIters) {
            changed = false;
            for (auto& p : positions) {
                bool newVal = p.nullableTail();
                switch (p.slotKind()) {
                case SlotKind::End:
                    newVal = true;
                    break;
                case SlotKind::TokenLeaf:
                    newVal = false;
                    break;
                case SlotKind::RuleLeaf: {
                    // Crossing a rule boundary requires both legs to be
                    // nullable — the callee rule itself AND the parent's
                    // continuation. Dropping either invalidates the path.
                    auto it = data.compiledRules.find(p.ruleId().v);
                    const bool ruleNullable =
                        (it != data.compiledRules.end()) && it->second.nullable;
                    newVal = ruleNullable && positions[p.nextPos()].nullableTail();
                    break;
                }
                case SlotKind::AltChoice:
                    newVal = false;
                    for (auto bid : p.branches()) {
                        if (positions[bid].nullableTail()) { newVal = true; break; }
                    }
                    break;
                }
                if (newVal != p.nullableTail()) {
                    p.setNullableTail(newVal);
                    changed = true;
                }
            }
        }
    }
}

} // anonymous namespace

LoadResult<std::shared_ptr<GrammarSchema>> buildSchemaFromJsonText(
    std::string_view jsonText,
    std::string_view sourceLabel) {

    Collector coll;
    json doc;
    try {
        doc = json::parse(jsonText);
    } catch (json::parse_error const& e) {
        coll.emit(DiagnosticCode::C_MalformedJson,
                  std::string{sourceLabel},
                  std::format("JSON parse error: {}", e.what()));
        return std::unexpected(std::move(coll.diagnostics));
    }
    if (!doc.is_object()) {
        coll.emit(DiagnosticCode::C_MalformedJson, std::string{sourceLabel},
                  "top-level value must be a JSON object");
        return std::unexpected(std::move(coll.diagnostics));
    }

    // dssSchemaVersion ──
    if (!present(doc, "dssSchemaVersion", coll, std::string{sourceLabel})) {
        return std::unexpected(std::move(coll.diagnostics));
    }
    if (!doc.at("dssSchemaVersion").is_number_integer()) {
        coll.emit(DiagnosticCode::C_VersionMismatch, "/dssSchemaVersion",
                  "must be a positive integer");
        return std::unexpected(std::move(coll.diagnostics));
    }
    // Inclusive window of schema versions this loader understands. Bump
    // the upper bound only after the loader actually parses the new
    // version's exclusive fields — accepting a version implies all
    // documented features for that version round-trip cleanly.
    constexpr std::uint32_t kMinSchemaVersion = 1;
    constexpr std::uint32_t kMaxSchemaVersion = 2;
    const auto schemaVer = doc.at("dssSchemaVersion").get<std::uint32_t>();
    if (schemaVer < kMinSchemaVersion || schemaVer > kMaxSchemaVersion) {
        coll.emit(DiagnosticCode::C_VersionMismatch, "/dssSchemaVersion",
                  std::format("unsupported dssSchemaVersion {} (this build supports {}..{})",
                              schemaVer, kMinSchemaVersion, kMaxSchemaVersion));
        return std::unexpected(std::move(coll.diagnostics));
    }

    // language ──
    if (!present(doc, "language", coll, std::string{sourceLabel})) {
        return std::unexpected(std::move(coll.diagnostics));
    }
    json const& langObj = doc.at("language");
    if (!langObj.is_object()) {
        coll.emit(DiagnosticCode::C_MissingField, "/language", "must be an object");
        return std::unexpected(std::move(coll.diagnostics));
    }
    if (!present(langObj, "name",    coll, "/language") ||
        !present(langObj, "version", coll, "/language")) {
        return std::unexpected(std::move(coll.diagnostics));
    }

    GrammarSchemaData data;
    data.name          = langObj.at("name").get<std::string>();
    data.version       = langObj.at("version").get<std::string>();
    data.schemaVersion = schemaVer;

    if (langObj.contains("fileExtensions") && langObj.at("fileExtensions").is_array()) {
        for (auto const& ext : langObj.at("fileExtensions")) {
            if (ext.is_string()) data.fileExtensions.push_back(ext.get<std::string>());
        }
    }

    // reservedWordPolicy: "strict" (default) | "contextual". Contextual
    // makes every keyword soft — the loader sets `contextual = true` on
    // every keyword's LexemeMeaning further down. The builder then
    // demotes any keyword whose schemaTokenId isn't in the cursor's
    // expectedSet at resolution time.
    if (doc.contains("reservedWordPolicy")) {
        if (!doc.at("reservedWordPolicy").is_string()) {
            coll.emit(DiagnosticCode::C_MissingField, "/reservedWordPolicy",
                      "'reservedWordPolicy' must be a string ('strict' or 'contextual')");
        } else {
            const auto v = doc.at("reservedWordPolicy").get<std::string>();
            if      (v == "strict")     data.reservedWordPolicy = ReservedWordPolicy::Strict;
            else if (v == "contextual") data.reservedWordPolicy = ReservedWordPolicy::Contextual;
            else {
                coll.emit(DiagnosticCode::C_MissingField, "/reservedWordPolicy",
                          std::format("unknown reservedWordPolicy '{}' (expected 'strict' or 'contextual')", v));
            }
        }
    }

    data.rules        = std::make_shared<RuleInterner>();
    data.schemaTokens = std::make_shared<SchemaTokenInterner>();

    // Pre-intern built-in token kinds.
    for (auto name : kBuiltinTokenKindNames) {
        (void)data.schemaTokens->intern(name);
    }

    // tokens ──
    if (doc.contains("tokens")) {
        if (!doc.at("tokens").is_object()) {
            coll.emit(DiagnosticCode::C_MissingField, "/tokens",
                      "'tokens' must be an object");
        } else {
            // Pass A: count anyOf/forbid lists upfront to reserve the
            // backing pool exactly. Reallocation would invalidate the
            // ScopeMatch spans we set up next.
            //   - legacy `validScopes`         → 1 list (anyOf)
            //   - `scopeRequire.anyOf`         → 1 list
            //   - `scopeRequire.forbid`        → 1 list
            std::size_t poolNeeded = 0;
            for (auto const& [lex, arr] : doc.at("tokens").items()) {
                if (!arr.is_array()) continue;
                for (auto const& m : arr) {
                    if (!m.is_object()) continue;
                    if (m.contains("validScopes")) ++poolNeeded;
                    if (m.contains("scopeRequire") && m.at("scopeRequire").is_object()) {
                        auto const& sr = m.at("scopeRequire");
                        if (sr.contains("anyOf"))  ++poolNeeded;
                        if (sr.contains("forbid")) ++poolNeeded;
                    }
                }
            }
            data.scopeListPool.reserve(poolNeeded);

            // Pass B: populate lexemeTable + spans.
            for (auto const& [lex, arr] : doc.at("tokens").items()) {
                if (!arr.is_array()) {
                    coll.emit(DiagnosticCode::C_UnknownToken,
                              std::format("/tokens/{}", lex),
                              "value must be an array of meaning objects");
                    continue;
                }
                auto& meanings = data.lexemeTable[lex];
                meanings.reserve(arr.size());

                for (std::size_t i = 0; i < arr.size(); ++i) {
                    json const& m = arr[i];
                    const auto entryPath = std::format("/tokens/{}/{}", lex, i);
                    if (!m.is_object()) {
                        coll.emit(DiagnosticCode::C_UnknownToken, entryPath,
                                  "meaning entry must be an object");
                        continue;
                    }
                    if (!m.contains("kind") || !m.at("kind").is_string()) {
                        coll.emit(DiagnosticCode::C_MissingField, entryPath,
                                  "meaning must have a string 'kind' field");
                        continue;
                    }

                    LexemeMeaning lm{};
                    lm.id          = data.schemaTokens->intern(m.at("kind").get<std::string>());
                    lm.priority    = m.value("priority", 0);
                    lm.closesScope = m.value("closesScope", false);

                    // `contextual` is a keyword-only concept. Tokens are
                    // operators / punctuation / built-ins — they don't
                    // degrade to identifiers based on parse position.
                    if (m.contains("contextual")) {
                        coll.emit(DiagnosticCode::C_MissingField,
                                  std::format("{}/contextual", entryPath),
                                  "'contextual' is only valid on keyword entries — tokens never degrade to identifiers");
                    }

                    if (m.contains("flags")) {
                        lm.flagsApplied = parseFlagList(m.at("flags"));
                    }
                    if (isEmptySpace(lm.flagsApplied)) {
                        data.emptySpaceTokens.insert(lm.id.v);
                    }
                    if (m.contains("opensScope") && m.at("opensScope").is_string()) {
                        const auto sk = parseScopeName(m.at("opensScope").get<std::string>());
                        if (!sk) {
                            coll.emit(DiagnosticCode::C_UnclosableScope, entryPath,
                                      std::format("unknown scope name '{}'",
                                                  m.at("opensScope").get<std::string>()));
                        } else {
                            lm.opensScope = *sk;
                        }
                    }
                    // scopeRequire — new in v2. Reject co-existence with
                    // legacy validScopes; the loader insists on exactly
                    // one source of truth per meaning so spans don't
                    // collide and the author can't accidentally split
                    // their intent across two fields.
                    const bool hasLegacy = m.contains("validScopes");
                    const bool hasNew    = m.contains("scopeRequire");
                    if (hasLegacy && hasNew) {
                        coll.emit(DiagnosticCode::C_MissingField, entryPath,
                                  "meaning declares both 'validScopes' and "
                                  "'scopeRequire' — use 'scopeRequire' only "
                                  "(v1 'validScopes' is shorthand for 'scopeRequire.anyOf')");
                    } else if (hasLegacy && m.at("validScopes").is_array()) {
                        auto anyOf = parseScopeArray(m.at("validScopes"),
                                                     std::format("{}/validScopes", entryPath),
                                                     coll);
                        if (!anyOf.empty()) {
                            data.scopeListPool.push_back(std::move(anyOf));
                            lm.scopeRequire.anyOf = std::span<ScopeKind const>{
                                data.scopeListPool.back()};
                        }
                    } else if (hasNew && m.at("scopeRequire").is_object()) {
                        json const& sr = m.at("scopeRequire");
                        if (sr.contains("anyOf") && sr.at("anyOf").is_array()) {
                            auto anyOf = parseScopeArray(sr.at("anyOf"),
                                                         std::format("{}/scopeRequire/anyOf", entryPath),
                                                         coll);
                            if (!anyOf.empty()) {
                                data.scopeListPool.push_back(std::move(anyOf));
                                lm.scopeRequire.anyOf = std::span<ScopeKind const>{
                                    data.scopeListPool.back()};
                            }
                        }
                        if (sr.contains("forbid") && sr.at("forbid").is_array()) {
                            auto forbid = parseScopeArray(sr.at("forbid"),
                                                          std::format("{}/scopeRequire/forbid", entryPath),
                                                          coll);
                            if (!forbid.empty()) {
                                data.scopeListPool.push_back(std::move(forbid));
                                lm.scopeRequire.forbid = std::span<ScopeKind const>{
                                    data.scopeListPool.back()};
                            }
                        }
                        if (sr.contains("topMustBe") && sr.at("topMustBe").is_string()) {
                            const auto sk = parseScopeName(sr.at("topMustBe").get<std::string>());
                            if (!sk) {
                                coll.emit(DiagnosticCode::C_UnclosableScope,
                                          std::format("{}/scopeRequire/topMustBe", entryPath),
                                          std::format("unknown scope '{}'",
                                                      sr.at("topMustBe").get<std::string>()));
                            } else {
                                lm.scopeRequire.topMustBe = *sk;
                            }
                        }
                        if (sr.contains("outermost") && sr.at("outermost").is_string()) {
                            const auto sk = parseScopeName(sr.at("outermost").get<std::string>());
                            if (!sk) {
                                coll.emit(DiagnosticCode::C_UnclosableScope,
                                          std::format("{}/scopeRequire/outermost", entryPath),
                                          std::format("unknown scope '{}'",
                                                      sr.at("outermost").get<std::string>()));
                            } else {
                                lm.scopeRequire.outermost = *sk;
                            }
                        }
                        // topMustBe makes anyOf redundant: a topMustBe match
                        // implies the named scope is on the stack, which is
                        // strictly stronger than anyOf. Warn but accept.
                        if (lm.scopeRequire.topMustBe.has_value() &&
                            !lm.scopeRequire.anyOf.empty()) {
                            coll.emit(DiagnosticCode::C_RedundantScopeRequire,
                                      std::format("{}/scopeRequire", entryPath),
                                      "'anyOf' is redundant when 'topMustBe' is set "
                                      "(topMustBe is the stricter constraint)",
                                      DiagnosticSeverity::Warning);
                        }
                    }

                    meanings.push_back(lm);
                }

                // Deterministic ordering: stable sort by (priority asc).
                // Stable so original declaration order breaks ties — the
                // rigor review mandates "lowest priority wins, then first-
                // declared wins".
                std::stable_sort(meanings.begin(), meanings.end(),
                    [](LexemeMeaning const& a, LexemeMeaning const& b) {
                        return a.priority < b.priority;
                    });
            }
        }
    }

    // keywords ──
    // `contextual: true` per entry marks soft keywords (await/yield etc.).
    // The top-level `reservedWordPolicy: "contextual"` flag forces every
    // keyword soft regardless of the per-entry setting.
    if (doc.contains("keywords") && doc.at("keywords").is_array()) {
        const bool forceContextual =
            (data.reservedWordPolicy == ReservedWordPolicy::Contextual);
        for (std::size_t i = 0; i < doc.at("keywords").size(); ++i) {
            json const& kw = doc.at("keywords").at(i);
            const auto kwPath = std::format("/keywords/{}", i);
            if (!kw.is_object() || !kw.contains("word") || !kw.contains("kind") ||
                !kw.at("word").is_string() || !kw.at("kind").is_string()) {
                coll.emit(DiagnosticCode::C_MissingField, kwPath,
                          "keyword entry needs string 'word' and string 'kind'");
                continue;
            }
            LexemeMeaning lm{};
            lm.id = data.schemaTokens->intern(kw.at("kind").get<std::string>());
            if (kw.contains("contextual")) {
                if (!kw.at("contextual").is_boolean()) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              std::format("{}/contextual", kwPath),
                              "'contextual' must be a boolean");
                } else {
                    lm.contextual = kw.at("contextual").get<bool>();
                }
            }
            if (forceContextual) lm.contextual = true;

            // Identifier IS the demotion target for contextual keywords.
            // A keyword whose own kind is Identifier cannot meaningfully
            // be contextual — the would-be demotion is a no-op, and the
            // marking is almost always a config mistake (Identifier
            // declared as a keyword by accident). Reject loudly rather
            // than silently treat the entry as plain Identifier.
            if (lm.contextual &&
                kw.at("kind").get<std::string>() == "Identifier") {
                coll.emit(DiagnosticCode::C_MissingField,
                          std::format("{}/contextual", kwPath),
                          std::format("keyword '{}' cannot be contextual: "
                                      "kind 'Identifier' is itself the "
                                      "contextual-demotion target",
                                      kw.at("word").get<std::string>()));
                continue;
            }

            data.lexemeTable[kw.at("word").get<std::string>()].push_back(lm);
        }
    }

    // operators ──
    // Runs after tokens + keywords so referenced lexemes and kinds are
    // already interned. Operator metadata lives only on data.operators,
    // not on LexemeMeaning — keeps lexeme resolution and operator
    // semantics independent. `precedence` is required per group;
    // `associativity` defaults to None and `arity` defaults to Infix
    // when omitted. Multi-meaning lexemes require an explicit `kind`.
    if (doc.contains("operators")) {
        json const& opsObj = doc.at("operators");
        if (!opsObj.is_object()) {
            coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, "/operators",
                      "'operators' must be an object");
        } else if (!opsObj.contains("groups") || !opsObj.at("groups").is_array()) {
            coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, "/operators/groups",
                      "'operators.groups' must be an array");
        } else {
            json const& groups = opsObj.at("groups");
            for (std::size_t i = 0; i < groups.size(); ++i) {
                json const& g = groups[i];
                const auto gPath = std::format("/operators/groups/{}", i);

                if (!g.is_object()) {
                    coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, gPath,
                              "group must be an object");
                    continue;
                }
                if (!g.contains("precedence") || !g.at("precedence").is_number_integer()) {
                    coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, gPath,
                              "group requires integer 'precedence'");
                    continue;
                }
                const auto precedence = g.at("precedence").get<std::int32_t>();

                OperatorAssoc assoc = OperatorAssoc::None;
                if (g.contains("associativity")) {
                    if (!g.at("associativity").is_string()) {
                        coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, gPath,
                                  "'associativity' must be a string");
                        continue;
                    }
                    const auto a = g.at("associativity").get<std::string>();
                    if      (a == "left")  assoc = OperatorAssoc::Left;
                    else if (a == "right") assoc = OperatorAssoc::Right;
                    else if (a == "none")  assoc = OperatorAssoc::None;
                    else {
                        coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, gPath,
                                  std::format("unknown associativity '{}' (expected left|right|none)", a));
                        continue;
                    }
                }

                OperatorArity arity = OperatorArity::Infix;
                if (g.contains("arity")) {
                    if (!g.at("arity").is_string()) {
                        coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, gPath,
                                  "'arity' must be a string");
                        continue;
                    }
                    const auto a = g.at("arity").get<std::string>();
                    if      (a == "infix")   arity = OperatorArity::Infix;
                    else if (a == "prefix")  arity = OperatorArity::Prefix;
                    else if (a == "postfix") arity = OperatorArity::Postfix;
                    else {
                        coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, gPath,
                                  std::format("unknown arity '{}' (expected infix|prefix|postfix)", a));
                        continue;
                    }
                }

                if (!g.contains("operators") || !g.at("operators").is_array()) {
                    coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, gPath,
                              "group requires an 'operators' array");
                    continue;
                }

                // Optional `kind` disambiguates groups whose lexemes have
                // multiple meanings. When omitted, every lexeme in this
                // group must have a single meaning. find() is used (not
                // intern()) so post-frozen lookup is a pure const op.
                std::optional<std::string> explicitKind;
                if (g.contains("kind")) {
                    if (!g.at("kind").is_string()) {
                        coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, gPath,
                                  "'kind' must be a string");
                        continue;
                    }
                    explicitKind = g.at("kind").get<std::string>();
                    if (!data.schemaTokens->contains(*explicitKind)) {
                        coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, gPath,
                                  std::format("group 'kind' '{}' is not a declared token kind",
                                              *explicitKind));
                        continue;
                    }
                }

                const OperatorTable::Entry entry{precedence, assoc};

                for (std::size_t j = 0; j < g.at("operators").size(); ++j) {
                    json const& op = g.at("operators")[j];
                    const auto opPath = std::format("{}/operators/{}", gPath, j);
                    if (!op.is_string()) {
                        coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, opPath,
                                  "operator entry must be a string lexeme");
                        continue;
                    }
                    const auto lex = op.get<std::string>();

                    auto it = data.lexemeTable.find(lex);
                    if (it == data.lexemeTable.end() || it->second.empty()) {
                        coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, opPath,
                                  std::format("operator lexeme '{}' is not declared in 'tokens' or 'keywords'", lex));
                        continue;
                    }
                    auto const& meanings = it->second;

                    SchemaTokenId targetId;
                    if (explicitKind) {
                        const auto explicitId = data.schemaTokens->find(*explicitKind);
                        bool found = false;
                        for (auto const& m : meanings) {
                            if (m.id.v == explicitId.v) { found = true; break; }
                        }
                        if (!found) {
                            coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, opPath,
                                      std::format("group 'kind' '{}' is not a meaning of lexeme '{}'",
                                                  *explicitKind, lex));
                            continue;
                        }
                        targetId = explicitId;
                    } else if (meanings.size() == 1) {
                        targetId = meanings[0].id;
                    } else {
                        coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, opPath,
                                  std::format("lexeme '{}' has {} meanings; group must specify 'kind' to disambiguate",
                                              lex, meanings.size()));
                        continue;
                    }

                    // Duplicate-detect: two groups stamping the same
                    // (id, arity) silently lose the first via insert_or_assign.
                    // Loader rejects so the config bug is visible.
                    if (data.operators.lookup(targetId, arity).has_value()) {
                        coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, opPath,
                                  std::format("operator '{}' ({}) declared twice in the precedence table",
                                              lex, operatorArityName(arity)));
                        continue;
                    }

                    data.operators.insert(targetId, arity, entry);
                }
            }
        }
    }

    // scopes.validity ──
    if (doc.contains("scopes") && doc.at("scopes").is_object()) {
        auto const& sv = doc.at("scopes");
        if (sv.contains("validity") && sv.at("validity").is_array()) {
            for (std::size_t i = 0; i < sv.at("validity").size(); ++i) {
                json const& v = sv.at("validity").at(i);
                const auto vp = std::format("/scopes/validity/{}", i);
                if (!v.is_object() || !v.contains("scope") || !v.at("scope").is_string()) {
                    coll.emit(DiagnosticCode::C_MissingField, vp,
                              "validity entry needs string 'scope'");
                    continue;
                }
                const auto sk = parseScopeName(v.at("scope").get<std::string>());
                if (!sk) {
                    coll.emit(DiagnosticCode::C_UnclosableScope, vp,
                              std::format("unknown scope '{}'",
                                          v.at("scope").get<std::string>()));
                    continue;
                }
                if (v.contains("forbid") && v.at("forbid").is_array()) {
                    auto& set = data.scopeForbid[static_cast<std::uint16_t>(*sk)];
                    for (auto const& tk : v.at("forbid")) {
                        if (!tk.is_string()) continue;
                        const auto kindName = tk.get<std::string>();
                        if (!data.schemaTokens->contains(kindName)) {
                            coll.emit(DiagnosticCode::C_UnknownToken,
                                      std::format("{}/forbid", vp),
                                      std::format("forbid references unknown token kind '{}'",
                                                  kindName));
                            continue;
                        }
                        const auto id = data.schemaTokens->intern(kindName);
                        set.insert(id.v);
                    }
                }
            }
        }
    }

    // shapes ──
    if (doc.contains("shapes")) {
        if (!doc.at("shapes").is_object()) {
            coll.emit(DiagnosticCode::C_MissingField, "/shapes",
                      "'shapes' must be an object");
        } else {
            // Pass A: intern every shape name so cross-references resolve
            // regardless of definition order.
            for (auto const& [shapeName, _] : doc.at("shapes").items()) {
                (void)data.rules->intern(shapeName);
            }

            // Pass B: validate references — every string atom must resolve
            // to either a shape (RuleInterner) or a token kind
            // (SchemaTokenInterner). Diagnostics surface here; the compile
            // pass below trusts the references have already resolved.
            for (auto const& [shapeName, body] : doc.at("shapes").items()) {
                const auto shapePath = std::format("/shapes/{}", shapeName);
                std::vector<std::string> refs;
                collectReferences(body, shapePath, refs, coll);

                for (auto const& r : refs) {
                    if (data.rules->contains(r))        continue;
                    if (data.schemaTokens->contains(r)) continue;
                    coll.emit(DiagnosticCode::C_UnknownShape, shapePath,
                              std::format("unknown reference '{}'", r));
                }
            }

            if (data.rules->contains("root")) {
                data.rootRule = data.rules->intern("root");
            } else if (!doc.at("shapes").empty()) {
                coll.emit(DiagnosticCode::C_MissingField, "/shapes",
                          "missing required 'root' shape");
            }

            // Compile every rule body into a flat position table and
            // precompute FIRST sets, nullability flags, and per-position
            // nullable-tail flags. Only worth doing when references
            // validated cleanly — otherwise the compile pass would crash
            // on broken references it expects the prior pass to reject.
            // Ambiguity detection runs last: it reads the position tables
            // to surface alt branches whose FIRST sets overlap.
            if (!coll.hasErrors()) {
                computeFirstAndNullable    (data, doc.at("shapes"));
                buildPositionTables        (data, doc.at("shapes"));
                detectAmbiguousAlternatives(data, coll, doc.at("shapes"));
                computeNullableTails       (data);
            }
        }
    }

    // Freeze interners — post-load, no further internment allowed.
    data.rules->freeze();
    data.schemaTokens->freeze();

    if (coll.hasErrors()) {
        return std::unexpected(std::move(coll.diagnostics));
    }
    return std::make_shared<GrammarSchema>(std::move(data));
}

} // namespace dss::detail
