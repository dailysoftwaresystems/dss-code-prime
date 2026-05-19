#include "core/types/grammar_schema_json.hpp"

#include "core/types/grammar_schema.hpp"
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
        for (std::size_t i = 0; i < arr.size(); ++i) {
            collectReferences(arr[i], std::format("{}/{}/{}", path, kind, i),
                              outRefs, c);
        }
    };

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
    // `binary` etc. — future expansion. Silently skip.
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
            // Pass A: count entries with validScopes to reserve the pool
            // exactly. Reserving avoids reallocation that would invalidate
            // the LexemeMeaning::validScopes spans we set up next.
            std::size_t poolNeeded = 0;
            for (auto const& [lex, arr] : doc.at("tokens").items()) {
                if (!arr.is_array()) continue;
                for (auto const& m : arr) {
                    if (m.is_object() && m.contains("validScopes")) ++poolNeeded;
                }
            }
            data.validScopesPool.reserve(poolNeeded);

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
                    if (m.contains("validScopes") && m.at("validScopes").is_array()) {
                        std::vector<ScopeKind> scopes;
                        scopes.reserve(m.at("validScopes").size());
                        for (auto const& sn : m.at("validScopes")) {
                            if (!sn.is_string()) continue;
                            const auto sk = parseScopeName(sn.get<std::string>());
                            if (!sk) {
                                coll.emit(DiagnosticCode::C_UnclosableScope, entryPath,
                                          std::format("unknown scope '{}' in validScopes",
                                                      sn.get<std::string>()));
                                continue;
                            }
                            scopes.push_back(*sk);
                        }
                        data.validScopesPool.push_back(std::move(scopes));
                        auto const& backing = data.validScopesPool.back();
                        lm.validScopes = std::span<ScopeKind const>{backing};
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
    if (doc.contains("keywords") && doc.at("keywords").is_array()) {
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
            data.lexemeTable[kw.at("word").get<std::string>()].push_back(lm);
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
            // (SchemaTokenInterner). Build expectedAt in the same walk.
            for (auto const& [shapeName, body] : doc.at("shapes").items()) {
                const auto shapePath = std::format("/shapes/{}", shapeName);
                std::vector<std::string> refs;
                collectReferences(body, shapePath, refs, coll);

                std::vector<std::string> expected;
                expected.reserve(refs.size());
                for (auto const& r : refs) {
                    if (data.rules->contains(r))        { expected.push_back(r); continue; }
                    if (data.schemaTokens->contains(r)) { expected.push_back(r); continue; }
                    coll.emit(DiagnosticCode::C_UnknownShape, shapePath,
                              std::format("unknown reference '{}'", r));
                }

                const auto rid = data.rules->intern(shapeName);
                data.expectedAt[rid.v] = std::move(expected);
            }

            if (data.rules->contains("root")) {
                data.rootRule = data.rules->intern("root");
            } else if (!doc.at("shapes").empty()) {
                coll.emit(DiagnosticCode::C_MissingField, "/shapes",
                          "missing required 'root' shape");
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
