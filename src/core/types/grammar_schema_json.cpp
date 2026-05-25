#include "core/types/grammar_schema_json.hpp"

#include "core/types/grammar_schema.hpp"
#include "core/types/operator_table.hpp"
#include "core/types/rule_id.hpp"
#include "core/types/schema_token_interner.hpp"
#include "core/types/scope_kind.hpp"
#include "core/types/tree_node.hpp"
#include "core/types/well_known_names.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <regex>
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

// Soft upper bound on how many entries a single scopeRequire list may
// hold. Real configs use 1–4; anything larger is almost certainly a
// config bug. Exceeding this is a load warning, not an error — the
// builder still works, but every pushToken pays an O(N×stack) scan.
constexpr std::size_t kScopeListSoftCap = 32;

void parseModeFields(json const& m,
                     std::string const& entryPath,
                     std::unordered_map<std::string, LexerModeId> const& modeIds,
                     LexemeMeaning& lm,
                     Collector& c) {
    const bool hasOp  = m.contains("modeOp");
    const bool hasArg = m.contains("modeArg");
    if (!hasOp && !hasArg) return;

    if (!hasOp) {
        c.emit(DiagnosticCode::C_ConflictingField,
               std::format("{}/modeArg", entryPath),
               "'modeArg' is meaningless without 'modeOp'");
        return;
    }
    if (!m.at("modeOp").is_string()) {
        c.emit(DiagnosticCode::C_ConflictingField,
               std::format("{}/modeOp", entryPath),
               "'modeOp' must be a string ('pushMode' | 'popMode' | 'replaceMode')");
        return;
    }
    const auto opStr = m.at("modeOp").get<std::string>();
    ModeOp op = ModeOp::None;
    if      (opStr == "pushMode")    op = ModeOp::PushMode;
    else if (opStr == "popMode")     op = ModeOp::PopMode;
    else if (opStr == "replaceMode") op = ModeOp::ReplaceMode;
    else {
        c.emit(DiagnosticCode::C_ConflictingField,
               std::format("{}/modeOp", entryPath),
               std::format("unknown modeOp '{}' (expected 'pushMode' | 'popMode' | 'replaceMode')",
                           opStr));
        return;
    }

    LexerModeId arg{};
    if (hasArg) {
        if (!m.at("modeArg").is_string()) {
            c.emit(DiagnosticCode::C_ConflictingField,
                   std::format("{}/modeArg", entryPath),
                   "'modeArg' must be a string mode name");
            return;
        }
        const auto target = m.at("modeArg").get<std::string>();
        auto it = modeIds.find(target);
        if (it == modeIds.end()) {
            c.emit(DiagnosticCode::C_UnknownLexerMode,
                   std::format("{}/modeArg", entryPath),
                   std::format("unknown lexer mode '{}'", target));
            return;
        }
        arg = it->second;
    }

    if (op == ModeOp::PushMode || op == ModeOp::ReplaceMode) {
        if (!hasArg) {
            c.emit(DiagnosticCode::C_ConflictingField,
                   std::format("{}/modeOp", entryPath),
                   std::format("'modeOp: {}' requires 'modeArg' naming the target mode",
                               opStr));
            return;
        }
    } else if (op == ModeOp::PopMode) {
        if (hasArg) {
            c.emit(DiagnosticCode::C_RedundantField,
                   std::format("{}/modeArg", entryPath),
                   "'modeArg' has no effect with 'modeOp: popMode'",
                   DiagnosticSeverity::Warning);
            arg = LexerModeId{};                     // ignore the value
        }
    }

    lm.modeOp  = op;
    lm.modeArg = arg;
}

// Parse a `stringStyle` object on a token meaning. Returns the parsed
// StringStyle when the field is present and well-formed. Returns
// nullopt when the field is absent OR when parsing failed (in which
// case at least one diagnostic was emitted). Caller decides whether
// to attach the resulting style to a LexemeMeaning.
//
// Validation:
//   - `escapeKind`: required string, one of "char"/"doubled-delimiter"/"none".
//   - `escapeChar`: required when escapeKind == "char"; single character.
//   - `endsAt`: required non-empty string.
//   - `endsAtLongestMatch`/`multiline`: optional bools.
//   - `delimiterTag`: optional string; only "matched" is recognized.
//   - `tagPattern`: optional regex string (default `[A-Za-z0-9_]{0,16}`);
//     compiled at load time via std::regex to surface malformed patterns.
inline std::optional<StringStyle> parseStringStyle(json const& obj,
                                                   std::string const& path,
                                                   struct Collector& c);

std::optional<StringStyle> parseStringStyle(json const& obj,
                                            std::string const& path,
                                            Collector& c) {
    // Top-level shape.
    if (!obj.is_object()) {
        c.emit(DiagnosticCode::C_InvalidStringStyle, path,
               "'stringStyle' must be an object");
        return std::nullopt;
    }

    StringStyle s;

    // escapeKind — required.
    if (!obj.contains("escapeKind") || !obj.at("escapeKind").is_string()) {
        c.emit(DiagnosticCode::C_InvalidStringStyle,
               std::format("{}/escapeKind", path),
               "'escapeKind' is required and must be a string "
               "('char' | 'doubled-delimiter' | 'none')");
        return std::nullopt;
    }
    const auto ek = obj.at("escapeKind").get<std::string>();
    if      (ek == "none")              s.escapeKind = EscapeKind::None;
    else if (ek == "char")              s.escapeKind = EscapeKind::Char;
    else if (ek == "doubled-delimiter") s.escapeKind = EscapeKind::DoubledDelimiter;
    else {
        c.emit(DiagnosticCode::C_InvalidStringStyle,
               std::format("{}/escapeKind", path),
               std::format("unknown escapeKind '{}'", ek));
        return std::nullopt;
    }

    // escapeChar — required iff escapeKind == Char. Other escapeKinds
    // must not carry one; mixing them is always a config bug. Single
    // ASCII byte; multi-byte UTF-8 chars are rejected (the tokenizer
    // works on raw bytes).
    const bool hasEsc = obj.contains("escapeChar");
    if (s.escapeKind == EscapeKind::Char) {
        if (!hasEsc || !obj.at("escapeChar").is_string() ||
            obj.at("escapeChar").get<std::string>().size() != 1) {
            c.emit(DiagnosticCode::C_InvalidStringStyle,
                   std::format("{}/escapeChar", path),
                   "'escapeChar' is required when escapeKind is 'char' and "
                   "must be a single ASCII character");
            return std::nullopt;
        }
        s.escapeChar = obj.at("escapeChar").get<std::string>().front();
    } else if (hasEsc) {
        c.emit(DiagnosticCode::C_InvalidStringStyle,
               std::format("{}/escapeChar", path),
               "'escapeChar' is only meaningful when escapeKind is 'char'");
        return std::nullopt;
    }

    // endsAt — required, non-empty.
    if (!obj.contains("endsAt") || !obj.at("endsAt").is_string() ||
        obj.at("endsAt").get<std::string>().empty()) {
        c.emit(DiagnosticCode::C_MissingField,
               std::format("{}/endsAt", path),
               "'endsAt' is required and must be a non-empty string");
        return std::nullopt;
    }
    s.endsAt = obj.at("endsAt").get<std::string>();

    // endsAtLongestMatch — optional bool.
    if (obj.contains("endsAtLongestMatch")) {
        if (!obj.at("endsAtLongestMatch").is_boolean()) {
            c.emit(DiagnosticCode::C_InvalidStringStyle,
                   std::format("{}/endsAtLongestMatch", path),
                   "'endsAtLongestMatch' must be a boolean");
            return std::nullopt;
        }
        s.endsAtLongestMatch = obj.at("endsAtLongestMatch").get<bool>();
    }
    // Cross-field warning: longest-match with a 1-char terminator is
    // a no-op (the longest run of one char IS that char). Surface so
    // the author notices their intent didn't take effect.
    if (s.endsAtLongestMatch && s.endsAt.size() == 1) {
        c.emit(DiagnosticCode::C_RedundantField,
               std::format("{}/endsAtLongestMatch", path),
               "'endsAtLongestMatch' has no effect with a 1-character 'endsAt'",
               DiagnosticSeverity::Warning);
    }

    // multiline — optional bool.
    if (obj.contains("multiline")) {
        if (!obj.at("multiline").is_boolean()) {
            c.emit(DiagnosticCode::C_InvalidStringStyle,
                   std::format("{}/multiline", path),
                   "'multiline' must be a boolean");
            return std::nullopt;
        }
        s.multiline = obj.at("multiline").get<bool>();
    }

    // delimiterTag — optional. Only "matched" is recognized; presence
    // enables dynamic-tag capture. The signal at runtime is
    // `tagPattern.empty()` — non-empty pattern means dynamic tag.
    bool dynamicTagRequested = false;
    if (obj.contains("delimiterTag")) {
        if (!obj.at("delimiterTag").is_string() ||
            obj.at("delimiterTag").get<std::string>() != "matched") {
            c.emit(DiagnosticCode::C_InvalidStringStyle,
                   std::format("{}/delimiterTag", path),
                   "'delimiterTag' is optional; the only recognized value is 'matched'");
            return std::nullopt;
        }
        dynamicTagRequested = true;
    }

    // tagPattern — optional regex. Must accompany delimiterTag:"matched"
    // (otherwise the pattern is dead data). Default applied when
    // delimiterTag is "matched" but tagPattern is omitted.
    constexpr std::string_view kDefaultTagPattern = "[A-Za-z0-9_]{0,16}";
    if (obj.contains("tagPattern")) {
        if (!dynamicTagRequested) {
            c.emit(DiagnosticCode::C_InvalidStringStyle,
                   std::format("{}/tagPattern", path),
                   "'tagPattern' requires 'delimiterTag: \"matched\"'");
            return std::nullopt;
        }
        if (!obj.at("tagPattern").is_string()) {
            c.emit(DiagnosticCode::C_InvalidStringStyle,
                   std::format("{}/tagPattern", path),
                   "'tagPattern' must be a regex string");
            return std::nullopt;
        }
        s.tagPattern = obj.at("tagPattern").get<std::string>();
    } else if (dynamicTagRequested) {
        s.tagPattern = kDefaultTagPattern;
    }

    // Cross-field validation: longest-match terminator does not
    // compose with dynamic-tag matching. The tokenizer's tag-aware
    // close (regex+endsAt) doesn't honor longest-match, so the two
    // flags silently disagree if both are set. Reject at load.
    if (s.endsAtLongestMatch && !s.tagPattern.empty()) {
        c.emit(DiagnosticCode::C_ConflictingField,
               std::format("{}/endsAtLongestMatch", path),
               "'endsAtLongestMatch' is incompatible with 'delimiterTag: \"matched\"' "
               "(longest-match close is not honored for dynamic tags)");
        return std::nullopt;
    }

    // Validate regex at load. Catch std::regex_error specifically; any
    // other exception (std::bad_alloc, system errors) signals a deeper
    // problem we don't want to silently translate to a diagnostic.
    if (!s.tagPattern.empty()) {
        try {
            std::regex compiled(s.tagPattern);
            (void)compiled;
        } catch (std::regex_error const& e) {
            c.emit(DiagnosticCode::C_InvalidStringStyle,
                   std::format("{}/tagPattern", path),
                   std::format("'tagPattern' is not a valid regex: {}", e.what()));
            return std::nullopt;
        } catch (std::exception const& e) {
            c.emit(DiagnosticCode::C_InvalidStringStyle,
                   std::format("{}/tagPattern", path),
                   std::format("'tagPattern' regex compile failed: {}", e.what()));
            return std::nullopt;
        }
    }

    return s;
}

// Parse a JSON array of scope names into vector<ScopeKind>. Unknown
// names emit C_UnknownScopeName and are dropped; non-string elements
// emit C_ConflictingField with the index path. The walk continues on
// either error so one bad entry doesn't hide a subsequent one.
std::vector<ScopeKind> parseScopeArray(json const& arr,
                                       std::string const& fieldPath,
                                       Collector& c) {
    std::vector<ScopeKind> out;
    if (!arr.is_array()) return out;
    out.reserve(arr.size());
    for (std::size_t i = 0; i < arr.size(); ++i) {
        json const& sn = arr[i];
        if (!sn.is_string()) {
            c.emit(DiagnosticCode::C_ConflictingField,
                   std::format("{}/{}", fieldPath, i),
                   "scope-list entry must be a string");
            continue;
        }
        const auto sk = parseScopeName(sn.get<std::string>());
        if (!sk) {
            c.emit(DiagnosticCode::C_UnknownScopeName, fieldPath,
                   std::format("unknown scope '{}'", sn.get<std::string>()));
            continue;
        }
        out.push_back(*sk);
    }
    if (out.size() > kScopeListSoftCap) {
        c.emit(DiagnosticCode::C_RedundantScopeRequire, fieldPath,
               std::format("scope list has {} entries (soft cap is {}); "
                           "consider tightening the constraint",
                           out.size(), kScopeListSoftCap),
               DiagnosticSeverity::Warning);
    }
    return out;
}

// Parse a single scope-name string field into ScopeKind. Returns
// nullopt if the field is missing, wrong type, or names a scope the
// loader doesn't recognize — emitting diagnostics in the latter two
// cases so the author isn't left wondering why their constraint
// silently disappeared.
std::optional<ScopeKind> parseScopeNameField(json const& parent,
                                             std::string_view key,
                                             std::string const& parentPath,
                                             Collector& c) {
    if (!parent.contains(key)) return std::nullopt;
    json const& v = parent.at(key);
    const auto fieldPath = std::format("{}/{}", parentPath, key);
    if (!v.is_string()) {
        c.emit(DiagnosticCode::C_ConflictingField, fieldPath,
               std::format("'{}' must be a string scope name", key));
        return std::nullopt;
    }
    const auto sk = parseScopeName(v.get<std::string>());
    if (!sk) {
        c.emit(DiagnosticCode::C_UnknownScopeName, fieldPath,
               std::format("unknown scope '{}'", v.get<std::string>()));
        return std::nullopt;
    }
    return *sk;
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
        // FIRST(expr-rule) = FIRST(atom) ∪ {tokens registered as Prefix
        // operators}. The Pratt walker dispatches on prefix tokens
        // before descending into the atom rule, so the parser's
        // dispatch loop must accept a prefix token as a legitimate
        // entry into the expression rule. Without this union, the
        // `tokInFirst` check in `RuleLeaf` dispatch rejects bare
        // prefix expressions like `-a;` even though the walker would
        // happily consume them.
        auto result = firstOfBody(body.at("expr").at("atom"), data);
        for (auto const& [lex, meanings] : data.lexemeTable) {
            (void)lex;
            for (auto const& m : meanings) {
                if (data.operators.lookup(m.id, OperatorArity::Prefix)) {
                    insertSorted(result, m.id);
                }
            }
        }
        return result;
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
    Collector&                     coll;
    std::string const&             shapePath;   // e.g. "/shapes/expression"

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
            const auto id = emplace(detail::Position::makeAltChoice(
                std::move(branches), std::move(ex)));
            // Speculative-alt metadata stored for the parser to consume.
            constexpr std::uint16_t kDefaultLookahead = 8;
            bool speculative = false;
            std::uint16_t lookahead = kDefaultLookahead;
            if (body.contains("speculative")) {
                json const& sv = body.at("speculative");
                if (!sv.is_boolean()) {
                    coll.emit(DiagnosticCode::C_ConflictingField,
                              std::format("{}/speculative", shapePath),
                              "'speculative' must be a boolean");
                } else {
                    speculative = sv.get<bool>();
                }
            }
            if (body.contains("lookahead")) {
                json const& lv = body.at("lookahead");
                if (!lv.is_number_integer()) {
                    coll.emit(DiagnosticCode::C_ConflictingField,
                              std::format("{}/lookahead", shapePath),
                              "'lookahead' must be a positive integer");
                } else {
                    const auto raw = lv.get<std::int64_t>();
                    if (raw <= 0 ||
                        raw > std::numeric_limits<std::uint16_t>::max()) {
                        coll.emit(DiagnosticCode::C_ConflictingField,
                                  std::format("{}/lookahead", shapePath),
                                  std::format("'lookahead' value {} is out of "
                                              "range (must be 1..{})",
                                              raw, std::numeric_limits<std::uint16_t>::max()));
                    } else {
                        lookahead = static_cast<std::uint16_t>(raw);
                    }
                }
                if (!speculative) {
                    coll.emit(DiagnosticCode::C_RedundantField,
                              std::format("{}/lookahead", shapePath),
                              "'lookahead' has no effect without 'speculative: true'",
                              DiagnosticSeverity::Warning);
                }
            }
            if (speculative) positions[id].setSpeculative(true, lookahead);
            return id;
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

void buildPositionTables(GrammarSchemaData& data, json const& shapesJson,
                         Collector& coll) {
    for (auto const& [shapeName, body] : shapesJson.items()) {
        const auto rid = data.rules->find(shapeName);
        auto& rule = data.compiledRules[rid.v];
        rule.positions.clear();
        // positions[0] is the sentinel for cursor posId==0 (invalid).
        // positions[1] is the End that body completions fall through to.
        rule.positions.emplace_back();
        const auto shapePath = std::format("/shapes/{}", shapeName);
        PositionBuilder builder{rule.positions, data, coll, shapePath};
        const auto endId   = builder.emplace(detail::Position::makeEnd());
        const auto entryId = builder.build(body, endId);
        rule.entryPos = entryId;

        // Record `expr`-shape metadata so the parser can hand off to a
        // Pratt walker. The cursor side already compiles the body as a
        // transparent ref to atom (see `PositionBuilder::build`); this
        // is the parser-side signal that operator-precedence climbing
        // applies to this rule. The `expr` body was schema-validated
        // upstream in `collectRuleRefs` — both `atom` (required) and
        // `minPrecedence` (optional) are well-formed if we reach here.
        if (body.is_object() && body.contains("expr")) {
            json const& exprBody = body.at("expr");
            if (exprBody.is_object() && exprBody.contains("atom")
                && exprBody.at("atom").is_string()) {
                const auto& atomName = exprBody.at("atom").get<std::string>();
                if (data.rules->contains(atomName)) {
                    rule.isExpr   = true;
                    rule.exprAtom = data.rules->find(atomName);
                    if (exprBody.contains("minPrecedence")
                        && exprBody.at("minPrecedence").is_number_integer()) {
                        rule.exprMinPrecedence =
                            exprBody.at("minPrecedence").get<std::int32_t>();
                    }
                }
            }
        }
    }
}

// Scan every compiled AltChoice for branches whose precomputed FIRST sets
// overlap. The cursor's `advance` does silent first-branch-wins on overlap,
// which is the wrong default for a grammar that wants disjoint alts; this
// pass surfaces the conflict at load time so the config author resolves
// it by either restructuring the grammar OR marking the alt as
// `speculative: true` to opt in to backtracking semantics. Speculative
// alts are exempt from ambiguity-detect — overlapping FIRST sets are the
// whole reason a config opts in to backtracking.
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
            // Speculative alts deliberately allow overlapping FIRST sets
            // — the whole point is backtracking when the disambiguating
            // token is N positions deep. Skip the ambiguity check.
            if (p.speculative()) continue;
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

// FOLLOW(R) = the set of token kinds that can legitimately follow a
// successful parse of rule R, anywhere it's referenced. Used by the
// parser's panic-mode recovery to decide "have I reached a resync
// point for the currently-failing rule".
//
// Algorithm (textbook fixed-point):
//   For each parent rule, for each RuleLeaf(R) position p:
//     - Add `expectedSet(positions[p.nextPos()])` to FOLLOW(R).
//     - If `positions[p.nextPos()].nullableTail()`, also add
//       FOLLOW(parentRule) to FOLLOW(R) (the path continues past
//       parentRule's end, picking up the grandparent's follow).
// Iterate until no follow set grows.
//
// Root rule's FOLLOW stays empty — nothing in the grammar references
// it. EOF is handled separately by the parser's `canEndSource` check.
void computeFollowSets(GrammarSchemaData& data, Collector& coll) {
    // Bound is far above the O(rules × positions) convergence the
    // monotone lattice guarantees — functions as a runaway guard
    // against a future refactor that breaks monotonicity. If we hit
    // it with `changed == true` we emit a loader diagnostic rather
    // than silently shipping a truncated FOLLOW set.
    constexpr int kMaxIters = 10000;
    auto addToFollow = [](std::vector<SchemaTokenId>& follow,
                          SchemaTokenId tok) -> bool {
        auto it = std::lower_bound(follow.begin(), follow.end(), tok,
            [](SchemaTokenId a, SchemaTokenId b) { return a.v < b.v; });
        if (it != follow.end() && it->v == tok.v) return false;
        follow.insert(it, tok);
        return true;
    };

    bool changed = true;
    int iters = 0;
    while (changed && iters++ < kMaxIters) {
        changed = false;
        for (auto& [parentIdV, parentRule] : data.compiledRules) {
            auto const& positions = parentRule.positions;
            // Snapshot parent's follow set up-front so a same-rule
            // self-reference can't read a follow set we just mutated
            // in this iteration. Reads stay stable within one pass;
            // changes propagate on the next iteration.
            const auto parentFollowSnapshot = parentRule.followSet;
            for (auto const& p : positions) {
                if (p.slotKind() != SlotKind::RuleLeaf) continue;
                const RuleId childRule = p.ruleId();
                auto childIt = data.compiledRules.find(childRule.v);
                if (childIt == data.compiledRules.end()) continue;
                auto& childFollow = childIt->second.followSet;
                if (p.nextPos() >= positions.size()) continue;
                auto const& next = positions[p.nextPos()];
                for (auto tok : next.expectedSet()) {
                    if (addToFollow(childFollow, tok)) changed = true;
                }
                if (next.nullableTail()) {
                    for (auto tok : parentFollowSnapshot) {
                        if (addToFollow(childFollow, tok)) changed = true;
                    }
                }
            }
        }
    }
    if (changed) {
        coll.emit(DiagnosticCode::C_CircularShape,
                  "/shapes",
                  std::format("FOLLOW-set fixed-point did not converge in "
                              "{} iterations — the grammar likely violates "
                              "monotonicity (a loader bug); FOLLOW data is "
                              "truncated and recovery quality degraded",
                              kMaxIters));
    }
}

// Validate that every operator-table `bodyRule` resolves to an
// actually-declared shape (has a compiled body). The operators block
// runs before shape interning so the loader can only intern the name
// at that point; the real check happens here, post-shapes-compile.
void validateOperatorBodyRules(GrammarSchemaData& data, Collector& coll) {
    // For each interned rule lacking a compiled body, check whether
    // any postfix operator entry's `bodyRule` references it. The
    // operators block runs before shape interning, so the loader
    // could only intern the name at operator-load time; the real
    // "shape exists" check happens here, post-shapes-compile.
    auto const& interner = *data.rules;
    for (std::uint32_t r = 1; r < interner.size(); ++r) {
        const RuleId rule{r};
        if (data.compiledRules.contains(rule.v)) continue;
        // Pratt walker synthesizes `binaryExpr`/`unaryExpr`/`postfixExpr`
        // frames without a schema shape; any new walker-only wrapper
        // must be added to this skip list (and to `well_known_names.hpp`).
        const auto name = interner.name(rule);
        if (name == rules::kBinaryExpr
            || name == rules::kUnaryExpr
            || name == rules::kPostfixExpr) {
            continue;
        }
        // Postfix is the only arity that carries `bodyRule`, so iterate
        // just that slice — O(N) in the declared-token count, trivial
        // at load time.
        bool referenced = false;
        for (std::uint32_t t = 1; t < data.schemaTokens->size() && !referenced; ++t) {
            const SchemaTokenId tid{t};
            const auto entry = data.operators.lookup(tid, OperatorArity::Postfix);
            if (entry && entry->grouped
                && entry->grouped->bodyRule.v == rule.v) {
                referenced = true;
            }
        }
        if (referenced) {
            coll.emit(DiagnosticCode::C_UnknownShape,
                      "/operators/groups",
                      std::format("postfix operator group references "
                                  "'bodyRule' = '{}' but no shape with "
                                  "that name is declared", name));
        }
    }
}

// Reject any rule shape OR scope-forbid entry that references a
// SchemaTokenId declared as a lexer mode's `defaultToken.kind`.
// Body-default kinds are off-grammar by construction (see
// `TreeBuilder::bodyDefaultTokenKinds_`): the cursor-advance gate
// silently skips them, so either reference would silently never fire.
//
// Main-mode lexeme meanings DELIBERATELY may map to body-default
// kinds (this is the OR-merge pattern pinned by
// `Tokenizer.TokenFlags_PropagateToBuilderLeafFlags`). When a main-
// mode lexeme resolves to a body-default kind the cursor-skip
// correctly treats it like trivia — harmless as long as no shape
// expects the kind, which the shape check below already enforces.
void validateBodyDefaultKindsOffGrammar(GrammarSchemaData& data, Collector& coll) {
    auto const& bodyKinds = data.bodyDefaultTokenKinds;
    if (bodyKinds.empty()) return;

    // Shape references.
    for (auto const& [ruleIdV, rule] : data.compiledRules) {
        const auto ruleName = data.rules->name(RuleId{ruleIdV});
        for (auto const& p : rule.positions) {
            if (p.slotKind() != SlotKind::TokenLeaf) continue;
            if (!bodyKinds.contains(p.tokenId())) continue;
            coll.emit(DiagnosticCode::C_BodyDefaultKindInShape,
                      std::format("/shapes/{}", ruleName),
                      std::format("token '{}' is declared as a lexer mode's "
                                  "defaultToken.kind and is therefore off-"
                                  "grammar — referencing it from a shape "
                                  "would silently never match",
                                  data.schemaTokens->name(p.tokenId())));
        }
    }

    // scopes.validity[].forbid entries — a forbid on a body-default
    // kind couldn't fire (the kind only appears inside its body mode,
    // and synthesis goes through scope filtering which would reject
    // before the forbid was ever consulted). Author probably meant a
    // different kind.
    for (auto const& [scopeV, forbidSet] : data.scopeForbid) {
        for (auto const tokIdV : forbidSet) {
            const SchemaTokenId id{tokIdV};
            if (!bodyKinds.contains(id)) continue;
            coll.emit(DiagnosticCode::C_BodyDefaultKindInShape,
                      std::format("/scopes/validity"),
                      std::format("token '{}' (forbidden inside scope id {}) is "
                                  "declared as a lexer mode's defaultToken.kind "
                                  "and is off-grammar; the forbid cannot fire",
                                  data.schemaTokens->name(id),
                                  scopeV));
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
    // v3 adds the optional `typeExtensions[]` field (SP2); v4 adds the optional
    // `imports` block. Earlier configs remain valid since both are optional.
    constexpr std::uint32_t kMaxSchemaVersion = 4;
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
    // Per-instance monotonic schema id stamped onto every LexemeMeaning
    // so cross-schema misuse is catchable at the failing lookup.
    static std::atomic<std::uint32_t> sNextSchemaId{1};
    data.id            = SchemaId{sNextSchemaId.fetch_add(1, std::memory_order_relaxed)};

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

    // Pass 1: register `lexerModes` names → LexerModeIds before any
    // token meaning is parsed, so `modeArg` references resolve against
    // forward declarations. Synthesize "main" at id 1; slot 0 is the
    // InvalidLexerMode sentinel. Case-folded near-miss detection warns
    // on configs that mix `"String-body"` and `"string-body"`.
    auto toLower = [](std::string s) {
        for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    };
    std::unordered_map<std::string, std::string> caseFoldedSeen;
    auto registerMode = [&](std::string const& name) -> LexerModeId {
        if (auto it = data.lexerModeIds.find(name); it != data.lexerModeIds.end()) {
            return it->second;
        }
        // Slot 0 = InvalidLexerMode sentinel; real ids start at 1.
        if (data.lexerModes.empty()) {
            data.lexerModes.push_back(LexerMode{});
        }
        const auto folded = toLower(name);
        if (auto seenIt = caseFoldedSeen.find(folded);
            seenIt != caseFoldedSeen.end()) {
            coll.emit(DiagnosticCode::C_ConflictingField,
                      std::format("/lexerModes/{}", name),
                      std::format("mode name '{}' differs only by case from "
                                  "already-declared mode '{}' — references will "
                                  "resolve unpredictably; pick one spelling",
                                  name, seenIt->second),
                      DiagnosticSeverity::Warning);
        }
        caseFoldedSeen.emplace(folded, name);
        const LexerModeId realId{
            static_cast<std::uint32_t>(data.lexerModes.size())};
        data.lexerModes.push_back(LexerMode::make(name, realId, std::nullopt));
        data.lexerModeIds.emplace(name, realId);
        return realId;
    };

    const LexerModeId mainModeId = registerMode("main");

    if (doc.contains("lexerModes")) {
        if (!doc.at("lexerModes").is_object()) {
            coll.emit(DiagnosticCode::C_ConflictingField, "/lexerModes",
                      "'lexerModes' must be an object");
        } else {
            for (auto const& [modeName, _] : doc.at("lexerModes").items()) {
                (void)registerMode(modeName);
            }
        }
    }

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
                            coll.emit(DiagnosticCode::C_UnknownScopeName, entryPath,
                                      std::format("unknown scope name '{}'",
                                                  m.at("opensScope").get<std::string>()));
                        } else {
                            lm.opensScope = *sk;
                        }
                    }
                    // Reject co-existence of legacy `validScopes` and
                    // `scopeRequire`. One source of truth per meaning
                    // keeps span backing unambiguous and prevents an
                    // author from splitting their intent across two
                    // fields and ending up with neither.
                    const bool hasLegacy = m.contains("validScopes");
                    const bool hasNew    = m.contains("scopeRequire");
                    bool scopeRequireFatal = false;
                    if (hasLegacy && hasNew) {
                        coll.emit(DiagnosticCode::C_ConflictingField, entryPath,
                                  "meaning declares both 'validScopes' and "
                                  "'scopeRequire' — use 'scopeRequire' only "
                                  "(legacy 'validScopes' is shorthand for 'scopeRequire.anyOf')");
                        scopeRequireFatal = true;
                    } else if (hasLegacy) {
                        if (!m.at("validScopes").is_array()) {
                            coll.emit(DiagnosticCode::C_ConflictingField,
                                      std::format("{}/validScopes", entryPath),
                                      "'validScopes' must be an array of scope-name strings");
                        } else {
                            auto anyOf = parseScopeArray(m.at("validScopes"),
                                                         std::format("{}/validScopes", entryPath),
                                                         coll);
                            if (!anyOf.empty()) {
                                data.scopeListPool.push_back(std::move(anyOf));
                                lm.scopeRequire.anyOf = std::span<ScopeKind const>{
                                    data.scopeListPool.back()};
                            } else if (!m.at("validScopes").empty()) {
                                // Author wrote a non-empty array but every
                                // entry failed parsing — the diagnostics
                                // already flagged each bad name. No extra
                                // emission needed; the meaning loads with
                                // no anyOf constraint, the way it would
                                // have with an explicit `[]`.
                            } else {
                                coll.emit(DiagnosticCode::C_RedundantScopeRequire,
                                          std::format("{}/validScopes", entryPath),
                                          "'validScopes' is an empty array — drop the "
                                          "field or list at least one scope",
                                          DiagnosticSeverity::Warning);
                            }
                        }
                    } else if (hasNew) {
                        if (!m.at("scopeRequire").is_object()) {
                            coll.emit(DiagnosticCode::C_ConflictingField,
                                      std::format("{}/scopeRequire", entryPath),
                                      "'scopeRequire' must be an object");
                        } else {
                            json const& sr = m.at("scopeRequire");
                            auto loadList = [&](std::string_view key,
                                                std::span<ScopeKind const>& outSpan) {
                                if (!sr.contains(key)) return;
                                json const& v = sr.at(key);
                                const auto path = std::format("{}/scopeRequire/{}",
                                                              entryPath, key);
                                if (!v.is_array()) {
                                    coll.emit(DiagnosticCode::C_ConflictingField, path,
                                              std::format("'{}' must be an array of scope-name strings", key));
                                    return;
                                }
                                if (v.empty()) {
                                    coll.emit(DiagnosticCode::C_RedundantScopeRequire, path,
                                              std::format("'{}' is an empty array — drop the field "
                                                          "or list at least one scope", key),
                                              DiagnosticSeverity::Warning);
                                    return;
                                }
                                auto parsed = parseScopeArray(v, path, coll);
                                if (!parsed.empty()) {
                                    data.scopeListPool.push_back(std::move(parsed));
                                    outSpan = std::span<ScopeKind const>{
                                        data.scopeListPool.back()};
                                }
                            };
                            loadList("anyOf",  lm.scopeRequire.anyOf);
                            loadList("forbid", lm.scopeRequire.forbid);
                            lm.scopeRequire.topMustBe =
                                parseScopeNameField(sr, "topMustBe", entryPath + "/scopeRequire", coll);
                            lm.scopeRequire.outermost =
                                parseScopeNameField(sr, "outermost", entryPath + "/scopeRequire", coll);

                            // Redundancy / contradiction checks. None of
                            // these reject the meaning — the builder will
                            // run the rule as-written — but the author
                            // almost always wants to know.
                            auto const& sm = lm.scopeRequire;
                            const auto srPath = std::format("{}/scopeRequire", entryPath);
                            if (sm.topMustBe.has_value() && !sm.anyOf.empty()) {
                                coll.emit(DiagnosticCode::C_RedundantScopeRequire, srPath,
                                          "'anyOf' is redundant when 'topMustBe' is set "
                                          "(topMustBe is the stricter constraint)",
                                          DiagnosticSeverity::Warning);
                            }
                            if (sm.topMustBe.has_value() && sm.outermost.has_value() &&
                                *sm.topMustBe == *sm.outermost) {
                                coll.emit(DiagnosticCode::C_RedundantScopeRequire, srPath,
                                          "'topMustBe' and 'outermost' name the same scope; "
                                          "the rule matches only single-scope stacks",
                                          DiagnosticSeverity::Warning);
                            }
                            auto forbidContains = [&](ScopeKind k) {
                                for (auto x : sm.forbid) if (x == k) return true;
                                return false;
                            };
                            if (sm.topMustBe.has_value() && forbidContains(*sm.topMustBe)) {
                                coll.emit(DiagnosticCode::C_RedundantScopeRequire, srPath,
                                          "'forbid' lists the same scope as 'topMustBe' — "
                                          "the rule can never match",
                                          DiagnosticSeverity::Warning);
                            }
                            if (sm.outermost.has_value() && forbidContains(*sm.outermost)) {
                                coll.emit(DiagnosticCode::C_RedundantScopeRequire, srPath,
                                          "'forbid' lists the same scope as 'outermost' — "
                                          "the rule can never match",
                                          DiagnosticSeverity::Warning);
                            }
                            for (auto a : sm.anyOf) {
                                if (forbidContains(a)) {
                                    coll.emit(DiagnosticCode::C_RedundantScopeRequire, srPath,
                                              std::format("scope '{}' appears in both "
                                                          "'anyOf' and 'forbid' — the rule "
                                                          "can never match",
                                                          scopeName(a)),
                                              DiagnosticSeverity::Warning);
                                    break;
                                }
                            }
                        }
                    }

                    if (scopeRequireFatal) {
                        // Don't push a meaning with no constraint when the
                        // author asked for one via conflicting fields —
                        // skip it entirely so the rest of the schema isn't
                        // weakened by the loader's silent fallback.
                        continue;
                    }
                    parseModeFields(m, entryPath, data.lexerModeIds, lm, coll);

                    if (m.contains("stringStyle")) {
                        auto ss = parseStringStyle(m.at("stringStyle"),
                                                   std::format("{}/stringStyle", entryPath),
                                                   coll);
                        if (ss.has_value()) {
                            // Reserve slot 0 as the InvalidStringStyle
                            // sentinel; real ids 1..N. Matches the
                            // LexerModeId / NodeId / RuleId convention.
                            if (data.stringStyles.empty()) {
                                data.stringStyles.push_back(StringStyle{});
                            }
                            lm.stringStyleId = StringStyleId{
                                static_cast<std::uint32_t>(data.stringStyles.size())};
                            data.stringStyles.push_back(std::move(*ss));
                        }
                    }

                    meanings.push_back(lm);
                }

                // Deterministic ordering: lowest priority wins; ties
                // broken by declaration order (hence stable_sort).
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
            // Mode operations and string-style descriptors belong on
            // `tokens` entries — keywords resolve contextually (soft-
            // keyword demotion), a distinct mechanism from mode
            // switching or delimited-string opening. Reject loudly.
            if (kw.contains("modeOp") || kw.contains("modeArg")) {
                coll.emit(DiagnosticCode::C_ConflictingField, kwPath,
                          "mode operations belong on 'tokens' entries; keywords "
                          "cannot switch lexer modes — move the entry to "
                          "'tokens' or drop the mode fields");
                continue;
            }
            if (kw.contains("stringStyle")) {
                coll.emit(DiagnosticCode::C_ConflictingField, kwPath,
                          "'stringStyle' belongs on 'tokens' entries; keywords "
                          "are word-shaped and cannot open delimited strings");
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

    // Pass 2: populate per-mode tokens + defaultToken. "main" inherits
    // top-level lexemeTable; other modes parse inline.
    if (mainModeId.valid()) {
        data.lexerModeTokens[mainModeId.v] = data.lexemeTable;
    }
    if (doc.contains("lexerModes") && doc.at("lexerModes").is_object()) {
        for (auto const& [modeName, modeObj] : doc.at("lexerModes").items()) {
            const auto modePath = std::format("/lexerModes/{}", modeName);
            if (!modeObj.is_object()) {
                coll.emit(DiagnosticCode::C_ConflictingField, modePath,
                          "mode definition must be an object");
                continue;
            }
            auto modeIt = data.lexerModeIds.find(modeName);
            if (modeIt == data.lexerModeIds.end()) continue;   // unreachable
            const LexerModeId modeId = modeIt->second;

            // Parse defaultToken into a local; the mode entry is then
            // rebuilt via the factory rather than mutated in place.
            std::optional<DefaultTokenSpec> defaultToken;
            if (modeObj.contains("defaultToken")) {
                json const& dt = modeObj.at("defaultToken");
                if (!dt.is_object() || !dt.contains("kind") ||
                    !dt.at("kind").is_string()) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              std::format("{}/defaultToken", modePath),
                              "'defaultToken' must be an object with a string 'kind'");
                } else {
                    DefaultTokenSpec spec;
                    spec.kind = data.schemaTokens->intern(
                        dt.at("kind").get<std::string>());
                    // Optional `flags` field — propagates onto every
                    // per-codepoint emission the tokenizer makes in
                    // this mode. Lets a comment-body mode flag its
                    // chars as EmptySpace so the AST cursor skips them
                    // wholesale (closes v2-gap-catalog row 3 cleanly).
                    if (dt.contains("flags")) {
                        spec.flags = parseFlagList(dt.at("flags"));
                    }
                    defaultToken = spec;
                }
            }

            // Optional `unterminatedAs` field — declares the
            // diagnostic flavor when this mode is still open at EOF.
            // Replaces the previous tokenizer-side substring heuristic
            // ("mode name contains 'comment'") with an explicit
            // schema-declared value. Defaults to `String` for
            // backward compat with existing tsql-subset configs.
            UnterminatedFlavor flavor = UnterminatedFlavor::String;
            if (modeObj.contains("unterminatedAs")) {
                json const& ua = modeObj.at("unterminatedAs");
                if (!ua.is_string()) {
                    coll.emit(DiagnosticCode::C_ConflictingField,
                              std::format("{}/unterminatedAs", modePath),
                              "'unterminatedAs' must be a string");
                } else {
                    const auto v = ua.get<std::string>();
                    if      (v == "string")  flavor = UnterminatedFlavor::String;
                    else if (v == "comment") flavor = UnterminatedFlavor::Comment;
                    else if (v == "generic") flavor = UnterminatedFlavor::Generic;
                    else {
                        coll.emit(DiagnosticCode::C_ConflictingField,
                                  std::format("{}/unterminatedAs", modePath),
                                  std::format("unknown 'unterminatedAs' value '{}' — "
                                              "expected one of \"string\", \"comment\", \"generic\"", v));
                    }
                }
            }
            data.lexerModes[modeId.v] = LexerMode::make(modeName, modeId, defaultToken, flavor);

            // tokens field: "default" inherits top-level lexemeTable;
            // an inline object IS the per-mode table (parsing deferred).
            const bool hasTokens = modeObj.contains("tokens");
            if (hasTokens) {
                json const& tk = modeObj.at("tokens");
                if (tk.is_string() && tk.get<std::string>() == "default") {
                    data.lexerModeTokens[modeId.v] = data.lexemeTable;
                } else if (tk.is_object()) {
                    coll.emit(DiagnosticCode::C_RedundantField,
                              std::format("{}/tokens", modePath),
                              "per-mode inline 'tokens' objects are not yet "
                              "parsed; the mode's lookup table will be empty. "
                              "Use 'tokens: \"default\"' to inherit the top-level "
                              "map, or list mode-specific tokens at top-level "
                              "with appropriate 'modeOp' entries to switch in",
                              DiagnosticSeverity::Warning);
                } else {
                    coll.emit(DiagnosticCode::C_ConflictingField,
                              std::format("{}/tokens", modePath),
                              "'tokens' must be 'default' or an inline tokens object");
                }
            } else if (defaultToken.has_value()) {
                // A mode with defaultToken but no `tokens` field will
                // only ever match its defaultToken — legitimate for
                // string-body-style scanners, but a frequent typo. Warn
                // so authors who meant `tokens: "default"` don't get a
                // silently empty mode.
                coll.emit(DiagnosticCode::C_RedundantField, modePath,
                          "mode declares 'defaultToken' but no 'tokens' field — "
                          "only 'defaultToken' will ever match. Add "
                          "'tokens: \"default\"' if you also want the top-level "
                          "token map active in this mode",
                          DiagnosticSeverity::Warning);
            }
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

                // Grouped-postfix delimiter + body rule. Optional;
                // only meaningful for postfix arity. The walker reads
                // `endsAt` to know "this postfix has a closing token"
                // and `bodyRule` to know "what to parse between
                // opener and closer". Both fields validated below
                // BEFORE we stamp the entry into the table.
                SchemaTokenId endsAtId{};
                RuleId        bodyRuleId{};
                if (g.contains("endsAt")) {
                    if (arity != OperatorArity::Postfix) {
                        coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, gPath,
                                  "'endsAt' is only valid on a postfix group");
                        continue;
                    }
                    if (!g.at("endsAt").is_string()) {
                        coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, gPath,
                                  "'endsAt' must be a string lexeme");
                        continue;
                    }
                    const auto closer = g.at("endsAt").get<std::string>();
                    auto closerIt = data.lexemeTable.find(closer);
                    if (closerIt == data.lexemeTable.end()
                        || closerIt->second.empty()) {
                        coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, gPath,
                                  std::format("'endsAt' lexeme '{}' is not declared in 'tokens'",
                                              closer));
                        continue;
                    }
                    if (closerIt->second.size() != 1) {
                        coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, gPath,
                                  std::format("'endsAt' lexeme '{}' has multiple meanings; "
                                              "ambiguous closers are unsupported",
                                              closer));
                        continue;
                    }
                    endsAtId = closerIt->second[0].id;
                }
                if (g.contains("bodyRule")) {
                    if (arity != OperatorArity::Postfix) {
                        coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, gPath,
                                  "'bodyRule' is only valid on a postfix group");
                        continue;
                    }
                    if (!endsAtId.valid()) {
                        coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, gPath,
                                  "'bodyRule' requires a paired 'endsAt' delimiter");
                        continue;
                    }
                    if (!g.at("bodyRule").is_string()) {
                        coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, gPath,
                                  "'bodyRule' must be a string rule name");
                        continue;
                    }
                    // The operators block runs BEFORE shapes are
                    // interned in Pass A, so `data.rules->contains`
                    // would falsely report unknown for every entry.
                    // Intern the name now (creates the RuleId if
                    // absent, returns existing one if a later shape
                    // shares the name); validate-against-declared-
                    // shapes happens in a post-shapes pass below.
                    const auto rname = g.at("bodyRule").get<std::string>();
                    bodyRuleId = data.rules->intern(rname);
                }
                OperatorTable::Entry entry{precedence, assoc, std::nullopt};
                if (endsAtId.valid()) {
                    entry.grouped = OperatorTable::GroupedPostfix{
                        endsAtId, bodyRuleId};
                }

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
                    coll.emit(DiagnosticCode::C_UnknownScopeName, vp,
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

    // syncTokens ──
    // Optional array of token-kind names treated as panic-mode resync
    // points by the parser. Loader validates: each entry must be a
    // string naming a declared / built-in token kind, and Eof/Error are
    // rejected (Eof is always an implicit sync; Error would short-
    // circuit recovery before it ever reached a real syntactic site).
    // Stored sorted ascending by id.v for fast contains-probes in the
    // parser's hot recovery loop.
    if (doc.contains("syncTokens")) {
        json const& sv = doc.at("syncTokens");
        if (!sv.is_array()) {
            coll.emit(DiagnosticCode::C_MissingField, "/syncTokens",
                      "'syncTokens' must be an array of token-kind names");
        } else {
            std::vector<SchemaTokenId> collected;
            for (std::size_t i = 0; i < sv.size(); ++i) {
                json const& entry = sv[i];
                const auto path = std::format("/syncTokens/{}", i);
                if (!entry.is_string()) {
                    coll.emit(DiagnosticCode::C_MissingField, path,
                              "syncToken entry must be a string token-kind name");
                    continue;
                }
                const auto name = entry.get<std::string>();
                if (!data.schemaTokens->contains(name)) {
                    coll.emit(DiagnosticCode::C_UnknownToken, path,
                              std::format("syncToken '{}' is not a declared "
                                          "or built-in token kind", name));
                    continue;
                }
                const auto id = data.schemaTokens->find(name);
                if (name == "Eof" || name == "Error") {
                    coll.emit(DiagnosticCode::C_ConflictingField, path,
                              std::format("syncToken '{}' is reserved — Eof is "
                                          "an implicit sync, Error would short-"
                                          "circuit recovery", name));
                    continue;
                }
                if (data.emptySpaceTokens.contains(id.v)) {
                    coll.emit(DiagnosticCode::C_ConflictingField, path,
                              std::format("syncToken '{}' is flagged "
                                          "`EmptySpace`; using trivia as a "
                                          "panic-mode break would stop "
                                          "recovery at every whitespace", name));
                    continue;
                }
                auto it = std::lower_bound(collected.begin(), collected.end(), id,
                    [](SchemaTokenId a, SchemaTokenId b) { return a.v < b.v; });
                if (it == collected.end() || it->v != id.v) {
                    collected.insert(it, id);
                }
            }
            data.syncTokens = std::move(collected);
        }
    }

    // typeExtensions ── per-language extension type-kinds (SP2; schema v3).
    // Optional; absent in v1/v2 configs. Each entry:
    //   { "name": "<Lang>::<Type>",
    //     "parameters": [ { "name": "N", "kind": "Integer"|"Type" } ] }
    // The descriptors are registered into a CU's TypeRegistry at CU build time
    // (registerSchemaTypeExtensions). kindIds are minted there, not here.
    if (doc.contains("typeExtensions")) {
        json const& te = doc.at("typeExtensions");
        if (!te.is_array()) {
            coll.emit(DiagnosticCode::C_UnknownTypeExtension, "/typeExtensions",
                      "'typeExtensions' must be an array of extension declarations");
        } else {
            std::unordered_set<std::string> seenNames;
            for (std::size_t i = 0; i < te.size(); ++i) {
                json const& entry = te[i];
                const auto path = std::format("/typeExtensions/{}", i);
                if (!entry.is_object()) {
                    coll.emit(DiagnosticCode::C_UnknownTypeExtension, path,
                              "type-extension entry must be an object");
                    continue;
                }
                if (!entry.contains("name") || !entry.at("name").is_string()) {
                    coll.emit(DiagnosticCode::C_MissingField, path + "/name",
                              "type-extension entry must have a string 'name'");
                    continue;
                }
                auto extName = entry.at("name").get<std::string>();
                if (!seenNames.insert(extName).second) {
                    coll.emit(DiagnosticCode::C_ConflictingField, path + "/name",
                              std::format("type extension '{}' declared more than once", extName));
                    continue;
                }
                TypeExtensionDescriptor desc;
                desc.name = std::move(extName);
                bool paramsOk = true;
                if (entry.contains("parameters")) {
                    json const& params = entry.at("parameters");
                    if (!params.is_array()) {
                        coll.emit(DiagnosticCode::C_TypeExtensionParamMismatch,
                                  path + "/parameters", "'parameters' must be an array");
                        paramsOk = false;
                    } else {
                        for (std::size_t p = 0; p < params.size(); ++p) {
                            json const& param = params[p];
                            const auto ppath = std::format("{}/parameters/{}", path, p);
                            if (!param.is_object()
                                || !param.contains("name") || !param.at("name").is_string()
                                || !param.contains("kind") || !param.at("kind").is_string()) {
                                coll.emit(DiagnosticCode::C_TypeExtensionParamMismatch, ppath,
                                          "parameter must be an object with string 'name' and 'kind'");
                                paramsOk = false;
                                continue;
                            }
                            const auto kindStr = param.at("kind").get<std::string>();
                            TypeParamKind pk{};
                            if (kindStr == "Integer") {
                                pk = TypeParamKind::Integer;
                            } else if (kindStr == "Type") {
                                pk = TypeParamKind::Type;
                            } else {
                                coll.emit(DiagnosticCode::C_TypeExtensionParamMismatch, ppath,
                                          std::format("unknown parameter kind '{}' (expected "
                                                      "'Integer' or 'Type')", kindStr));
                                paramsOk = false;
                                continue;
                            }
                            desc.parameters.push_back(
                                TypeParam{param.at("name").get<std::string>(), pk});
                        }
                    }
                }
                if (paramsOk) data.typeExtensions.push_back(std::move(desc));
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

            // Auto-intern the Pratt-walker wrapper rules. The parser
            // synthesizes `binaryExpr`/`unaryExpr`/`postfixExpr` frames
            // around operator-precedence results; those frames need
            // valid RuleIds. We auto-intern only when the schema
            // actually uses `expr` shapes (i.e. opts into Pratt
            // dispatch); other schemas keep their lean rule interner.
            // The wrapper rules have no compiled body — they're
            // parser-managed and the schema cursor walks transparently
            // past them (silently invalid through the wrapper frame,
            // recovering on close; no P_SchemaCursorDesync since the
            // transitions skip the wasValid→!nowValid check).
            //
            // Reject user-declared shapes named `binaryExpr` /
            // `unaryExpr` / `postfixExpr` — they're walker-synthesized
            // and a user redeclaration would let the schema cursor see
            // a body for them, breaking the "transparent wrapper"
            // invariant. Surface as C_UnknownShape at the named path.
            for (auto const& reserved
                 : {"binaryExpr", "unaryExpr", "postfixExpr"}) {
                if (doc.at("shapes").contains(reserved)) {
                    coll.emit(
                        DiagnosticCode::C_UnknownShape,
                        std::format("/shapes/{}", reserved),
                        std::format(
                            "shape name '{}' is reserved for the "
                            "Pratt-walker wrapper rule (auto-interned "
                            "when the schema declares any `expr` shape); "
                            "rename the user shape", reserved));
                }
            }

            // Recursive scan: `expr` may be nested inside any
            // sequence/alt/optional/repeat body, not only top-level.
            // A flat scan would miss `{ "sequence": [ { "expr": {...} } ] }`
            // and the walker would later fatal-abort on a missing
            // wrapper-rule lookup.
            auto containsExprBody = [](auto const& self,
                                       json const& body) -> bool {
                if (!body.is_object()) return false;
                if (body.contains("expr")) return true;
                for (auto const& key : {"sequence", "alt"}) {
                    if (body.contains(key) && body.at(key).is_array()) {
                        for (auto const& child : body.at(key)) {
                            if (self(self, child)) return true;
                        }
                    }
                }
                for (auto const& key : {"optional", "repeat"}) {
                    if (body.contains(key)) {
                        if (self(self, body.at(key))) return true;
                    }
                }
                return false;
            };
            bool schemaUsesExpr = false;
            for (auto const& [shapeName, body] : doc.at("shapes").items()) {
                (void)shapeName;
                if (containsExprBody(containsExprBody, body)) {
                    schemaUsesExpr = true;
                    break;
                }
            }
            if (schemaUsesExpr) {
                for (auto const& name : {"binaryExpr", "unaryExpr", "postfixExpr"}) {
                    (void)data.rules->intern(name);
                }
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
                // Compute the body-mode kind union BEFORE the
                // validators run so they can consume the schema-
                // owned set (single source of truth shared with
                // `Parser::Impl` and `TreeBuilder`).
                for (auto const& mode : data.lexerModes) {
                    if (mode.defaultToken) {
                        data.bodyDefaultTokenKinds.insert(
                            mode.defaultToken->kind);
                    }
                }
                computeFirstAndNullable    (data, doc.at("shapes"));
                buildPositionTables        (data, doc.at("shapes"), coll);
                detectAmbiguousAlternatives(data, coll, doc.at("shapes"));
                computeNullableTails       (data);
                computeFollowSets          (data, coll);
                validateBodyDefaultKindsOffGrammar(data, coll);
                validateOperatorBodyRules  (data, coll);
            }
        }
    }

    // imports ── config-driven import resolution (schema v4). Optional; absent
    // in v1/v2/v3 configs (strategy stays None → no cross-refs). Parsed LATE,
    // after `shapes`/`tokens` populated the interners, so referenced rule/token
    // names can be checked for existence here rather than silently tolerated at
    // resolve-time. Two generic strategies:
    //   "include-following" — needs `directiveRule` + `pathToken`.
    //   "name-matching"     — needs `nameRule` + `definitionRule` +
    //                         non-empty `referenceParents[]` + `nameToken`;
    //                         optional `caseSensitive` (default true).
    if (doc.contains("imports")) {
        json const& imp = doc.at("imports");
        if (!imp.is_object()) {
            coll.emit(DiagnosticCode::C_InvalidImports, "/imports",
                      "'imports' must be an object");
        } else if (!imp.contains("strategy") || !imp.at("strategy").is_string()) {
            coll.emit(DiagnosticCode::C_InvalidImports, "/imports/strategy",
                      "'imports.strategy' is required and must be a string "
                      "('none', 'include-following', or 'name-matching')");
        } else {
            // Read a required-by-strategy string field. Missing → C_MissingField;
            // present-but-wrong-type → C_InvalidImports; empty string → treated
            // as missing (an empty rule/token name resolves to nothing).
            auto const readField = [&](char const* key, std::string& out) {
                const auto path = std::format("/imports/{}", key);
                if (!imp.contains(key)) {
                    coll.emit(DiagnosticCode::C_MissingField, path,
                              std::format("'imports.{}' is required for this strategy", key));
                    return;
                }
                if (!imp.at(key).is_string()) {
                    coll.emit(DiagnosticCode::C_InvalidImports, path,
                              std::format("'imports.{}' must be a string", key));
                    return;
                }
                out = imp.at(key).get<std::string>();
                if (out.empty()) {
                    coll.emit(DiagnosticCode::C_MissingField, path,
                              std::format("'imports.{}' must be non-empty", key));
                }
            };
            // Validate a declared rule name resolves to a known shape; a token
            // name to a known token kind. Skip the check for an empty name —
            // readField already reported it as missing.
            auto const checkRule = [&](std::string const& name, char const* key) {
                if (!name.empty() && !data.rules->contains(name)) {
                    coll.emit(DiagnosticCode::C_UnknownShape,
                              std::format("/imports/{}", key),
                              std::format("'imports.{}' references unknown shape '{}'", key, name));
                }
            };
            auto const checkToken = [&](std::string const& name, char const* key) {
                if (!name.empty() && !data.schemaTokens->contains(name)) {
                    coll.emit(DiagnosticCode::C_UnknownToken,
                              std::format("/imports/{}", key),
                              std::format("'imports.{}' references unknown token '{}'", key, name));
                }
            };

            ImportConfig cfg;
            const auto strategyStr = imp.at("strategy").get<std::string>();
            if (strategyStr == "none") {
                cfg.strategy = ImportStrategy::None;
            } else if (strategyStr == "include-following") {
                cfg.strategy = ImportStrategy::IncludeFollowing;
                readField("directiveRule", cfg.directiveRule);
                readField("pathToken",     cfg.pathToken);
                checkRule (cfg.directiveRule, "directiveRule");
                checkToken(cfg.pathToken,     "pathToken");
            } else if (strategyStr == "name-matching") {
                cfg.strategy = ImportStrategy::NameMatching;
                readField("nameRule",       cfg.nameRule);
                readField("definitionRule", cfg.definitionRule);
                readField("nameToken",      cfg.nameToken);
                if (!imp.contains("referenceParents")) {
                    coll.emit(DiagnosticCode::C_MissingField, "/imports/referenceParents",
                              "'imports.referenceParents' is required for name-matching");
                } else if (!imp.at("referenceParents").is_array()
                           || imp.at("referenceParents").empty()) {
                    coll.emit(DiagnosticCode::C_InvalidImports, "/imports/referenceParents",
                              "'imports.referenceParents' must be a non-empty array of shape names");
                } else {
                    json const& parents = imp.at("referenceParents");
                    for (std::size_t i = 0; i < parents.size(); ++i) {
                        if (!parents[i].is_string()) {
                            coll.emit(DiagnosticCode::C_InvalidImports,
                                      std::format("/imports/referenceParents/{}", i),
                                      "each 'referenceParents' entry must be a string");
                            continue;
                        }
                        cfg.referenceParents.push_back(parents[i].get<std::string>());
                    }
                }
                checkRule (cfg.nameRule,       "nameRule");
                checkRule (cfg.definitionRule, "definitionRule");
                checkToken(cfg.nameToken,      "nameToken");
                for (std::size_t i = 0; i < cfg.referenceParents.size(); ++i) {
                    if (!data.rules->contains(cfg.referenceParents[i])) {
                        coll.emit(DiagnosticCode::C_UnknownShape,
                                  std::format("/imports/referenceParents/{}", i),
                                  std::format("'imports.referenceParents[{}]' references "
                                              "unknown shape '{}'", i, cfg.referenceParents[i]));
                    }
                }
            } else {
                coll.emit(DiagnosticCode::C_InvalidImports, "/imports/strategy",
                          std::format("unknown import strategy '{}' (expected 'none', "
                                      "'include-following', or 'name-matching')", strategyStr));
            }

            if (imp.contains("caseSensitive")) {
                if (!imp.at("caseSensitive").is_boolean()) {
                    coll.emit(DiagnosticCode::C_InvalidImports, "/imports/caseSensitive",
                              "'imports.caseSensitive' must be a boolean");
                } else {
                    cfg.caseSensitive = imp.at("caseSensitive").get<bool>();
                }
            }

            data.imports = std::move(cfg);
        }
    }

    // Freeze interners — post-load, no further internment allowed.
    data.rules->freeze();
    data.schemaTokens->freeze();

    // Stamp every LexemeMeaning with the owning schema's id. Done in a
    // single pass after all parsing so loader code never has to thread
    // the id through dozens of construction sites.
    for (auto& [lex, meanings] : data.lexemeTable) {
        (void)lex;
        for (auto& m : meanings) m.schemaId = data.id;
    }
    for (auto& [modeId, table] : data.lexerModeTokens) {
        (void)modeId;
        for (auto& [lex, meanings] : table) {
            (void)lex;
            for (auto& m : meanings) m.schemaId = data.id;
        }
    }

    // (`bodyDefaultTokenKinds` was populated above before the
    // validators ran — see the shapes block.)

    // Longest declared lexeme key — consumed by the tokenizer (TZ1+)
    // to cap its longest-match probe length. Iterate every per-mode
    // table too in case a mode-only token exceeds the main table's max.
    data.maxLexemeLength = 0;
    for (auto const& [lex, meanings] : data.lexemeTable) {
        (void)meanings;
        if (lex.size() > data.maxLexemeLength) data.maxLexemeLength = lex.size();
    }
    for (auto const& [modeId, table] : data.lexerModeTokens) {
        (void)modeId;
        for (auto const& [lex, meanings] : table) {
            (void)meanings;
            if (lex.size() > data.maxLexemeLength) data.maxLexemeLength = lex.size();
        }
    }

    if (coll.hasErrors()) {
        return std::unexpected(std::move(coll.diagnostics));
    }
    return std::make_shared<GrammarSchema>(std::move(data));
}

} // namespace dss::detail
