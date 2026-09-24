#pragma once

// ── Program-entry vocabulary (D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE) ─────────────
//
// The closed vocabulary for "what shape is a program entry, and what has to be
// materialized before it is called". EXTRACTED into this leaf header for the
// same reason `enum_name_table.hpp` was: the vocabulary is now read from BOTH
// sides of an include cycle.
//
//   * `semantic_config.hpp` (the SOURCE LANGUAGE's declaration rules) owns the
//     `name -> (signature, verb)` mapping — `DeclarationRule::entryFunctions`.
//   * `object_format_schema.hpp` (the LINKER FORMAT) owns the realized VERB set
//     — `ObjectFormatData::entryVerbs`.
//
// `grammar_schema.hpp` includes `semantic_config.hpp` and `target_schema.hpp`
// includes `grammar_schema.hpp`, so the vocabulary CANNOT live in
// `target_schema.hpp` where it started — `semantic_config.hpp` including that
// header is a cycle. This file depends on nothing but the enum-table substrate.
//
// ★★★ THE TWO OWNERS ARE NOT TWO COPIES OF ONE FACT, and that distinction is
// the whole design:
//
//   * WHICH SIGNATURES SPELL AN ENTRY is a SOURCE-LANGUAGE fact. `main` may be
//     written `int main(void)` or `int main(int, char**)`; MSVC's `wmain` is
//     spelled `int wmain(int, wchar_t**)` and MEANS the wide argument vector.
//     A format file cannot know that, because the spelling is C's, not the
//     loader's. The language config is therefore the SINGLE OWNER of the
//     signature, and the per-definition check against it is a SINGLE-TU fact
//     with a real source span (see `S_EntryShapeNotDeclared`).
//   * WHICH VERBS CAN ACTUALLY BE REALIZED is a LINKER-FORMAT fact. Only a
//     Windows image can produce a wide argument vector, because only its CRT
//     publishes one. The format config is the SINGLE OWNER of that set.
//
// Program-entry CANDIDATE SELECTION is the INTERSECTION the engine computes:
// a defined function is an entry candidate iff its name matches a declared
// entry row AND its signature matches that row AND the row's verb is in the
// active format's realized set. That intersection is why `wmain` is a
// candidate on `pe64-x86_64-windows-exec` and NOT on any ELF or Mach-O format,
// with NO format-identity branch anywhere in shared substrate — the engine
// reads two declared sets and intersects them.
//
// ⛔ WHY THIS IS DATA AND NOT `if (arity == 3) error` (or `if (format == pe)`).
// A refusal is code too: a hardcoded arity check is the SAME defect class as an
// `msvcrt.dll` literal in `src/mir` — a platform fact spelled in shared
// substrate — and it makes "should envp be supported" a C++ question forever
// instead of a config edit. It also cannot express the platforms that exist:
// ★ Darwin calls an `LC_MAIN` entry with FOUR values, `(argc, argv, envp,
// apple)`, already in the argument registers, which a declared table states and
// an arity check cannot.
//
// WHAT THIS VOCABULARY FIXES (MEASURED 2026-08-10 on `3e86a187`, `build-dbg`):
// `int main(int argc, char **argv, char **envp)` compiled **rc=0 with ZERO
// diagnostics** on BOTH `pe64-x86_64-windows-exec` and
// `elf64-x86_64-linux-exec`, and the emitted programs FAULT — observed
// `argc=3 argv=0x…7D10 envp=0x0000000000000004`, dereferencing envp gives
// `0xC0000005` on pe and SIGSEGV (rc=139) on elf. gcc compiles the identical
// source and it works. (The ORIGIN of the `0x4` is UNDETERMINED; an earlier
// probe's "it is argc left in a register" explanation is REFUTED — that run had
// argc=3 with envp still 0x4. Do not write a mechanism claim about it.)
//
// C23 5.1.2.2.1 lets an implementation accept `main` "in some other
// IMPLEMENTATION-DEFINED MANNER", so supporting 3-param main is conforming AND
// refusing it is conforming — ★ but ACCEPTING IT AND FAULTING is neither, and
// that was the shipped behaviour. C23 3.4.1 then defines implementation-defined
// behavior as behavior each implementation DOCUMENTS, so the DECLARED PAIR —
// the language file's `entryFunctions` mapping and each format file's
// `entryVerbs` set, each carrying its `$…Comment` — IS that documentation
// artifact. Neither half documents the accepted set alone; the accepted set is
// their intersection, and both comments say so and name the other file.
//
// ★ THE ENVIRONMENT FORMS ARE SUPPORTED, NOT REFUSED (the divergence this header
// used to record is closed). gcc, clang AND MSVC all accept
// `int main(int, char**, char**)`, MSVC also the 3-parameter `wmain`, and
// Darwin's loader calls `main(argc, argv, envp, apple)`; the refusal that
// replaced the fault was the SAFETY half, and these verbs are the SUPPORT half.
// Each format realizes them its own declared way — a CRT's environment
// accessors (pe), the entry-stack vector after argv's terminator (elf), or the
// loader's argument registers (Mach-O) — and a format that realizes none of
// them still refuses the shape, by the same intersection as before.

#include "core/export.hpp"
#include "core/types/enum_name_table.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dss {

// A single entry parameter, classified STRUCTURALLY. These are TYPE SHAPES, not
// semantic roles: the vocabulary says "pointer to pointer to a 16-bit unsigned"
// and NOT "the wide argv", precisely so nothing in shared substrate has to
// assume which platform spells a wide character how. MEANING is carried by the
// row's materialization verb, which the language declares alongside the shape.
enum class EntryParamShape : std::uint8_t {
    None       = 0,  // default-constructed sentinel; loader rejects "none"
    I32        = 1,  // `int`            — C23 requires main's argc to be int
    PtrPtrChar = 2,  // `char**`         — ptr → ptr → char/i8
    PtrPtrU16  = 3,  // `wchar_t**` on a 16-bit-wchar platform (ptr → ptr → u16)
};

inline constexpr EnumNameTable<EntryParamShape, 4> kEntryParamShapeTable{{{
    { EntryParamShape::None,       "none"         },
    { EntryParamShape::I32,        "i32"          },
    { EntryParamShape::PtrPtrChar, "ptr-ptr-char" },
    { EntryParamShape::PtrPtrU16,  "ptr-ptr-u16"  },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kEntryParamShapeTable);

[[nodiscard]] constexpr std::string_view
entryParamShapeName(EntryParamShape p) noexcept {
    return kEntryParamShapeTable.name(p);
}
[[nodiscard]] constexpr std::optional<EntryParamShape>
entryParamShapeFromName(std::string_view s) noexcept {
    return kEntryParamShapeTable.fromName(s);
}

// The shapes a language file may actually DECLARE — the table minus the
// sentinel, which `fromName("none")` resolves but every loader then refuses.
// A loader's "accepted: …" half renders THIS, never a retyped list: the
// retyped one advertises whatever the enum looked like on the day it was
// typed (D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET). The
// `3` is checked by `namesWhere` at compile time.
//
// ★ AND THE `+ 1` BELOW IS THE OTHER HALF OF THAT CHECK, NOT DECORATION.
// D-CORE-NAMESWHERE-LITERAL-COUNT-IS-BLIND-TO-A-SECOND-SENTINEL:
// `namesWhere<M>` only ever compares `M` against the rows the predicate
// ACCEPTS, so a SECOND undeclarable shape moves nothing it can see. ✔MEASURED
// 2026-08-23 in a worktree with `g++ -std=c++23 -fsyntax-only -I src`: a fifth
// row plus the widened `isDeclarableEntryParamShape` that rejects it COMPILED
// CLEAN before this assert existed — the projection stayed a correct list of
// three while the SENTENCE this header tells about itself ("the table minus
// THE sentinel") had quietly become false, and a loader refusing the second
// undeclarable spelling would have had no set to name it against. The assert
// relates the table's own row total to this projection's literal — two numbers
// with DIFFERENT OWNERS, so it is not the tautology a `rows.size() - 1`
// spelling would have been
// (D-CORE-NAMESWHERE-COUNT-DERIVED-FROM-THE-TABLE-IS-A-TAUTOLOGY).
[[nodiscard]] constexpr bool
isDeclarableEntryParamShape(EntryParamShape p) noexcept {
    return p != EntryParamShape::None;
}
inline constexpr auto kDeclarableEntryParamShapeNames =
    namesWhere<3>(kEntryParamShapeTable, isDeclarableEntryParamShape);
static_assert(kEntryParamShapeTable.rows.size()
                  == kDeclarableEntryParamShapeNames.size() + 1,
              "kEntryParamShapeTable must have exactly ONE undeclarable row "
              "(the 'none' sentinel) — a second one leaves `namesWhere`'s "
              "literal count matching while every language-file 'accepted: …' "
              "message silently stops naming the set the loader enforces");

// The entry's RETURN shape. Only `i32` is declared, and the omission of `void`
// is the point: C23 5.1.2.2.1 says main's return type "shall be int", so
// `void main()` — accepted by MSVC, refused by `gcc -pedantic-errors` — falls
// out of the SAME declared-set check as any undeclared parameter list, with NO
// second mechanism. Adding a `void` row to a LANGUAGE file is how a source language
// would opt in, and it would need a materialization verb to go with it.
enum class EntryReturnShape : std::uint8_t {
    None = 0,  // default-constructed sentinel; loader rejects "none"
    I32  = 1,  // `int`
};

inline constexpr EnumNameTable<EntryReturnShape, 2> kEntryReturnShapeTable{{{
    { EntryReturnShape::None, "none" },
    { EntryReturnShape::I32,  "i32"  },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kEntryReturnShapeTable);

[[nodiscard]] constexpr std::string_view
entryReturnShapeName(EntryReturnShape r) noexcept {
    return kEntryReturnShapeTable.name(r);
}
[[nodiscard]] constexpr std::optional<EntryReturnShape>
entryReturnShapeFromName(std::string_view s) noexcept {
    return kEntryReturnShapeTable.fromName(s);
}

// HOW a matched shape's parameters are MATERIALIZED. This is the verb the
// engine dispatches on — the same discipline `processArgs.mechanism` already
// uses, and the reason the wide-vs-narrow choice stays a property of the
// RESOLVED ENTRY'S SIGNATURE rather than of the format.
//
// ★ THE VERB IS ALSO THE FORMAT-INTERSECTION KEY, which is the job it did not
// have before this cycle. The language declares `wmain -> argc-wargv`; a
// format declares the verbs it realizes; `argc-wargv` appears in
// `pe64-x86_64-windows-exec`'s set and in no other shipped format's, so
// `wmain` is a program-entry candidate on Windows only. Before the verb was
// the key, candidate selection matched entry NAMES alone and was therefore
// FORMAT-BLIND — MEASURED: `int wmain(int, unsigned short**)` with no `main`,
// targeting `elf64-x86_64-linux-exec`, was accepted AS THE LINUX ENTRY by the
// name scan and then refused by a shape gate whose message asserted `wmain`
// was the entry and prescribed adding a Linux config row to make it so.
//
// ★ THE TWO AXES ARE NOT REDUNDANT. This verb says WHICH arguments the shape
// needs. `processArgs.mechanism` says HOW this format obtains them (from the
// entry stack, from the CRT's accessors, or not at all because the loader
// already put them in the argument registers). `argc-argv` under
// `stack-vector` is materialized by the entry trampoline; under
// `crt-argv-accessors` by the synthesized init; under no mechanism at all by
// dyld, before DSS code runs. The ENVIRONMENT verbs follow the same split: the
// CRT's environment accessors in the synthesized init, a synthesized init that
// computes envp from the trampoline's (argc, argv) on the entry stack, and dyld's
// own registers — and which verbs a mechanism can realize at all is refused at
// format load when it cannot (`ObjectFormatData::validate`).
enum class EntryMaterialization : std::uint8_t {
    None              = 0,  // NOT a sentinel here — see below. Loader ACCEPTS "none".
    ArgcArgv          = 1,  // materialize (argc, narrow argv)
    ArgcWargv         = 2,  // materialize (argc, wide argv)
    ArgcArgvEnvp      = 3,  // materialize (argc, narrow argv, narrow envp)
    ArgcWargvWenvp    = 4,  // materialize (argc, wide argv, wide envp) — MSVC's 3-param wmain
    ArgcArgvEnvpApple = 5,  // materialize (argc, argv, envp, apple) — Darwin's LC_MAIN delivery
};

// ⚠ `None` IS A REAL, DECLARABLE VERB ON THIS ENUM, unlike the sibling
// vocabularies above where zero is the invalid sentinel. `fn() -> i32` (which
// C23's `()`≡`(void)` makes the resolution of `int main()`) needs NO
// materialization, and that is an ANSWER rather than an omission. The loaders
// therefore accept `"none"` here — and the price is that a default-constructed
// `EntryFunctionShape` looks like a legal no-arg row, which is why
// `EntryFunctionShape::returns` carries the sentinel duty for the struct as a
// whole.
inline constexpr EnumNameTable<EntryMaterialization, 6>
kEntryMaterializationTable{{{
    { EntryMaterialization::None,              "none"                 },
    { EntryMaterialization::ArgcArgv,          "argc-argv"            },
    { EntryMaterialization::ArgcWargv,         "argc-wargv"           },
    { EntryMaterialization::ArgcArgvEnvp,      "argc-argv-envp"       },
    { EntryMaterialization::ArgcWargvWenvp,    "argc-wargv-wenvp"     },
    { EntryMaterialization::ArgcArgvEnvpApple, "argc-argv-envp-apple" },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kEntryMaterializationTable);

[[nodiscard]] constexpr std::string_view
entryMaterializationName(EntryMaterialization m) noexcept {
    return kEntryMaterializationTable.name(m);
}
[[nodiscard]] constexpr std::optional<EntryMaterialization>
entryMaterializationFromName(std::string_view s) noexcept {
    return kEntryMaterializationTable.fromName(s);
}

// ── WHAT EACH VERB MATERIALIZES — THE ONE OWNER ──────────────────────────────
//
// The parameter shapes a verb's emitter hands the entry, in order. Three readers,
// ONE table:
//   * the language loader's VERB ⟺ SIGNATURE coherence check (an `entryFunctions`
//     row whose signature is not exactly its verb's list is refused at load);
//   * the synthesized pre-main inits (`realizeEntryShape`), which read WIDE vs
//     NARROW off the second shape and whether an environment is passed off the
//     count, instead of re-classifying the verb;
//   * the entry trampoline, which saves across the static initializers exactly
//     the argument registers a loader-delivered entry form fills (the LARGEST
//     count among the format's own `entryVerbs`, on a format whose loader puts
//     the arguments in registers).
// A new verb that forgets its row here is a compile error (the switch has no
// `default:`), never a permissive empty list.
inline constexpr std::array<EntryParamShape, 2> kEntryVerbArgcArgvParams{
    EntryParamShape::I32, EntryParamShape::PtrPtrChar};
inline constexpr std::array<EntryParamShape, 2> kEntryVerbArgcWargvParams{
    EntryParamShape::I32, EntryParamShape::PtrPtrU16};
inline constexpr std::array<EntryParamShape, 3> kEntryVerbArgcArgvEnvpParams{
    EntryParamShape::I32, EntryParamShape::PtrPtrChar, EntryParamShape::PtrPtrChar};
inline constexpr std::array<EntryParamShape, 3> kEntryVerbArgcWargvWenvpParams{
    EntryParamShape::I32, EntryParamShape::PtrPtrU16, EntryParamShape::PtrPtrU16};
// Darwin's fourth value, `apple`, is a NULL-terminated vector of C strings
// (`executable_path=…` first) — the same shape as argv and envp.
inline constexpr std::array<EntryParamShape, 4> kEntryVerbArgcArgvEnvpAppleParams{
    EntryParamShape::I32, EntryParamShape::PtrPtrChar, EntryParamShape::PtrPtrChar,
    EntryParamShape::PtrPtrChar};

[[nodiscard]] constexpr std::span<EntryParamShape const>
entryVerbParams(EntryMaterialization m) noexcept {
    switch (m) {
    case EntryMaterialization::None:              return {};
    case EntryMaterialization::ArgcArgv:          return kEntryVerbArgcArgvParams;
    case EntryMaterialization::ArgcWargv:         return kEntryVerbArgcWargvParams;
    case EntryMaterialization::ArgcArgvEnvp:      return kEntryVerbArgcArgvEnvpParams;
    case EntryMaterialization::ArgcWargvWenvp:    return kEntryVerbArgcWargvWenvpParams;
    case EntryMaterialization::ArgcArgvEnvpApple: return kEntryVerbArgcArgvEnvpAppleParams;
    }
    return {};
}

// Derived views of the table, never a second classification of the verb.
[[nodiscard]] constexpr bool entryVerbIsWide(EntryMaterialization m) noexcept {
    auto const p = entryVerbParams(m);
    return p.size() >= 2 && p[1] == EntryParamShape::PtrPtrU16;
}
[[nodiscard]] constexpr bool entryVerbPassesEnvironment(EntryMaterialization m) noexcept {
    return entryVerbParams(m).size() >= 3;
}

static_assert(entryVerbParams(EntryMaterialization::None).empty());
static_assert(entryVerbParams(EntryMaterialization::ArgcArgvEnvpApple).size() == 4);
static_assert(entryVerbIsWide(EntryMaterialization::ArgcWargvWenvp)
              && !entryVerbIsWide(EntryMaterialization::ArgcArgvEnvp));

// One declared entry row: the NAME the source language spells this entry with,
// the signature that spelling must have, and the verb that realizes it.
//
// ★ `name` LIVES ON THE ROW rather than the row living under a name, because a
// name maps to SEVERAL signatures — `main` is both `fn() -> i32` (verb `none`)
// and `fn(i32, ptr-ptr-char) -> i32` (verb `argc-argv`). The JSON spells the
// mapping as an object keyed by name whose value is the array of that name's
// shapes; the loader flattens it to a row list, so the in-memory form is a flat
// scan and the config form is the mapping.
struct DSS_EXPORT EntryFunctionShape {
    std::string                  name;
    EntryReturnShape             returns = EntryReturnShape::None;
    std::vector<EntryParamShape> params;
    EntryMaterialization         verb    = EntryMaterialization::None;

    [[nodiscard]] bool
    sameSignatureAs(EntryFunctionShape const& o) const noexcept {
        return returns == o.returns && params == o.params;
    }
};

// Render a signature the way the declared table spells it — `fn(i32,
// ptr-ptr-char) -> i32`. Used by BOTH halves of every entry diagnostic (the
// observed signature and each declared row), so the reader compares like with
// like instead of one C++-rendered string against one JSON-rendered one.
// Deliberately EXCLUDES the name and the verb: callers that want those print
// them around this, and the two-line "observed vs declared" comparison stays
// aligned.
[[nodiscard]] inline std::string
entrySignatureSpelling(EntryReturnShape                    returns,
                       std::vector<EntryParamShape> const& params) {
    std::string out = "fn(";
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (i) out += ", ";
        out += entryParamShapeName(params[i]);
    }
    out += ") -> ";
    out += entryReturnShapeName(returns);
    return out;
}

[[nodiscard]] inline std::string
entrySignatureSpelling(EntryFunctionShape const& s) {
    return entrySignatureSpelling(s.returns, s.params);
}

}  // namespace dss
