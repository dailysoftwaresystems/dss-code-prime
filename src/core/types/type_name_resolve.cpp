#include "core/types/type_name_resolve.hpp"

#include "core/types/grammar_schema.hpp"   // GrammarSchema::lookupLexeme / semantics()

#include <algorithm>
#include <format>
#include <vector>

namespace dss {

// See the header. This is the grammar loader's former `resolveTypeName` lambda,
// moved whole: the word split, the one-meaning rule, the sorted-multiset match
// and the single-word `builtinTypes` fallback are unchanged, and so are the two
// messages — the loader still emits them at its own JSON path.
std::expected<DataModelTypeRef, std::string>
resolveLanguageTypeName(std::string_view                    name,
                        TypeNameWordKind const&             wordKind,
                        std::span<TypeSpecifierRule const>  typeSpecifiers,
                        std::span<BuiltinTypeMapping const> builtinTypes) {
    DataModelTypeRef out;
    out.name = std::string{name};
    // Split on single spaces (the JSON spelling convention); a run of spaces
    // yields no empty word.
    std::vector<std::string_view> words;
    {
        std::size_t pos = 0;
        while (pos < name.size()) {
            std::size_t const sp = name.find(' ', pos);
            if (sp == std::string_view::npos) {
                words.push_back(name.substr(pos));
                break;
            }
            if (sp > pos) words.push_back(name.substr(pos, sp - pos));
            pos = sp + 1;
        }
    }
    if (words.empty()) {
        return std::unexpected(std::string{"type name must be non-empty"});
    }
    // The typeSpecifiers multiset (each word a declared keyword lexeme with
    // exactly one meaning).
    if (!typeSpecifiers.empty()) {
        std::vector<SchemaTokenId> kinds;
        bool allWordsKnown = true;
        for (std::string_view const w : words) {
            std::optional<SchemaTokenId> const k = wordKind(w);
            if (!k.has_value()) {
                allWordsKnown = false;
                break;
            }
            kinds.push_back(*k);
        }
        if (allWordsKnown) {
            std::sort(kinds.begin(), kinds.end(),
                      [](SchemaTokenId a, SchemaTokenId b) { return a.v < b.v; });
            for (auto const& row : typeSpecifiers) {
                if (row.tokens.size() != kinds.size()) continue;
                bool same = true;
                for (std::size_t n = 0; n < kinds.size(); ++n) {
                    if (row.tokens[n].v != kinds[n].v) {
                        same = false;
                        break;
                    }
                }
                if (!same) continue;
                out.core            = row.core;
                out.coreByDataModel = row.coreByDataModel;
                // FC17.9(e): the long-double axis map rides along — dropping it
                // would silently type the "long double" literal rule at the base
                // core.
                out.coreByLongDoubleFormat = row.coreByLongDoubleFormat;
                // D-LANG-TYPE-IDENTITY-VOCABULARY: and the identity tag.
                out.vocabularyName = row.name;
                return out;
            }
        }
    }
    // Single-word fallback: a builtinTypes text match (core rows only — an
    // extension type has no width semantics).
    if (words.size() == 1) {
        for (auto const& bt : builtinTypes) {
            if (bt.name == words.front() && !bt.extension.has_value()) {
                out.core            = bt.core;
                out.coreByDataModel = bt.coreByDataModel;
                out.vocabularyName  = bt.vocabularyName;
                return out;
            }
        }
    }
    return std::unexpected(std::format(
        "type name '{}' resolves to no 'typeSpecifiers' multiset and no "
        "'builtinTypes' entry", name));
}

std::expected<DataModelTypeRef, std::string>
resolveLanguageTypeName(GrammarSchema const& language, std::string_view name) {
    TypeNameWordKind const wordKind =
        [&language](std::string_view w) -> std::optional<SchemaTokenId> {
        auto const meanings = language.lookupLexeme(w);
        if (meanings.size() != 1) return std::nullopt;
        return meanings.front().id;
    };
    SemanticConfig const& cfg = language.semantics();
    return resolveLanguageTypeName(name, wordKind, cfg.typeSpecifiers,
                                   cfg.builtinTypes);
}

} // namespace dss
