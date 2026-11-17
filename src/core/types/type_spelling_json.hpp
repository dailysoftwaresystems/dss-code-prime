#pragma once

#include "core/substrate/diagnostic_collector.hpp"
#include "core/types/grammar_schema.hpp"   // detail::LexemeTable
#include "core/types/schema_token_interner.hpp"
#include "core/types/type_spelling.hpp"

#include <nlohmann/json.hpp>

#include <optional>

// ══ THE LOAD OF `semantics.typeSpelling` — A TABLE THAT SPELLS EVERY KIND IT IS ASKED FOR ══
//
// INTERNAL to the grammar loader. Whether or not the block is stated, the load
// records whether the document's own rows reach a core type kind
// (`TypeSpelling::documentHasTypes`): the pin over the SHIPPED documents holds
// every document with types to a declared table. The block is OPTIONAL, and its absence is
// meaningful as every optional block of a language document is: that language's
// diagnostics name no types (each site that would name one states its sentence
// without it). A block that is PRESENT is complete or the document is refused,
// by name and pointer:
//   * every key is one this reader reads (a misspelled form is refused, never
//     silently unspelled);
//   * `kinds` names only core kinds, each with a non-empty spelling, and EVERY
//     core kind a `"core"` cell of the document reaches is spelled there — so no
//     core kind's name can reach a diagnostic;
//   * when the document has `declarators`, every declarator form (`pointer`,
//     `array`, `function`, `group`, `parameterSeparator`, `variadic`,
//     `emptyPrototype`, `declaratorSeparator`) and the tag keywords are stated;
//     when it has `bitInt`, both bit-precise patterns; when a row reaches the
//     Complex kind (or marks its type `"complex": true`), the complex pattern;
//   * a kind the document's own rows NAME (a row's `name`, or its `tokens`'
//     lexemes joined by a space) is spelled by one of those names: the table
//     chooses, it never restates (`int` written twice could drift);
//   * each qualifier word is stated exactly when the document has that marker,
//     and LEXES AS the marker's token: the word is the marker's spelling, one
//     meaning, never a second copy free to drift (the schema has no token-to-
//     lexeme lookup, so the word is stated and checked against the lexeme table).

namespace dss {

void readTypeSpelling(nlohmann::json const& doc, nlohmann::json const& sem, detail::LexemeTable const& lexemes,
                      SchemaTokenInterner const& tokens, std::optional<SchemaTokenId> volatileMarker,
                      std::optional<SchemaTokenId> atomicMarker, substrate::DiagnosticCollector& coll,
                      TypeSpelling& out);

} // namespace dss
