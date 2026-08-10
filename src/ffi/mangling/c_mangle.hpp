#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "link/object_format_schema.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

// Plan 11 FF4 — C name mangling (per-platform underscoring).
// Symmetric apply/unapply functions transform a canonical C
// identifier to/from its per-platform linker-visible decorated
// form.
//
// ★ THE RULE IS CONFIG, NOT CODE (D-FFI-CMANGLING-RULE-NOT-CONFIG-DRIVEN,
// step C4). Every function here takes a `CSymbolDecorationScheme` — the verb
// the ACTIVE OBJECT FORMAT declares in its `.format.json` under
// `cSymbolDecoration.scheme` — and NOT an `ObjectFormatKind`. A closed C++
// table keyed on the format identity (`kCManglingRules`) used to live in the
// .cpp; it was one of two owners of a single per-format fact, the other being
// the descriptors' own `importMangledName` literals, with nothing forcing the
// two to agree.
//
// Passing the declared verb rather than the identity is deliberate and is the
// stronger half of the fix: this header cannot branch on "which format is
// this?" because it is never told. Callers obtain the scheme from the loaded
// schema (`format.cSymbolDecoration().scheme`), which `validate()` guarantees
// is a real scheme and never the `Unspecified` sentinel.
//
// The two schemes that ship today:
//   * `none`               → the linker-visible name IS the C identifier
//                            (ELF/System V, PE/COFF x64, WASM's import
//                            namespace, SPIR-V's decoration-based linkage).
//   * `leading-underscore` → `_printf` — Apple's convention, which has no
//                            bitness axis (32- and 64-bit Mach-O agree), which
//                            is exactly why it is a per-FORMAT declaration.
// 32-bit PE cdecl `_func` / stdcall `_func@N` arrive as their own scheme
// enumerators WITH their engine arms, never as a re-spelling of these.
//
// Source-language agnostic: this is C-name mangling (FF4's plan
// row). C++/Rust mangling is post-v1 (FF7/FF8) and lives in its
// own file.

namespace dss::ffi {

// Decorate a canonical C identifier with the per-platform
// linker-visible prefix/suffix. Returns the decorated name.
// Pure function — caller assigns the result back into
// `ImportSurface::mangledName` (FFI ingestion side) or feeds it
// to the linker's import-resolution path (FFI export side).
//
// Mechanical: `applyCMangling("_x", MachO)` returns `"__x"`. The
// function applies the format rule blindly — caller passes
// CANONICAL (undecorated) names. No dedup, no idempotence.
//
// Empty input → empty output (callers gate empties upstream;
// FF4 does not synthesize a name).
[[nodiscard]] DSS_EXPORT std::string
applyCMangling(std::string_view canonicalName, CSymbolDecorationScheme scheme);

// TF-C88 (D-CSUBSET-ASM-LABEL-SYMBOL-RENAME — GNU/Clang ASM LABEL, GCC 6.47.5) +
// TF-C121 (D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME) —
// THE one rule that turns a declared
// symbol into its ON-BINARY name, with BOTH override channels folded in:
//
//     linkNameFor(canonical, asmLabel, scheme, linkBaseName)
//         = asmLabel.empty()
//               ? applyCMangling(linkBaseName.empty() ? canonical : linkBaseName, scheme)
//               : asmLabel
//
// ★ THE TWO OVERRIDES ARE DIFFERENT KINDS OF FACT, AND THAT IS WHY THEY COMPOSE
// DIFFERENTLY — the whole reason this is one function with two override inputs
// rather than two functions:
//
//   * `asmLabel` answers "the USER'S C SOURCE wrote `__asm("x")`". C semantics
//     say that string IS the symbol, so it REPLACES the mangling; it is never
//     mangled on top of. MEASURED against /usr/bin/clang on arm64-darwin: `int
//     gv __asm("myglobal");` emits `myglobal` (no leading `_`) while an
//     undecorated `caller` emits `_caller` — which is exactly why the macOS
//     SDK's `__DARWIN_ALIAS` family writes its own underscore
//     (`__asm("_" __STRING(sym) …)`). The in-tree precedent for a pre-decorated,
//     never-re-mangled name is a format descriptor's `importMangledName`
//     (macho64-arm64-darwin-exec ships `"_exit"`).
//
//   * `linkBaseName` answers "which UNDECORATED BASE NAME does the shipped
//     library export for this C identifier ON THIS TARGET" — DSS vocabulary,
//     resolved per-(arch,format) by the descriptor reader (`ShippedSymbol::
//     linkName`). It replaces the BASE that gets decorated, NOT the decoration:
//     Darwin's 64-bit-inode ABI is reached through `fstat$INODE64` on x86_64 and
//     through the plain name on arm64, and BOTH are Mach-O C symbols that carry
//     the format's leading `_`. So the override path and the default path go
//     through the SAME SINGLE `applyCMangling` call below — there is one
//     decoration rule and this function is where it is consulted. Writing the
//     `_` per-symbol in config instead would make a THIRD in-tree copy of a
//     PER-FORMAT fact. Before step C4 that fact had TWO owners (a C++ table
//     here and the descriptors' `importMangledName` literals); it now has ONE,
//     the declared `cSymbolDecoration.scheme`, and a per-symbol `_` would
//     immediately re-create the problem AND grow with the descriptor corpus.
//     A per-format fact must not be encoded per-symbol.
//
// PRECEDENCE, stated once: asmLabel > linkBaseName > canonical. A user's explicit
// source-level `__asm` outranks DSS's config-level answer for the same name (the
// user is overriding DSS deliberately); both outrank the plain identifier.
//
// ★ IT EXISTS AS A FUNCTION, NOT AS FOUR `if`s, BECAUSE THE FOUR CALLERS MUST
// AGREE BYTE-FOR-BYTE. Two of them build the cross-CU merge KEY (the definition
// side in `program.cpp` and the import side via `ExternImport::mangledName`); if
// one honors a label and the other does not, `mir_merge`'s
// `definedNames.count(e.mangledName)` misses, the sibling-defined extern is NOT
// stripped, and an intra-image call is silently emitted as a dynamic import
// against the format-default library — green build, wrong binding, no diagnostic.
// `linkBaseName` is therefore a REQUIRED parameter, not a defaulted one: a
// caller that has no such name must say `{}` out loud rather than inherit a
// default that silently drops one rail's override.
//
// Empty label + empty linkBaseName ⇒ the pre-TF-C88 behavior, byte-identical.
// Empty canonical name with both empty ⇒ empty (the `nameOf` "module-private"
// signal, unchanged).
[[nodiscard]] DSS_EXPORT std::string
linkNameFor(std::string_view canonicalName, std::string_view asmLabel,
            CSymbolDecorationScheme scheme, std::string_view linkBaseName);

// Inverse of `applyCMangling`: strip the per-platform decoration
// to recover the canonical C identifier. Used by FF1 binary
// readers when ingesting a library whose symbol names are
// already-decorated (Mach-O `_printf` → `printf` for HIR-side
// matching against the user's source `extern int printf(...)`).
//
// Conservative: if the input does NOT carry the expected
// decoration (e.g. a Mach-O symbol `printf` without underscore),
// the function returns the input unchanged rather than
// fabricating semantics — operators usually ship clean libraries
// and a missing prefix is rarely the user's bug. (Strict-mode
// variant that errors on missing-prefix is anchored at
// D-FF4-3 — pairs with FF5 ingest where the format-kind is
// known authoritative.)
//
// Empty input → empty output (mirrors `applyCMangling`).
[[nodiscard]] DSS_EXPORT std::string
unapplyCMangling(std::string_view decoratedName, CSymbolDecorationScheme scheme);

// Does the DECLARED scheme add a leading underscore for C symbols? True for
// `leading-underscore`, false for `none`, and false for the `Unspecified`
// sentinel — which no loaded schema can carry (`validate()` rejects it on
// every format, unconditionally), so the sentinel arm is a defensive floor
// rather than a reachable policy. Single-sourced: the three manglers below all
// consult this, so the rule exists once.
[[nodiscard]] DSS_EXPORT bool
cFormatAddsLeadingUnderscore(CSymbolDecorationScheme scheme) noexcept;

// Closed-set failure modes for `unapplyCManglingStrict`.
enum class MangleErrorKind : std::uint8_t {
    MissingExpectedPrefix = 0,  // format expects decoration; input lacks it
    Count_                      // table-size sentinel — keep LAST
};

struct DSS_EXPORT MangleError {
    MangleErrorKind kind = MangleErrorKind::MissingExpectedPrefix;
    std::string     detail;
};

[[nodiscard]] DSS_EXPORT std::string_view
    mangleErrorKindName(MangleErrorKind k) noexcept;

// Strict-mode inverse of `applyCMangling`: returns an error if the
// `decoratedName` does NOT carry the per-format decoration the rule
// expects. Used by FF5 ingest where the format-kind is authoritative
// — a Mach-O binary's `.dynsym` entry that lacks the leading `_` is
// a structural anomaly worth surfacing rather than the conservative
// pass-through that `unapplyCMangling` does.
//
// Under the `none` scheme, strict mode is structurally a no-op: input passes
// through unchanged and success is returned. The strict check only fires under
// a decorating scheme (`leading-underscore` today; PE32 cdecl post-D-FF4-1).
//
// Empty input → empty output success (mirrors `applyCMangling`
// empty-input contract; an empty name is never "decorated" so there's
// nothing to enforce). `reporter` receives an `F_*` diagnostic on
// the error path.
[[nodiscard]] DSS_EXPORT std::expected<std::string, MangleError>
unapplyCManglingStrict(std::string_view        decoratedName,
                       CSymbolDecorationScheme scheme,
                       DiagnosticReporter&     reporter);

} // namespace dss::ffi
