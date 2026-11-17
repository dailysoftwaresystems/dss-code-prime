#include "core/types/type_spelling_json.hpp"

#include "core/types/config_key_vocabulary.hpp"
#include "core/types/type_lattice/core_type.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace dss {

namespace {

using json = nlohmann::json;
constexpr char const* kPath = "/semantics/typeSpelling";

constexpr std::array<std::string_view, 15> kTypeSpellingKeys{
    "kinds", "pointer", "array", "function", "group", "parameterSeparator", "variadic", "emptyPrototype",
    "declaratorSeparator", "arraySuffixJoinsSpecifiers", "bitInt", "complex", "tags", "anonymousTag",
    "qualifiers"};

// What the document's own rows say about its types, read once.
struct DocumentTypes {
    // Every core kind a cell reaches, with the cell's pointer for the refusal: a `"core"` string, and each value
    // of a `"coreBy…"` selector object (`coreByDataModel`, `coreByLongDoubleFormat`: the config vocabulary's
    // spelling of "this kind, chosen by a fact of the pair").
    std::vector<std::pair<TypeKind, std::string>> cells;
    // The document's OWN names of each kind: a row's `name`, and the lexemes of a row's `tokens` joined by one
    // space. A kind some row names is spelled by one of these, never by a second copy free to drift.
    std::map<TypeKind, std::set<std::string>> names;
    bool reachesComplex = false;   // a row reaches the Complex kind, or marks its type `"complex": true`
};

// Every lexeme a token kind is written with, by the token's NAME (a row's `tokens` names kinds).
[[nodiscard]] std::map<std::string, std::vector<std::string>, std::less<>>
lexemesByTokenName(detail::LexemeTable const& lexemes, SchemaTokenInterner const& tokens) {
    std::map<std::string, std::vector<std::string>, std::less<>> out;
    for (auto const& [lexeme, meanings] : lexemes)
        for (auto const& m : meanings) out[std::string{tokens.name(m.id)}].push_back(lexeme);
    for (auto& [_, v] : out) std::sort(v.begin(), v.end());
    return out;
}

// An explicit work list (the document's depth is bounded by the nesting limit, but a walk proportional to it is a
// stack all the same).
[[nodiscard]] DocumentTypes readDocumentTypes(
    json const& doc, std::map<std::string, std::vector<std::string>, std::less<>> const& lexemesOf) {
    DocumentTypes out;
    std::vector<std::pair<json const*, std::string>> work{{&doc, ""}};
    while (!work.empty()) {
        auto [node, at] = work.back();
        work.pop_back();
        if (node->is_array()) {
            for (std::size_t i = 0; i < node->size(); ++i)
                if ((*node)[i].is_structured()) work.emplace_back(&(*node)[i], at + "/" + std::to_string(i));
            continue;
        }
        if (!node->is_object()) continue;
        // the kinds this object reaches, and the names it gives them
        std::vector<TypeKind> reached;
        for (auto it = node->begin(); it != node->end(); ++it) {
            std::string const p = at + "/" + it.key();
            if (it.key() == "core" && it->is_string()) {
                if (auto k = typeKindFromName(it->get<std::string>())) {
                    out.cells.emplace_back(*k, p);
                    reached.push_back(*k);
                }
            } else if (it.key().starts_with("coreBy") && it->is_object()) {
                for (auto jt = it->begin(); jt != it->end(); ++jt)
                    if (jt->is_string())
                        if (auto k = typeKindFromName(jt->get<std::string>())) {
                            out.cells.emplace_back(*k, p + "/" + jt.key());
                            reached.push_back(*k);
                        }
            } else if (it->is_structured()) {
                work.emplace_back(&*it, p);
            }
        }
        if (reached.empty()) continue;
        for (TypeKind k : reached) out.reachesComplex = out.reachesComplex || k == TypeKind::Complex;
        if (node->contains("complex") && node->at("complex").is_boolean() && node->at("complex").get<bool>())
            out.reachesComplex = true;
        std::vector<std::string> spelled;
        if (node->contains("name") && node->at("name").is_string()) spelled.push_back(node->at("name").get<std::string>());
        if (node->contains("tokens") && node->at("tokens").is_array()) {
            // every combination of the tokens' lexemes, one space between (a row has a handful of tokens)
            std::vector<std::string> combos{""};
            for (json const& tok : node->at("tokens")) {
                if (!tok.is_string()) { combos.clear(); break; }
                auto const lx = lexemesOf.find(tok.get<std::string>());
                if (lx == lexemesOf.end()) { combos.clear(); break; }
                std::vector<std::string> next;
                for (std::string const& c : combos)
                    for (std::string const& l : lx->second) next.push_back(c.empty() ? l : c + ' ' + l);
                combos = std::move(next);
            }
            spelled.insert(spelled.end(), combos.begin(), combos.end());
        }
        for (TypeKind k : reached)
            for (std::string const& s : spelled) out.names[k].insert(s);
    }
    return out;
}

} // namespace

void readTypeSpelling(json const& doc, json const& sem, detail::LexemeTable const& lexemes,
                      SchemaTokenInterner const& tokens, std::optional<SchemaTokenId> volatileMarker,
                      std::optional<SchemaTokenId> atomicMarker, substrate::DiagnosticCollector& coll,
                      TypeSpelling& out) {
    DocumentTypes const types = readDocumentTypes(doc, lexemesByTokenName(lexemes, tokens));
    auto const& cells = types.cells;
    out.documentHasTypes = !cells.empty();
    if (!sem.contains("typeSpelling")) return;   // this language's diagnostics name no types
    json const& t = sem.at("typeSpelling");
    if (!t.is_object()) {
        coll.emit(DiagnosticCode::C_InvalidSemantics, kPath, "'semantics.typeSpelling' must be an object");
        return;
    }
    DSS_CHECK_KEY_VOCABULARY(kTypeSpellingKeys);
    detail::rejectUnknownKeys(t, kTypeSpellingKeys, "the 'semantics.typeSpelling' block",
                      [&](std::string_view key, std::string message) {
                          coll.emit(DiagnosticCode::C_InvalidSemantics, std::format("{}/{}", kPath, key),
                                    std::move(message));
                      });
    out.declared = true;

    auto text = [&](json const& obj, std::string_view key, std::string const& at, std::string& into) {
        if (!obj.contains(key)) return;
        json const& v = obj.at(key);
        if (!v.is_string() || v.get<std::string>().empty()) {
            coll.emit(DiagnosticCode::C_InvalidSemantics, at, std::format("'{}' must be a non-empty string", key));
            return;
        }
        into = v.get<std::string>();
    };
    auto pair = [&](std::string_view key, std::string& open, std::string& close) {
        if (!t.contains(key)) return;
        json const& v = t.at(key);
        std::string const at = std::format("{}/{}", kPath, key);
        if (!v.is_array() || v.size() != 2 || !v[0].is_string() || !v[1].is_string()
            || v[0].get<std::string>().empty() || v[1].get<std::string>().empty()) {
            coll.emit(DiagnosticCode::C_InvalidSemantics, at,
                      std::format("'{}' must be an array of two non-empty strings: the opener and the closer", key));
            return;
        }
        open = v[0].get<std::string>();
        close = v[1].get<std::string>();
    };

    // ── the kinds ──
    if (t.contains("kinds")) {
        json const& kinds = t.at("kinds");
        if (!kinds.is_object()) {
            coll.emit(DiagnosticCode::C_InvalidSemantics, std::format("{}/kinds", kPath),
                      "'kinds' must be an object of core kind -> spelling");
        } else {
            for (auto it = kinds.begin(); it != kinds.end(); ++it) {
                if (detail::isDocumentationKey(it.key())) continue;
                std::string const at = std::format("{}/kinds/{}", kPath, it.key());
                auto const k = typeKindFromName(it.key());
                if (!k) {
                    coll.emit(DiagnosticCode::C_InvalidSemantics, at,
                              std::format("'{}' is no core type kind", it.key()));
                    continue;
                }
                if (!it->is_string() || it->get<std::string>().empty()) {
                    coll.emit(DiagnosticCode::C_InvalidSemantics, at, "a kind's spelling must be a non-empty string");
                    continue;
                }
                std::string const word = it->get<std::string>();
                // ONE OWNER OF A KIND'S NAME: a kind the document's own rows name is spelled by one of those
                // names; the table only CHOOSES among them (a second copy of `int` could drift from the row's).
                if (auto const named = types.names.find(*k); named != types.names.end() && !named->second.empty()
                                                             && !named->second.contains(word)) {
                    std::string own;
                    for (std::string const& n : named->second) own += (own.empty() ? "'" : ", '") + n + "'";
                    coll.emit(DiagnosticCode::C_ConflictingField, at,
                              std::format("'{}' is not a name the document's own rows give the core kind '{}' "
                                          "(they name it {}): the table chooses one of them, it does not restate it",
                                          word, it.key(), own));
                    continue;
                }
                out.kinds[static_cast<std::size_t>(*k)] = word;
            }
        }
    }
    std::set<TypeKind> refused;
    bool const reachesComplex = types.reachesComplex;
    for (auto const& [k, at] : cells) {
        if (k == TypeKind::Complex) continue;   // the complex PATTERN spells it, below
        if (!out.kind(k).empty() || !refused.insert(k).second) continue;
        coll.emit(DiagnosticCode::C_MissingField, std::format("{}/kinds/{}", kPath, typeKindNameOrEmpty(k)),
                  std::format("the document's type rows reach the core kind '{}' (at {}), and 'typeSpelling.kinds' "
                              "spells no word for it: a diagnostic naming such a type would print the engine's "
                              "kind name", typeKindNameOrEmpty(k), at));
    }

    // ── the declarator forms and the tags ──
    text(t, "pointer", std::format("{}/pointer", kPath), out.pointer);
    pair("array", out.arrayOpen, out.arrayClose);
    pair("function", out.functionOpen, out.functionClose);
    pair("group", out.groupOpen, out.groupClose);
    text(t, "parameterSeparator", std::format("{}/parameterSeparator", kPath), out.parameterSeparator);
    text(t, "variadic", std::format("{}/variadic", kPath), out.variadic);
    text(t, "emptyPrototype", std::format("{}/emptyPrototype", kPath), out.emptyPrototype);
    text(t, "declaratorSeparator", std::format("{}/declaratorSeparator", kPath), out.declaratorSeparator);
    text(t, "anonymousTag", std::format("{}/anonymousTag", kPath), out.anonymousTag);
    if (t.contains("arraySuffixJoinsSpecifiers")) {
        if (!t.at("arraySuffixJoinsSpecifiers").is_boolean())
            coll.emit(DiagnosticCode::C_InvalidSemantics, std::format("{}/arraySuffixJoinsSpecifiers", kPath),
                      "'arraySuffixJoinsSpecifiers' must be true or false");
        else
            out.suffixJoinsSpecifiers = t.at("arraySuffixJoinsSpecifiers").get<bool>();
    }
    if (t.contains("tags")) {
        json const& tags = t.at("tags");
        std::string const at = std::format("{}/tags", kPath);
        if (!tags.is_object()) {
            coll.emit(DiagnosticCode::C_InvalidSemantics, at, "'tags' must be an object: Struct, Union, Enum");
        } else {
            text(tags, "Struct", at + "/Struct", out.structTag);
            text(tags, "Union", at + "/Union", out.unionTag);
            text(tags, "Enum", at + "/Enum", out.enumTag);
        }
    }
    if (t.contains("bitInt")) {
        json const& b = t.at("bitInt");
        std::string const at = std::format("{}/bitInt", kPath);
        if (!b.is_object()) {
            coll.emit(DiagnosticCode::C_InvalidSemantics, at, "'bitInt' must be an object: signed, unsigned");
        } else {
            text(b, "signed", at + "/signed", out.bitIntSigned);
            text(b, "unsigned", at + "/unsigned", out.bitIntUnsigned);
        }
    }
    text(t, "complex", std::format("{}/complex", kPath), out.complex);

    auto require = [&](std::string const& field, std::string_view key, std::string_view because) {
        if (field.empty())
            coll.emit(DiagnosticCode::C_MissingField, std::format("{}/{}", kPath, key),
                      std::format("'typeSpelling.{}' is required: {}", key, because));
    };
    if (sem.contains("declarators")) {
        constexpr std::string_view why = "the document declares declarators, so its types have that form";
        require(out.pointer, "pointer", why);
        require(out.arrayOpen, "array", why);
        require(out.functionOpen, "function", why);
        require(out.groupOpen, "group", why);
        require(out.parameterSeparator, "parameterSeparator", why);
        require(out.variadic, "variadic", why);
        require(out.emptyPrototype, "emptyPrototype", why);
        require(out.declaratorSeparator, "declaratorSeparator", why);
        require(out.structTag, "tags/Struct", why);
        require(out.unionTag, "tags/Union", why);
        require(out.enumTag, "tags/Enum", why);
        require(out.anonymousTag, "anonymousTag", why);
    }
    if (sem.contains("bitInt")) {
        constexpr std::string_view why = "the document declares bit-precise integers";
        require(out.bitIntSigned, "bitInt/signed", why);
        require(out.bitIntUnsigned, "bitInt/unsigned", why);
    }
    if (reachesComplex) require(out.complex, "complex", "the document's type rows reach the Complex kind");

    // ── the qualifier words: each is its marker's lexeme ──
    json const empty = json::object();
    json const& q = t.contains("qualifiers") ? t.at("qualifiers") : empty;
    std::string const qat = std::format("{}/qualifiers", kPath);
    if (!q.is_object()) {
        coll.emit(DiagnosticCode::C_InvalidSemantics, qat, "'qualifiers' must be an object: volatile, atomic");
        return;
    }
    auto marker = [&](std::string_view key, std::optional<SchemaTokenId> id, std::string_view markerKey,
                      std::string& into) {
        text(q, key, std::format("{}/{}", qat, key), into);
        if (!id.has_value()) {
            if (!into.empty())
                coll.emit(DiagnosticCode::C_ConflictingField, std::format("{}/{}", qat, key),
                          std::format("'{}' is spelled but the document declares no 'semantics.{}'", key, markerKey));
            return;
        }
        if (into.empty()) {
            coll.emit(DiagnosticCode::C_MissingField, std::format("{}/{}", qat, key),
                      std::format("the document declares 'semantics.{}', so its qualifier is spelled here", markerKey));
            return;
        }
        auto const it = lexemes.find(into);
        bool lexesAsMarker = false;
        if (it != lexemes.end())
            for (auto const& m : it->second) lexesAsMarker = lexesAsMarker || m.id.v == id->v;
        if (!lexesAsMarker)
            coll.emit(DiagnosticCode::C_ConflictingField, std::format("{}/{}", qat, key),
                      std::format("'{}' does not lex as the token 'semantics.{}' names: the word must be the "
                                  "marker's own spelling", into, markerKey));
    };
    marker("volatile", volatileMarker, "volatileMarker", out.volatileWord);
    marker("atomic", atomicMarker, "atomicMarker", out.atomicWord);
}

} // namespace dss
