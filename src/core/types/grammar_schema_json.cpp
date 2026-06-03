#include "core/types/grammar_schema_json.hpp"

#include "core/substrate/diagnostic_collector.hpp"
#include "core/substrate/mint_monotonic_id.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/operator_table.hpp"
#include "core/types/rule_id.hpp"
#include "core/types/schema_token_interner.hpp"
#include "core/types/scope_kind.hpp"
#include "core/types/tree_node.hpp"
#include "core/types/object_format_kind.hpp"  // objectFormatKindFromName

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_set>
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
//
// Paradigm-specific kinds (`BoolLiteral`/`CharLiteral`/`NullLiteral`)
// were removed from this list in the 08.55 cleanup — not every plausible
// programming language has those literal categories. A language that
// uses them declares them explicitly in its `tokens`/`keywords` block.
// Universal categories remain pre-interned:
//   - `Identifier`, `IntLiteral`, `FloatLiteral`, `StringLiteral` are
//     emitted directly by the tokenizer's lexical layers (identifier
//     scan + numeric scan + string-literal opener), and the scanner
//     resolves the kind by name; pre-interning keeps every shipped
//     schema able to reference them in a `shapes` body without an
//     explicit token declaration.
//   - `Whitespace`/`Newline`/`Eof`/`Error` are tokenizer-internal
//     kinds the engine emits unconditionally.
constexpr std::string_view kBuiltinTokenKindNames[] = {
    "Identifier",
    "IntLiteral",
    "FloatLiteral",
    "StringLiteral",
    "Eof",
    "Error",
    "Whitespace",
    "Newline",
};

using Collector = substrate::DiagnosticCollector;

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
                                                   Collector& c);

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
    // `expr` body is `{ atom: <rule>, minPrecedence?: <int>,
    // wrapperRules: { binary, unary, postfix } }`. The loader validates
    // the atom reference resolves and rejects unknown keys so a typo
    // (e.g. `minPrecdence`) doesn't silently use the default.
    // `wrapperRules` is REQUIRED — every `expr` shape declares the
    // three Pratt-walker wrapper rule names the engine will synthesize
    // around operator-precedence results (08.55 cleanup). Operator-
    // table consumption is the parser's responsibility, not the
    // loader's.
    if (body.contains("expr")) {
        json const& exprBody = body.at("expr");
        const auto exprPath = std::format("{}/expr", path);
        // Nested-expr rejection. Wrapper-rule packs are keyed by the
        // OWNING top-level shape's RuleId (see `walkExprBodies` Pass A.2
        // below); an `expr` declared inside a sequence/alt/optional/repeat
        // body would key on the wrong rule and silently miss at parse
        // time. Top-level path is `/shapes/<name>`; anything deeper means
        // we've recursed through a non-expr kind. Reject loudly here
        // rather than threading per-position keying through the loader.
        const std::string kShapesPrefix = "/shapes/";
        const bool topLevel =
            path.starts_with(kShapesPrefix)
            && path.find('/', kShapesPrefix.size()) == std::string::npos;
        if (!topLevel) {
            c.emit(DiagnosticCode::C_UnknownShape, exprPath,
                   "'expr' body must be at the top of a shape definition; "
                   "nested `expr` declarations are not supported");
            return;
        }
        if (!exprBody.is_object()) {
            c.emit(DiagnosticCode::C_MissingField, exprPath,
                   "'expr' body must be an object with an 'atom' field");
        } else if (!exprBody.contains("atom") || !exprBody.at("atom").is_string()) {
            c.emit(DiagnosticCode::C_MissingField, exprPath,
                   "'expr' requires a string 'atom' field naming the operand rule");
        } else {
            outRefs.push_back(exprBody.at("atom").get<std::string>());
            // Allowlist: anything other than atom/minPrecedence/wrapperRules
            // is a typo.
            for (auto const& [k, _] : exprBody.items()) {
                if (k != "atom" && k != "minPrecedence" && k != "wrapperRules") {
                    c.emit(DiagnosticCode::C_UnknownShape,
                           std::format("{}/{}", exprPath, k),
                           std::format("unknown 'expr' field '{}' — expected 'atom', 'minPrecedence', or 'wrapperRules'", k));
                }
            }
            // `wrapperRules` validation. The actual interning of the
            // declared names happens later (Pass A on shapes), so here
            // we only emit the shape diagnostic — missing block or any
            // missing/empty field is C_MissingWrapperRules. The walker
            // and the shape-existence validator both read the resulting
            // RuleIds out of the schema; if validation surfaces an
            // error here, those downstream stages are skipped.
            if (!exprBody.contains("wrapperRules")) {
                c.emit(DiagnosticCode::C_MissingWrapperRules,
                       std::format("{}/wrapperRules", exprPath),
                       "'expr' requires a 'wrapperRules' object with "
                       "'binary', 'unary', and 'postfix' rule names");
            } else {
                json const& wr = exprBody.at("wrapperRules");
                const auto wrPath = std::format("{}/wrapperRules", exprPath);
                if (!wr.is_object()) {
                    c.emit(DiagnosticCode::C_MissingWrapperRules, wrPath,
                           "'wrapperRules' must be an object with 'binary', "
                           "'unary', and 'postfix' string fields");
                } else {
                    for (auto const* key : {"binary", "unary", "postfix"}) {
                        if (!wr.contains(key) || !wr.at(key).is_string()
                            || wr.at(key).get<std::string>().empty()) {
                            c.emit(DiagnosticCode::C_MissingWrapperRules,
                                   std::format("{}/{}", wrPath, key),
                                   std::format("'wrapperRules.{}' is required and "
                                               "must be a non-empty string naming "
                                               "the Pratt-walker wrapper rule", key));
                        }
                    }
                    // Pairwise-distinct check. The walker tags each Pratt
                    // frame by the wrapper RuleId; duplicates collapse
                    // distinct frame classes into one bucket and silently
                    // miscount nesting depth. A dedicated diagnostic code
                    // (`C_DuplicateWrapperRules`) keeps "missing" and
                    // "duplicate" as distinct failure classes for tooling.
                    auto wrapperName = [&](char const* key) -> std::string {
                        if (!wr.contains(key) || !wr.at(key).is_string()) return {};
                        return wr.at(key).get<std::string>();
                    };
                    const auto nb = wrapperName("binary");
                    const auto nu = wrapperName("unary");
                    const auto np = wrapperName("postfix");
                    if (!nb.empty() && !nu.empty() && nb == nu) {
                        c.emit(DiagnosticCode::C_DuplicateWrapperRules, wrPath,
                               std::format("'wrapperRules.binary' and 'wrapperRules.unary' "
                                           "must name distinct rules (both are '{}')", nb));
                    }
                    if (!nu.empty() && !np.empty() && nu == np) {
                        c.emit(DiagnosticCode::C_DuplicateWrapperRules, wrPath,
                               std::format("'wrapperRules.unary' and 'wrapperRules.postfix' "
                                           "must name distinct rules (both are '{}')", nu));
                    }
                    if (!nb.empty() && !np.empty() && nb == np) {
                        c.emit(DiagnosticCode::C_DuplicateWrapperRules, wrPath,
                               std::format("'wrapperRules.binary' and 'wrapperRules.postfix' "
                                           "must name distinct rules (both are '{}')", nb));
                    }
                    // Allowlist: anything other than the three known keys
                    // is a typo. Use C_UnknownShape (matches the expr-body
                    // unknown-key check a few lines above) — C_MissingWrapperRules
                    // is reserved for "block/field absent or empty".
                    // Optional `ternary` wrapper (mixfix `?:`). When present it
                    // must be a non-empty string distinct from the other three.
                    const auto nt = wrapperName("ternary");
                    if (wr.contains("ternary")) {
                        if (nt.empty()) {
                            c.emit(DiagnosticCode::C_MissingWrapperRules,
                                   std::format("{}/ternary", wrPath),
                                   "'wrapperRules.ternary' (optional) must be a non-empty "
                                   "string naming the ternary wrapper rule when present");
                        } else if (nt == nb || nt == nu || nt == np) {
                            c.emit(DiagnosticCode::C_DuplicateWrapperRules, wrPath,
                                   std::format("'wrapperRules.ternary' must name a rule distinct "
                                               "from binary/unary/postfix (got '{}')", nt));
                        }
                    }
                    for (auto const& [k, _] : wr.items()) {
                        if (k != "binary" && k != "unary" && k != "postfix" && k != "ternary") {
                            c.emit(DiagnosticCode::C_UnknownShape,
                                   std::format("{}/{}", wrPath, k),
                                   std::format("unknown 'wrapperRules' field '{}' "
                                               "— expected 'binary', 'unary', "
                                               "'postfix', or 'ternary'", k));
                        }
                    }
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
        // Pratt walker synthesizes per-`expr`-shape wrapper-rule
        // frames without a schema shape — the loader's wrapper-rule
        // intern pass populates `data.wrapperRuleIds` from each
        // `expr.wrapperRules` block, and this skip list is keyed on
        // that set. No hardcoded names anywhere in the engine
        // (08.55 cleanup).
        if (data.wrapperRuleIds.contains(rule.v)) continue;
        const auto name = interner.name(rule);
        // Postfix is the only arity that carries `bodyRule`, so iterate
        // just that slice — O(N) in the declared-token count, trivial
        // at load time.
        bool referencedAsBody     = false;
        bool referencedAsFollower = false;
        for (std::uint32_t t = 1;
             t < data.schemaTokens->size()
             && !(referencedAsBody || referencedAsFollower);
             ++t) {
            const SchemaTokenId tid{t};
            const auto entry = data.operators.lookup(tid, OperatorArity::Postfix);
            if (!entry) continue;
            if (entry->grouped && entry->grouped->bodyRule.v == rule.v) {
                referencedAsBody = true;
            }
            // D5.1: `followerRule` also references a rule that must be
            // declared — same audit gap that `bodyRule` had. The loader
            // interns the name at operator-load time (shapes haven't been
            // compiled yet); the real "shape exists" check is here.
            if (entry->followerRule.has_value()
                && entry->followerRule->v == rule.v) {
                referencedAsFollower = true;
            }
        }
        if (referencedAsBody) {
            coll.emit(DiagnosticCode::C_UnknownShape,
                      "/operators/groups",
                      std::format("postfix operator group references "
                                  "'bodyRule' = '{}' but no shape with "
                                  "that name is declared", name));
        }
        if (referencedAsFollower) {
            coll.emit(DiagnosticCode::C_UnknownShape,
                      "/operators/groups",
                      std::format("postfix operator group references "
                                  "'followerRule' = '{}' but no shape with "
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
        return std::unexpected(std::move(coll).release());
    }
    if (!doc.is_object()) {
        coll.emit(DiagnosticCode::C_MalformedJson, std::string{sourceLabel},
                  "top-level value must be a JSON object");
        return std::unexpected(std::move(coll).release());
    }

    // dssSchemaVersion ──
    if (!present(doc, "dssSchemaVersion", coll, std::string{sourceLabel})) {
        return std::unexpected(std::move(coll).release());
    }
    if (!doc.at("dssSchemaVersion").is_number_integer()) {
        coll.emit(DiagnosticCode::C_VersionMismatch, "/dssSchemaVersion",
                  "must be a positive integer");
        return std::unexpected(std::move(coll).release());
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
        return std::unexpected(std::move(coll).release());
    }

    // language ──
    if (!present(doc, "language", coll, std::string{sourceLabel})) {
        return std::unexpected(std::move(coll).release());
    }
    json const& langObj = doc.at("language");
    if (!langObj.is_object()) {
        coll.emit(DiagnosticCode::C_MissingField, "/language", "must be an object");
        return std::unexpected(std::move(coll).release());
    }
    if (!present(langObj, "name",    coll, "/language") ||
        !present(langObj, "version", coll, "/language")) {
        return std::unexpected(std::move(coll).release());
    }

    GrammarSchemaData data;
    data.name          = langObj.at("name").get<std::string>();
    data.version       = langObj.at("version").get<std::string>();
    data.schemaVersion = schemaVer;
    // Per-instance monotonic schema id stamped onto every LexemeMeaning
    // so cross-schema misuse is catchable at the failing lookup.
    data.id            = substrate::mintMonotonicId<SchemaId>();

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
                    // Optional `coalesce` flag (default false): emit ONE
                    // in-grammar token for the whole body instead of one per
                    // codepoint. A value-bearing literal mode (char / string)
                    // sets this so `operand` can reference the body token and
                    // the value can be decoded; comment-style modes leave it
                    // false (per-codepoint, off-grammar, EmptySpace-skipped).
                    if (dt.contains("coalesce")) {
                        if (!dt.at("coalesce").is_boolean()) {
                            coll.emit(DiagnosticCode::C_InvalidSemantics,
                                      std::format("{}/defaultToken/coalesce", modePath),
                                      "'defaultToken.coalesce' must be a boolean");
                        } else {
                            spec.coalesce = dt.at("coalesce").get<bool>();
                        }
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
                    else if (a == "ternary") arity = OperatorArity::Ternary;
                    else {
                        coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, gPath,
                                  std::format("unknown arity '{}' (expected infix|prefix|postfix|ternary)", a));
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
                // D5.1: `followerRule` — a postfix operator followed by
                // exactly one occurrence of a named rule shape (e.g. `.field`
                // is `DotOp` + a `memberFollower` rule). No closer; the rule's
                // own shape terminates the body. Mutually exclusive with the
                // `endsAt`/`bodyRule` (grouped) form — postfix forms are
                // either bracketed-body OR single-rule-follower OR bare (`++`).
                RuleId followerRuleId{};
                if (g.contains("followerRule")) {
                    if (arity != OperatorArity::Postfix) {
                        coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, gPath,
                                  "'followerRule' is only valid on a postfix group");
                        continue;
                    }
                    if (endsAtId.valid() || bodyRuleId.valid()) {
                        coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, gPath,
                                  "'followerRule' is mutually exclusive with "
                                  "'endsAt'/'bodyRule' (grouped postfix)");
                        continue;
                    }
                    if (!g.at("followerRule").is_string()) {
                        coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, gPath,
                                  "'followerRule' must be a string rule name");
                        continue;
                    }
                    const auto rname = g.at("followerRule").get<std::string>();
                    followerRuleId = data.rules->intern(rname);
                }
                // Ternary `middle` separator (C's `:`). Required for ternary
                // arity, forbidden otherwise.
                SchemaTokenId middleId{};
                if (g.contains("middle")) {
                    if (arity != OperatorArity::Ternary) {
                        coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, gPath,
                                  "'middle' is only valid on a ternary group");
                        continue;
                    }
                    if (!g.at("middle").is_string()) {
                        coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, gPath,
                                  "'middle' must be a string lexeme");
                        continue;
                    }
                    const auto mid = g.at("middle").get<std::string>();
                    auto midIt = data.lexemeTable.find(mid);
                    if (midIt == data.lexemeTable.end() || midIt->second.empty()) {
                        coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, gPath,
                                  std::format("'middle' lexeme '{}' is not declared in 'tokens'", mid));
                        continue;
                    }
                    if (midIt->second.size() != 1) {
                        coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, gPath,
                                  std::format("'middle' lexeme '{}' has multiple meanings; "
                                              "ambiguous separators are unsupported", mid));
                        continue;
                    }
                    middleId = midIt->second[0].id;
                }
                if (arity == OperatorArity::Ternary && !middleId.valid()) {
                    coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, gPath,
                              "a ternary group requires a 'middle' separator lexeme");
                    continue;
                }
                // A group is either grouped-postfix (endsAt) OR ternary (middle),
                // never both — the two payloads are mutually exclusive by arity
                // (grouped is postfix-only, middle is ternary-only, each rejected
                // on the wrong arity above), so a single group can't reach here
                // carrying both. Assert to catch a future producer that bypasses
                // those arity gates.
                if (endsAtId.valid() && middleId.valid()) {
                    coll.emit(DiagnosticCode::C_InvalidPrecedenceTable, gPath,
                              "a group cannot be both grouped-postfix ('endsAt') and "
                              "ternary ('middle')");
                    continue;
                }
                OperatorTable::Entry entry{precedence, assoc,
                                           std::nullopt, std::nullopt, std::nullopt};
                if (endsAtId.valid()) {
                    entry.grouped = OperatorTable::GroupedPostfix{
                        endsAtId, bodyRuleId};
                }
                if (followerRuleId.valid()) entry.followerRule = followerRuleId;
                if (middleId.valid()) entry.ternaryMiddle = middleId;

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

    // artifactProfiles ── profiles this language supports (plan 06 AP1;
    // schema v4). Optional; absent ⇒ empty list. Each entry must be a name
    // in the registered profile set (plan 06 §3). AP1 is the schema-field +
    // loader-validation slice only — no codegen/driver consumes it yet.
    // The registered set is the loader-owned vocabulary; new profiles arrive
    // with the backend plan that introduces them (plan 06 §3 "registered set,
    // not compile-time enum"). Listed here as the v1 ship set.
    static constexpr std::string_view kRegisteredArtifactProfiles[] = {
        "cli", "gui", "lib", "staticlib", "script", "sproc",
        "transpile", "shader", "hdl",
    };
    // Single source of truth for the human-readable list in diagnostics —
    // derived from the array above so the two can never drift.
    auto registeredProfileList = [&] {
        std::string out;
        for (auto const& p : kRegisteredArtifactProfiles) {
            if (!out.empty()) out += ", ";
            out += p;
        }
        return out;
    };
    if (doc.contains("artifactProfiles")) {
        json const& ap = doc.at("artifactProfiles");
        if (!ap.is_array()) {
            coll.emit(DiagnosticCode::C_UnknownArtifactProfile, "/artifactProfiles",
                      "'artifactProfiles' must be an array of profile names");
        } else {
            std::unordered_set<std::string> seen;
            for (std::size_t i = 0; i < ap.size(); ++i) {
                const auto path = std::format("/artifactProfiles/{}", i);
                json const& entry = ap[i];
                if (!entry.is_string()) {
                    coll.emit(DiagnosticCode::C_UnknownArtifactProfile, path,
                              "each 'artifactProfiles' entry must be a string");
                    continue;
                }
                auto profile = entry.get<std::string>();
                const bool known = std::any_of(
                    std::begin(kRegisteredArtifactProfiles),
                    std::end(kRegisteredArtifactProfiles),
                    [&](std::string_view p) { return p == profile; });
                if (!known) {
                    coll.emit(DiagnosticCode::C_UnknownArtifactProfile, path,
                              std::format("unknown artifact profile '{}' (registered "
                                          "profiles: {})", profile, registeredProfileList()));
                    continue;
                }
                if (!seen.insert(profile).second) {
                    // A name listed twice in a top-level array — same situation
                    // as a duplicate typeExtensions name; use the same code
                    // (C_ConflictingField) for consistency with that precedent.
                    coll.emit(DiagnosticCode::C_ConflictingField, path,
                              std::format("artifact profile '{}' declared more than once",
                                          profile));
                    continue;
                }
                data.artifactProfiles.push_back(std::move(profile));
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

            // Auto-intern each `expr` shape's declared wrapper rule
            // names (08.55 cleanup; schema v4 `expr.wrapperRules`).
            // Per-`expr`-shape — the engine no longer hardcodes
            // `binaryExpr`/`unaryExpr`/`postfixExpr` anywhere. The
            // walker reads the resulting RuleIds out of the schema
            // (`GrammarSchema::exprWrapperRules(rule)`) and synthesizes
            // wrapper frames around operator-precedence results.
            //
            // Wrapper rules have no compiled body — they're walker-
            // managed and the schema cursor walks transparently past
            // them. To keep the cursor from re-entering through a
            // user-declared rule of the same name, reject any
            // `shapes.<name>` redeclaration of a wrapper rule (a
            // user-defined body for a wrapper name would let the
            // cursor walk through it instead of the walker).
            //
            // Recursive scan: `expr` may be nested inside any
            // sequence/alt/optional/repeat body, not only top-level.
            // A flat scan would miss `{ "sequence": [ { "expr": {...} } ] }`.
            auto walkExprBodies = [](auto const& self,
                                     json const& body,
                                     auto const& sink) -> void {
                if (!body.is_object()) return;
                if (body.contains("expr") && body.at("expr").is_object()) {
                    sink(body.at("expr"));
                }
                for (auto const& key : {"sequence", "alt"}) {
                    if (body.contains(key) && body.at(key).is_array()) {
                        for (auto const& child : body.at(key)) {
                            self(self, child, sink);
                        }
                    }
                }
                for (auto const& key : {"optional", "repeat"}) {
                    if (body.contains(key)) {
                        self(self, body.at(key), sink);
                    }
                }
            };

            // Pass A.1: collect declared wrapper-rule names + reject
            // shape redeclarations of any of them. Done before the
            // intern pass so the shape redeclaration check runs
            // against the union of names actually declared in this
            // schema (one config might use `bExpr`/`uExpr`/`pExpr`,
            // another `binaryExpr`/...).
            std::vector<std::string> wrapperNames;
            for (auto const& [shapeName, body] : doc.at("shapes").items()) {
                (void)shapeName;
                walkExprBodies(walkExprBodies, body,
                               [&](json const& exprBody) {
                    if (!exprBody.contains("wrapperRules")) return;
                    json const& wr = exprBody.at("wrapperRules");
                    if (!wr.is_object()) return;
                    for (auto const* key : {"binary", "unary", "postfix", "ternary"}) {
                        if (wr.contains(key) && wr.at(key).is_string()) {
                            const auto name = wr.at(key).get<std::string>();
                            if (!name.empty()) wrapperNames.push_back(name);
                        }
                    }
                });
            }
            // Dedup + reject shape redeclarations.
            std::sort(wrapperNames.begin(), wrapperNames.end());
            wrapperNames.erase(std::unique(wrapperNames.begin(),
                                           wrapperNames.end()),
                               wrapperNames.end());
            for (auto const& name : wrapperNames) {
                if (doc.at("shapes").contains(name)) {
                    coll.emit(
                        DiagnosticCode::C_UnknownShape,
                        std::format("/shapes/{}", name),
                        std::format(
                            "shape name '{}' is declared as a "
                            "Pratt-walker wrapper rule by an `expr` "
                            "shape's `wrapperRules` — wrapper rules "
                            "are walker-synthesized and cannot have a "
                            "compiled shape body; rename the user "
                            "shape", name));
                }
            }
            // Intern every declared wrapper-rule name; record their
            // RuleIds in `data.wrapperRuleIds` so the shape-existence
            // skip-list (`validateOperatorBodyRules`) reads them out
            // of the schema rather than hardcoding names.
            for (auto const& name : wrapperNames) {
                const auto rid = data.rules->intern(name);
                data.wrapperRuleIds.insert(rid.v);
            }
            // Per-`expr`-rule wrapper-rule resolution. Keyed by the
            // owning expr rule's RuleId so the walker can look up the
            // bundle on entry.
            for (auto const& [shapeName, body] : doc.at("shapes").items()) {
                walkExprBodies(walkExprBodies, body,
                               [&](json const& exprBody) {
                    // Only record when the owning rule is a top-level
                    // shape (the walker dispatch happens per rule, and
                    // every shipped schema declares its `expr` at the
                    // top level of a rule body). The `walkExprBodies`
                    // call site iterates shapes so we know `shapeName`
                    // is the owning rule.
                    if (!exprBody.contains("wrapperRules")) return;
                    json const& wr = exprBody.at("wrapperRules");
                    if (!wr.is_object()) return;
                    ExprWrapperRules pack{};
                    for (auto const* key : {"binary", "unary", "postfix"}) {
                        if (!wr.contains(key) || !wr.at(key).is_string()) return;
                        const auto n = wr.at(key).get<std::string>();
                        if (n.empty()) return;
                        const auto rid = data.rules->intern(n);
                        if (std::string{key} == "binary")       pack.binary  = rid;
                        else if (std::string{key} == "unary")   pack.unary   = rid;
                        else /* postfix */                       pack.postfix = rid;
                    }
                    // Optional ternary wrapper (mixfix `?:`).
                    if (wr.contains("ternary") && wr.at("ternary").is_string()) {
                        const auto n = wr.at("ternary").get<std::string>();
                        if (!n.empty()) pack.ternary = data.rules->intern(n);
                    }
                    const auto ownerRid = data.rules->intern(shapeName);
                    data.exprWrapperRules.emplace(ownerRid.v, pack);
                });
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
                    // A COALESCED body kind is in-grammar (one token, like
                    // IntLiteral) — it must NOT join the off-grammar set, so
                    // that `operand` can reference it and the schema cursor
                    // advances through it normally.
                    if (mode.defaultToken && !mode.defaultToken->coalesce) {
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

    // numberStyle ── numeric-literal lexical grammar (08.55 cleanup;
    // schema v4). Optional; absent for languages that don't lex numeric
    // literals (toy). REQUIRED when any shape references `IntLiteral` or
    // `FloatLiteral` — the tokenizer's scanNumber() drives entirely from
    // this block, so omitting it would leave numeric scanning without
    // rules. Parsed AFTER shapes so the gate ("does any shape reference
    // IntLiteral/FloatLiteral?") has authoritative data.
    {
        // Compute the "language uses numeric literals" gate.
        // Reaches into the compiled shapes' position tables, which are
        // already populated by the buildPositionTables() pass above.
        const auto intLitId   = data.schemaTokens->find("IntLiteral");
        const auto floatLitId = data.schemaTokens->find("FloatLiteral");
        bool usesNumericLiteralTokens = false;
        for (auto const& [ruleIdV, rule] : data.compiledRules) {
            (void)ruleIdV;
            for (auto const& p : rule.positions) {
                if (p.slotKind() != SlotKind::TokenLeaf) continue;
                if (p.tokenId().v == intLitId.v || p.tokenId().v == floatLitId.v) {
                    usesNumericLiteralTokens = true;
                    break;
                }
            }
            if (usesNumericLiteralTokens) break;
        }

        if (doc.contains("numberStyle")) {
            json const& ns = doc.at("numberStyle");
            if (!ns.is_object()) {
                coll.emit(DiagnosticCode::C_InvalidNumberStyle, "/numberStyle",
                          "'numberStyle' must be an object");
            } else {
                NumberStyle style;

                // decimal — optional bool, default false.
                if (ns.contains("decimal")) {
                    if (!ns.at("decimal").is_boolean()) {
                        coll.emit(DiagnosticCode::C_InvalidNumberStyle,
                                  "/numberStyle/decimal",
                                  "'numberStyle.decimal' must be a boolean");
                    } else {
                        style.decimal = ns.at("decimal").get<bool>();
                    }
                }

                // integerPrefixes — optional array of objects.
                if (ns.contains("integerPrefixes")) {
                    if (!ns.at("integerPrefixes").is_array()) {
                        coll.emit(DiagnosticCode::C_InvalidNumberStyle,
                                  "/numberStyle/integerPrefixes",
                                  "'numberStyle.integerPrefixes' must be an array");
                    } else {
                        json const& arr = ns.at("integerPrefixes");
                        for (std::size_t i = 0; i < arr.size(); ++i) {
                            const auto ppath = std::format("/numberStyle/integerPrefixes/{}", i);
                            if (!arr[i].is_object()) {
                                coll.emit(DiagnosticCode::C_InvalidNumberStyle, ppath,
                                          "each integer prefix must be an object with "
                                          "'prefix', 'radix', and 'digits' fields");
                                continue;
                            }
                            NumberPrefix np{};
                            if (!arr[i].contains("prefix") || !arr[i].at("prefix").is_string()
                                || arr[i].at("prefix").get<std::string>().empty()) {
                                coll.emit(DiagnosticCode::C_MissingField,
                                          std::format("{}/prefix", ppath),
                                          "'prefix' must be a non-empty string");
                                continue;
                            }
                            np.prefix = arr[i].at("prefix").get<std::string>();
                            if (!arr[i].contains("radix") || !arr[i].at("radix").is_number_integer()) {
                                coll.emit(DiagnosticCode::C_MissingField,
                                          std::format("{}/radix", ppath),
                                          "'radix' must be an integer in [2,36]");
                                continue;
                            }
                            const auto r = arr[i].at("radix").get<int>();
                            if (r < 2 || r > 36) {
                                coll.emit(DiagnosticCode::C_InvalidNumberStyle,
                                          std::format("{}/radix", ppath),
                                          std::format("radix {} is out of range [2,36]", r));
                                continue;
                            }
                            np.radix = static_cast<std::uint8_t>(r);
                            if (!arr[i].contains("digits") || !arr[i].at("digits").is_string()
                                || arr[i].at("digits").get<std::string>().empty()) {
                                coll.emit(DiagnosticCode::C_MissingField,
                                          std::format("{}/digits", ppath),
                                          "'digits' must be a non-empty character-class "
                                          "string (e.g. \"0-9a-fA-F\")");
                                continue;
                            }
                            np.digits = arr[i].at("digits").get<std::string>();
                            style.integerPrefixes.push_back(std::move(np));
                        }
                    }
                }

                // exponent — optional object.
                if (ns.contains("exponent")) {
                    json const& e = ns.at("exponent");
                    if (!e.is_object()) {
                        coll.emit(DiagnosticCode::C_InvalidNumberStyle,
                                  "/numberStyle/exponent",
                                  "'numberStyle.exponent' must be an object");
                    } else {
                        NumberExponent ne{};
                        if (!e.contains("letters") || !e.at("letters").is_array()
                            || e.at("letters").empty()) {
                            coll.emit(DiagnosticCode::C_MissingField,
                                      "/numberStyle/exponent/letters",
                                      "'exponent.letters' must be a non-empty "
                                      "array of single-character strings");
                        } else {
                            bool ok = true;
                            for (std::size_t i = 0; i < e.at("letters").size(); ++i) {
                                auto const& lj = e.at("letters")[i];
                                if (!lj.is_string() || lj.get<std::string>().size() != 1) {
                                    coll.emit(DiagnosticCode::C_InvalidNumberStyle,
                                              std::format("/numberStyle/exponent/letters/{}", i),
                                              "each exponent letter must be a "
                                              "single-character string");
                                    ok = false;
                                    continue;
                                }
                                ne.letters.push_back(lj.get<std::string>()[0]);
                            }
                            if (ok) {
                                if (e.contains("signOptional")) {
                                    if (!e.at("signOptional").is_boolean()) {
                                        coll.emit(DiagnosticCode::C_InvalidNumberStyle,
                                                  "/numberStyle/exponent/signOptional",
                                                  "'signOptional' must be a boolean");
                                    } else {
                                        ne.signOptional = e.at("signOptional").get<bool>();
                                    }
                                }
                                style.exponent = std::move(ne);
                            }
                        }
                    }
                }

                // fractionPoint — optional single char.
                if (ns.contains("fractionPoint")) {
                    if (!ns.at("fractionPoint").is_string()
                        || ns.at("fractionPoint").get<std::string>().size() != 1) {
                        coll.emit(DiagnosticCode::C_InvalidNumberStyle,
                                  "/numberStyle/fractionPoint",
                                  "'numberStyle.fractionPoint' must be a "
                                  "single-character string");
                    } else {
                        style.fractionPoint = ns.at("fractionPoint").get<std::string>()[0];
                    }
                }

                // digitSeparator — optional single char.
                if (ns.contains("digitSeparator")) {
                    if (!ns.at("digitSeparator").is_string()
                        || ns.at("digitSeparator").get<std::string>().size() != 1) {
                        coll.emit(DiagnosticCode::C_InvalidNumberStyle,
                                  "/numberStyle/digitSeparator",
                                  "'numberStyle.digitSeparator' must be a "
                                  "single-character string");
                    } else {
                        style.digitSeparator = ns.at("digitSeparator").get<std::string>()[0];
                    }
                }

                // integerSuffixes / floatSuffixes — optional string arrays.
                auto readSuffixes = [&](char const* key,
                                        std::vector<std::string>& out) {
                    if (!ns.contains(key)) return;
                    if (!ns.at(key).is_array()) {
                        coll.emit(DiagnosticCode::C_InvalidNumberStyle,
                                  std::format("/numberStyle/{}", key),
                                  std::format("'numberStyle.{}' must be an array of strings",
                                              key));
                        return;
                    }
                    for (std::size_t i = 0; i < ns.at(key).size(); ++i) {
                        auto const& sj = ns.at(key)[i];
                        if (!sj.is_string() || sj.get<std::string>().empty()) {
                            coll.emit(DiagnosticCode::C_InvalidNumberStyle,
                                      std::format("/numberStyle/{}/{}", key, i),
                                      "each suffix must be a non-empty string");
                            continue;
                        }
                        out.push_back(sj.get<std::string>());
                    }
                };
                readSuffixes("integerSuffixes", style.integerSuffixes);
                readSuffixes("floatSuffixes",   style.floatSuffixes);

                // emitKind — required when the block is present.
                if (!ns.contains("emitKind") || !ns.at("emitKind").is_object()) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              "/numberStyle/emitKind",
                              "'numberStyle.emitKind' is required and must be an "
                              "object with 'integer' (and optionally 'float') "
                              "token-kind names");
                } else {
                    json const& ek = ns.at("emitKind");
                    auto readKind = [&](char const* key, SchemaTokenId& out, bool required) {
                        if (!ek.contains(key)) {
                            if (required) {
                                coll.emit(DiagnosticCode::C_MissingField,
                                          std::format("/numberStyle/emitKind/{}", key),
                                          std::format("'emitKind.{}' is required", key));
                            }
                            return;
                        }
                        if (!ek.at(key).is_string()) {
                            coll.emit(DiagnosticCode::C_InvalidNumberStyle,
                                      std::format("/numberStyle/emitKind/{}", key),
                                      std::format("'emitKind.{}' must be a string "
                                                  "naming a token kind", key));
                            return;
                        }
                        const auto n = ek.at(key).get<std::string>();
                        if (!data.schemaTokens->contains(n)) {
                            coll.emit(DiagnosticCode::C_UnknownToken,
                                      std::format("/numberStyle/emitKind/{}", key),
                                      std::format("'emitKind.{}' references unknown "
                                                  "token kind '{}'", key, n));
                            return;
                        }
                        out = data.schemaTokens->find(n);
                    };
                    readKind("integer", style.emitKind.integer, /*required=*/true);
                    // `float` is required iff any float-producing facet is declared.
                    const bool needsFloat =
                        style.exponent.has_value()
                        || style.fractionPoint.has_value()
                        || !style.floatSuffixes.empty();
                    readKind("float", style.emitKind.floating, needsFloat);
                }

                data.numberStyle = std::move(style);
            }
        } else if (usesNumericLiteralTokens) {
            coll.emit(DiagnosticCode::C_MissingNumberStyle, "/numberStyle",
                      "language references 'IntLiteral'/'FloatLiteral' in a "
                      "shape but declares no 'numberStyle' block — the "
                      "tokenizer's numeric scanner is config-driven, so the "
                      "block is required when numeric literal tokens are used");
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

    // semantics ── per-language semantic config (plan 08.6; schema v4).
    // Optional; absent ⇒ analyzer performs no semantic analysis. Parsed
    // LATE, after `shapes`/`tokens` populated the interners, so referenced
    // rule/token names resolve here (`C_UnknownShape`/`C_UnknownToken`) and
    // a loaded schema is guaranteed analyzable.
    if (doc.contains("semantics")) {
        json const& sem = doc.at("semantics");
        if (!sem.is_object()) {
            coll.emit(DiagnosticCode::C_InvalidSemantics, "/semantics",
                      "'semantics' must be an object");
        } else {
            SemanticConfig cfg;

            // Core-kind name → TypeKind (only the subset that makes sense as
            // a source-language built-in type or literal type). Extension
            // kinds are NEVER named here — they live in the registry, not on
            // the core lattice.
            auto const parseCore = [](std::string_view name) -> std::optional<TypeKind> {
                if (name == "Bool")    return TypeKind::Bool;
                if (name == "I8")      return TypeKind::I8;
                if (name == "I16")     return TypeKind::I16;
                if (name == "I32")     return TypeKind::I32;
                if (name == "I64")     return TypeKind::I64;
                if (name == "I128")    return TypeKind::I128;
                if (name == "U8")      return TypeKind::U8;
                if (name == "U16")     return TypeKind::U16;
                if (name == "U32")     return TypeKind::U32;
                if (name == "U64")     return TypeKind::U64;
                if (name == "U128")    return TypeKind::U128;
                if (name == "F16")     return TypeKind::F16;
                if (name == "F32")     return TypeKind::F32;
                if (name == "F64")     return TypeKind::F64;
                if (name == "F128")    return TypeKind::F128;
                if (name == "Char")    return TypeKind::Char;
                if (name == "Byte")    return TypeKind::Byte;
                if (name == "Void")    return TypeKind::Void;
                return std::nullopt;
            };

            auto const parseKind = [](std::string_view name) -> std::optional<DeclarationKind> {
                if (name == "variable") return DeclarationKind::Variable;
                if (name == "function") return DeclarationKind::Function;
                if (name == "table")    return DeclarationKind::Table;
                if (name == "type")     return DeclarationKind::Type;
                return std::nullopt;
            };

            auto const parseConstructor = [](std::string_view name) -> std::optional<TypeConstructor> {
                if (name == "pointer")   return TypeConstructor::Pointer;
                if (name == "reference") return TypeConstructor::Reference;
                if (name == "nullable")  return TypeConstructor::Nullable;
                if (name == "optional")  return TypeConstructor::Optional;
                if (name == "slice")     return TypeConstructor::Slice;
                return std::nullopt;
            };

            auto const parseNameMatch = [](std::string_view name) -> std::optional<NameMatchMode> {
                if (name == "self")           return NameMatchMode::Self;
                if (name == "lastIdentifier") return NameMatchMode::LastIdentifier;
                return std::nullopt;
            };

            // Shared "required, non-negative, in-range, integer" reader for
            // visible-child indices on the new SE4-SE7 facets (assignments,
            // callRules, kindByChild). Out param + boolean ok so the caller
            // can short-circuit when any facet field is malformed without
            // partially-pushing the entry. Mirrors the readIndex closure in
            // the declarations loader but for REQUIRED slots.
            auto const readReqIndex = [&](json const& entry, char const* key,
                                          std::string const& path,
                                          std::uint32_t& out, bool& ok) {
                if (!entry.contains(key)) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              path + "/" + key,
                              std::format("'{}' visible-child index is required",
                                          key));
                    ok = false;
                    return;
                }
                if (!entry.at(key).is_number_integer()) {
                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                              path + "/" + key,
                              std::format("'{}' must be a non-negative integer", key));
                    ok = false;
                    return;
                }
                auto v = entry.at(key).get<std::int64_t>();
                if (v < 0 || v > std::numeric_limits<std::int32_t>::max()) {
                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                              path + "/" + key,
                              std::format("'{}' visible-child index out of range", key));
                    ok = false;
                    return;
                }
                out = static_cast<std::uint32_t>(v);
            };

            // ── declarations ──
            if (sem.contains("declarations")) {
                json const& arr = sem.at("declarations");
                if (!arr.is_array()) {
                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                              "/semantics/declarations",
                              "'semantics.declarations' must be an array");
                } else {
                    for (std::size_t i = 0; i < arr.size(); ++i) {
                        const auto path = std::format("/semantics/declarations/{}", i);
                        json const& entry = arr[i];
                        if (!entry.is_object()) {
                            coll.emit(DiagnosticCode::C_InvalidSemantics, path,
                                      "each 'declarations' entry must be an object");
                            continue;
                        }
                        if (!entry.contains("rule") || !entry.at("rule").is_string()) {
                            coll.emit(DiagnosticCode::C_MissingField, path + "/rule",
                                      "'rule' is required and must be a string");
                            continue;
                        }
                        DeclarationRule rule;
                        rule.ruleName = entry.at("rule").get<std::string>();
                        if (!data.rules->contains(rule.ruleName)) {
                            coll.emit(DiagnosticCode::C_UnknownShape, path + "/rule",
                                      std::format("'declarations[{}].rule' references "
                                                  "unknown shape '{}'", i, rule.ruleName));
                            continue;
                        }
                        rule.rule = data.rules->find(rule.ruleName);

                        auto readIndex = [&](char const* key,
                                             std::optional<std::uint32_t>& out) {
                            if (!entry.contains(key)) return;
                            if (!entry.at(key).is_number_integer()) {
                                coll.emit(DiagnosticCode::C_InvalidSemantics,
                                          path + "/" + key,
                                          std::format("'{}' must be a non-negative integer", key));
                                return;
                            }
                            auto v = entry.at(key).get<std::int64_t>();
                            if (v < 0 || v > std::numeric_limits<std::int32_t>::max()) {
                                coll.emit(DiagnosticCode::C_InvalidSemantics,
                                          path + "/" + key,
                                          std::format("'{}' visible-child index out of range", key));
                                return;
                            }
                            out = static_cast<std::uint32_t>(v);
                        };
                        readIndex("name",   rule.nameChild);
                        readIndex("type",   rule.typeChild);
                        readIndex("init",   rule.initChild);
                        readIndex("params", rule.paramsChild);
                        readIndex("body",   rule.bodyChild);

                        // SE4: optional const-marker token. A bad token
                        // name is C_UnknownToken; the symbol is still
                        // minted (just never marked const).
                        if (entry.contains("constMarker")) {
                            if (!entry.at("constMarker").is_string()) {
                                coll.emit(DiagnosticCode::C_InvalidSemantics,
                                          path + "/constMarker",
                                          "'constMarker' must be a string");
                            } else {
                                auto const cm = entry.at("constMarker").get<std::string>();
                                if (!data.schemaTokens->contains(cm)) {
                                    coll.emit(DiagnosticCode::C_UnknownToken,
                                              path + "/constMarker",
                                              std::format("'declarations[{}].constMarker' "
                                                          "references unknown token kind '{}'",
                                                          i, cm));
                                } else {
                                    rule.constMarker = data.schemaTokens->find(cm);
                                }
                            }
                        }

                        // D-LANG-VARIADIC (step 13.4, 2026-06-02): optional
                        // C-style variadic-marker token. Same shape as
                        // `constMarker` above: a bad token name is
                        // C_UnknownToken; the declaration is still usable
                        // (just never marked variadic). Source-language
                        // agnostic — each language declares its own marker.
                        if (entry.contains("variadicMarker")) {
                            if (!entry.at("variadicMarker").is_string()) {
                                coll.emit(DiagnosticCode::C_InvalidSemantics,
                                          path + "/variadicMarker",
                                          "'variadicMarker' must be a string");
                            } else {
                                auto const vm = entry.at("variadicMarker").get<std::string>();
                                if (!data.schemaTokens->contains(vm)) {
                                    coll.emit(DiagnosticCode::C_UnknownToken,
                                              path + "/variadicMarker",
                                              std::format("'declarations[{}].variadicMarker' "
                                                          "references unknown token kind '{}'",
                                                          i, vm));
                                } else {
                                    rule.variadicMarker = data.schemaTokens->find(vm);
                                }
                            }
                        }

                        // SE-arrays (HR9): optional C-style declarator suffix.
                        //   "arraySuffix": { "rule": "arrayDeclSuffix",
                        //                    "lengthChild": 1 }
                        // `rule` names the suffix shape (matched among the
                        // declaration's visible children); `lengthChild` is the
                        // visible-child index of the length expression inside
                        // that suffix. An unknown rule is C_UnknownShape; the
                        // declaration is still usable (just without arrays).
                        if (entry.contains("arraySuffix")) {
                            json const& as = entry.at("arraySuffix");
                            if (!as.is_object()) {
                                coll.emit(DiagnosticCode::C_InvalidSemantics,
                                          path + "/arraySuffix",
                                          "'arraySuffix' must be an object");
                            } else if (!as.contains("rule") || !as.at("rule").is_string()) {
                                coll.emit(DiagnosticCode::C_MissingField,
                                          path + "/arraySuffix/rule",
                                          "'arraySuffix.rule' is required and must be a "
                                          "rule-name string");
                            } else {
                                auto const rn = as.at("rule").get<std::string>();
                                if (!data.rules->contains(rn)) {
                                    coll.emit(DiagnosticCode::C_UnknownShape,
                                              path + "/arraySuffix/rule",
                                              std::format("'arraySuffix.rule' references "
                                                          "unknown shape '{}'", rn));
                                } else {
                                    ArraySuffix suffix;
                                    suffix.rule     = data.rules->find(rn);
                                    suffix.ruleName = rn;
                                    if (as.contains("lengthChild")) {
                                        auto const& lc = as.at("lengthChild");
                                        if (!lc.is_number_integer()
                                            || lc.get<std::int64_t>() < 0
                                            || lc.get<std::int64_t>()
                                                   > std::numeric_limits<std::int32_t>::max()) {
                                            coll.emit(DiagnosticCode::C_InvalidSemantics,
                                                      path + "/arraySuffix/lengthChild",
                                                      "'lengthChild' must be a non-negative "
                                                      "integer");
                                        } else {
                                            suffix.lengthChild =
                                                static_cast<std::uint32_t>(lc.get<std::int64_t>());
                                        }
                                    }
                                    rule.arraySuffix = std::move(suffix);
                                }
                            }
                        }

                        // D5.1: optional `fieldChildren` descriptor — declares
                        // this declaration as a composite-type introducer.
                        //   "fieldChildren": { "rule": "structField" }
                        // Pass 1.5 collects the named field-rule symbols in the
                        // declaration's scope (in fieldIndex order) and
                        // composes `interner.structType(name, fieldTypes)`.
                        if (entry.contains("fieldChildren")) {
                            json const& fc = entry.at("fieldChildren");
                            if (!fc.is_object()) {
                                coll.emit(DiagnosticCode::C_InvalidSemantics,
                                          path + "/fieldChildren",
                                          "'fieldChildren' must be an object");
                            } else if (!fc.contains("rule") || !fc.at("rule").is_string()) {
                                coll.emit(DiagnosticCode::C_MissingField,
                                          path + "/fieldChildren/rule",
                                          "'fieldChildren.rule' is required and must "
                                          "be a rule-name string");
                            } else {
                                auto const rn = fc.at("rule").get<std::string>();
                                if (!data.rules->contains(rn)) {
                                    coll.emit(DiagnosticCode::C_UnknownShape,
                                              path + "/fieldChildren/rule",
                                              std::format("'fieldChildren.rule' references "
                                                          "unknown shape '{}'", rn));
                                } else if (!fc.contains("compositeKind")) {
                                    // D5.4 gap-closure: `compositeKind`
                                    // is REQUIRED when `fieldChildren` is
                                    // present. Defaulting silently to
                                    // Struct would let a future union-
                                    // or enum-bearing schema mis-type
                                    // its composites with no signal.
                                    coll.emit(DiagnosticCode::C_MissingField,
                                              path + "/fieldChildren/compositeKind",
                                              "'fieldChildren.compositeKind' is required and "
                                              "must be 'struct', 'union' or 'enum' — explicit "
                                              "declaration guards against silently mis-interning "
                                              "a future composite type");
                                } else if (!fc.at("compositeKind").is_string()) {
                                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                                              path + "/fieldChildren/compositeKind",
                                              "'compositeKind' must be a string "
                                              "'struct' or 'union'");
                                } else {
                                    auto const k = fc.at("compositeKind").get<std::string>();
                                    FieldChildrenDescriptor fcd;
                                    fcd.rule     = data.rules->find(rn);
                                    fcd.ruleName = rn;
                                    if (k == "struct") {
                                        fcd.compositeKind = CompositeKind::Struct;
                                    } else if (k == "union") {
                                        fcd.compositeKind = CompositeKind::Union;
                                    } else if (k == "enum") {
                                        fcd.compositeKind = CompositeKind::Enum;
                                    } else {
                                        coll.emit(DiagnosticCode::C_InvalidSemantics,
                                                  path + "/fieldChildren/compositeKind",
                                                  std::format("'compositeKind' must be 'struct', "
                                                              "'union' or 'enum' (got '{}')", k));
                                        // Keep loading; the default Struct
                                        // is the safer fallback for an
                                        // unrecognized value (still emits
                                        // C_InvalidSemantics so res->ok is
                                        // false).
                                    }
                                    // D5.5-FU2: optional `liftToEnclosingScope`
                                    // flag controls C-classic enumerator
                                    // visibility (only meaningful for
                                    // `compositeKind: enum`; ignored
                                    // otherwise but loader-validated).
                                    if (fc.contains("liftToEnclosingScope")) {
                                        if (!fc.at("liftToEnclosingScope").is_boolean()) {
                                            coll.emit(DiagnosticCode::C_InvalidSemantics,
                                                      path + "/fieldChildren/liftToEnclosingScope",
                                                      "'liftToEnclosingScope' must be a boolean");
                                        } else {
                                            fcd.liftToEnclosingScope =
                                                fc.at("liftToEnclosingScope").get<bool>();
                                        }
                                    }
                                    rule.fieldChildren = std::move(fcd);
                                }
                            }
                        }

                        // D8: optional `warnIfUnused` flag (default false).
                        // A non-bool value is the same C_InvalidSemantics
                        // discipline used for other declaration sub-fields.
                        if (entry.contains("warnIfUnused")) {
                            if (!entry.at("warnIfUnused").is_boolean()) {
                                coll.emit(DiagnosticCode::C_InvalidSemantics,
                                          path + "/warnIfUnused",
                                          "'warnIfUnused' must be a boolean");
                            } else {
                                rule.warnIfUnused =
                                    entry.at("warnIfUnused").get<bool>();
                            }
                        }

                        // D-LK10-ENTRY-MAIN-IMPLICIT-RETURN: optional
                        // `implicitReturnZeroForFunctionNames` string-
                        // array. Function declarations whose declared
                        // symbol name appears in this list get a
                        // synthetic `return <zero>` appended to their
                        // body when the body fails to structurally
                        // terminate (C99 §5.1.2.2.3 for `main`).
                        // Source-agnostic: each language declares its
                        // own entry-fn names. Absent / empty → no
                        // implicit insertion for this declaration form.
                        if (entry.contains(
                                "implicitReturnZeroForFunctionNames")) {
                            auto const& arr = entry.at(
                                "implicitReturnZeroForFunctionNames");
                            if (!arr.is_array()) {
                                coll.emit(DiagnosticCode::C_InvalidSemantics,
                                          path + "/implicitReturnZeroForFunctionNames",
                                          "'implicitReturnZeroForFunctionNames' "
                                          "must be an array of strings");
                            } else {
                                rule.implicitReturnZeroForFunctionNames
                                    .reserve(arr.size());
                                for (std::size_t ni = 0; ni < arr.size(); ++ni) {
                                    if (!arr[ni].is_string()) {
                                        coll.emit(DiagnosticCode::C_InvalidSemantics,
                                                  std::format(
                                                      "{}/implicitReturnZeroForFunctionNames/{}",
                                                      path, ni),
                                                  "each entry must be a string");
                                        continue;
                                    }
                                    auto const s =
                                        arr[ni].get<std::string>();
                                    if (s.empty()) {
                                        coll.emit(DiagnosticCode::C_InvalidSemantics,
                                                  std::format(
                                                      "{}/implicitReturnZeroForFunctionNames/{}",
                                                      path, ni),
                                                  "function name must be non-empty");
                                        continue;
                                    }
                                    rule.implicitReturnZeroForFunctionNames
                                        .push_back(s);
                                }
                                // Silent-failure F2 fold (3rd-order
                                // audit on 39897eb): scan for
                                // duplicate names and emit one
                                // C_InvalidSemantics per occurrence.
                                // Functionally idempotent at the
                                // consumer (`std::ranges::find` is
                                // find-first-then-stop), but a paste-
                                // error duplicate in a language config
                                // is a config bug the user wants to
                                // catch at load time — mirrors the
                                // codebase's `kUnsuppressableCodes`
                                // consteval uniqueness invariant for
                                // compile-time tables.
                                std::size_t const n = rule
                                    .implicitReturnZeroForFunctionNames
                                    .size();
                                for (std::size_t a = 0; a < n; ++a) {
                                    for (std::size_t b = a + 1; b < n; ++b) {
                                        if (rule.implicitReturnZeroForFunctionNames[a]
                                         == rule.implicitReturnZeroForFunctionNames[b]) {
                                            coll.emit(
                                                DiagnosticCode::C_InvalidSemantics,
                                                std::format(
                                                    "{}/implicitReturnZeroForFunctionNames/{}",
                                                    path, b),
                                                std::format(
                                                    "duplicate function name '{}' "
                                                    "(already declared at index {})",
                                                    rule.implicitReturnZeroForFunctionNames[a],
                                                    a));
                                        }
                                    }
                                }
                            }
                        }

                        if (entry.contains("kind")) {
                            if (!entry.at("kind").is_string()) {
                                coll.emit(DiagnosticCode::C_InvalidSemantics,
                                          path + "/kind",
                                          "'kind' must be a string");
                            } else {
                                auto k = parseKind(entry.at("kind").get<std::string>());
                                if (!k) {
                                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                                              path + "/kind",
                                              std::format("unknown declaration kind '{}' "
                                                          "(expected 'variable', 'function', "
                                                          "'table', or 'type')",
                                                          entry.at("kind").get<std::string>()));
                                } else {
                                    rule.kind = *k;
                                }
                            }
                        }
                        if (entry.contains("nameMatch")) {
                            if (!entry.at("nameMatch").is_string()) {
                                coll.emit(DiagnosticCode::C_InvalidSemantics,
                                          path + "/nameMatch",
                                          "'nameMatch' must be a string");
                            } else {
                                auto m = parseNameMatch(entry.at("nameMatch").get<std::string>());
                                if (!m) {
                                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                                              path + "/nameMatch",
                                              std::format("unknown nameMatch '{}' (expected "
                                                          "'self' or 'lastIdentifier')",
                                                          entry.at("nameMatch").get<std::string>()));
                                } else {
                                    rule.nameMatch = *m;
                                }
                            }
                        }

                        // kindByChild — optional kind-discriminator. Lets a
                        // single declaration shape decide its effective
                        // `kind` by inspecting a child sub-rule. Schema:
                        //   { "child": <int>, "whenRule": "<ruleName>",
                        //     "whenKind": "<kind>",
                        //     "paramsPath": [n,m,...]?,
                        //     "bodyPath":   [n,m,...]? }
                        if (entry.contains("kindByChild")) {
                            json const& k = entry.at("kindByChild");
                            const auto kPath = path + "/kindByChild";
                            if (!k.is_object()) {
                                coll.emit(DiagnosticCode::C_InvalidSemantics, kPath,
                                          "'kindByChild' must be an object");
                            } else {
                                KindDiscriminator disc;
                                bool dOk = true;
                                // Accept EITHER a single `child` int OR a
                                // `childPath` array; presence of both is a
                                // config bug. At least one is required so
                                // the discriminator has something to
                                // descend.
                                const bool hasChild     = k.contains("child");
                                const bool hasChildPath = k.contains("childPath");
                                if (hasChild && hasChildPath) {
                                    coll.emit(DiagnosticCode::C_ConflictingField,
                                              kPath,
                                              "specify either 'child' (single index) "
                                              "or 'childPath' (array), not both");
                                    dOk = false;
                                } else if (!hasChild && !hasChildPath) {
                                    coll.emit(DiagnosticCode::C_MissingField,
                                              kPath,
                                              "'kindByChild' requires 'child' (single "
                                              "index) or 'childPath' (array of indices)");
                                    dOk = false;
                                } else if (hasChild) {
                                    std::uint32_t idx = 0;
                                    readReqIndex(k, "child", kPath, idx, dOk);
                                    if (dOk) disc.childPath.push_back(idx);
                                } else {
                                    json const& cp = k.at("childPath");
                                    if (!cp.is_array()) {
                                        coll.emit(DiagnosticCode::C_InvalidSemantics,
                                                  kPath + "/childPath",
                                                  "'childPath' must be an array of "
                                                  "non-negative integers");
                                        dOk = false;
                                    } else {
                                        for (auto const& e : cp) {
                                            if (!e.is_number_integer()) {
                                                coll.emit(DiagnosticCode::C_InvalidSemantics,
                                                          kPath + "/childPath",
                                                          "each 'childPath' entry must "
                                                          "be a non-negative integer");
                                                dOk = false;
                                                break;
                                            }
                                            auto v = e.get<std::int64_t>();
                                            if (v < 0
                                                || v > std::numeric_limits<std::int32_t>::max()) {
                                                coll.emit(DiagnosticCode::C_InvalidSemantics,
                                                          kPath + "/childPath",
                                                          "'childPath' entry out of range");
                                                dOk = false;
                                                break;
                                            }
                                            disc.childPath.push_back(
                                                static_cast<std::uint32_t>(v));
                                        }
                                    }
                                }
                                if (!k.contains("whenRule")
                                    || !k.at("whenRule").is_string()) {
                                    coll.emit(DiagnosticCode::C_MissingField,
                                              kPath + "/whenRule",
                                              "'whenRule' is required and must be a string");
                                    dOk = false;
                                } else {
                                    disc.whenRuleName =
                                        k.at("whenRule").get<std::string>();
                                    if (!data.rules->contains(disc.whenRuleName)) {
                                        coll.emit(DiagnosticCode::C_UnknownShape,
                                                  kPath + "/whenRule",
                                                  std::format("'kindByChild.whenRule' "
                                                              "references unknown shape '{}'",
                                                              disc.whenRuleName));
                                        dOk = false;
                                    } else {
                                        disc.whenRule = data.rules->find(disc.whenRuleName);
                                    }
                                }
                                if (k.contains("whenKind")) {
                                    if (!k.at("whenKind").is_string()) {
                                        coll.emit(DiagnosticCode::C_InvalidSemantics,
                                                  kPath + "/whenKind",
                                                  "'whenKind' must be a string");
                                        dOk = false;
                                    } else {
                                        auto wk = parseKind(
                                            k.at("whenKind").get<std::string>());
                                        if (!wk) {
                                            coll.emit(DiagnosticCode::C_InvalidSemantics,
                                                      kPath + "/whenKind",
                                                      std::format("unknown declaration "
                                                                  "kind '{}' (expected "
                                                                  "'variable', 'function', "
                                                                  "'table', or 'type')",
                                                                  k.at("whenKind").get<std::string>()));
                                            dOk = false;
                                        } else {
                                            disc.whenKind = *wk;
                                        }
                                    }
                                }
                                auto readPath = [&](char const* key,
                                                    std::vector<std::uint32_t>& out) {
                                    if (!k.contains(key)) return;
                                    if (!k.at(key).is_array()) {
                                        coll.emit(DiagnosticCode::C_InvalidSemantics,
                                                  kPath + "/" + key,
                                                  std::format("'{}' must be an array of "
                                                              "non-negative integers", key));
                                        dOk = false;
                                        return;
                                    }
                                    for (auto const& e : k.at(key)) {
                                        if (!e.is_number_integer()) {
                                            coll.emit(DiagnosticCode::C_InvalidSemantics,
                                                      kPath + "/" + key,
                                                      std::format("each '{}' entry must "
                                                                  "be a non-negative integer",
                                                                  key));
                                            dOk = false;
                                            return;
                                        }
                                        auto v = e.get<std::int64_t>();
                                        if (v < 0
                                            || v > std::numeric_limits<std::int32_t>::max()) {
                                            coll.emit(DiagnosticCode::C_InvalidSemantics,
                                                      kPath + "/" + key,
                                                      std::format("'{}' entry out of range",
                                                                  key));
                                            dOk = false;
                                            return;
                                        }
                                        out.push_back(static_cast<std::uint32_t>(v));
                                    }
                                };
                                readPath("paramsPath", disc.paramsPath);
                                readPath("bodyPath",   disc.bodyPath);
                                if (dOk) {
                                    rule.kindByChild = std::move(disc);
                                }
                            }
                        }

                        cfg.declarations.push_back(std::move(rule));
                    }
                }
            }

            // ── references ──
            if (sem.contains("references")) {
                json const& arr = sem.at("references");
                if (!arr.is_array()) {
                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                              "/semantics/references",
                              "'semantics.references' must be an array");
                } else {
                    for (std::size_t i = 0; i < arr.size(); ++i) {
                        const auto path = std::format("/semantics/references/{}", i);
                        json const& entry = arr[i];
                        if (!entry.is_object()) {
                            coll.emit(DiagnosticCode::C_InvalidSemantics, path,
                                      "each 'references' entry must be an object");
                            continue;
                        }
                        if (!entry.contains("rule") || !entry.at("rule").is_string()) {
                            coll.emit(DiagnosticCode::C_MissingField, path + "/rule",
                                      "'rule' is required and must be a string");
                            continue;
                        }
                        ReferenceRule rule;
                        rule.ruleName = entry.at("rule").get<std::string>();
                        if (!data.rules->contains(rule.ruleName)) {
                            coll.emit(DiagnosticCode::C_UnknownShape, path + "/rule",
                                      std::format("'references[{}].rule' references "
                                                  "unknown shape '{}'", i, rule.ruleName));
                            continue;
                        }
                        rule.rule = data.rules->find(rule.ruleName);
                        if (entry.contains("nameMatch")) {
                            if (!entry.at("nameMatch").is_string()) {
                                coll.emit(DiagnosticCode::C_InvalidSemantics,
                                          path + "/nameMatch",
                                          "'nameMatch' must be a string");
                            } else {
                                auto m = parseNameMatch(entry.at("nameMatch").get<std::string>());
                                if (!m) {
                                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                                              path + "/nameMatch",
                                              std::format("unknown nameMatch '{}' (expected "
                                                          "'self' or 'lastIdentifier')",
                                                          entry.at("nameMatch").get<std::string>()));
                                } else {
                                    rule.nameMatch = *m;
                                }
                            }
                        }
                        if (entry.contains("hardParents")) {
                            json const& hp = entry.at("hardParents");
                            if (!hp.is_array()) {
                                coll.emit(DiagnosticCode::C_InvalidSemantics,
                                          path + "/hardParents",
                                          "'hardParents' must be an array of rule names");
                            } else {
                                for (std::size_t k = 0; k < hp.size(); ++k) {
                                    if (!hp[k].is_string()) {
                                        coll.emit(DiagnosticCode::C_InvalidSemantics,
                                                  std::format("{}/hardParents/{}", path, k),
                                                  "each 'hardParents' entry must be a string");
                                        continue;
                                    }
                                    auto name = hp[k].get<std::string>();
                                    if (!data.rules->contains(name)) {
                                        coll.emit(DiagnosticCode::C_UnknownShape,
                                                  std::format("{}/hardParents/{}", path, k),
                                                  std::format("'hardParents' references unknown "
                                                              "shape '{}'", name));
                                        continue;
                                    }
                                    rule.hardParents.push_back(data.rules->find(name));
                                    rule.hardParentNames.push_back(std::move(name));
                                }
                            }
                        }
                        cfg.references.push_back(std::move(rule));
                    }
                }
            }

            // ── memberAccesses (D5.1) ──
            // Each entry names a member-access rule (`obj.field` or
            // `ptr->field`), the visible-child indices of LHS + name, and
            // whether the form dereferences first. The engine reads only
            // these — there is no per-language member-access C++ branch.
            if (sem.contains("memberAccesses")) {
                json const& arr = sem.at("memberAccesses");
                if (!arr.is_array()) {
                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                              "/semantics/memberAccesses",
                              "'semantics.memberAccesses' must be an array");
                } else {
                    for (std::size_t i = 0; i < arr.size(); ++i) {
                        const auto path = std::format("/semantics/memberAccesses/{}", i);
                        json const& entry = arr[i];
                        if (!entry.is_object()) {
                            coll.emit(DiagnosticCode::C_InvalidSemantics, path,
                                      "each 'memberAccesses' entry must be an object");
                            continue;
                        }
                        if (!entry.contains("rule") || !entry.at("rule").is_string()) {
                            coll.emit(DiagnosticCode::C_MissingField, path + "/rule",
                                      "'rule' is required and must be a string");
                            continue;
                        }
                        MemberAccessRule rule;
                        rule.ruleName = entry.at("rule").get<std::string>();
                        if (!data.rules->contains(rule.ruleName)) {
                            coll.emit(DiagnosticCode::C_UnknownShape, path + "/rule",
                                      std::format("'memberAccesses[{}].rule' references "
                                                  "unknown shape '{}'", i, rule.ruleName));
                            continue;
                        }
                        rule.rule = data.rules->find(rule.ruleName);
                        auto readReqUint = [&](char const* key, std::uint32_t& out) -> bool {
                            if (!entry.contains(key) || !entry.at(key).is_number_integer()) {
                                coll.emit(DiagnosticCode::C_MissingField,
                                          path + "/" + key,
                                          std::format("'{}' is required and must be a "
                                                      "non-negative integer", key));
                                return false;
                            }
                            auto v = entry.at(key).get<std::int64_t>();
                            if (v < 0 || v > std::numeric_limits<std::int32_t>::max()) {
                                coll.emit(DiagnosticCode::C_InvalidSemantics,
                                          path + "/" + key,
                                          std::format("'{}' visible-child index out of range", key));
                                return false;
                            }
                            out = static_cast<std::uint32_t>(v);
                            return true;
                        };
                        bool ok = readReqUint("lhsChild",  rule.lhsChild)
                              &&  readReqUint("nameChild", rule.nameChild);
                        if (!ok) continue;
                        if (rule.lhsChild == rule.nameChild) {
                            coll.emit(DiagnosticCode::C_InvalidSemantics,
                                      path,
                                      std::format("'memberAccesses[{}]': "
                                                  "lhsChild and nameChild must "
                                                  "be distinct visible-child "
                                                  "indices (got {})",
                                                  i, rule.lhsChild));
                            continue;
                        }
                        if (entry.contains("dereferences")) {
                            if (!entry.at("dereferences").is_boolean()) {
                                coll.emit(DiagnosticCode::C_InvalidSemantics,
                                          path + "/dereferences",
                                          "'dereferences' must be a boolean");
                            } else {
                                rule.dereferences = entry.at("dereferences").get<bool>();
                            }
                        }
                        // Optional gating token kind. Same discipline as
                        // `AssignmentRule.operatorToken`: multiple entries
                        // sharing a `rule` are distinguished by which operator
                        // token (`.` vs `->`) appears in the node.
                        if (entry.contains("operatorToken")) {
                            if (!entry.at("operatorToken").is_string()) {
                                coll.emit(DiagnosticCode::C_InvalidSemantics,
                                          path + "/operatorToken",
                                          "'operatorToken' must be a string");
                            } else {
                                auto const tk = entry.at("operatorToken").get<std::string>();
                                if (!data.schemaTokens->contains(tk)) {
                                    coll.emit(DiagnosticCode::C_UnknownToken,
                                              path + "/operatorToken",
                                              std::format("'memberAccesses[{}].operatorToken' "
                                                          "references unknown token kind '{}'",
                                                          i, tk));
                                } else {
                                    rule.operatorToken = data.schemaTokens->find(tk);
                                }
                            }
                        }
                        cfg.memberAccesses.push_back(std::move(rule));
                    }
                }
            }

            // ── scopes ──
            if (sem.contains("scopes")) {
                json const& arr = sem.at("scopes");
                if (!arr.is_array()) {
                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                              "/semantics/scopes",
                              "'semantics.scopes' must be an array of rule names");
                } else {
                    for (std::size_t i = 0; i < arr.size(); ++i) {
                        const auto path = std::format("/semantics/scopes/{}", i);
                        if (!arr[i].is_string()) {
                            coll.emit(DiagnosticCode::C_InvalidSemantics, path,
                                      "each 'scopes' entry must be a string");
                            continue;
                        }
                        auto const name = arr[i].get<std::string>();
                        if (!data.rules->contains(name)) {
                            coll.emit(DiagnosticCode::C_UnknownShape, path,
                                      std::format("'scopes[{}]' references unknown shape '{}'",
                                                  i, name));
                            continue;
                        }
                        ScopeRule scopeRule;
                        scopeRule.rule     = data.rules->find(name);
                        scopeRule.ruleName = name;
                        cfg.scopes.push_back(std::move(scopeRule));
                    }
                }
            }

            // ── D5.1: cross-field validation for `fieldChildren` ──
            // The header docstring promises that a declaration carrying
            // `fieldChildren` must (a) also appear in `scopes` (so fields
            // bind into the struct's own scope, not the enclosing one),
            // and (b) declare `kind: type`. Pass 1 silently no-ops a
            // misconfigured fieldChildren (via the `here != current` gate)
            // and Pass 1.5 silently leaves the struct type unresolved —
            // exactly the kind of "documented invariant, unenforced"
            // anti-pattern the project rejects. Validate here, before
            // analysis ever runs.
            for (std::size_t i = 0; i < cfg.declarations.size(); ++i) {
                auto const& d = cfg.declarations[i];
                if (!d.fieldChildren.has_value()) continue;
                const auto path = std::format("/semantics/declarations/{}", i);
                if (d.kind != DeclarationKind::Type) {
                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                              path + "/fieldChildren",
                              std::format("declaration '{}' carries "
                                          "'fieldChildren' but its 'kind' is "
                                          "not 'type' — a composite-type "
                                          "introducer must declare kind:type",
                                          d.ruleName));
                }
                bool inScopes = false;
                for (auto const& sc : cfg.scopes) {
                    if (sc.rule.v == d.rule.v) { inScopes = true; break; }
                }
                if (!inScopes) {
                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                              path + "/fieldChildren",
                              std::format("declaration '{}' carries "
                                          "'fieldChildren' but its rule is "
                                          "not in 'scopes' — fields must bind "
                                          "into the struct's own scope",
                                          d.ruleName));
                }
            }

            // ── builtinTypes ──
            if (sem.contains("builtinTypes")) {
                json const& arr = sem.at("builtinTypes");
                if (!arr.is_array()) {
                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                              "/semantics/builtinTypes",
                              "'semantics.builtinTypes' must be an array");
                } else {
                    for (std::size_t i = 0; i < arr.size(); ++i) {
                        const auto path = std::format("/semantics/builtinTypes/{}", i);
                        json const& entry = arr[i];
                        if (!entry.is_object()) {
                            coll.emit(DiagnosticCode::C_InvalidSemantics, path,
                                      "each 'builtinTypes' entry must be an object");
                            continue;
                        }
                        if (!entry.contains("name") || !entry.at("name").is_string()) {
                            coll.emit(DiagnosticCode::C_MissingField, path + "/name",
                                      "'name' is required and must be a string");
                            continue;
                        }
                        // A mapping resolves to EITHER a core primitive
                        // (`core`) OR a registered type-extension
                        // (`extension`). Exactly one must be present.
                        const bool hasCore = entry.contains("core");
                        const bool hasExt  = entry.contains("extension");
                        if (hasCore && hasExt) {
                            coll.emit(DiagnosticCode::C_ConflictingField, path,
                                      "'builtinTypes' entry must specify either 'core' "
                                      "or 'extension', not both");
                            continue;
                        }
                        if (!hasCore && !hasExt) {
                            coll.emit(DiagnosticCode::C_MissingField, path + "/core",
                                      "'builtinTypes' entry requires 'core' (a core "
                                      "TypeKind) or 'extension' (a typeExtensions name)");
                            continue;
                        }
                        BuiltinTypeMapping m;
                        m.name = entry.at("name").get<std::string>();
                        if (hasExt) {
                            if (!entry.at("extension").is_string()) {
                                coll.emit(DiagnosticCode::C_InvalidSemantics,
                                          path + "/extension",
                                          "'extension' must be a string");
                                continue;
                            }
                            auto extName = entry.at("extension").get<std::string>();
                            // The extension must be declared in the top-level
                            // `typeExtensions[]` block (parsed earlier).
                            const bool declared = std::any_of(
                                data.typeExtensions.begin(), data.typeExtensions.end(),
                                [&](TypeExtensionDescriptor const& d) {
                                    return d.name == extName;
                                });
                            if (!declared) {
                                coll.emit(DiagnosticCode::C_UnknownTypeExtension,
                                          path + "/extension",
                                          std::format("'builtinTypes[{}].extension' references "
                                                      "type extension '{}' not declared in "
                                                      "'typeExtensions'", i, extName));
                                continue;
                            }
                            m.extension = std::move(extName);
                            cfg.builtinTypes.push_back(std::move(m));
                            continue;
                        }
                        if (!entry.at("core").is_string()) {
                            coll.emit(DiagnosticCode::C_MissingField, path + "/core",
                                      "'core' is required and must be a string");
                            continue;
                        }
                        auto k = parseCore(entry.at("core").get<std::string>());
                        if (!k) {
                            coll.emit(DiagnosticCode::C_InvalidSemantics, path + "/core",
                                      std::format("unknown core TypeKind '{}' (expected one "
                                                  "of Bool/I*/U*/F*/Char/Byte/Void)",
                                                  entry.at("core").get<std::string>()));
                            continue;
                        }
                        m.core = *k;
                        cfg.builtinTypes.push_back(std::move(m));
                    }
                }
            }

            // ── typeShapes ──
            if (sem.contains("typeShapes")) {
                json const& arr = sem.at("typeShapes");
                if (!arr.is_array()) {
                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                              "/semantics/typeShapes",
                              "'semantics.typeShapes' must be an array");
                } else {
                    for (std::size_t i = 0; i < arr.size(); ++i) {
                        const auto path = std::format("/semantics/typeShapes/{}", i);
                        json const& entry = arr[i];
                        if (!entry.is_object()) {
                            coll.emit(DiagnosticCode::C_InvalidSemantics, path,
                                      "each 'typeShapes' entry must be an object");
                            continue;
                        }
                        if (!entry.contains("rule") || !entry.at("rule").is_string()) {
                            coll.emit(DiagnosticCode::C_MissingField, path + "/rule",
                                      "'rule' is required and must be a string");
                            continue;
                        }
                        if (!entry.contains("constructor") || !entry.at("constructor").is_string()) {
                            coll.emit(DiagnosticCode::C_MissingField, path + "/constructor",
                                      "'constructor' is required and must be a string");
                            continue;
                        }
                        TypeShapeRule rule;
                        rule.ruleName = entry.at("rule").get<std::string>();
                        if (!data.rules->contains(rule.ruleName)) {
                            coll.emit(DiagnosticCode::C_UnknownShape, path + "/rule",
                                      std::format("'typeShapes[{}].rule' references "
                                                  "unknown shape '{}'", i, rule.ruleName));
                            continue;
                        }
                        rule.rule = data.rules->find(rule.ruleName);
                        auto c = parseConstructor(entry.at("constructor").get<std::string>());
                        if (!c) {
                            coll.emit(DiagnosticCode::C_InvalidSemantics, path + "/constructor",
                                      std::format("unknown type constructor '{}' (expected "
                                                  "'pointer', 'reference', 'nullable', "
                                                  "'optional', or 'slice')",
                                                  entry.at("constructor").get<std::string>()));
                            continue;
                        }
                        rule.constructor = *c;
                        if (entry.contains("operandChild")) {
                            if (!entry.at("operandChild").is_number_integer()) {
                                coll.emit(DiagnosticCode::C_InvalidSemantics,
                                          path + "/operandChild",
                                          "'operandChild' must be a non-negative integer");
                                continue;
                            }
                            auto v = entry.at("operandChild").get<std::int64_t>();
                            if (v < 0 || v > std::numeric_limits<std::int32_t>::max()) {
                                coll.emit(DiagnosticCode::C_InvalidSemantics,
                                          path + "/operandChild",
                                          "'operandChild' visible-child index out of range");
                                continue;
                            }
                            rule.operandChild = static_cast<std::int32_t>(v);
                        }
                        cfg.typeShapes.push_back(std::move(rule));
                    }
                }
            }

            // ── literalTypes ──
            if (sem.contains("literalTypes")) {
                json const& arr = sem.at("literalTypes");
                if (!arr.is_array()) {
                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                              "/semantics/literalTypes",
                              "'semantics.literalTypes' must be an array");
                } else {
                    for (std::size_t i = 0; i < arr.size(); ++i) {
                        const auto path = std::format("/semantics/literalTypes/{}", i);
                        json const& entry = arr[i];
                        if (!entry.is_object()) {
                            coll.emit(DiagnosticCode::C_InvalidSemantics, path,
                                      "each 'literalTypes' entry must be an object");
                            continue;
                        }
                        if (!entry.contains("literal") || !entry.at("literal").is_string()) {
                            coll.emit(DiagnosticCode::C_MissingField, path + "/literal",
                                      "'literal' is required and must be a string");
                            continue;
                        }
                        if (!entry.contains("core") || !entry.at("core").is_string()) {
                            coll.emit(DiagnosticCode::C_MissingField, path + "/core",
                                      "'core' is required and must be a string");
                            continue;
                        }
                        LiteralTypeMapping m;
                        m.literalName = entry.at("literal").get<std::string>();
                        if (!data.schemaTokens->contains(m.literalName)) {
                            coll.emit(DiagnosticCode::C_UnknownToken, path + "/literal",
                                      std::format("'literalTypes[{}].literal' references "
                                                  "unknown token kind '{}'", i, m.literalName));
                            continue;
                        }
                        m.literal = data.schemaTokens->find(m.literalName);
                        auto k = parseCore(entry.at("core").get<std::string>());
                        if (!k) {
                            coll.emit(DiagnosticCode::C_InvalidSemantics, path + "/core",
                                      std::format("unknown core TypeKind '{}'",
                                                  entry.at("core").get<std::string>()));
                            continue;
                        }
                        m.core = *k;
                        cfg.literalTypes.push_back(std::move(m));
                    }
                }
            }

            // ── assignments (SE4 const-correctness) ──
            // A rule whose match is an assignment. `lhs`/`rhs` are
            // required visible-child indices; `operatorToken` is an
            // optional gating token kind (for operator-table rules reused
            // across every binary operator).
            if (sem.contains("assignments")) {
                json const& arr = sem.at("assignments");
                if (!arr.is_array()) {
                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                              "/semantics/assignments",
                              "'semantics.assignments' must be an array");
                } else {
                    for (std::size_t i = 0; i < arr.size(); ++i) {
                        const auto path = std::format("/semantics/assignments/{}", i);
                        json const& entry = arr[i];
                        if (!entry.is_object()) {
                            coll.emit(DiagnosticCode::C_InvalidSemantics, path,
                                      "each 'assignments' entry must be an object");
                            continue;
                        }
                        if (!entry.contains("rule") || !entry.at("rule").is_string()) {
                            coll.emit(DiagnosticCode::C_MissingField, path + "/rule",
                                      "'rule' is required and must be a string");
                            continue;
                        }
                        AssignmentRule rule;
                        rule.ruleName = entry.at("rule").get<std::string>();
                        if (!data.rules->contains(rule.ruleName)) {
                            coll.emit(DiagnosticCode::C_UnknownShape, path + "/rule",
                                      std::format("'assignments[{}].rule' references "
                                                  "unknown shape '{}'", i, rule.ruleName));
                            continue;
                        }
                        rule.rule = data.rules->find(rule.ruleName);

                        bool ok = true;
                        readReqIndex(entry, "lhs", path, rule.lhsChild, ok);
                        readReqIndex(entry, "rhs", path, rule.rhsChild, ok);
                        if (!ok) continue;

                        if (entry.contains("operatorToken")) {
                            if (!entry.at("operatorToken").is_string()) {
                                coll.emit(DiagnosticCode::C_InvalidSemantics,
                                          path + "/operatorToken",
                                          "'operatorToken' must be a string");
                            } else {
                                auto const ot = entry.at("operatorToken").get<std::string>();
                                if (!data.schemaTokens->contains(ot)) {
                                    coll.emit(DiagnosticCode::C_UnknownToken,
                                              path + "/operatorToken",
                                              std::format("'assignments[{}].operatorToken' "
                                                          "references unknown token kind '{}'",
                                                          i, ot));
                                } else {
                                    rule.operatorToken = data.schemaTokens->find(ot);
                                }
                            }
                        }
                        cfg.assignments.push_back(std::move(rule));
                    }
                    // Invariant (see AssignmentRule doc): an ungated entry (no
                    // operatorToken) matches every node of its rule, so it must
                    // be the SOLE entry for that rule. Reject a config that
                    // mixes an ungated entry with other entries on the same
                    // rule — otherwise the first-match-wins engine loop would
                    // silently let the catch-all shadow the gated entries.
                    for (std::size_t i = 0; i < cfg.assignments.size(); ++i) {
                        if (cfg.assignments[i].operatorToken.has_value()) continue;
                        bool shared = false;
                        for (std::size_t j = 0; j < cfg.assignments.size(); ++j) {
                            if (j != i &&
                                cfg.assignments[j].rule.v == cfg.assignments[i].rule.v) {
                                shared = true;
                                break;
                            }
                        }
                        if (shared) {
                            coll.emit(DiagnosticCode::C_ConflictingField,
                                      "/semantics/assignments",
                                      std::format("assignment rule '{}' mixes an ungated "
                                                  "entry (no operatorToken) with other "
                                                  "entries; an ungated entry must be the "
                                                  "sole entry for its rule",
                                                  cfg.assignments[i].ruleName));
                            break;
                        }
                    }
                }
            }

            // ── callRules (SE6 call checking) ──
            if (sem.contains("callRules")) {
                json const& arr = sem.at("callRules");
                if (!arr.is_array()) {
                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                              "/semantics/callRules",
                              "'semantics.callRules' must be an array");
                } else {
                    for (std::size_t i = 0; i < arr.size(); ++i) {
                        const auto path = std::format("/semantics/callRules/{}", i);
                        json const& entry = arr[i];
                        if (!entry.is_object()) {
                            coll.emit(DiagnosticCode::C_InvalidSemantics, path,
                                      "each 'callRules' entry must be an object");
                            continue;
                        }
                        if (!entry.contains("rule") || !entry.at("rule").is_string()) {
                            coll.emit(DiagnosticCode::C_MissingField, path + "/rule",
                                      "'rule' is required and must be a string");
                            continue;
                        }
                        CallRule rule;
                        rule.ruleName = entry.at("rule").get<std::string>();
                        if (!data.rules->contains(rule.ruleName)) {
                            coll.emit(DiagnosticCode::C_UnknownShape, path + "/rule",
                                      std::format("'callRules[{}].rule' references "
                                                  "unknown shape '{}'", i, rule.ruleName));
                            continue;
                        }
                        rule.rule = data.rules->find(rule.ruleName);

                        bool ok = true;
                        readReqIndex(entry, "callee", path, rule.calleeChild, ok);
                        readReqIndex(entry, "args",   path, rule.argsChild,   ok);
                        if (!ok) continue;

                        // Optional `operatorToken` gate. Mirrors
                        // AssignmentRule's gating — needed when a single
                        // shape (e.g. c-subset's `postfixExpr`) covers
                        // call AND non-call postfix forms.
                        if (entry.contains("operatorToken")) {
                            if (!entry.at("operatorToken").is_string()) {
                                coll.emit(DiagnosticCode::C_InvalidSemantics,
                                          path + "/operatorToken",
                                          "'operatorToken' must be a string");
                            } else {
                                auto const ot = entry.at("operatorToken").get<std::string>();
                                if (!data.schemaTokens->contains(ot)) {
                                    coll.emit(DiagnosticCode::C_UnknownToken,
                                              path + "/operatorToken",
                                              std::format("'callRules[{}].operatorToken' "
                                                          "references unknown token kind '{}'",
                                                          i, ot));
                                } else {
                                    rule.operatorToken = data.schemaTokens->find(ot);
                                }
                            }
                        }
                        cfg.callRules.push_back(std::move(rule));
                    }
                }
            }

            // ── builtinFunctions (SE6) ──
            // A named function the engine interns + binds into a CU-wide
            // builtins scope. `params` is an array of core-kind names;
            // `result` is a single core-kind name; `variadic` (optional)
            // makes the call-check skip arg-count enforcement.
            if (sem.contains("builtinFunctions")) {
                json const& arr = sem.at("builtinFunctions");
                if (!arr.is_array()) {
                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                              "/semantics/builtinFunctions",
                              "'semantics.builtinFunctions' must be an array");
                } else {
                    for (std::size_t i = 0; i < arr.size(); ++i) {
                        const auto path = std::format("/semantics/builtinFunctions/{}", i);
                        json const& entry = arr[i];
                        if (!entry.is_object()) {
                            coll.emit(DiagnosticCode::C_InvalidSemantics, path,
                                      "each 'builtinFunctions' entry must be an object");
                            continue;
                        }
                        if (!entry.contains("name") || !entry.at("name").is_string()) {
                            coll.emit(DiagnosticCode::C_MissingField, path + "/name",
                                      "'name' is required and must be a string");
                            continue;
                        }
                        if (!entry.contains("result") || !entry.at("result").is_string()) {
                            coll.emit(DiagnosticCode::C_MissingField, path + "/result",
                                      "'result' is required and must be a string");
                            continue;
                        }
                        BuiltinFunctionMapping m;
                        m.name = entry.at("name").get<std::string>();
                        auto rk = parseCore(entry.at("result").get<std::string>());
                        if (!rk) {
                            coll.emit(DiagnosticCode::C_InvalidSemantics, path + "/result",
                                      std::format("unknown core TypeKind '{}'",
                                                  entry.at("result").get<std::string>()));
                            continue;
                        }
                        m.resultCore = *rk;
                        bool paramsOk = true;
                        if (entry.contains("params")) {
                            if (!entry.at("params").is_array()) {
                                coll.emit(DiagnosticCode::C_InvalidSemantics,
                                          path + "/params",
                                          "'params' must be an array of core TypeKind names");
                                continue;
                            }
                            for (auto const& p : entry.at("params")) {
                                if (!p.is_string()) {
                                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                                              path + "/params",
                                              "each 'params' entry must be a string");
                                    paramsOk = false;
                                    break;
                                }
                                auto pk = parseCore(p.get<std::string>());
                                if (!pk) {
                                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                                              path + "/params",
                                              std::format("unknown core TypeKind '{}'",
                                                          p.get<std::string>()));
                                    paramsOk = false;
                                    break;
                                }
                                m.paramCores.push_back(*pk);
                            }
                        }
                        if (!paramsOk) continue;
                        if (entry.contains("variadic")) {
                            if (!entry.at("variadic").is_boolean()) {
                                coll.emit(DiagnosticCode::C_InvalidSemantics,
                                          path + "/variadic",
                                          "'variadic' must be a boolean");
                                continue;
                            }
                            m.variadic = entry.at("variadic").get<bool>();
                        }
                        cfg.builtinFunctions.push_back(std::move(m));
                    }
                }
            }

            // ── returnRules (GAP A return-type checking) ──
            // A return-statement shape. `value` is an OPTIONAL visible-child
            // index naming the returned expression; absent ⇒ a bare
            // `return;` shape.
            if (sem.contains("returnRules")) {
                json const& arr = sem.at("returnRules");
                if (!arr.is_array()) {
                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                              "/semantics/returnRules",
                              "'semantics.returnRules' must be an array");
                } else {
                    for (std::size_t i = 0; i < arr.size(); ++i) {
                        const auto path = std::format("/semantics/returnRules/{}", i);
                        json const& entry = arr[i];
                        if (!entry.is_object()) {
                            coll.emit(DiagnosticCode::C_InvalidSemantics, path,
                                      "each 'returnRules' entry must be an object");
                            continue;
                        }
                        if (!entry.contains("rule") || !entry.at("rule").is_string()) {
                            coll.emit(DiagnosticCode::C_MissingField, path + "/rule",
                                      "'rule' is required and must be a string");
                            continue;
                        }
                        ReturnRule rule;
                        rule.ruleName = entry.at("rule").get<std::string>();
                        if (!data.rules->contains(rule.ruleName)) {
                            coll.emit(DiagnosticCode::C_UnknownShape, path + "/rule",
                                      std::format("'returnRules[{}].rule' references "
                                                  "unknown shape '{}'", i, rule.ruleName));
                            continue;
                        }
                        rule.rule = data.rules->find(rule.ruleName);
                        // `value` is OPTIONAL — absent ⇒ bare return shape.
                        if (entry.contains("value")) {
                            if (!entry.at("value").is_number_integer()) {
                                coll.emit(DiagnosticCode::C_InvalidSemantics,
                                          path + "/value",
                                          "'value' must be a non-negative integer");
                                continue;
                            }
                            auto v = entry.at("value").get<std::int64_t>();
                            if (v < 0 || v > std::numeric_limits<std::int32_t>::max()) {
                                coll.emit(DiagnosticCode::C_InvalidSemantics,
                                          path + "/value",
                                          "'value' visible-child index out of range");
                                continue;
                            }
                            rule.valueChild = static_cast<std::uint32_t>(v);
                        }
                        cfg.returnRules.push_back(std::move(rule));
                    }
                }
            }

            // ── loopRules (GAP C loop contexts) ──
            // Rules whose subtree establishes a break/continue-valid context
            // (while/for/do/switch). Array of rule-name strings, mirroring
            // `scopes`.
            if (sem.contains("loopRules")) {
                json const& arr = sem.at("loopRules");
                if (!arr.is_array()) {
                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                              "/semantics/loopRules",
                              "'semantics.loopRules' must be an array of rule names");
                } else {
                    for (std::size_t i = 0; i < arr.size(); ++i) {
                        const auto path = std::format("/semantics/loopRules/{}", i);
                        if (!arr[i].is_string()) {
                            coll.emit(DiagnosticCode::C_InvalidSemantics, path,
                                      "each 'loopRules' entry must be a string");
                            continue;
                        }
                        auto const name = arr[i].get<std::string>();
                        if (!data.rules->contains(name)) {
                            coll.emit(DiagnosticCode::C_UnknownShape, path,
                                      std::format("'loopRules[{}]' references unknown shape '{}'",
                                                  i, name));
                            continue;
                        }
                        ScopeRule loopRule;
                        loopRule.rule     = data.rules->find(name);
                        loopRule.ruleName = name;
                        cfg.loopRules.push_back(std::move(loopRule));
                    }
                }
            }

            // ── loopControls (GAP C break/continue statements) ──
            if (sem.contains("loopControls")) {
                json const& arr = sem.at("loopControls");
                if (!arr.is_array()) {
                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                              "/semantics/loopControls",
                              "'semantics.loopControls' must be an array");
                } else {
                    for (std::size_t i = 0; i < arr.size(); ++i) {
                        const auto path = std::format("/semantics/loopControls/{}", i);
                        json const& entry = arr[i];
                        if (!entry.is_object()) {
                            coll.emit(DiagnosticCode::C_InvalidSemantics, path,
                                      "each 'loopControls' entry must be an object");
                            continue;
                        }
                        if (!entry.contains("rule") || !entry.at("rule").is_string()) {
                            coll.emit(DiagnosticCode::C_MissingField, path + "/rule",
                                      "'rule' is required and must be a string");
                            continue;
                        }
                        LoopControlRule rule;
                        rule.ruleName = entry.at("rule").get<std::string>();
                        if (!data.rules->contains(rule.ruleName)) {
                            coll.emit(DiagnosticCode::C_UnknownShape, path + "/rule",
                                      std::format("'loopControls[{}].rule' references "
                                                  "unknown shape '{}'", i, rule.ruleName));
                            continue;
                        }
                        rule.rule = data.rules->find(rule.ruleName);
                        cfg.loopControls.push_back(std::move(rule));
                    }
                }
            }

            // ── identifierToken ──
            // Names the token kind whose text is a language identifier. The
            // engine reads the resolved SchemaTokenId instead of hardcoding
            // a token name, so the analyzer is name-independent (a language
            // whose identifier token is "Word" works unchanged).
            if (sem.contains("identifierToken")) {
                json const& tok = sem.at("identifierToken");
                if (!tok.is_string()) {
                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                              "/semantics/identifierToken",
                              "'identifierToken' must be a string");
                } else {
                    auto const name = tok.get<std::string>();
                    if (!data.schemaTokens->contains(name)) {
                        coll.emit(DiagnosticCode::C_UnknownToken,
                                  "/semantics/identifierToken",
                                  std::format("'identifierToken' references unknown "
                                              "token kind '{}'", name));
                    } else {
                        cfg.identifierToken = data.schemaTokens->find(name);
                    }
                }
            }

            // ── bracketIdentifierToken (GAP D) ──
            // An OPTIONAL second token kind whose leaf also counts as a name
            // in LastIdentifier mode (tsql's `[Orders]` → "BracketIdStart").
            // A bad token name is C_UnknownToken.
            if (sem.contains("bracketIdentifierToken")) {
                json const& tok = sem.at("bracketIdentifierToken");
                if (!tok.is_string()) {
                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                              "/semantics/bracketIdentifierToken",
                              "'bracketIdentifierToken' must be a string");
                } else {
                    auto const name = tok.get<std::string>();
                    if (!data.schemaTokens->contains(name)) {
                        coll.emit(DiagnosticCode::C_UnknownToken,
                                  "/semantics/bracketIdentifierToken",
                                  std::format("'bracketIdentifierToken' references unknown "
                                              "token kind '{}'", name));
                    } else {
                        cfg.bracketIdentifierToken = data.schemaTokens->find(name);
                    }
                }
            }

            // ── pointerToken (SE-pointers / G5) ──
            // An OPTIONAL token whose occurrence in a type-position subtree wraps
            // the resolved type one level in Ptr (C's `int *p` declarator stars).
            if (sem.contains("pointerToken")) {
                json const& tok = sem.at("pointerToken");
                if (!tok.is_string()) {
                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                              "/semantics/pointerToken",
                              "'pointerToken' must be a string");
                } else {
                    auto const name = tok.get<std::string>();
                    if (!data.schemaTokens->contains(name)) {
                        coll.emit(DiagnosticCode::C_UnknownToken,
                                  "/semantics/pointerToken",
                                  std::format("'pointerToken' references unknown "
                                              "token kind '{}'", name));
                    } else {
                        cfg.pointerToken = data.schemaTokens->find(name);
                    }
                }
            }

            // FF6 Slice 2 + audit fold (2026-06-02): per-language
            // `externLibraryByFormat` — runtime library identity
            // per ObjectFormatKind for source-declared externs.
            // Object: keys are ObjectFormatKind names ("pe", "elf",
            // "macho", "wasm", "spirv" — validated via
            // `objectFormatKindFromName`); values are runtime
            // library identities. Lives at the LANGUAGE level
            // (not per-declaration-rule — the pre-fold #1 placement
            // on `DeclarationRule` was fragile: the pipeline
            // first-match loop gave non-obvious behavior on
            // grammars with multiple extern-shaped declaration
            // rules). c-subset's `externDecl` is the only shipped
            // declaration that needs it; per-symbol overrides
            // (`extern "otherlib.dll" int foo();`) are anchored
            // D-CSUBSET-EXTERN-LIBRARY-SYNTAX.
            if (sem.contains("externLibraryByFormat")) {
                auto const& obj = sem.at("externLibraryByFormat");
                if (!obj.is_object()) {
                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                              "/semantics/externLibraryByFormat",
                              "'externLibraryByFormat' must be an object "
                              "mapping format-kind names to library "
                              "identities");
                } else {
                    for (auto it = obj.begin(); it != obj.end(); ++it) {
                        auto const& formatName = it.key();
                        auto const& val = it.value();
                        auto const kindPath = std::format(
                            "/semantics/externLibraryByFormat/{}",
                            formatName);
                        if (!objectFormatKindFromName(
                                formatName).has_value()) {
                            coll.emit(
                                DiagnosticCode::C_InvalidSemantics,
                                kindPath,
                                std::format(
                                    "'{}' is not a recognized "
                                    "object-format kind "
                                    "(expected one of 'elf', "
                                    "'pe', 'macho', 'wasm', "
                                    "'spirv')",
                                    formatName));
                            continue;
                        }
                        if (!val.is_string()) {
                            coll.emit(
                                DiagnosticCode::C_InvalidSemantics,
                                kindPath,
                                "library identity must be a "
                                "string (e.g. \"msvcrt.dll\", "
                                "\"libc.so.6\")");
                            continue;
                        }
                        auto const libName = val.get<std::string>();
                        if (libName.empty()) {
                            coll.emit(
                                DiagnosticCode::C_InvalidSemantics,
                                kindPath,
                                "library identity must be a "
                                "non-empty string");
                            continue;
                        }
                        cfg.externLibraryByFormat
                            .emplace(formatName, libName);
                    }
                }
            }

            // D-LANG-POINTER-VOID-CONVERT (step 13.2, 2026-06-02):
            // per-language `pointerConversions` block. Loads two
            // independent direction flags:
            //   * `implicitToVoidPtr`: T* → void* without cast (safe).
            //   * `implicitFromVoidPtr`: void* → T* without cast (unsafe).
            // Both default false (strict — opt-in safety relaxation).
            // C-family languages declare both true; C++ declares only
            // `implicitToVoidPtr`; Rust/Swift declare neither.
            if (sem.contains("pointerConversions")) {
                auto const& obj = sem.at("pointerConversions");
                if (!obj.is_object()) {
                    coll.emit(DiagnosticCode::C_InvalidSemantics,
                              "/semantics/pointerConversions",
                              "'pointerConversions' must be an object "
                              "with optional `implicitToVoidPtr` and "
                              "`implicitFromVoidPtr` boolean fields");
                } else {
                    auto readBool = [&](char const* field, bool& out) {
                        if (!obj.contains(field)) return;
                        auto const& val = obj.at(field);
                        if (!val.is_boolean()) {
                            coll.emit(DiagnosticCode::C_InvalidSemantics,
                                      std::format(
                                          "/semantics/pointerConversions/{}",
                                          field),
                                      std::format("'{}' must be a boolean",
                                                  field));
                            return;
                        }
                        out = val.get<bool>();
                    };
                    readBool("implicitToVoidPtr",
                             cfg.pointerConversions.implicitToVoidPtr);
                    readBool("implicitFromVoidPtr",
                             cfg.pointerConversions.implicitFromVoidPtr);
                    readBool("nullPointerConstantFromIntegerZero",
                             cfg.pointerConversions
                                 .nullPointerConstantFromIntegerZero);
                    // Typo defense: reject unknown sub-keys with a
                    // fail-loud diagnostic. Pre-guard, an editor typo
                    // like `implictToVoidPtr` (missing 'i') would
                    // silently fall back to default-false strict
                    // mode and flip the language's semantics. Mirrors
                    // the unknown-field discipline at line 548 (the
                    // `expr` block's allowlist pattern). Diagnostics
                    // about expected fields are sufficient — the
                    // closed allowlist forecloses unknown subkeys.
                    for (auto const& [k, _] : obj.items()) {
                        if (k != "implicitToVoidPtr" &&
                            k != "implicitFromVoidPtr" &&
                            k != "nullPointerConstantFromIntegerZero") {
                            coll.emit(DiagnosticCode::C_InvalidSemantics,
                                      std::format(
                                          "/semantics/pointerConversions/{}",
                                          k),
                                      std::format(
                                          "unknown 'pointerConversions' "
                                          "field '{}' — expected one of "
                                          "'implicitToVoidPtr', "
                                          "'implicitFromVoidPtr', or "
                                          "'nullPointerConstantFromIntegerZero'",
                                          k));
                        }
                    }
                }
            }

            // A `nameMatch: "lastIdentifier"` rule (declaration or
            // reference) descends a subtree for its LAST identifier token —
            // which requires the engine to know which token kind IS the
            // identifier. Declaring lastIdentifier without an
            // identifierToken is a config gap (C_MissingField).
            if (!cfg.identifierToken.valid()) {
                bool usesLastIdentifier = false;
                for (auto const& d : cfg.declarations) {
                    if (d.nameMatch == NameMatchMode::LastIdentifier) usesLastIdentifier = true;
                }
                for (auto const& r : cfg.references) {
                    if (r.nameMatch == NameMatchMode::LastIdentifier) usesLastIdentifier = true;
                }
                if (usesLastIdentifier) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              "/semantics/identifierToken",
                              "a 'lastIdentifier' nameMatch rule requires "
                              "'semantics.identifierToken' to be declared");
                }
            }

            data.semantics = std::move(cfg);
        }
    }

    // hirLowering ── per-language CST→HIR lowering config (plan 09 HR8; schema
    // v4). Optional; absent ⇒ no lowering. Parsed LATE (after shapes/tokens
    // interned) so rule/token references resolve here. HIR kind/op NAMES are
    // stored raw (the `core` loader can't see the `hir` enums) and resolved by
    // the lowering engine, which reports an unknown name as
    // H_UnsupportedLoweringForKind.
    if (doc.contains("hirLowering")) {
        json const& hl = doc.at("hirLowering");
        if (!hl.is_object()) {
            coll.emit(DiagnosticCode::C_InvalidHirLowering, "/hirLowering",
                      "'hirLowering' must be an object");
        } else {
            HirLoweringConfig cfg;

            // Resolve a required rule-name field on `obj` → RuleId + name.
            auto resolveRuleField = [&](json const& obj, char const* key,
                                        std::string const& path,
                                        RuleId& outId, std::string& outName) -> bool {
                if (!obj.contains(key) || !obj.at(key).is_string()) {
                    coll.emit(DiagnosticCode::C_MissingField, path + "/" + key,
                              std::format("'{}' is required and must be a string", key));
                    return false;
                }
                outName = obj.at(key).get<std::string>();
                if (!data.rules->contains(outName)) {
                    coll.emit(DiagnosticCode::C_UnknownShape, path + "/" + key,
                              std::format("'{}' references unknown shape '{}'", key, outName));
                    return false;
                }
                outId = data.rules->find(outName);
                return true;
            };
            // Resolve a token-name string → SchemaTokenId (C_UnknownToken on miss).
            auto resolveToken = [&](std::string const& name, std::string const& path,
                                    SchemaTokenId& out) -> bool {
                if (!data.schemaTokens->contains(name)) {
                    coll.emit(DiagnosticCode::C_UnknownToken, path,
                              std::format("references unknown token kind '{}'", name));
                    return false;
                }
                out = data.schemaTokens->find(name);
                return true;
            };

            // ── ruleMappings: [{ rule, hirKind }] ──
            if (hl.contains("ruleMappings")) {
                json const& arr = hl.at("ruleMappings");
                if (!arr.is_array()) {
                    coll.emit(DiagnosticCode::C_InvalidHirLowering, "/hirLowering/ruleMappings",
                              "'hirLowering.ruleMappings' must be an array");
                } else {
                    for (std::size_t i = 0; i < arr.size(); ++i) {
                        const auto path = std::format("/hirLowering/ruleMappings/{}", i);
                        json const& entry = arr[i];
                        if (!entry.is_object()) {
                            coll.emit(DiagnosticCode::C_InvalidHirLowering, path,
                                      "each 'ruleMappings' entry must be an object");
                            continue;
                        }
                        HirRuleMapping m;
                        if (!resolveRuleField(entry, "rule", path, m.rule, m.ruleName)) continue;
                        if (!entry.contains("hirKind") || !entry.at("hirKind").is_string()) {
                            coll.emit(DiagnosticCode::C_MissingField, path + "/hirKind",
                                      "'hirKind' is required and must be a string");
                            continue;
                        }
                        m.hirKind = entry.at("hirKind").get<std::string>();
                        // HR10: optional childGathering [{ match:{rule|classifier},
                        // optional?, list?, lower, role? }] for extension-kind mappings.
                        if (entry.contains("childGathering")) {
                            json const& cg = entry.at("childGathering");
                            if (!cg.is_array()) {
                                coll.emit(DiagnosticCode::C_InvalidHirLowering, path + "/childGathering",
                                          "'childGathering' must be an array");
                            } else {
                                for (std::size_t j = 0; j < cg.size(); ++j) {
                                    const auto sPath = std::format("{}/childGathering/{}", path, j);
                                    json const& sj = cg[j];
                                    if (!sj.is_object() || !sj.contains("lower")
                                        || !sj.at("lower").is_string()) {
                                        coll.emit(DiagnosticCode::C_InvalidHirLowering, sPath,
                                                  "each childGathering slot needs a string 'lower'");
                                        continue;
                                    }
                                    ChildSlotSpec slot;
                                    {   // map the `lower` verb string → ChildLower (closed set)
                                        auto const lv = sj.at("lower").get<std::string>();
                                        if      (lv == "expr")     slot.lower = ChildLower::Expr;
                                        else if (lv == "flatExpr") slot.lower = ChildLower::FlatExpr;
                                        else if (lv == "ext")      slot.lower = ChildLower::Ext;
                                        else if (lv == "ref")      slot.lower = ChildLower::Ref;
                                        else if (lv == "varDecl")  slot.lower = ChildLower::VarDecl;
                                        else {
                                            coll.emit(DiagnosticCode::C_InvalidHirLowering, sPath + "/lower",
                                                      std::format("unknown childGathering lower verb '{}' "
                                                                  "(expected expr/flatExpr/ext/ref/varDecl)", lv));
                                            continue;
                                        }
                                    }
                                    if (sj.contains("role") && sj.at("role").is_string())
                                        slot.role = sj.at("role").get<std::string>();
                                    if (sj.contains("optional") && sj.at("optional").is_boolean())
                                        slot.optional = sj.at("optional").get<bool>();
                                    if (sj.contains("list") && sj.at("list").is_boolean())
                                        slot.list = sj.at("list").get<bool>();
                                    // match: exactly one of { rule } / { classifier }.
                                    if (sj.contains("match") && sj.at("match").is_object()) {
                                        json const& mj = sj.at("match");
                                        if (mj.contains("rule"))
                                            (void)resolveRuleField(mj, "rule", sPath + "/match",
                                                                   slot.matchRule, slot.matchRuleName);
                                        else if (mj.contains("classifier") && mj.at("classifier").is_string()) {
                                            slot.classifier = mj.at("classifier").get<std::string>();
                                            if (slot.classifier != "expr")
                                                coll.emit(DiagnosticCode::C_InvalidHirLowering,
                                                          sPath + "/match/classifier",
                                                          std::format("unknown classifier '{}' (only "
                                                                      "'expr' is supported)", slot.classifier));
                                        } else
                                            coll.emit(DiagnosticCode::C_InvalidHirLowering, sPath + "/match",
                                                      "'match' needs a 'rule' or 'classifier'");
                                    } else {
                                        coll.emit(DiagnosticCode::C_InvalidHirLowering, sPath,
                                                  "childGathering slot needs a 'match' object");
                                    }
                                    m.childGathering.push_back(std::move(slot));
                                }
                            }
                        }
                        cfg.ruleMappings.push_back(std::move(m));
                    }
                }
            }

            // ── the four Pratt expression rule ids (optional strings) ──
            auto readExprRule = [&](char const* key, RuleId& id, std::string& name) {
                if (!hl.contains(key)) return;
                (void)resolveRuleField(hl, key, "/hirLowering", id, name);
            };
            readExprRule("binaryExprRule",  cfg.binaryExprRule,  cfg.binaryExprRuleName);
            readExprRule("unaryExprRule",   cfg.unaryExprRule,   cfg.unaryExprRuleName);
            readExprRule("postfixExprRule", cfg.postfixExprRule, cfg.postfixExprRuleName);
            readExprRule("ternaryExprRule", cfg.ternaryExprRule, cfg.ternaryExprRuleName);
            readExprRule("operandRule",     cfg.operandRule,     cfg.operandRuleName);
            readExprRule("flatExprRule",     cfg.flatExprRule,     cfg.flatExprRuleName);
            readExprRule("flatBinaryOpRule", cfg.flatBinaryOpRule, cfg.flatBinaryOpRuleName);
            // D5.3 brace-init + designated-initializer + compound-
            // literal rule ids. Each optional — absent ⇒ language has
            // no such form; the engine simply never matches.
            readExprRule("braceInitListRule",   cfg.braceInitListRule,   cfg.braceInitListRuleName);
            readExprRule("initElementRule",     cfg.initElementRule,     cfg.initElementRuleName);
            readExprRule("designatedFieldRule", cfg.designatedFieldRule, cfg.designatedFieldRuleName);
            readExprRule("designatedIndexRule", cfg.designatedIndexRule, cfg.designatedIndexRuleName);
            readExprRule("compoundLiteralRule", cfg.compoundLiteralRule, cfg.compoundLiteralRuleName);

            // ── HR10: extensionKinds [{ name, lang }] ──
            if (hl.contains("extensionKinds")) {
                json const& arr = hl.at("extensionKinds");
                if (!arr.is_array()) {
                    coll.emit(DiagnosticCode::C_InvalidHirLowering, "/hirLowering/extensionKinds",
                              "'extensionKinds' must be an array");
                } else {
                    for (std::size_t i = 0; i < arr.size(); ++i) {
                        const auto path = std::format("/hirLowering/extensionKinds/{}", i);
                        json const& e = arr[i];
                        if (!e.is_object() || !e.contains("name") || !e.at("name").is_string()) {
                            coll.emit(DiagnosticCode::C_InvalidHirLowering, path,
                                      "each extensionKind needs a string 'name'");
                            continue;
                        }
                        ExtensionKindEntry ek;
                        ek.name = e.at("name").get<std::string>();
                        if (ek.name.find("::") == std::string::npos) {
                            coll.emit(DiagnosticCode::C_InvalidHirLowering, path + "/name",
                                      std::format("extension kind name '{}' must be language-"
                                                  "qualified (contain '::')", ek.name));
                            continue;
                        }
                        if (e.contains("lang") && e.at("lang").is_string())
                            ek.lang = e.at("lang").get<std::string>();
                        cfg.extensionKinds.push_back(std::move(ek));
                    }
                }
            }

            // An extension kind named by a non-array facet (nullExtensionKind /
            // refExtensionKind) MUST be declared in `extensionKinds`, so the
            // lowering engine's `extKind()` lookup is total (it never silently
            // registers an undeclared kind). Emits C_InvalidHirLowering otherwise.
            auto requireDeclaredExtKind = [&](std::string const& name, std::string const& path) {
                for (auto const& ek : cfg.extensionKinds) if (ek.name == name) return;
                coll.emit(DiagnosticCode::C_InvalidHirLowering, path,
                          std::format("extension kind '{}' is not declared in "
                                      "'hirLowering.extensionKinds'", name));
            };

            // ── HR10: nullLiteral { token, hirKind } ──
            if (hl.contains("nullLiteral")) {
                json const& nl = hl.at("nullLiteral");
                if (!nl.is_object() || !nl.contains("token") || !nl.at("token").is_string()
                    || !nl.contains("hirKind") || !nl.at("hirKind").is_string()) {
                    coll.emit(DiagnosticCode::C_InvalidHirLowering, "/hirLowering/nullLiteral",
                              "'nullLiteral' needs string 'token' and 'hirKind'");
                } else {
                    cfg.nullTokenName = nl.at("token").get<std::string>();
                    (void)resolveToken(cfg.nullTokenName, "/hirLowering/nullLiteral/token", cfg.nullToken);
                    cfg.nullExtensionKind = nl.at("hirKind").get<std::string>();
                    requireDeclaredExtKind(cfg.nullExtensionKind, "/hirLowering/nullLiteral/hirKind");
                }
            }

            // ── plan-12.5 §0.2 D3: globalsConstEval { allowFloat: bool } —
            // per-schema MIR-globals const-evaluation policy. Default is
            // IEEE 754 (allowFloat=true); a non-IEEE-float language
            // declares `false` and the engine refuses to fold its float
            // arithmetic into module-init literals. JSON-declared, NOT
            // C++-coded — config-driven discipline (no schema.name()
            // branches anywhere downstream). ──
            if (hl.contains("globalsConstEval")) {
                json const& gce = hl.at("globalsConstEval");
                if (!gce.is_object()) {
                    coll.emit(DiagnosticCode::C_InvalidHirLowering,
                              "/hirLowering/globalsConstEval",
                              "'globalsConstEval' must be an object");
                } else if (gce.contains("allowFloat")) {
                    if (!gce.at("allowFloat").is_boolean()) {
                        coll.emit(DiagnosticCode::C_InvalidHirLowering,
                                  "/hirLowering/globalsConstEval/allowFloat",
                                  "'allowFloat' must be a boolean");
                    } else {
                        cfg.globalsConstEval.allowFloat =
                            gce.at("allowFloat").get<bool>();
                    }
                }
            }

            // ── HR10: refExtensionKind (string) — name refs lower to this
            // extension kind instead of a core Ref (SQL relational names). ──
            if (hl.contains("refExtensionKind")) {
                if (!hl.at("refExtensionKind").is_string()) {
                    coll.emit(DiagnosticCode::C_InvalidHirLowering, "/hirLowering/refExtensionKind",
                              "'refExtensionKind' must be a string");
                } else {
                    cfg.refExtensionKind = hl.at("refExtensionKind").get<std::string>();
                    requireDeclaredExtKind(cfg.refExtensionKind, "/hirLowering/refExtensionKind");
                }
            }

            // ── operator dispatch tables: [{ token, target, compoundBase? }] ──
            auto readOps = [&](char const* key, std::vector<HirOperatorEntry>& out) {
                if (!hl.contains(key)) return;
                json const& arr = hl.at(key);
                std::string const base = std::format("/hirLowering/{}", key);
                if (!arr.is_array()) {
                    coll.emit(DiagnosticCode::C_InvalidHirLowering, base,
                              std::format("'hirLowering.{}' must be an array", key));
                    return;
                }
                for (std::size_t i = 0; i < arr.size(); ++i) {
                    const auto path = std::format("{}/{}", base, i);
                    json const& entry = arr[i];
                    if (!entry.is_object()) {
                        coll.emit(DiagnosticCode::C_InvalidHirLowering, path,
                                  "each operator entry must be an object");
                        continue;
                    }
                    if (!entry.contains("token") || !entry.at("token").is_string()) {
                        coll.emit(DiagnosticCode::C_MissingField, path + "/token",
                                  "'token' is required and must be a string");
                        continue;
                    }
                    if (!entry.contains("target") || !entry.at("target").is_string()) {
                        coll.emit(DiagnosticCode::C_MissingField, path + "/target",
                                  "'target' is required and must be a string");
                        continue;
                    }
                    HirOperatorEntry e;
                    e.tokenName = entry.at("token").get<std::string>();
                    if (!resolveToken(e.tokenName, path + "/token", e.token)) continue;
                    e.target = entry.at("target").get<std::string>();
                    if (entry.contains("compoundBase")) {
                        if (!entry.at("compoundBase").is_string()) {
                            coll.emit(DiagnosticCode::C_InvalidHirLowering, path + "/compoundBase",
                                      "'compoundBase' must be a string");
                        } else {
                            e.compoundBase = entry.at("compoundBase").get<std::string>();
                        }
                    }
                    out.push_back(std::move(e));
                }
            };
            readOps("binaryOps",  cfg.binaryOps);
            readOps("unaryOps",   cfg.unaryOps);
            readOps("postfixOps", cfg.postfixOps);

            // ── structural specials ──
            auto readTokenField = [&](char const* key, SchemaTokenId& id, std::string& name) {
                if (!hl.contains(key)) return;
                if (!hl.at(key).is_string()) {
                    coll.emit(DiagnosticCode::C_InvalidHirLowering,
                              std::format("/hirLowering/{}", key),
                              std::format("'{}' must be a string", key));
                    return;
                }
                name = hl.at(key).get<std::string>();
                (void)resolveToken(name, std::format("/hirLowering/{}", key), id);
            };
            if (hl.contains("deferredRules")) {
                json const& arr = hl.at("deferredRules");
                if (!arr.is_array()) {
                    coll.emit(DiagnosticCode::C_InvalidHirLowering, "/hirLowering/deferredRules",
                              "'hirLowering.deferredRules' must be an array");
                } else {
                    for (std::size_t i = 0; i < arr.size(); ++i) {
                        const auto path = std::format("/hirLowering/deferredRules/{}", i);
                        if (!arr[i].is_string()) {
                            coll.emit(DiagnosticCode::C_InvalidHirLowering, path,
                                      "each 'deferredRules' entry must be a rule-name string");
                            continue;
                        }
                        auto const name = arr[i].get<std::string>();
                        if (!data.rules->contains(name)) {
                            coll.emit(DiagnosticCode::C_UnknownShape, path,
                                      std::format("'deferredRules' references unknown shape '{}'", name));
                            continue;
                        }
                        cfg.deferredRules.push_back(data.rules->find(name));
                    }
                }
            }

            readTokenField("forClauseSeparator", cfg.forClauseSeparator, cfg.forClauseSeparatorName);
            readTokenField("caseDefaultToken",   cfg.caseDefaultToken,   cfg.caseDefaultTokenName);
            if (hl.contains("caseLabelRule")) {
                (void)resolveRuleField(hl, "caseLabelRule", "/hirLowering",
                                       cfg.caseLabelRule, cfg.caseLabelRuleName);
            }

            // Char / string literal lowering blocks:
            //   "charLiteral":   { "startToken": "CharStart",   "bodyToken": "CharLiteral"   }
            //   "stringLiteral": { "startToken": "StringStart", "bodyToken": "StringLiteral" }
            auto readBodyLiteral = [&](char const* key, SchemaTokenId& startId,
                                       std::string& startName, SchemaTokenId& bodyId,
                                       std::string& bodyName) {
                if (!hl.contains(key)) return;
                json const& bl = hl.at(key);
                auto readField = [&](char const* f, SchemaTokenId& id, std::string& name) {
                    if (!bl.contains(f) || !bl.at(f).is_string()) {
                        coll.emit(DiagnosticCode::C_InvalidHirLowering,
                                  std::format("/hirLowering/{}/{}", key, f),
                                  std::format("'{}.{}' is required and must be a token name", key, f));
                        return;
                    }
                    name = bl.at(f).get<std::string>();
                    (void)resolveToken(name, std::format("/hirLowering/{}/{}", key, f), id);
                };
                if (!bl.is_object()) {
                    coll.emit(DiagnosticCode::C_InvalidHirLowering,
                              std::format("/hirLowering/{}", key),
                              std::format("'{}' must be an object", key));
                    return;
                }
                readField("startToken", startId, startName);
                readField("bodyToken",  bodyId,  bodyName);
            };
            readBodyLiteral("charLiteral",   cfg.charStartToken,   cfg.charStartTokenName,
                            cfg.charBodyToken,   cfg.charBodyTokenName);
            readBodyLiteral("stringLiteral", cfg.stringStartToken, cfg.stringStartTokenName,
                            cfg.stringBodyToken, cfg.stringBodyTokenName);
            // HR10: a second string opener (SQL's `N'…'`) sharing stringBodyToken.
            if (hl.contains("unicodeStringStartToken")
                && hl.at("unicodeStringStartToken").is_string()) {
                cfg.unicodeStringStartTokenName = hl.at("unicodeStringStartToken").get<std::string>();
                (void)resolveToken(cfg.unicodeStringStartTokenName,
                                   "/hirLowering/unicodeStringStartToken",
                                   cfg.unicodeStringStartToken);
            }
            // HR10: string bodies use doubled-delimiter (`''`) escaping (SQL).
            // Derive the delimiter byte from the string opener's StringStyle.endsAt
            // (single source of truth) so the engine's decoder doesn't duplicate it.
            if (hl.contains("stringDoubledDelimiter")
                && hl.at("stringDoubledDelimiter").is_boolean()
                && hl.at("stringDoubledDelimiter").get<bool>()) {
                cfg.stringDoubledDelimiter = true;
                for (auto const& [lex, meanings] : data.lexemeTable) {
                    (void)lex;
                    for (auto const& mn : meanings) {
                        if (mn.id.v == cfg.stringStartToken.v && mn.stringStyleId.valid()
                            && mn.stringStyleId.v < data.stringStyles.size()) {
                            auto const& ss = data.stringStyles[mn.stringStyleId.v];
                            if (!ss.endsAt.empty()) cfg.stringDelimiter = ss.endsAt[0];
                        }
                    }
                }
                if (cfg.stringDelimiter == '\0') {
                    coll.emit(DiagnosticCode::C_InvalidHirLowering,
                              "/hirLowering/stringDoubledDelimiter",
                              "could not derive the doubled-delimiter byte from the "
                              "string opener's StringStyle.endsAt");
                }
            }

            data.hirLowering = std::move(cfg);
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
        return std::unexpected(std::move(coll).release());
    }
    return std::make_shared<GrammarSchema>(std::move(data));
}

} // namespace dss::detail
