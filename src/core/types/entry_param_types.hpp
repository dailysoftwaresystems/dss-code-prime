#pragma once

// THE ONE CORRESPONDENCE BETWEEN AN ENTRY PARAMETER SHAPE AND A TYPE (P69 fold 3f, lane `xa`, the GUI startups).
//
// An entry verb's slots each state a parameter SHAPE (`entry_shape.hpp`): "an int", "a pointer to a pointer to an
// 8-bit unit", "a pointer to an opaque object". Two readers turn a shape and a type into each other:
//   * the semantic tier CLASSIFIES a definition's parameter type into a shape, to match the definition against its
//     name's language rows (`S_EntryShapeNotDeclared` when none matches);
//   * the synthesized pre-main init BUILDS the type of each shape, to call an entry with its verb's signature (an
//     object's entry carries no type, and a definition whose signature is none of its rows is called with the
//     richest row's).
// They were two functions in two files -- an inline lambda in the semantic tier, a static builder in the synth -- and
// a shape the one learned and the other did not would have classified a definition the init then called with a
// different type. Here they are inverse functions of one table, and tests/link/test_gui_startups.cpp pins the round
// trip for every declarable shape.
//
// STRUCTURAL, never semantic: the vocabulary says "pointer to a 16-bit unit", never "the wide command line", so
// nothing here assumes how a platform spells a wide character -- a 32-bit-wchar_t platform's `wchar_t *` simply
// never classifies as `ptr-u16`. A qualifier is no part of a shape: the interner records no `const`, and `kind`
// sees through a `volatile` or `_Atomic` skin, so `char const *` classifies as `ptr-char` as `char *` does.

#include "core/export.hpp"
#include "core/types/entry_shape.hpp"                  // EntryParamShape
#include "core/types/type_lattice/core_type.hpp"       // TypeId
#include "core/types/type_lattice/type_interner.hpp"   // TypeInterner

#include <optional>

namespace dss {

// The shape of one parameter type, or nullopt when it is none of the declarable shapes:
//   `int`                                        -> i32
//   a pointer to a pointer to `char` / an 8-bit  -> ptr-ptr-char
//   a pointer to a pointer to a 16-bit unsigned  -> ptr-ptr-u16
//   a pointer to `char` / an 8-bit integer       -> ptr-char
//   a pointer to a 16-bit unsigned               -> ptr-u16
//   a pointer to void, a struct or a union       -> ptr-opaque (a handle: `HINSTANCE` is `struct HINSTANCE__ *`
//                                                   under <windows.h>'s STRICT and `void *` without it)
// Any other pointer (to an `int`, to a function, to a pointer to `int`) is no shape.
[[nodiscard]] DSS_EXPORT std::optional<EntryParamShape>
classifyEntryParam(TypeInterner const& intern, TypeId type);

// The type a parameter of `shape` has when the startup calls an entry with its verb's signature: the inverse of
// `classifyEntryParam` (an opaque pointer is built as a pointer to void). InvalidType for the `none` sentinel, which
// no loaded row holds.
[[nodiscard]] DSS_EXPORT TypeId typeOfEntryParamShape(TypeInterner& intern, EntryParamShape shape);

}  // namespace dss
