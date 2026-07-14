#pragma once

#include "core/export.hpp"
#include "core/types/data_model.hpp"   // DataModel (signatureByDataModel resolution)
#include "core/types/named_type_binding.hpp" // NamedTypeBinding (c82 va_list alias thread-through)
#include "core/types/object_format_kind.hpp" // ObjectFormatKind (availability predicate)
#include "core/types/strong_ids.hpp"   // TypeId

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ── LANGUAGE-NEUTRAL shipped-library FFI descriptor reader ───────────────────
//
// Closes D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC. The shipped system "headers"
// (the /usr/include analogue under `src/dss-config/shippedLibs/<platform>/`)
// are NOT per-language source files; they are a NEUTRAL JSON descriptor read
// by this UNIVERSAL reader. The user-facing UX is unchanged and C-faithful —
// `#include <stdio.h>` still works — but the on-disk shipped artifact is a
// language-agnostic schema, not a c-subset `.h`.
//
// Shape (`stdio.json`):
//   { "header": "stdio.h", "standard": "c89",
//     "library": { "pe": "msvcrt.dll", "elf": "libc.so.6",
//                  "macho": "/usr/lib/libSystem.B.dylib" },
//     "symbols": [
//       { "name": "puts", "signature": "fn(ptr<char>) -> i32",
//         "kind": "function", "linkage": "external" }
//     ] }
//
// Model 3 (D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC, 2026-06-09): a descriptor is
// PLATFORM-NEUTRAL — ONE descriptor per header, with a per-OBJECT-FORMAT
// `library` MAP keyed by the canonical `objectFormatKindName` vocabulary
// ("pe"/"elf"/"macho"). The active compilation target's format selects its
// runtime image at resolution time (compile_pipeline), so the SAME descriptor
// serves every target — no per-platform directory selection. This dissolves
// the former per-platform-directory layout + `D-FFI-SHIPPED-LIB-PLATFORM-SELECT`.
//
// PROVENANCE — every shipped symbol traces to header → standard → library →
// (object format). The `header` + `standard` fields make that origin
// FIRST-CLASS data, not just a filename convention: a tool / diagnostic can
// answer "where does `strlen` come from?" from the descriptor alone.
//
//   * `header`    — REQUIRED. The C (or other-language) header this descriptor
//                   represents, e.g. "stdio.h". The angle-include resolver maps
//                   `<stdio.h>` → `stdio.json` by filename; this field is the
//                   authoritative, machine-readable provenance (and should match
//                   the filename stem). A descriptor with no header is a
//                   provenance hole — fail loud.
//   * `standard`  — optional. The language standard the header/symbols belong to
//                   (e.g. "c89" / "c99" / "c11" / "posix"). Provenance only;
//                   carried for tooling, not consumed by lowering.
//   * `library`   — optional per-OBJECT-FORMAT map ("pe"/"elf"/"macho" →
//                   runtime image name). At resolution the active target's
//                   format selects its entry. A map MISSING the active format's
//                   key ⇒ the lowering falls back to the active language's
//                   `externLibraryByFormat[format]` default (so a descriptor MAY
//                   omit a format and inherit the language's default; an entirely
//                   absent map inherits for every format). A key NOT in the
//                   `objectFormatKindFromName` vocabulary is a typo/garbage and
//                   FAILS LOUD on read (F_ShippedLibDescriptorMalformed).
//   * `symbols`   — required, non-empty array.
//   * `name`      — required. The undecorated C identifier (the linker-visible
//                   name is produced downstream by FF4 mangling).
//   * `signature` — required. A hir-text TYPE STRING — a full `fn(...) -> ...`
//                   FnSig for functions, or a value type for an object —
//                   decoded by `dss::parseTypeFromText` into the CALLER's
//                   interner. There is exactly ONE type-text decoder in the
//                   codebase; this reader reuses it (no second grammar).
//   * `kind`      — optional (default "function"). Closed enum: "function" |
//                   "object". Selects the HIR node the lowering synthesizes
//                   (ExternFunction vs ExternGlobal).
//   * `linkage`   — optional (default "external"). Closed enum: "external" |
//                   "weak". Carried for completeness + validated; the FF5
//                   source-declared synthesis path currently emits Strong
//                   linkage uniformly (a shipped symbol is an authoritative
//                   import), so this is a forward-compatible descriptor field.
//
// Agnostic: the reader is a PURE function of (path, interner, typeReg,
// reporter). It branches on NO source language, NO CPU target, NO object
// format — every type is built via the passed `TypeInterner`. It is the same
// reader for every platform's descriptor.

namespace dss {

class DiagnosticReporter;
class TypeInterner;
class TypeRegistry;

namespace ffi {

// What kind of symbol the descriptor declares — selects the HIR extern node
// the CST→HIR lowering synthesizes. Closed enum (descriptor-local; deliberately
// decoupled from the FF1/FF2 `import_surface.hpp` SymbolKind, whose Tls/NoType
// members carry binary-reader semantics that don't apply to a neutral
// descriptor). Default is `Function`.
enum class ShippedSymbolKind : std::uint8_t {
    Function = 0,  // → makeExternFunction (the FnSig lives in `signature`)
    Object   = 1,  // → makeExternGlobal   (a data symbol; `signature` is its type)
};

// Symbol linkage as declared in the descriptor. Closed enum (descriptor-local).
// Default is `External`. Validated on read; carried for forward-compatibility.
enum class ShippedSymbolLinkage : std::uint8_t {
    External = 0,
    Weak     = 1,
};

// One decoded symbol. `signature` is already interned into the caller's
// interner (never InvalidType — a signature that fails to decode is a hard
// error that aborts the whole read, so a returned descriptor's symbols always
// carry valid types).
struct DSS_EXPORT ShippedSymbol {
    std::string          name;
    TypeId               signature;
    ShippedSymbolKind    kind    = ShippedSymbolKind::Function;
    ShippedSymbolLinkage linkage = ShippedSymbolLinkage::External;
    // Optional per-SYMBOL availability — which object-formats this symbol EXISTS
    // on, the symbol-granularity sibling of the header-level `availableObjectFormats`
    // (§ShippedLibDescriptor). EMPTY = every format (back-compat — almost every
    // symbol). A non-empty set restricts: errno's accessor diverges by NAME per
    // format (`__errno_location` is glibc-only ["elf"], `__error` is Darwin-only
    // ["macho"]); the Linux-only fdatasync/fallocate/mremap carry ["elf"]. CRITICAL:
    // DSS imports EVERY declared shipped extern (referenced or not), so a symbol
    // absent on the active format must not be DECLARED there or its import is
    // undefined at load (glibc has no __error; libSystem has no __errno_location).
    // Gated at semantic injection by `activeFormat` (mirrors the header gate) — a
    // format-absent symbol is not injected → not imported → a reference fails loud
    // (undefined name). Keys are the `objectFormatKindFromName` vocabulary; an
    // unknown name fails loud on read. (D-SHIPPED-SYMBOL-PER-TARGET-AVAILABILITY)
    std::vector<std::string> availableObjectFormats;
    // FC16 (D-CSUBSET-NORETURN): optional — TRUE iff this extern never returns
    // (C11 `_Noreturn`: `abort`/`exit`). A shipped extern has no user prototype to
    // carry `_Noreturn`, so the descriptor declares it. The semantic phase threads
    // it onto the injected `SymbolRecord.isNoreturn`, and a DIRECT call is wrapped
    // `Block{ ExprStmt(call), Unreachable }` at HIR lowering — the same treatment a
    // user-declared noreturn function gets. Default false.
    bool noreturn = false;
    // FC17.9(a) (D-CSUBSET-C11-THREADS-HEADER): optional PE-SHIM recipe tag — when
    // non-empty, this symbol is NOT a plain external FFI import but a
    // COMPILER-SYNTHESIZED function whose body `src/mir/merge/synth_threads_shim.cpp`
    // emits over kernel32 primitives (the C11 <threads.h> Win32 shim: the CRT exports
    // no thrd_*). The value is a recipe id from the CLOSED vocabulary
    // (`isKnownSynthesizeRecipe`) and MUST EQUAL this symbol's `name` (a validated
    // invariant — the pe64 synth pass identifies each recipe by its symbol name, so a
    // mismatch would synthesize the wrong body; the loader rejects both an unknown id
    // AND a name-mismatch, closed-vocab fail-loud). Present ONLY on the pe `variants`
    // of a threads symbol (availableObjectFormats:["pe"]); the elf entry carries no tag
    // and is a plain libc FFI import (glibc exports the C11 API from libc.so.6). At
    // CST->HIR a tagged symbol is SKIPPED from extern-import synthesis (kernel32 does
    // not export mtx_lock — the eager-import law) and instead recorded into
    // `CstToHirResult.synthRecipeBySymbol` so HIR->MIR seeds `functionSymbols` (the
    // call lowers to GlobalAddr) and the synth pass supplies the definition. Empty
    // (default) for every ordinary shipped extern. (D-CSUBSET-C11-THREADS-HEADER)
    std::string synthesize;
};

// True iff `id` is a member of the CLOSED pe64 <threads.h> synth-recipe vocabulary
// (the 18 NON-trampoline Cycle-1 recipes; thrd_sleep + thrd_join are elf-FFI-only —
// deferred on pe, see the .cpp vocab list). The SINGLE source of truth shared by the
// descriptor loader (which rejects an unknown `synthesize` value fail-loud —
// F_ShippedLibDescriptorMalformed) AND the driver's multi-CU merged-module recipe
// reconstruction (program.cpp). The pe64 synth pass (`synthesizeThreadsShim`) has the
// matching per-recipe body switch; a vocab id with no switch arm fails loud at synth
// (they cannot silently diverge). Cycle-2 recipes (thrd_create/call_once) are NOT
// here — they are the deferred trampolines. (D-CSUBSET-C11-THREADS-HEADER)
[[nodiscard]] DSS_EXPORT bool isKnownSynthesizeRecipe(std::string_view id);

// One decoded named CONSTANT — the neutral form of a header's object-like
// `#define CHAR_BIT 8` surface (a macro that IS a compile-time constant). A C
// `.h` macro is C-text and would couple the shipped config to C; the neutral
// answer is a typed named constant that the semantic phase injects + the HIR
// folds to a literal, exactly like an enum enumerator. Constrained to INTEGER
// SCALARS (`type.kind` ∈ I8..U128) — a float / pointer / string macro is out of
// scope and fails loud on read (a function-like macro is not a constant at all).
// `value` is the int64 BIT-PATTERN: for an unsigned `type` it is the uint64
// value reinterpreted, and the fold re-reads it per `type`'s signedness — so the
// full unsigned range (e.g. `UINT_MAX`) round-trips losslessly.
//
// PER-TARGET VALUE (plan 25 extension): a constant's VALUE/TYPE can diverge per
// (arch, format) — a per-platform `O_NONBLOCK`. The descriptor declares `variants`
// (each a `when:{arch?,format?}` + its own {value,type}) INSTEAD of a flat
// {value,type}; the decoder selects the variant matching the active target and
// produces THIS same flat shape — no inject-path / fold change. A flat-{value,type}
// constant (no `variants`) keeps single-value behavior.
struct DSS_EXPORT ShippedConstant {
    std::string  name;
    std::int64_t value = 0;
    TypeId       type;     // an integer scalar kind; decoded via parseTypeFromText
};

// One decoded named FLOAT CONSTANT — the float-valued sibling of `ShippedConstant`
// (c52, D-FFI-MATH-INFINITY). The integer `constants` surface is deliberately
// integer-ONLY (a float there still fails loud F_ShippedLibUnsupportedType); a
// header's float-valued object-like macros (`INFINITY`, `M_PI`, `DBL_MAX`) ship
// HERE instead. `type` MUST decode to a FLOAT scalar (F32/F64); `value` is the
// decoded `double` (an F32 constant is stored widened to double and the fold
// narrows it back at materialization). The semantic phase injects each as a named
// constant whose HIR Ref folds to a FLOAT literal — the SAME `isInjectedConstant`
// path as an integer constant, the only difference being the float core/value the
// shared `constantLiteralForSymbol` builder derives.
//
// VALUE ENCODING: JSON has no Infinity/NaN, so the descriptor's `value` is a
// STRING — the special tokens "inf"/"+inf"/"-inf" map to the IEEE-754 ±infinity
// bit patterns, and any other string is a finite float literal parsed by the ONE
// float decoder (`number_decode.hpp`). A finite literal that OVERFLOWS to ±inf
// fails loud (only the explicit "inf" tokens may produce an infinity — never a
// silent overflow).
struct DSS_EXPORT ShippedFloatConstant {
    std::string name;
    double      value = 0.0;
    TypeId      type;     // a FLOAT scalar kind (F32/F64); decoded via parseTypeFromText
};

// One decoded TYPEDEF — the neutral form of a header's `typedef … name;` (e.g.
// `size_t`). The semantic phase injects it as a `DeclarationKind::Type` symbol
// so the name resolves in type position. `type` is any hir-text-decodable type
// (a scalar, a pointer, a struct ref, a function pointer …).
//
// PER-TARGET WIDTH (plan 25 extension): a typedef's TYPE/WIDTH can diverge per
// (arch, format) — a `wchar_t` that is i32 on elf but i16 on pe. The name is
// invariant; the descriptor declares `variants` (each `when:{arch?,format?}` + its
// own `type`) INSTEAD of a flat `type`; the decoder selects the matching variant
// and produces THIS same flat shape. A flat-`type` typedef keeps single-type behavior.
struct DSS_EXPORT ShippedTypedef {
    std::string name;
    TypeId      type;
};

// One decoded preprocessor MACRO — the neutral form of a header's `#define`
// macro that is NOT a compile-time constant (e.g. `assert(e) -> ((void)0)`).
// Unlike `constants` (injected SEMANTICALLY + folded at HIR), a macro is a
// PREPROCESSOR substitution: when `#include <header.h>` is resolved, the
// preprocessor injects each macro into its macro table (via a synthetic
// `#define`) so it expands in the rest of the source BEFORE parse. `params`
// distinguishes the two forms — ABSENT (nullopt) = object-like (`#define X 1`);
// PRESENT (even empty `[]`) = function-like (`#define F() …` / `assert(e) …`).
// `replacement` is the replacement token text (may be empty — a null macro).
// `variadic` marks a trailing `...` catch-all (function-like only).
//
// PER-FORMAT REPLACEMENT (plan 25 extension): a macro's replacement can diverge
// per OBJECT-FORMAT — errno's `(*__errno_location())` on elf vs `(*__error())` on
// macho. The descriptor declares `variants` (each `when:{format}` + its own
// {replacement, params?, variadic?}) INSTEAD of a flat body; the decoder selects
// the variant matching the active format and produces THIS same flat shape.
// FORMAT-ONLY — arch is not threaded into the preprocessor (a macro variant's
// `when` carries `format` alone). A flat-body macro keeps single-replacement behavior.
struct DSS_EXPORT ShippedMacro {
    std::string                             name;
    std::optional<std::vector<std::string>> params;   // nullopt = object-like
    std::string                             replacement;
    bool                                    variadic = false;
};

// One field of a shipped STRUCT — a (name, type) pair. `type` is any
// hir-text-decodable type, spelled as its RESOLVED form (e.g. `i64` for an
// `off_t` field — parseTypeFromText resolves hir-text builtins, NOT descriptor
// typedef names like `off_t`).
//
// c107 (D-FFI-DESCRIPTOR-UNION-OVERLAY): an optional explicit byte `offset` models
// a foreign OVERLAPPING layout (an FFI union as an explicit-offset struct — e.g.
// ULARGE_INTEGER {QuadPart@0, LowPart@0, HighPart@4}). Within one struct it is
// ALL-fields-or-NONE (a mix is F_ShippedLibDescriptorMalformed); offsets may
// overlap and need not be sorted. Absent → the layout engine derives offsets by
// natural alignment (the ordinary case, byte-identical to pre-c107).
struct DSS_EXPORT ShippedField {
    std::string                 name;
    TypeId                      type;
    std::optional<std::uint64_t> offset;
};

// One decoded STRUCT — the neutral form of a header's `struct tag { … };` with
// NAMED fields (e.g. `struct timeval { i64 tv_sec; i64 tv_usec; }`). The semantic
// phase interns the struct type and injects the tag into the TAG namespace plus a
// field scope, so c-subset `struct tag v; v.field` resolves AND lays out at the
// ABI offsets the layout engine DERIVES from the field sizes (the descriptor
// declares names + types only — never explicit offsets). `typeId` is the interned
// struct type (its identity is the name + positional field types).
//
// PER-TARGET LAYOUT (plan 25, D-LANG-PLATFORM-DEPENDENT-PRIMITIVE-WIDTH): a struct's
// byte layout can diverge per (arch, format) — x86-64-linux `struct stat` = 144B,
// arm64-linux = 128B with a different field order, macOS differs again. The CRUX
// (plan-lock-VERIFIED): x86_64 and arm64 `.target.json` have BYTE-IDENTICAL
// `AggregateLayoutParams` and `computeLayout` is purely param-driven (no arch
// branch), so the per-target offset difference comes ENTIRELY from the FIELD LIST.
// Per-target layout is therefore per-target field-LIST SELECTION in the decoder:
// a descriptor declares `variants` (each a `when:{arch?,format?}` + its own field
// list) INSTEAD of a flat `fields`; the decoder selects the variant matching the
// active target and produces THIS same single-`fields`/`typeId` shape — so the
// injection + layout engine are UNCHANGED. A descriptor with flat `fields` (no
// `variants`) keeps single-layout behavior (every existing descriptor untouched).
struct DSS_EXPORT ShippedStruct {
    std::string               name;     // the struct tag, e.g. "timeval"
    std::vector<ShippedField> fields;   // the SELECTED variant's (or flat) fields, decl order
    TypeId                    typeId;   // interned struct type (set on decode)
};

// A decoded shipped-library descriptor. `header` is the authoritative
// provenance (which header these symbols come from); `standard` is optional
// provenance; `library` is a per-OBJECT-FORMAT map ("pe"/"elf"/"macho" → image
// name) that MAY be empty or omit a format (the resolution then falls back to
// the language's per-format default). Keys are validated against the
// `objectFormatKindFromName` vocabulary on read — an unknown key fails loud.
struct DSS_EXPORT ShippedLibDescriptor {
    std::string                header;    // REQUIRED provenance, e.g. "stdio.h".
    std::string                standard;  // optional provenance, e.g. "c89".
    // Per-object-format runtime image, keyed by `objectFormatKindName`
    // ("pe"/"elf"/"macho"). The compile pipeline selects the active target's
    // entry; a missing key inherits `externLibraryByFormat[format]`.
    std::unordered_map<std::string, std::string> library;
    // Optional per-target AVAILABILITY (which object-formats this header EXISTS
    // on), the sibling per-format axis to `library` (which IMAGE per format).
    // EMPTY = available on EVERY format (back-compat — C-standard headers omit
    // it). A non-empty set restricts: a POSIX header carries {"elf","macho"} (not
    // "pe"), so `#include <sys/time.h>` fails loud for a windows-pe target and
    // `__has_include` answers the per-target truth. Keys are the same
    // `objectFormatKindFromName` vocabulary `library` uses (an unknown name fails
    // loud on read). AGNOSTIC: a config-declared set the resolver tests membership
    // against — never an `if (format == ...)`. (D-SHIPPED-HEADER-PER-TARGET-AVAILABILITY)
    std::vector<std::string>   availableObjectFormats;
    // The full neutral surface a header provides. A descriptor must declare AT
    // LEAST ONE of these non-empty (a descriptor that declares NOTHING is a
    // no-op artifact and fails loud); a header may legitimately carry only
    // `constants` (e.g. `<limits.h>`), only `symbols`, or any mix.
    std::vector<ShippedSymbol>   symbols;     // extern functions/objects (linked)
    std::vector<ShippedConstant> constants;   // named integer constants (folded)
    std::vector<ShippedFloatConstant> floatConstants; // named float constants (folded; c52)
    std::vector<ShippedTypedef>  typedefs;    // type aliases (resolved in type pos)
    std::vector<ShippedMacro>    macros;      // preprocessor macros (injected at #include)
    std::vector<ShippedStruct>   structs;     // named-field structs (tag + field scope)
};

// Read + decode the neutral descriptor at `path`, interning each symbol's
// `signature` type into `interner` (+ `typeReg` for `ext<>` kinds). PURE —
// no language/target/format branch.
//
// Returns std::nullopt and emits at least one Error-severity diagnostic on
// ANY failure:
//   * file unreadable / not valid JSON / wrong top-level shape / missing or
//     wrong-typed required key / unknown key / unrecognized kind|linkage enum
//     → `F_ShippedLibDescriptorMalformed`.
//   * a symbol's `signature` failed to decode (`parseTypeFromText` returned
//     InvalidType) → `F_ShippedLibUnsupportedType`. CRITICAL: such a symbol is
//     NEVER returned with InvalidType — the whole read fails so no extern is
//     ever synthesized with an unresolved type.
//
// On success the returned descriptor is fully populated and every symbol's
// `signature` is a valid TypeId in `interner`.
// FC3 c1 `dataModel`: the ACTIVE format's width triple (threaded from
// `analyze()`, which is per-(CU × target)). A symbol MAY carry a
// `signatureByDataModel` object ({"LLP64": "fn(...) -> i32", …} — the
// Model-3 `library`-map shape) whose entry for the active model REPLACES
// the base `signature` (the base text is the LP64-correct form). Every
// declared override must parse — a malformed override fails the read
// even when its model is not the active one (it would otherwise lurk
// until that model's first compile). Unknown model keys fail loud.
// Defaulted for direct-API/unit callers (LP64 = the base-signature
// identity); the semantic analyzer always passes its threaded model.
// Plan-25 `activeTarget` / `activeFormat`: the ACTIVE compile target's
// (arch name, object-format) — the per-target STRUCT-VARIANT selector. A
// `structs` entry that declares `variants` is decoded by selecting the
// variant whose `when:{arch?,format?}` MATCHES (arch == `*activeTarget`,
// format == `objectFormatKindName(*activeFormat)`); EVERY specified key
// must equal the active value (generic string equality — no arch/format
// literal in the engine). >1 variant matches ⇒ fail loud
// (F_ShippedStructVariantAmbiguous). 0 match (variants present) ⇒ the
// struct is NOT injected (a later reference fails loud as an undefined
// type). EAGER: every variant's field list is decoded regardless of which
// is active (a malformed INACTIVE variant fails the whole read on EVERY
// target — anti-lurking, mirrors `signatureByDataModel`). Both default to
// nullopt for direct-API/LSP/unit callers ⇒ no variant selection (a
// flat-`fields` struct decodes exactly as before; a struct that carries
// ONLY `variants` is not injected when no selector is available).
// c82 `namedTypes` (D-FFI-DESCRIPTOR-VA-LIST-TYPE): optional caller-supplied
// NAME → TypeId bindings threaded verbatim into EVERY `parseTypeFromText`
// call this read performs (signatures, per-model overrides, typedefs, struct
// fields, constant types). The semantic analyzer passes its per-CC `va_list`
// binding so a descriptor can spell an ABI-defined C alias (stdio.json's
// `vfprintf(..., va_list)`) while staying arch-NEUTRAL — the alias resolves
// to the SAME TypeId a user-written prototype gets. Content-blind: the reader
// neither knows nor cares what the names mean; empty = pre-c82 behavior.
[[nodiscard]] DSS_EXPORT std::optional<ShippedLibDescriptor>
readShippedLibDescriptor(std::filesystem::path const&    path,
                         TypeInterner&                   interner,
                         TypeRegistry&                   typeReg,
                         DiagnosticReporter&             reporter,
                         DataModel                       dataModel    = DataModel::Lp64,
                         std::optional<std::string_view> activeTarget = std::nullopt,
                         std::optional<ObjectFormatKind> activeFormat = std::nullopt,
                         std::span<NamedTypeBinding const> namedTypes = {});

// Read ONLY the `macros` surface from the neutral descriptor at `path`, WITHOUT a
// TypeInterner. Macros are pure preprocessor token text (no types), so the
// preprocessor — which has no interner — can resolve a `#include <h>` to its
// descriptor's macros at PREPROCESS time (before parse), injecting each as a
// synthetic `#define`. Validates the same provenance gate as
// `readShippedLibDescriptor` (top-level object + non-empty `header`) plus each
// macro entry. Returns an EMPTY vector when the descriptor declares no `macros`
// (a typed-surface-only descriptor — the common case); returns std::nullopt and
// emits `F_ShippedLibDescriptorMalformed` on ANY malformed input. The typed
// surfaces (symbols/constants/typedefs) are NOT read here — the semantic phase
// reads those via `readShippedLibDescriptor`.
//
// `activeFormat` (plan-25 extension): a macro entry may carry per-FORMAT
// `variants` (each `when:{format}` + its own replacement — the errno
// `__errno_location`/elf vs `__error`/macho case). The active object-format
// selects the matching variant; macros are FORMAT-ONLY (arch is not threaded
// into the preprocessor), so this is the only selector. nullopt (a test caller
// / no target) ⇒ a variants-only macro is not injected; a flat macro is
// unaffected. The single production caller (SynthBuilder::build) passes its
// active format.
[[nodiscard]] DSS_EXPORT std::optional<std::vector<ShippedMacro>>
readShippedLibMacros(std::filesystem::path const&    path,
                     DiagnosticReporter&             reporter,
                     std::optional<ObjectFormatKind> activeFormat = std::nullopt);

// Read ONLY the `availableObjectFormats` set from the descriptor at `path`,
// WITHOUT a TypeInterner — the FRONT-END per-target availability gate (the
// preprocessor `__has_include` + the import resolver's `#include`). Returns the
// set of object-format names the header exists on (EMPTY ⇒ available on every
// format = back-compat); std::nullopt on a broken JSON / malformed availability.
// The caller tests membership of the active target's `objectFormatKindName`.
[[nodiscard]] DSS_EXPORT std::optional<std::vector<std::string>>
readShippedLibAvailability(std::filesystem::path const& path,
                           DiagnosticReporter&          reporter);

// Read ONLY the `typedefs[].name` list from the descriptor at `path`, WITHOUT a
// TypeInterner — the PARSE-TIME cast-vs-call ORACLE (D-CSUBSET-SHIPPED-TYPEDEF-CAST-PARSE).
// Shipped typedefs are injected SEMANTICALLY (post-parse), so the parser's binder
// sketch never sees `size_t` as a TYPE NAME and parses `(size_t)(expr)` as a CALL.
// The post-parse typedef-resolution reparse (compilation_unit.cpp) seeds these
// NAMES as parse-time global types so the reparse commits the cast. Only the names
// are needed (not the decoded `type`), so no interner — mirrors
// readShippedLibAvailability. LENIENT: malformed entries are skipped (the SEMANTIC
// read owns strict typedef validation — this must be no STRICTER). std::nullopt on
// a broken JSON; EMPTY ⇒ the descriptor declares no typedef surface.
[[nodiscard]] DSS_EXPORT std::optional<std::vector<std::string>>
readShippedLibTypedefNames(std::filesystem::path const& path,
                           DiagnosticReporter&          reporter);

// True iff a header carrying availability set `availableObjectFormats` is
// available on object-format `fmt`. EMPTY set ⇒ available on EVERY format
// (back-compat). The SINGLE membership predicate shared by the semantic
// `#include` availability gate (semantic_analyzer) AND the preprocessor
// consumers (`__has_include` + the macro-splice) below, so all three can never
// disagree — the FC15c funnel principle applied to per-target availability.
[[nodiscard]] DSS_EXPORT bool objectFormatInAvailabilitySet(
    std::span<std::string const> availableObjectFormats, ObjectFormatKind fmt);

// True iff the shipped header whose descriptor is at `descriptorPath` is
// available on `fmt`. Reads `availableObjectFormats` interner-free
// (`readShippedLibAvailability`) then applies `objectFormatInAvailabilitySet`.
// A MALFORMED descriptor ⇒ available (the header EXISTS; its malformedness is
// surfaced by the macros / typed reads on the same descriptor, NOT
// double-reported here). The preprocessor `__has_include` + macro-splice gate.
[[nodiscard]] DSS_EXPORT bool shippedHeaderAvailableForFormat(
    std::filesystem::path const& descriptorPath, ObjectFormatKind fmt);

} // namespace ffi
} // namespace dss
