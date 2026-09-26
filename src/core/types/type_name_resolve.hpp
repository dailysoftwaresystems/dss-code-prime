#pragma once

#include "core/export.hpp"
#include "core/types/semantic_config.hpp"   // DataModelTypeRef, TypeSpecifierRule, BuiltinTypeMapping
#include "core/types/strong_ids.hpp"        // SchemaTokenId

#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// ══ THE ONE RESOLVER OF A LANGUAGE TYPE NAME ═════════════════════════════════
// (P68 round 9, D-C-LIMITS-H-DEFINES-NINE-OF-THE-STANDARD-MACROS)
//
// A type NAME as a config document spells it — `"int"`, `"unsigned long"`,
// `"long double"` — resolved through the language's OWN vocabulary into the
// load-resolved `DataModelTypeRef` (the core, its per-data-model and
// per-long-double-format overrides, and the vocabulary identity tag). Split on
// single spaces; each word must be a keyword lexeme with exactly ONE meaning;
// the sorted token-kind multiset is looked up in `semantics.typeSpecifiers`
// (C 6.7.2: `unsigned long` ≡ `long unsigned`); a single word that misses
// falls back to a `builtinTypes` text match (a language with no specifier
// table).
//
// ★ ONE OWNER, TWO KINDS OF CALLER. It was the grammar loader's private
// `resolveTypeName` lambda — the path every config-named type takes at LOAD
// (the integer-literal ladder, the promotion rule, the synthesized types, the
// `type-size` predefined macros). A shipped-library descriptor names a type the
// same way AT USE — a lattice-derived constant's `of` (`LONG_MAX` is `of: "long"`)
// — and a second resolver for the same question would be the drift the loader's
// one resolver exists to prevent. Both now call this; the loader keeps its
// diagnostics (it emits the returned message at its own JSON path).
//
// ⓘ PURE: no data model is applied here. The caller selects the core for its
// pair (`DataModelTypeRef::resolveCore`), because the loader must see EVERY
// axis a name can take and a use site sees exactly one.

namespace dss {

class GrammarSchema;

// The token KIND a word of a type name denotes: the keyword lexeme's single
// meaning, or nullopt when the word is no lexeme or has more than one meaning
// (it then names no specifier, and the multiset lookup is skipped). The loader
// answers from the table it is building; a loaded schema from `lookupLexeme`.
using TypeNameWordKind =
    std::function<std::optional<SchemaTokenId>(std::string_view word)>;

// The name's resolved reference, or the message that says why it resolves to
// nothing (the loader emits it as `C_InvalidSemantics`; a use site reports it
// in its own vocabulary). `out.name` is the name as given.
[[nodiscard]] DSS_EXPORT std::expected<DataModelTypeRef, std::string>
resolveLanguageTypeName(std::string_view                   name,
                        TypeNameWordKind const&            wordKind,
                        std::span<TypeSpecifierRule const> typeSpecifiers,
                        std::span<BuiltinTypeMapping const> builtinTypes);

// The same question asked of a LOADED language: its keyword lexemes
// (`GrammarSchema::lookupLexeme`) and its `semantics()` tables.
[[nodiscard]] DSS_EXPORT std::expected<DataModelTypeRef, std::string>
resolveLanguageTypeName(GrammarSchema const& language, std::string_view name);

} // namespace dss
