#pragma once

#include "core/export.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_id.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// ══ THE ONE SPELLING OF A TYPE INSIDE A LANGUAGE'S DIAGNOSTIC ════════════════
//
// A diagnostic that names a type names it through `spellType` — never a core
// kind's name (`I32`), never the DSS type text (`ptr<i32>`), never a renderer
// of its own. Every word and every form comes from the language document's
// `semantics.typeSpelling` table; the engine knows only the SHAPE of a
// declarator: a prefix form (a pointer) is written before the inner text, a
// suffix form (an array, a function) after it, and an inner prefix met by a
// suffix is grouped. A second language spells its types with its own table,
// through the same code. ([[D-DIAG-A-DIAGNOSTIC-QUOTES-ITS-OPERAND-WHOLE]] found
// the class: the analyzer had six renderers, two of them printing core kind
// names into C diagnostics.)
//
// WHAT A TYPE CAN SAY. The lattice carries `volatile` and `_Atomic` as a
// qualifier skin's bits, so those two are spelled. `const` and `restrict` are no
// bits of the lattice (core_type.hpp, QualBit), so no TypeId carries them and no
// spelling can show them. A function declared without a prototype has the same
// TypeId as one with an empty prototype — that is a fact of the SYMBOL — so the
// caller that knows says so through `SpellOptions`.
//
// THE ORDER for one base type: the vocabulary name the type carries (the
// lattice's tag: `long`, `unsigned long`, `long double`, `__int128` …), else the
// table's spelling of its core kind; a structure, union or enumeration as its
// tag keyword and tag (the table's anonymous form when it has none); a
// bit-precise integer and a complex type from the table's patterns.
//
// A kind the document's OWN rows reach and the table does not spell is refused
// when the document LOADS (type_spelling_json.hpp). A kind reached from outside
// those rows — a shipped or user descriptor's `f16` constant or parameter, in a
// language with no half-precision type — has no word in the language: the
// renderer answers "not spelled" (nullopt) for it and for every type built on
// it, and the sentence takes its form without a type. Never the engine's kind
// name, and never a stop: a descriptor is input, and the renderer once aborted
// the compiler on one.

namespace dss {

class TypeInterner;

// The language document's table (`semantics.typeSpelling`). Every field is
// stated by the document; an empty string is "not spelled".
struct DSS_EXPORT TypeSpelling {
    bool declared = false;   // the document states a typeSpelling table
    // The document's own rows reach a core type kind (a `"core"` / `"coreByDataModel"` cell), so its diagnostics
    // have types to name. Read by the loader whether or not the table is declared: the ONE answer to "does this
    // language have types", which the pin over the SHIPPED documents holds against `declared` (ruling 621: an
    // absent table must not fail toward silence on a language that has types).
    bool documentHasTypes = false;

    // The spelling of each core kind the language's types reach untagged.
    std::array<std::string, static_cast<std::size_t>(TypeKind::Count_)> kinds{};

    std::string pointer;              // a prefix form: `*`
    std::string arrayOpen;            // a suffix form: `[`
    std::string arrayClose;           //                `]`
    std::string functionOpen;         // a suffix form: `(`
    std::string functionClose;        //                `)`
    std::string parameterSeparator;   // `, `
    std::string variadic;             // `...`
    std::string emptyPrototype;       // what an empty prototype lists: `void`
    std::string groupOpen;            // a prefix met by a suffix: `(`
    std::string groupClose;           //                           `)`
    std::string declaratorSeparator;  // between the specifiers and a declarator: ` `
    bool        suffixJoinsSpecifiers = false;   // an array suffix follows the specifiers with no separator
    std::string bitIntSigned;         // a pattern, `{}` the width: `_BitInt({})`
    std::string bitIntUnsigned;       // `unsigned _BitInt({})`
    std::string complex;              // a pattern, `{}` the element: `_Complex {}`
    std::string structTag;            // `struct`
    std::string unionTag;             // `union`
    std::string enumTag;              // `enum`
    std::string anonymousTag;         // a tag that has no name: `(unnamed)`
    std::string volatileWord;         // the lexeme of the document's volatile marker
    std::string atomicWord;           // the lexeme of the document's atomic marker

    [[nodiscard]] std::string const& kind(TypeKind k) const noexcept {
        return kinds[static_cast<std::size_t>(k)];
    }
};

struct SpellOptions {
    // The outermost function form was declared without a prototype: its empty
    // parameter list is written as nothing, not as `emptyPrototype`.
    bool declaredWithoutPrototype = false;
};

// `t` as the language's diagnostics name it; nullopt when the document states no
// table, or when the table has no word for a piece of `t`.
[[nodiscard]] DSS_EXPORT std::optional<std::string> spellType(TypeInterner const& in, TypeId t,
                                                              TypeSpelling const& table,
                                                              SpellOptions options = {});

// A type known only by its core kind and its vocabulary tag (a shipped descriptor's declared type, a predefined
// macro's): the tag when it carries one, else the table's word for the kind — the first two steps of
// `spellType`'s order for a base type. nullopt when the document states no table (its diagnostics name no
// types), or when its table has no word for the kind (one only a descriptor reaches), as in `spellType`.
[[nodiscard]] DSS_EXPORT std::optional<std::string> spellCoreKind(TypeKind k, std::string_view vocabularyName,
                                                                  TypeSpelling const& table);

} // namespace dss
