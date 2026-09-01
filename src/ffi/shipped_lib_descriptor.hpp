#pragma once

#include "core/substrate/path_identity.hpp"

#include "core/export.hpp"
#include "core/types/data_model.hpp"   // DataModel (signatureByDataModel resolution)
#include "core/types/declared_qualification.hpp" // DeclaredQualification (a row's const/restrict claim)
#include "core/types/include_path_resolve.hpp" // HeaderNameMatching + HeaderSearchResult (the `includes` closure walk's case policy)
#include "core/types/named_type_binding.hpp" // NamedTypeBinding (c82 va_list alias thread-through)
#include "core/types/object_format_kind.hpp" // ObjectFormatKind (availability predicate)
#include "core/types/preprocess_config.hpp"  // PredefinedMacroDef / ShippedSurfaceClaim (the `impliedSurface` satisfaction half)
#include "core/types/strong_ids.hpp"   // TypeId

#include <cstddef>     // std::size_t (ShippedDescriptorCacheStats)
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ── LANGUAGE-NEUTRAL shipped-library FFI descriptor reader ───────────────────
//
// Closes D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC. The shipped system "headers"
// (the /usr/include analogue under `src/dss-config/shippedLibs/<platform>/`)
// are NOT per-language source files; they are a NEUTRAL JSON descriptor read
// by this UNIVERSAL reader. The user-facing UX is unchanged and C-faithful —
// `#include <stdio.h>` still works — but the on-disk shipped artifact is a
// language-agnostic schema, not a c `.h`.
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
// ⚠ DECLARED, NOT INCLUDED — see `diagnosticCodeForShippedSourceLookup` below.
// `parse_diagnostic.hpp` is large and this header is widely included; a scoped enum
// with a declared underlying type is exactly the forward-declarable case, so the
// dependency stays in the `.cpp` where it already was.
// ★ It belongs HERE, in `dss`, beside the other diagnostics forward declarations —
// declaring it inside `dss::ffi` mints a SECOND, distinct type and makes every
// existing `DiagnosticCode` reference in the ffi tree ambiguous.
enum class DiagnosticCode : std::uint16_t;

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
    // ★★ P44 (item (a) of D-C23-REDECL-QUALIFIER-AXIS-HAS-THREE-UNCLAIMED-SOURCES):
    // WHAT THIS ROW CLAIMS ABOUT `const` / `restrict`, WHICH `signature` CANNOT
    // CARRY AND NEVER WILL. Neither qualifier is interned (type_interner.hpp:
    // `const` never affects codegen or layout), so `fn(ptr<const<char>>, ...)
    // -> i32` and `fn(ptr<char>, ...) -> i32` intern to the SAME TypeId by
    // design. C23 6.7.6.1p2 nonetheless makes a pointed-to qualifier part of the
    // type for REDECLARATION compatibility, and ✔MEASURED, gcc 13.3.0
    // (`-std=c2x`) and clang 18.1.3 (`-std=c23`) probed SEPARATELY both REFUSE
    // `extern int printf(char *, ...);` over `#include <stdio.h>` while both
    // ACCEPT the `const char *` twin — a divergence DSS could not see while this
    // field did not exist.
    //
    // ⚠ NULL IS "NO CLAIM", NEVER "UNQUALIFIED". A row that spells no qualifier
    // says NOTHING about the axis and the oracle then does not judge it; reading
    // silence as `char *` would refuse the ubiquitous and legal
    // `int printf(const char *, ...);` against every row in the corpus that has
    // not been annotated. Shared, because a claim is immutable once read and one
    // descriptor row is consulted from several passes.
    std::shared_ptr<DeclaredQualification const> qualification;
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
    // FC17.9(c) (D-CSUBSET-SETJMP): optional — TRUE iff this extern "returns more
    // than once" (C11 7.13.1.1: `setjmp`/`_setjmp` — a `longjmp` makes an earlier
    // `setjmp` call appear to return a SECOND time from the caller's frame). Like
    // `noreturn` a shipped extern has no user prototype to carry the attribute, so
    // the descriptor declares it. The semantic phase threads it onto the injected
    // `SymbolRecord.returnsTwice`; UNLIKE noreturn (which is HIR-discharged into an
    // `Unreachable` and never reaches MIR) it must survive to MIR, so HIR->MIR sets a
    // per-Call `MirInstFlags::ReturnsTwice` on the lowered `Call` (the EXACT mirror of
    // how `MirInstFlags::Volatile` rides from `SymbolRecord.isVolatile`) — that flag is
    // what the optimizer's returns-twice-aware passes (mem2reg no-promote, inliner
    // callee-refusal) consult so a live-across-setjmp local is never promoted and a
    // setjmp-containing callee is never inlined (frame-sensitive). Default false.
    bool returnsTwice = false;
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
    // c156 (D-LK-ELF-SYMBOL-VERSIONING): optional REQUIRED symbol version — the
    // ELF version STRING (e.g. "GLIBC_2.3") this import must bind so ld.so
    // resolves the DEFAULT version instead of misbinding an unversioned
    // reference to a multi-versioned glibc symbol's OLDEST compat instance
    // (glibc `realpath`: the unversioned ref lands `@GLIBC_2.2.5`, whose
    // pre-2.3 form EINVALs a NULL resolved buffer, instead of the `@@GLIBC_2.3`
    // default). EMPTY (default, every symbol until opted in) ⇒ UNVERSIONED,
    // byte-identical to the pre-c156 image. The version is inherently
    // PER-TARGET (glibc's `realpath` is `GLIBC_2.3` on x86_64 but the single
    // baseline `GLIBC_2.17` on aarch64), so the descriptor resolves it via the
    // SAME per-target `variants` (when:{arch?,format?}) mechanism the
    // structs/constants/typedefs surfaces use: the reader selects the variant
    // matching the active (arch, format) and produces THIS single string (0
    // matches ⇒ empty ⇒ unversioned on that target — the aarch64 realpath
    // case). A flat string is also accepted (arch-invariant). ELF-only
    // semantics; carried but unused on PE/Mach-O.
    std::string version;
    // TF-C121 (D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME): optional per-target
    // LINK BASE NAME — the UNDECORATED name the shipped library actually exports
    // for this C identifier ON THIS TARGET, when it is not the identifier itself.
    // EMPTY (default, every symbol until opted in) ⇒ the `name` above, which is
    // byte-identical to the pre-TF-C121 image.
    //
    // ★ WHY IT EXISTS — MEASURED, a SILENT MISBINDING, not a theoretical gap.
    // Darwin reaches its modern 64-bit-inode ABI through `$INODE64` asm-label
    // ALIASES on x86_64 (`sys/cdefs.h`: `__DARWIN_SUF_64_BIT_INO_T` is
    // `"$INODE64"` on x86_64 and EMPTY otherwise), while on arm64 that ABI is the
    // only one and the plain names are correct. Declaring the plain name on
    // x86_64 binds the LEGACY 32-bit-inode implementation while DSS compiles the
    // MODERN 144-byte `struct stat`: the callee writes only 120 bytes, `st_size`
    // is read at offset 96 but written at 72, `fstat` hands back `st_size == 0`,
    // and sqlite concludes every database file is empty ("database disk image is
    // malformed"). Four-arm differential: `cc -arch x86_64` imports
    // `_fstat$INODE64` and sees st_size 4096; DSS imported `_fstat` and saw 0,
    // with bytes 120-143 still holding the pre-call 0xA5 poison.
    //
    // ★ IT DECLARES THE UNDECORATED BASE NAME — the leading `_` is composed by
    // the ENGINE (`ffi::linkNameFor` -> `applyCMangling`), NEVER written here.
    // That `_` is a per-FORMAT fact. When this field landed (TF-C121) the fact
    // had TWO owners -- a closed C++ table `kCManglingRules` and the format
    // descriptors' `importMangledName` literals -- and a per-symbol third copy
    // would have grown with the descriptor corpus and drifted. That two-owner
    // problem is now CLOSED (D-FFI-CMANGLING-RULE-NOT-CONFIG-DRIVEN step C4,
    // TF-C122): the C++ table is gone and the rule is the format's declared
    // `cSymbolDecoration.scheme`. The argument against writing `_` here is
    // UNCHANGED and is now simply the ordinary one -- a per-format fact belongs
    // on the format, not repeated on every symbol that happens to use it. The ELF `version` field
    // above is the exact precedent and it COMPOSES the same way: config says
    // `realpath` + `version:"GLIBC_2.3"`, never `realpath@GLIBC_2.3`.
    //
    // ★ NOT `asmName`. `asmName` answers "the user's C source wrote `__asm("x")`"
    // — verbatim, C semantics, and it BYPASSES the mangling. This answers "which
    // base name does the shipped library export on this target" — DSS vocabulary,
    // and it is the INPUT to the mangling. A user `__asm` still outranks it.
    //
    // PER-TARGET by the SAME `variants` (when:{arch?,format?,dataModel?})
    // mechanism `version`/structs/constants/typedefs use — `{"variants":[{"when":
    // {"format":"macho","arch":"x86_64"},"value":"fstat$INODE64"}]}` — resolved
    // by the reader to THIS single string for the ACTIVE target (0 matches ⇒
    // empty ⇒ the canonical name; the arm64-Darwin and Linux arms). A flat string
    // is also accepted (target-invariant).
    //
    // ★ AUTHORING CHECK — the old "verify the symbol is exported" rule does NOT
    // catch this class: BOTH `_fstat` and `_fstat$INODE64` exist in libSystem's
    // x86_64 slice, so an export check passes on the wrong one. The check that
    // works is "does a REAL compiler for THIS target emit THIS name for THIS C
    // identifier" — compile a one-line TU with the platform toolchain and read
    // the undefined symbol it emits.
    std::string linkName;
    // Optional per-SYMBOL `library` OVERRIDE — the per-object-format runtime
    // image for THIS symbol alone, SAME shape as the descriptor-level `library`
    // map ("pe"/"elf"/"macho" -> image name). EMPTY (default, almost every
    // symbol) means the symbol INHERITS the descriptor's map, byte-identical to
    // the pre-override image. When NON-EMPTY the semantic injector MERGES it OVER
    // the descriptor map (symbol keys WIN; a format the symbol OMITS inherits the
    // descriptor's entry — the same "a missing format key inherits" semantics the
    // descriptor map itself has). This lets ONE symbol bind a DIFFERENT image than
    // its header's default — e.g. pe `strftime`->`ucrtbase.dll` (C99-complete
    // `%e`/`%F`/`%R`) while the rest of <time.h> stays on the legacy `msvcrt.dll`
    // (whose bare `time`/`localtime`/... have no ucrtbase export, so the whole
    // descriptor cannot move). This field is the RAW declared override — the merge
    // lives at INJECTION, where the descriptor map is in scope. Keys are the
    // `objectFormatKindFromName` vocabulary; an unknown key / non-string value
    // fails loud on read (the SAME `decodeLibraryMap` chokepoint the
    // descriptor-level map uses).
    std::unordered_map<std::string, std::string> library;
    // ★★★ D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF (operator ruling, 2026-08-17) —
    // optional per-SYMBOL `realization` OVERRIDE: the per-object-format map
    // ("pe"/"elf"/"macho" → the CONFIG-ROOT-RELATIVE PATH of a shipped source file) that answers
    // WHETHER this symbol is imported at all, as opposed to `library` above,
    // which answers which IMAGE it is imported FROM. EMPTY (the default, every
    // symbol today) ⇒ IMPORT, i.e. byte-identical to the pre-ruling image.
    //
    // The two maps are SIBLINGS BY CONSTRUCTION — same closed object-format key
    // vocabulary, same `decodeLibraryMap`-shaped chokepoint, and the SAME merge
    // rule (a per-symbol map is merged OVER the descriptor's; symbol keys win, a
    // format the symbol omits inherits the descriptor's entry). That is why this
    // is an EXTENSION of the `library` axis rather than a parallel one: "where
    // does this symbol's body come from, on this format" was already the
    // descriptor's established shape, and this completes the answer set.
    //
    // ★ THE ENGINE NEVER BRANCHES ON FORMAT. It reads the declared realization
    // and either emits an import or adds the named source unit to the build
    // graph. There is no `if (format == "pe")` anywhere on this path — the
    // decision is DATA, exactly as the realization invariant that deleted
    // `externLibraryByFormat` requires (the declaration syntax has no authority
    // over realization; the PLATFORM, per format, carries it).
    //
    // ★ AND IT REFUSES, BIDIRECTIONALLY. Three load errors, all format-
    // independent so an inactive arm cannot rot (see `validateShippedSourceUnits`):
    //   R1 — a format naming a source that is not there ⇒ the error names BOTH
    //        the descriptor row and the missing path (LOAD time).
    //   R2 — a shipped source file NO descriptor names ⇒ inert config (gate test;
    //        see `validateShippedSourceTree` for why the placement differs).
    //   R3 — one format carrying BOTH a `library` image AND a `source` ⇒ two
    //        owners for one body is the defect, not a fallback resolved silently.
    std::unordered_map<std::string, std::string> realization;
};

// True iff `id` is a member of the CLOSED synth-recipe vocabulary — 22 recipes spanning
// TWO families:
//   * <threads.h> (21): the 18 non-trampoline (Cycle 1) + the 3 trampolines thrd_create/
//     call_once/thrd_join (Cycle 2). (thrd_sleep + the timed-waits stay elf-FFI-only —
//     deferred, see the .cpp vocab list.)
//   * <stdio.h> (1): `sprintf` — the whole shipped printf family for now; each further
//     recipe lands with its own descriptor row, its UCRT core row, and a runtime witness.
// The SINGLE source of truth shared by the descriptor loader (which rejects an unknown
// `synthesize` value fail-loud — F_ShippedLibDescriptorMalformed) AND the driver's multi-CU
// merged-module recipe reconstruction (program.cpp). Each family's synth pass has the
// matching per-recipe body switch — `synthesizeThreadsShim` PER VEHICLE (pe→win32/kernel32,
// macho→pthread/libSystem), `synthesizeStdioShim` over the UCRT `__stdio_common_v*` cores;
// a vocab id with no switch arm fails loud at synth (they cannot silently diverge).
// (D-CSUBSET-C11-THREADS-HEADER / D-FFI-PE-CRT-UCRT-MIGRATION)
[[nodiscard]] DSS_EXPORT bool isKnownSynthesizeRecipe(std::string_view id);

// Which SHIM FAMILY a recipe id belongs to. There is ONE recipe map
// (`CstToHirResult::synthRecipeBySymbol`) but more than one synthesis pass, and each pass
// FAIL-LOUDS on a recipe it has no arm for — deliberately, as its anti-vocab-drift
// backstop. So the driver seam partitions the map by family and hands each pass only its
// own entries; without that, adding <stdio.h> recipes would make the <threads.h> pass
// reject them and break the build before the stdio pass ever ran.
//
// The primary anti-drift guard is UPSTREAM of this: the descriptor loader already rejects
// an unknown `synthesize` value at READ time (a typo never reaches a pass at all). This
// split is what keeps each pass's own "no arm for MY family's id" check meaningful.
// (D-CSUBSET-C11-THREADS-HEADER / D-FFI-PE-CRT-UCRT-MIGRATION)
enum class ShimFamily : std::uint8_t {
    Threads,   // <threads.h> over kernel32 (win32) / libSystem (pthread)
    Stdio,     // <stdio.h> printf family over the UCRT __stdio_common_v* cores
};

// nullopt ⇔ !isKnownSynthesizeRecipe(id) — the two are kept in lockstep by construction
// (both read the same table), so a new recipe cannot be admitted by the loader while
// being invisible to the family split.
[[nodiscard]] DSS_EXPORT std::optional<ShimFamily> shimFamilyOf(std::string_view id);

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
    // ── D-FFI-DESCRIPTOR-CONSTANTS-INVISIBLE-TO-THE-PREPROCESSOR ────────────
    // WHICH SURFACE OF THE TRANSLATION UNIT THIS CONSTANT APPEARS ON, and it is
    // the ONE field both seams read: the PREPROCESSOR splice (a synthetic
    // `#define`, so `#if`/`#ifdef`/`defined()` see the name) and the SEMANTIC
    // injection (a named constant folded at HIR). Before this field existed the
    // reach question had NO owner: every `constants` entry reached the semantic
    // seam only, so `#if UINT_MAX > INT_MAX` read UINT_MAX as the C 6.10.1p4
    // "unknown identifier = 0" and took the OPPOSITE branch from gcc AND clang,
    // at rc=0 with no diagnostic — a different program, silently.
    //
    // DEFAULT `true`, and the default is the surface's OWN documented contract:
    // `limits.json` states that each entry is "each object-like `#define` that
    // IS a compile-time constant … expressed as a NEUTRAL typed named constant".
    // An object-like `#define` IS preprocessor-visible; that is what makes it a
    // `#define` rather than an enumerator.
    //
    // ⚠ THE `false` ROWS ARE MEASURED, NOT GUESSED. The surface was ALSO being
    // used for names that are ENUMERATION CONSTANTS in C, for which `defined(X)`
    // is FALSE in both references — making those visible would be an INVENTED
    // extension (above the gcc ∪ clang ∪ ISO union), which the bar forbids just
    // as firmly as falling below it. A `#include <h>` + `#if defined(NAME)` +
    // `nm` sweep over all 218 shipped `constants`/`floatConstants` names split
    // them 83 MACRO/MACRO · 36 NOT/NOT · 99 unclassifiable (the pe-only
    // `windows.h`/`io.h` names, which neither reference has a header for). The
    // `false` rows are exactly the ISO-mandated enumerations that sweep found —
    // `memory_order_*`, `thrd_*`, `mtx_*`, `PTHREAD_MUTEX_RECURSIVE` — plus the
    // Win32 `GET_FILEEX_INFO_LEVELS` enumerator `GetFileExInfoStandard`, which
    // is INFERRED from the Win32 SDK rather than measured, and is labelled that
    // way at its row.
    bool preprocessorVisible = true;
};

// One `constants` row PROJECTED into the PREPROCESSOR's vocabulary — the
// interner-free view `readShippedLibConstants` returns. `TypeId` cannot cross
// this boundary (it is per-CompilationUnit and the preprocessor has no
// interner), so the two facts a phase-4 spelling actually needs travel as DATA:
// the value's bit pattern and the declared type's signedness + width. Rendering
// them back into a source-language literal is the LANGUAGE tier's job (the
// preprocessor splice, driven by `semantics.integerLiteralTyping`), never this
// one — `src/ffi` stays free of any language's literal spelling.
struct DSS_EXPORT ShippedPpConstant {
    std::string  name;
    std::int64_t value      = 0;      // bit pattern, exactly as ShippedConstant::value
    bool         isUnsigned = false;  // the declared integer scalar's signedness
    unsigned     width      = 0;      // the declared integer scalar's width, in bits
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
// field scope, so c `struct tag v; v.field` resolves AND lays out at the
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

// One decoded UNION — the neutral form of a header's `union tag { … };` with
// NAMED members (e.g. the `key` union inside Tcl_HashEntry: the real
// `Tcl_GetHashKey` macro reads `h->key.oneWordValue` / `h->key.string`). The
// SIBLING of `ShippedStruct`: the semantic phase interns the union type
// (`TypeKind::Union` — every member overlaid at OFFSET 0, C 6.7.2.1) and injects
// a field scope + `compositeScopeByType` entry so `unionValue.member` resolves,
// MIRRORING the struct field-scope injection. This surface exists because the
// hir-text `union "N" { T,… }` spelling carries member TYPES positionally but NO
// names — so, exactly like `structs`, the member names live HERE. `typeId` is the
// interned union type (name + positional member types), byte-identical to the
// same-spelled `union "N" {…}` used by-name in a struct field (Option C).
//
// Members overlay at offset 0 by union semantics — a `ShippedField.offset` is
// REJECTED on a union member (an explicit-offset overlapping layout is the c107
// STRUCT channel, `D-FFI-DESCRIPTOR-UNION-OVERLAY`, not this). (D-FFI-DESCRIPTOR-UNION-MEMBER-INJECTION)
struct DSS_EXPORT ShippedUnion {
    std::string               name;     // the union tag, e.g. "Tcl_HashKey"
    std::vector<ShippedField> fields;   // named members, decl order (all @0)
    TypeId                    typeId;   // interned union type (set on decode)
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
    // Optional `includes` — the transitive sibling headers this descriptor
    // `#include`s in the real world (D-FFI-DESCRIPTOR-INCLUDES). When a TU
    // `#include`s the parent header (so DSS resolves THIS descriptor), DSS ALSO
    // resolves + injects each declared sibling descriptor's surface into that TU
    // — modeling the real transitive `#include` graph a flat descriptor cannot
    // carry (real `tcl.h` `#include`s `<stdio.h>`, so tcl.json declares
    // `includes:["stdio.h"]` and a `<tcl.h>` user reaches FILE/fopen/…). Each
    // entry is a header NAME resolved by the SAME `<stem>.json` convention as a
    // source `#include <…>` (`resolveSystemDescriptor`): "stdio.h"→stdio.json,
    // "sys/uio.h"→sys/uio.json (subdir-preserving, extension-agnostic). EMPTY/
    // absent ⇒ no transitive edges (every existing descriptor is untouched —
    // pure back-compat). Fully generic: any descriptor may declare `includes`;
    // the engine walks a config-declared graph via `forEachDescriptorInClosure`
    // with NO `if (name==…)` and no source/target/format identity branch.
    // D-FFI-DESCRIPTOR-INCLUDES-EDGE-GATE: an entry may instead be an object
    // `{header, when}` — a CONDITIONAL edge, taken only on a format its `when`
    // selects (see `readShippedLibIncludes` for the full entry grammar). This
    // vector holds the ALREADY-SELECTED header names for the `activeFormat` the
    // read was given: an inactive edge is not an edge on this target and never
    // appears here, so a reader of this field can never re-decide the question.
    std::vector<std::string>     includes;    // ACTIVE transitive sibling header names
    // The full neutral surface a header provides. A descriptor must declare AT
    // LEAST ONE of these non-empty (a descriptor that declares NOTHING is a
    // no-op artifact and fails loud); a header may legitimately carry only
    // `constants` (e.g. `<limits.h>`), only `symbols`, or any mix. (`includes`
    // above does NOT count toward "declares something" — an includes-only
    // umbrella descriptor is out of scope this cycle; add it here when a real
    // umbrella-header consumer lands.)
    std::vector<ShippedSymbol>   symbols;     // extern functions/objects (linked)
    std::vector<ShippedConstant> constants;   // named integer constants (folded)
    std::vector<ShippedFloatConstant> floatConstants; // named float constants (folded; c52)
    std::vector<ShippedTypedef>  typedefs;    // type aliases (resolved in type pos)
    std::vector<ShippedMacro>    macros;      // preprocessor macros (injected at #include)
    std::vector<ShippedStruct>   structs;     // named-field structs (tag + field scope)
    std::vector<ShippedUnion>    unions;      // named-member unions (tag + field scope; all @0)
    // ★★★ D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF — the descriptor-level per-object-
    // format REALIZATION map (format → shipped source unit NAME), the sibling of
    // `library` above and the DEFAULT each `symbols[i].realization` is merged
    // over. EMPTY/absent (every descriptor but `dirent.json` today) ⇒ every
    // symbol IMPORTS, byte-identical to the pre-ruling image. See the field of
    // the same name on `ShippedSymbol` for the full contract.
    std::unordered_map<std::string, std::string> realization;
};

// ═══ D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF — THE SHIPPED SOURCE TREE ═══
//
// DSS SHIPS THE SOURCE. The compiler synthesizes only STATELESS glue; anything
// with state, allocation or nontrivial control flow lives in a RUNTIME LIBRARY
// OF COMPILED SOURCE — libgcc / compiler-rt / libmingwex / newlib is the
// universal shape, and `opendir` on Windows is literally the libmingwex case.
//
// A descriptor's `realization` map points AT THE FILE, config-root-relative:
// `{"pe": {"source": "runtime/platform/pe/dirent.c"}}`. Nothing derives and
// nothing composes — the string in the descriptor is the string you can paste
// into `ls`, and it greps in BOTH directions (body→declaration and back). There
// is NO manifest: a unit list would be a third copy of facts the descriptor and
// the file tree already own, and `.c` already answers "which front end compiles
// this" through `fileExtensions`, the same mechanism every ordinary compile uses.
//
// ★ THE `<format>` SEGMENT IS ORGANIZATIONAL, NEVER SEMANTIC, and the
// distinction is worth keeping because it decides the next case. `pe/` organizes:
// the realization KEY owns which format uses a body, the PATH owns which file,
// and nothing is derived from the segment — a disagreement would be confusing,
// not wrong. A LANGUAGE segment (`c/`) would be semantic: it would compete
// with the extension for HOW the file is compiled, and the moment it won, a
// `foo.c` under `toy/` would compile as toy. It is therefore absent.
// ⚠ The one case that would genuinely need a language axis is real and is NOT
// this one: `.s`/`.S` is claimed by BOTH shipped asm dialects, so a hand-written
// assembly runtime unit (soft-float helpers, setjmp/longjmp bodies — classic
// contents of this tier) is genuinely ambiguous by extension. It would need the
// ARCH, not the language, and the realization key is FORMAT-keyed, so it is a
// different shape; building for it now is the speculative structure §A.2 rules
// out.

// The shipped-config ROOT (`…/src/dss-config`), by the SAME precedence every
// other shipped thing uses. nullopt ⇒ discovery failed; the caller decides what
// that means (it is a statement about the environment, never about the corpus).
[[nodiscard]] DSS_EXPORT std::optional<std::filesystem::path>
findShippedConfigRootDir();

// Why a shipped-source path did not resolve.
//
// ★★ THESE USED TO BE ONE `nullopt`, AND THE COLLAPSE WAS A MISATTRIBUTION WITH
// TEETH. The predecessor returned `optional<path>` and its docblock called the
// collapse deliberate — "discovery failed OR no readable file is there — the two are
// one answer to the caller". It was really THREE answers: `is_regular_file(p, ec)`
// reports false both when nothing is there AND when the query itself failed, and the
// `ec` that distinguishes them was discarded unread. Every caller then stated ABSENCE
// as fact.
// ✔MEASURED 2026-08-25 on the Windows gate under concurrent load: two entries failed
// naming `runtime/platform/src/unistd.c` and `dirent.c` as files that are not there,
// while both were present, regular and readable seconds later. A user with antivirus,
// a network share, a locked file or a concurrent writer gets sent hunting for a file
// sitting exactly where it belongs.
// ★ The fix is the TYPE, not the message: a caller cannot report a difference its
// return value cannot carry.
enum class ShippedSourceResolution : std::uint8_t {
    Resolved,      // an absolute path to a regular file
    NoConfigRoot,  // `src/dss-config/` was not discovered — about the ENVIRONMENT
    NotPresent,    // the root resolved and nothing exists at that path
    NotAFile,      // something exists there, but it is not a regular file
    QueryFailed,   // the filesystem could not answer — carries the `error_code`
};

struct ShippedSourceLookup {
    ShippedSourceResolution status{ShippedSourceResolution::NotPresent};
    std::filesystem::path   path{};   // the resolved absolute path when known
    std::error_code         error{};  // set iff `status == QueryFailed`

    [[nodiscard]] bool resolved() const noexcept {
        return status == ShippedSourceResolution::Resolved;
    }
};

// Resolve a descriptor's config-root-relative `realization.<fmt>.source`.
[[nodiscard]] DSS_EXPORT ShippedSourceLookup
resolveShippedSource(std::string_view configRelativePath);

// One clause naming what ACTUALLY happened, for a diagnostic to embed. Never claims
// absence unless absence was established.
[[nodiscard]] DSS_EXPORT std::string
describeShippedSourceLookup(ShippedSourceLookup const& lookup,
                            std::string_view           configRelativePath);

// The diagnostic CODE this outcome deserves.
//
// ★ A CODE IS A CLAIM IN THE SAME WAY ITS MESSAGE IS, which is why this is a named
// function and not a ternary at the emit site: anything that filters, counts or
// suppresses `D_FileNotFound` was silently counting I/O failures as missing files, and
// no amount of message rewording reaches that. Naming it also makes it PINNABLE —
// `check-diagnostic-codes` refuses a code no compiled test names, and it is right to.
//
// ⚠ Forward-declared rather than including `parse_diagnostic.hpp`: that header is
// large and this one is widely included. A scoped enum with a declared underlying type
// is precisely the forward-declarable case, so the dependency stays in the `.cpp`.
[[nodiscard]] DSS_EXPORT DiagnosticCode
diagnosticCodeForShippedSourceLookup(ShippedSourceLookup const& lookup);

// The interner-free FAST reader the DRIVER uses: every config-root-relative
// shipped SOURCE path the descriptor at `path` realizes on `formatName`, from the
// descriptor-level `realization` map with each symbol's override merged over it.
// Deliberately parallel to `readShippedLibAvailability` — no interner, no type
// decoding, no diagnostics — because the driver asks this once per resolved
// descriptor per build and must not pay for a full read.
[[nodiscard]] DSS_EXPORT std::vector<std::string>
readShippedSourcesForFormat(std::filesystem::path const& path,
                            std::string_view             formatName);

// EVERY config-root-relative shipped SOURCE path that ANY descriptor under
// `descriptorDir` realizes on `formatName`, deduplicated and in a deterministic
// order (the corpus walk's order, which is the filesystem's sorted order).
//
// ★★★ THE RUNTIME IS COMPILED FOR EVERY BUILD OF A FORMAT THAT REALIZES IT,
// NOT ONLY FOR BUILDS THAT HAPPEN TO INCLUDE THE HEADER — which is why this
// reads the CORPUS rather than the CUs. On-demand compilation would make the
// runtime object set depend on which headers somebody happened to `#include`,
// and an object set that varies by that cannot be cached, packaged or shipped.
// Always-compile makes it a PURE FUNCTION OF (target, config), which is the
// property a shippable runtime needs.
//
// ⚠ COMPILE-ALWAYS IS NOT LINK-ALWAYS. This answers what to COMPILE; what
// ends up in the image stays demand-driven, so a `hello.c` on pe64 carries no
// directory-walking code it never calls.
[[nodiscard]] DSS_EXPORT std::vector<std::string>
allShippedSourcesForFormat(std::filesystem::path const& descriptorDir,
                           std::string_view             formatName);

// ★★★ THE CORPUS-WIDE REFUSALS, run over the WHOLE tree and INDEPENDENT OF THE
// ACTIVE FORMAT — so an arm no current target selects cannot rot, which is the
// bidirectional half of the bar.
//
//   R1  a realization naming a source that is not there ⇒ refusal naming BOTH the
//       descriptor row and the missing path. ALSO enforced at DESCRIPTOR READ TIME
//       (`readShippedLibDescriptor`), where it is one `is_regular_file` per
//       declared entry and therefore free — that is its load-bearing home, because
//       it is the refusal that can otherwise produce a build silently missing a
//       body. It is repeated here so the sweep is a TOTAL statement about the tree.
//   R2  a source file NO descriptor names ⇒ refusal (inert config: nothing can
//       ever add it to a build graph). ⚠ GATE-TEST ONLY, and that is a deliberate
//       departure from the ruling's "LOAD ERROR" wording, stated rather than left
//       to look like verbatim compliance: without the deleted manifest this costs
//       a directory walk PLUS a corpus scan on EVERY compile, and an inert `.c`
//       can only waste disk while R1's failure can produce a wrong binary.
//       Severity matched to the failure.
//   R4  no HEADERS in the tree. Folded into R2 rather than given its own check,
//       which is strictly stronger: a header is never a translation unit, so no
//       `realization` can name one, so the unclaimed-file rule refuses it BY
//       CONSTRUCTION with no extension vocabulary to enumerate or keep current.
//
// R3 (an image AND a source for one format — two owners for one body) is NOT here:
// it is a WITHIN-DESCRIPTOR check and lives at descriptor read time, where it
// costs nothing and fires on the descriptor actually being loaded.
//
// Returns false (diagnostics already reported) on any refusal.
// ⚠ `runtimeRootDir` IS THE RUNTIME ROOT (`.../src/dss-config/runtime`), never a
// tier and never a tier's `src/`. The function DERIVES `<root>/<tier>/src` and
// walks only that — deliberately, because a tier also holds `dist/`, the
// GENERATED object cache: handed a tier, an unfiltered walk would refuse every
// cached object as unclaimed and red EVERY WARM BUILD, and the failure would read
// as a config error rather than a wrong argument. Deriving the subtree makes the
// wrong subject unrepresentable, and a root that HAS a `src/` child is refused
// outright rather than scanned. (⚠⚠ Not closed by filtering to `.c`: that
// would make this silently ignore a stray `.txt`/`.o` in the AUTHORED tree, which
// is exactly the inert-config case R2 exists to catch. The subject was the bug.)
[[nodiscard]] DSS_EXPORT bool
validateShippedSourceTree(std::filesystem::path const& descriptorDir,
                          std::filesystem::path const& runtimeRootDir,
                          DiagnosticReporter&          reporter);

// Every NAME the descriptor at `path` contributes on object format `fmt` — the
// ONE surface-presence oracle, shared by the corpus invariants and by the
// predefined-macro `requires` satisfaction check, so "is X visible here?" has a
// single answer.
//
// A name is any declaration a `#include` of this header brings into scope: a
// symbol, a constant, a float constant, a typedef, a struct or union tag, a
// macro. Per-format selection is applied EXACTLY as the real reads apply it —
// a symbol's own `availableObjectFormats` (falling back to the document's), and
// the real `macros` decode with the real format, so a variants-only macro that
// selects no arm on `fmt` contributes no name here for the same reason it
// contributes no `#define` to a compile.
//
// ⚠ THE TYPED SURFACES ARE ANSWERED AT FORMAT REACHABILITY, NOT FULL SELECTION.
// A `variants`-bearing typedef/constant/struct contributes its name on `fmt` if
// ANY of its arms is format-compatible, because this scan has no arch and no
// data model and does not need them: a variant set gives a name a different
// TYPE or LAYOUT per arch, never existence. See `WhenAxes::FormatReachability`.
//
// std::nullopt (and `reporter` carries the reason) when the descriptor does not
// decode far enough to answer — callers must report that, never treat it as an
// empty surface. Callers that own their own wording pass a throwaway reporter.
[[nodiscard]] DSS_EXPORT std::optional<std::vector<std::string>>
shippedSurfaceNamesForFormat(std::filesystem::path const& path,
                             ObjectFormatKind             fmt,
                             DiagnosticReporter&          reporter);

// ★★★ THE TWO CORPUS-WIDE INVARIANTS THAT SHIP WITH THE `includes` EDGE GATE
// (D-FFI-DESCRIPTOR-INCLUDES-EDGE-GATE). Run over the WHOLE tree under
// `descriptorDir` and INDEPENDENT OF ANY BUILD'S ACTIVE FORMAT — the
// `validateShippedSourceTree` posture, for its reason: an arm no current target
// selects must not be allowed to rot.
//
//   (i)  EDGE FIRES ⇒ CHILD AVAILABLE. For every descriptor D and every object
//        format F in D's availability set, every `includes` edge of D ACTIVE on
//        F must resolve to a child descriptor that is itself available on F.
//        The config may not promise, on F, a surface it declares absent on F.
//   (ii) NO EMPTY SURFACE ON A SERVED FORMAT. A descriptor available on F must
//        contribute at least one name on F — its own surfaces or its active
//        closure. A header whose `#include` compiles and declares nothing is a
//        header that silently is not there.
//
// WHY THEY SHIP WITH THE GATE RATHER THAN LATER: a conditional edge lets an
// author buy silence for free (write a `when` that never fires and get an empty
// surface instead of a complaint). A mechanism that can convert a loud failure
// into a quiet one must arrive with the checks that keep silence expensive.
//
// `servedFormats` is the set of object formats the corpus is expected to serve,
// and it is a PARAMETER rather than an enumeration of `ObjectFormatKind`
// deliberately: the enum carries reserved slots (`wasm`, `spirv`) that no
// shipped object-format document declares, and sweeping them would refuse the
// corpus for failing to serve a platform nobody targets. An EMPTY span is
// refused outright — a sweep that cannot fail is not a sweep.
//
// ⚠ WHAT THEY DO NOT COVER (stated, not implied): PARTIAL OMISSION — (ii) is an
// EXISTENCE claim, so a descriptor declaring one of a real header's forty names
// passes exactly as a complete one does, and nothing here can measure
// completeness (that is what a consumer's `requires` is for); SEMANTIC
// correctness of any declaration (signatures, layouts — `ShippedTypeConsistency`
// owns those); the ARCH and DATA-MODEL axes (both invariants are FORMAT-keyed —
// see `shippedSurfaceNamesForFormat`); any format outside `servedFormats`; and
// anything outside the shipped descriptor corpus.
//
// Returns false (diagnostics already reported) on any refusal.
[[nodiscard]] DSS_EXPORT bool
validateShippedIncludeClosure(std::filesystem::path const&      descriptorDir,
                              std::span<ObjectFormatKind const> servedFormats,
                              DiagnosticReporter&               reporter);

// ★★★ D-LANG-PREDEFINED-MACRO-REQUIRES-REALIZED-SURFACE — the SATISFACTION half.
//
// The predicate's SHAPE lives in core (`ShippedSurfaceClaim`); this is the
// half that can see the shipped corpus. For every macro in `macros` that
// declares a non-null `impliedSurface`, every claimed header must resolve, must exist
// on each format the claim covers, and must make every claimed NAME visible
// through its ACTIVE include closure on that format.
//
// FORMAT SET, per macro: its own `availableObjectFormats` when non-empty (so a
// pe-gated macro is checked on pe from EVERY leg — the arm nobody selects is the
// arm that rots), else `activeFormat` when known. An ungated macro with no
// active format has no platform in scope and therefore no claim to check; that
// is a check whose SUBJECT does not exist, not a check that was skipped — and it
// is unreachable from the driver, which builds a CU once per (target, format).
//
// It is an ASSERTION, never a suppression: a macro whose backing is missing
// fails the build. Withdrawing it quietly would flip `#ifdef` branches under the
// user with no diagnostic — the same quiet wrongness as the silently-PRESENT
// macro this mechanism exists to end.
//
// `declaringDocument` is the config FILE FAMILY the rows came from (e.g.
// "<lang>.lang.json /preprocess/predefinedMacros"); combined with each row's own
// `declaredAt` JSON pointer it makes the diagnostic name the file and the row,
// instead of naming a macro and leaving the author to grep three families.
//
// Returns false (diagnostics already reported) on any unsatisfied requirement.
[[nodiscard]] DSS_EXPORT bool
validateShippedSurfaceRequirements(
    std::span<PredefinedMacroDef const>    macros,
    std::string_view                       declaringDocument,
    std::span<std::filesystem::path const> systemDirs,
    std::optional<ObjectFormatKind>        activeFormat,
    DiagnosticReporter&                    reporter);

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
// ── THE DESCRIPTOR PARSE CACHE'S OWN VACUITY WITNESS ────────────────────────
//
// Every reader below goes through ONE thread-local parse cache
// (`cachedDescriptorJson`). It is CONTENT-VALIDATED: the cached document is
// served only when the file's bytes are byte-identical to the bytes it was
// parsed from, so a descriptor rewritten IN PLACE is re-parsed rather than
// answered from the old document — see the long note at that function for the
// LSP session this defect was live in and for the three measurements that ruled
// out an `mtime`/`size` stamp.
//
// ★ THE COUNTERS EXIST BECAUSE BOTH OF THE CACHE'S FAILURE MODES ARE SILENT AND
// LOOK ALIKE FROM OUTSIDE. A cache that serves a stale document and a cache that
// works are both "the read succeeded"; a cache that has been quietly reduced to
// re-parsing every time is ALSO "the read succeeded", just slower — and no
// assertion on a returned descriptor can tell any of the three apart. These are
// the only observable difference, so the regression pin asserts on them: the
// same precedent as `ShippedTypeConsistency::duplicateRealizationsCompared()`.
//
// THREAD-LOCAL, like the cache — the numbers are the CALLING thread's.
struct DSS_EXPORT ShippedDescriptorCacheStats {
    std::size_t lookups         = 0;  // calls that reached the cache
    std::size_t parses          = 0;  // documents actually `json::parse`d
    std::size_t revalidatedHits = 0;  // served from cache after a byte-compare
    std::size_t staleEvictions  = 0;  // dropped because the file's bytes changed
};
[[nodiscard]] DSS_EXPORT ShippedDescriptorCacheStats shippedDescriptorCacheStats();

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

// Read the PREPROCESSOR-VISIBLE half of the `constants` surface at `path`,
// WITHOUT a TypeInterner — the constants sibling of `readShippedLibMacros`, and
// the second seam of the ONE owner `ShippedConstant::preprocessorVisible`
// names. (D-FFI-DESCRIPTOR-CONSTANTS-INVISIBLE-TO-THE-PREPROCESSOR.)
//
// ★ IT SHARES THE `decodeShippedConstants` CHOKEPOINT WITH THE SEMANTIC READ,
// the same way `readShippedLibMacros`/`readShippedLibAvailability`/
// `readShippedLibIncludes` share theirs — ONE decode, so the two seams cannot
// validate differently, cannot select a different per-target variant, and
// cannot drift on the `preprocessorVisible` field that decides which of them a
// row reaches. A `TypeInterner` is still needed for each row's hir-text `type`,
// so this read owns a PRIVATE function-local `TypeLattice`; the TypeIds never
// escape, only the projected {value, signedness, width} triple does.
//
// ⚠ IT IS NOT A CALL TO `readShippedLibDescriptor`, AND THE DIFFERENCE IS
// MEASURED. The first cut did exactly that — attractive, because the decode is
// then literally shared — and it silently dropped EVERY `stdio.json` constant:
// the full read also decodes `symbols`, whose `FILE*`/`size_t` operands resolve
// through cross-descriptor `namedTypes` bindings only the semantic tier holds,
// so with an empty binding set the whole read failed and `EOF`/`SEEK_SET`/
// `FILENAME_MAX` never reached the preprocessor. That is this row's own defect,
// reintroduced by its own fix; the `EOF == -1` cell of
// `examples/c/c_pp_shipped_constants` is what caught it.
//
// Returns the `preprocessorVisible` rows only, already variant-selected for
// (`activeTarget`, `activeFormat`) exactly as the semantic read selects them —
// so the two seams cannot disagree on a per-target VALUE either. EMPTY when the
// descriptor declares no `constants` (or none that are visible). std::nullopt
// on any read the full read would reject.
//
// NO STRICTER than the full read, like every sibling: no `header` provenance
// gate and no other-surface validation, so a descriptor that the semantic tier
// would accept always yields its constants here. The one production caller
// (`SynthBuilder::build`) passes a THROWAWAY reporter and discards the
// diagnostics, exactly as it already does for `readShippedLibMacros`, so a
// malformed `constants` surface is reported ONCE — by the import-resolver /
// semantic tier that owns the positioned message.
[[nodiscard]] DSS_EXPORT std::optional<std::vector<ShippedPpConstant>>
readShippedLibConstants(std::filesystem::path const&    path,
                        DiagnosticReporter&             reporter,
                        std::optional<std::string_view> activeTarget = std::nullopt,
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

// Read ONLY the `includes` surface (the transitive sibling-header NAMES) from the
// descriptor at `path`, WITHOUT a TypeInterner — the interner-free sibling of
// `readShippedLibMacros`/`readShippedLibAvailability` for the two tiers that have
// `systemDirs` (the preprocessor macro-splice + the import resolver) and no
// interner. Returns the declared header-name list (EMPTY when the descriptor
// declares no `includes` — the common case, every existing descriptor);
// std::nullopt + `F_ShippedLibDescriptorMalformed` on a malformed `includes` field
// (not an array, or an entry that is neither a non-empty string nor a well-formed
// `{header, when}` object). Validated through the SAME shared decode as the full
// `readShippedLibDescriptor` read, so the interner-free and interned reads never
// drift (the `readShippedLibMacros` lock-step precedent).
// (D-FFI-DESCRIPTOR-INCLUDES)
//
// ── THE `includes` ENTRY GRAMMAR (D-FFI-DESCRIPTOR-INCLUDES-EDGE-GATE) ──
// An entry is EITHER
//   "stdio.h"                                  — an UNCONDITIONAL edge, and
//                                                byte-identical to the pre-gate
//                                                shape, OR
//   {"header":"windows.h","when":{"format":"pe"}}
//                                              — a CONDITIONAL edge, taken only
//                                                on a format the `when` selects.
// `when` is REQUIRED and NON-EMPTY in the object form (an absent or `{}` `when`
// is a second spelling of the string form and FAILS LOUD), and is evaluated by
// the ONE shared `when` evaluator in its FORMAT-ONLY mode — the SAME closed
// vocabulary and the same unknown-key/unknown-format-name refusals the `macros`
// surface gets. Every entry's SHAPE is validated EAGERLY regardless of whether
// the edge is active, so a malformed INACTIVE edge fails the read on EVERY
// target (the anti-lurking property the variant surfaces already have).
//
// `activeFormat` nullopt (LSP / direct-API / test callers, and any caller with no
// target) ⇒ NO conditional edge is taken; unconditional edges are unaffected.
// This is the same rule the `macros` surface applies to a variants-only macro.
[[nodiscard]] DSS_EXPORT std::optional<std::vector<std::string>>
readShippedLibIncludes(std::filesystem::path const&    path,
                       DiagnosticReporter&             reporter,
                       std::optional<ObjectFormatKind> activeFormat = std::nullopt);

// Walk the transitive shipped-descriptor closure rooted at `startPath`, invoking
// `visit(path)` once for EACH DISTINCT descriptor in the closure, PARENT-FIRST (a
// descriptor before the siblings its `includes` declares). The SHARED cycle-safe
// walker both `systemDirs`-bearing tiers use (the preprocessor macro-splice + the
// import resolver typed-surface record), so the two can never disagree on the
// transitive set. (D-FFI-DESCRIPTOR-INCLUDES)
//
//   * CYCLE / DIAMOND SAFE: a single DFS keyed on the WEAKLY-CANONICAL descriptor
//     path in `visited` — the SAME key the semantic `readDescriptors` dedup +
//     `cachedDescriptorJson` cache use. A path is visited AT MOST ONCE, so A→B→A
//     terminates at the second A and a diamond's shared leaf is visited once. The
//     recursion is bounded by the finite shipped-descriptor count. `visited` is
//     in/out: pass ONE shared set across multiple roots (the import resolver's
//     CU-wide set — a sibling reached from two parents or also included directly
//     is recorded once) or a fresh set per root (the preprocessor's per-call set).
//   * `includes` is read interner-free with a THROWAWAY reporter: a malformed
//     `includes` FIELD is surfaced by the semantic `readShippedLibDescriptor` that
//     reads the SAME descriptor (the import resolver records a ref per closure
//     descriptor) — never silent, never double-reported here.
//   * An `includes` entry that resolves to NO descriptor on `systemDirs` (a typo
//     `stdioo.h`) invokes `onUnresolvedInclude(headerName)` — a config error the
//     caller surfaces LOUD (the import resolver positions an
//     `F_ShippedHeaderNotFound` on the `#include` line; this is the ONLY tier that
//     can catch it, since the interner-less semantic tier has no `systemDirs`).
//   * `matching` is the ACTIVE OBJECT FORMAT's header-name case rule
//     (D-PP-HEADER-CASE-INSENSITIVE-PE). The closure's `includes` entries are
//     header NAMES resolved by the same funnel a source `#include <h>` uses, so
//     they MUST honour the same case policy — a descriptor declaring
//     `includes:["Windows.h"]` has to reach `windows.json` on a pe build from a
//     case-sensitive host exactly as the source spelling does.
//   * `onUnresolvedInclude(headerName, outcome)` fires for any entry that did
//     NOT resolve to exactly one descriptor. `outcome.status` separates a plain
//     miss (NotFound) from a fold COLLISION (AmbiguousCase, whose
//     `ambiguousCandidates` name every colliding file) so the caller can emit
//     the right loud diagnostic; collapsing the two would report a typo for a
//     tree that actually holds two case-colliding descriptors.
//   * `activeFormat` makes the walk PER-FORMAT, and it is what lets every tier
//     agree by construction:
//       - a CONDITIONAL `includes` entry whose `when` does not select this format
//         is NOT AN EDGE here. It is absent from the closure, so no tier forms an
//         opinion about it and no two tiers can hold different ones.
//       - a descriptor UNAVAILABLE on this format is still VISITED when it is the
//         ROOT (the caller owns that verdict — it is the header the user named),
//         but is never DESCENDED INTO: a header that does not exist on a format
//         declares nothing on it, its `includes` closure included.
//       - an ACTIVE edge whose child is UNAVAILABLE on this format is a config
//         contradiction and fires `onUnavailableChild(headerName, childPath)`
//         instead of being visited. `validateShippedIncludeClosure` invariant (i)
//         refuses that statically over the whole corpus; this callback is the
//         runtime belt to that sweep's braces.
//     nullopt (LSP / direct-API / tests) ⇒ no conditional edge is taken and no
//     availability test is applied — the pre-gate walk over unconditional edges.
//   * `onUnavailableChild(headerName, childPath)` is MANDATORY, deliberately: a
//     defaulted no-op is how one tier ends up silently dropping what another
//     reports, which is the exact drift this parameter exists to end. A tier that
//     genuinely must stay silent (the preprocessor macro-splice, whose loud twin
//     is the import resolver) passes an empty lambda AND says why.
DSS_EXPORT void forEachDescriptorInClosure(
    std::filesystem::path const&                            startPath,
    std::span<std::filesystem::path const>                  systemDirs,
    HeaderNameMatching                                      matching,
    std::optional<ObjectFormatKind>                         activeFormat,
    std::unordered_set<core::PathIdentity>&                 visited,
    std::function<void(std::filesystem::path const&)> const& visit,
    std::function<void(std::string const&,
                       HeaderSearchResult const&)> const&    onUnresolvedInclude,
    std::function<void(std::string const&,
                       std::filesystem::path const&)> const& onUnavailableChild);

// The FORMAT-BLIND walk, for callers that genuinely have no active object format
// (the LSP, the direct API, unit tests over a synthetic corpus). Equivalent to
// passing `activeFormat = std::nullopt` above.
//
// ★ IT OMITS `onUnavailableChild` BECAUSE THAT ARM IS UNREACHABLE HERE, NOT
// BECAUSE IT IS OPTIONAL. "This child is unavailable on the active format" is
// not a statement that exists without an active format, so there is nothing for
// the callback to report and nothing being silently dropped. A caller that DOES
// hold a format must use the full form and say what it does with the answer —
// which is the property that keeps two tiers from disagreeing, and it is why
// this is an overload with a stated precondition rather than a default argument
// on the real one.
DSS_EXPORT void forEachDescriptorInClosure(
    std::filesystem::path const&                            startPath,
    std::span<std::filesystem::path const>                  systemDirs,
    HeaderNameMatching                                      matching,
    std::unordered_set<core::PathIdentity>&                 visited,
    std::function<void(std::filesystem::path const&)> const& visit,
    std::function<void(std::string const&,
                       HeaderSearchResult const&)> const&    onUnresolvedInclude);

// True iff a header carrying availability set `availableObjectFormats` is
// available on object-format `fmt`. EMPTY set ⇒ available on EVERY format
// (back-compat). The SINGLE membership predicate shared by the semantic
// `#include` availability gate (semantic_analyzer) AND the preprocessor
// consumers (`__has_include` + the macro-splice) below, so all three can never
// disagree — the FC15c funnel principle applied to per-target availability.
[[nodiscard]] DSS_EXPORT bool objectFormatInAvailabilitySet(
    std::span<std::string const> availableObjectFormats, ObjectFormatKind fmt);

// ★★★ D-FFI-DESCRIPTOR-KNOWN-NAME-HAS-NO-LIBRARY-FOR-FORMAT — THE ONE OWNER of
// "which IMAGE does this per-object-format `library` map name on `formatName`".
// Returns the image, or an EMPTY view when the row names NONE.
//
// `formatName` is the DECLARED object-format spelling (`objectFormatKindName`),
// which is what the maps are keyed on — this predicate knows no format identity
// and no image name, exactly like `objectFormatInAvailabilitySet` above.
//
// ★ BOTH SPELLINGS OF ABSENCE COLLAPSE TO ONE ANSWER. A map with no entry for
// the format, and a map whose entry names the empty string, are the same fact
// and answer the same way. That is the reason this exists as a function rather
// than an inline `find`: the two spellings were read DIFFERENTLY by the two
// tiers that ask (`realizeRow` asked `contains`, the binder folds ask for the
// VALUE), so one row could be REALIZED to the tier that states the platform's
// answer and UNBOUND to the tier that acts on it. The empty spelling is now
// refused at descriptor load, and this accessor keeps the interior total even
// for a map a direct-API caller built itself.
//
// ⛔ AN EMPTY ANSWER IS NOT AN ERROR. "The corpus knows this name, it IS
// available here, and no image is named for this format" is a LEGAL, STATED
// platform answer — `ShippedRealizationStatus::NoLibraryForFormat` — and it
// routes the reference UNBOUND to the link tier, where C23 5.1.1.2 phase 8 puts
// every unresolved external reference. It is never a compile error, because it
// is a statement about the PLATFORM's image inventory and not about the user's
// program: a sibling translation unit or an operator-named library may still
// provide the symbol.
[[nodiscard]] DSS_EXPORT std::string_view shippedLibraryImageForFormat(
    std::unordered_map<std::string, std::string> const& library,
    std::string_view                                    formatName);

// True iff the shipped header whose descriptor is at `descriptorPath` is
// available on `fmt`. Reads `availableObjectFormats` interner-free
// (`readShippedLibAvailability`) then applies `objectFormatInAvailabilitySet`.
// A MALFORMED descriptor ⇒ available (the header EXISTS; its malformedness is
// surfaced by the macros / typed reads on the same descriptor, NOT
// double-reported here). The preprocessor `__has_include` + macro-splice gate.
[[nodiscard]] DSS_EXPORT bool shippedHeaderAvailableForFormat(
    std::filesystem::path const& descriptorPath, ObjectFormatKind fmt);

// c162 fold (D-FF1-READER-CONSUMER), made FORMAT-AWARE by
// D-FFI-SHIPPED-SYMBOL-ORACLE-IGNORES-OBJECT-FORMATS: every extern symbol NAME
// declared by any shipped-library descriptor under `src/dss-config/shippedLibs`,
// mapped to the set of OBJECT FORMATS that name is AVAILABLE on. This is the
// "is X a known system symbol ON THIS TARGET" oracle the `--resolve-library`
// consumer uses to distinguish a bare `extern puts;` (a real libc symbol the
// user did not #include -- resolve it against the format-default library, NOT
// fail loud) from a genuine typo (`dss_lib_answr` -- in neither the named
// binary nor any descriptor -> route unbound, the link tier judges) from a
// name that is real BUT NOT ON THIS FORMAT (elf-only `fdatasync` in a macho
// build -> fail loud F_ShippedSymbolUnavailableForTarget instead of binding it
// to a libSystem that has no such export and dying silently at load).
//
// ★ THE VALUE IS A UNION ACROSS ROWS, NOT ONE ROW'S GATE. The SAME name is
// routinely declared by SEVERAL rows with DIFFERENT gates: `call_once` /
// `thrd_create` / `mtx_lock` and ~20 more carry three separate rows in
// threads.json gated ["elf"] / ["macho"] / ["pe"]; `sprintf` has an
// ["elf","macho"] row AND a ["pe"] row in stdio.json; `fabsf`/`ldexpf` appear in
// both math.json and tgmath.json. The predicate the consumer needs is therefore
// "does ANY row declare this name available on the target format?", so this map
// accumulates the union -- a per-row answer would turn the whole C11 threads
// surface red on every format.
//
// VALUE ENCODING (the `objectFormatInAvailabilitySet` contract, verbatim): an
// EMPTY vector means AVAILABLE ON EVERY FORMAT, exactly as an empty
// `availableObjectFormats` does everywhere else in this header. The union
// therefore SATURATES: once any row of a name is unrestricted, the name is
// unrestricted. Test membership with `objectFormatInAvailabilitySet` -- the ONE
// shared predicate, so this oracle can never drift from the `#include` /
// `__has_include` / semantic-injection gates.
//
// TWO-LEVEL FALLBACK per row, mirroring the semantic injector: a symbol with no
// `availableObjectFormats` key inherits the DOCUMENT-level
// `availableObjectFormats`; if the document has none either, the row is
// available everywhere.
//
// Returns std::nullopt iff the shippedLibs directory cannot be located
// (DSS_CONFIG_ROOT unset + no ancestor hit) -- the caller then treats every
// symbol as "possibly known" and falls through to the format-default (SAFE:
// never a false-positive fail-loud on a legitimate program just because config
// discovery failed).
//
// Names + availability only (no signature decode / interner needed) -- a
// lightweight scan distinct from the full `readShippedLibDescriptor`. Lenient
// per-file: an unreadable / malformed descriptor is SKIPPED (its symbols are
// absent from the map, so they route unbound under --resolve-library -- an
// acceptable, LOUD, user-fixable outcome; the descriptor's malformedness is
// caught for real by the semantic-injection reader on #include + the
// AllShippedDescriptorsDecode test). A malformed/unknown availability ENTRY
// inside an otherwise-readable descriptor is likewise skipped, which can only
// WIDEN the row's set -- never narrow it into a false fail-loud. Not cached --
// runs only on the --resolve-library path when a governed extern is unmatched by
// the named binaries (the uncommon case).
[[nodiscard]] DSS_EXPORT
std::optional<std::unordered_map<std::string, std::vector<std::string>>>
collectShippedExternSymbolFormats();

// ── THE PLATFORM REALIZATION ORACLE ──────────────────────────────────────────
//
// ★★★ THE DECLARATION SYNTAX HAS NO AUTHORITY OVER REALIZATION, EVER.
//
// A user declaration carries the SIGNATURE. The PLATFORM — this shipped-descriptor
// corpus, per object format — carries the REALIZATION: `library`,
// `availableObjectFormats`, the `synthesize` recipe, `linkName`, `version`,
// `signatureByDataModel`. `#include <stdio.h>` and a hand-written
// `extern int printf(const char *, ...);` are two ways to obtain a TYPE; NEITHER
// is a way to obtain a different PLATFORM.
//
// C23 makes this a CONFORMANCE requirement, not a preference:
//   * 6.2.2p5 — a file-scope function declaration with no storage-class specifier
//     has its linkage determined EXACTLY AS IF declared `extern`. So
//     `extern int printf(const char*, ...);` and `int printf(const char*, ...);`
//     are THE SAME DECLARATION. Two spellings C says declare the same thing MUST
//     realize the same thing.
//   * 7.1.4p2 — a library function needing no header-defined type MAY be declared
//     and used WITHOUT including its header. The program is ENTITLED to write the
//     prototype by hand and still get the real printf.
//   * 5.1.1.2 phase 8 — external references are resolved AT LINK. An "I don't know
//     that name" verdict therefore belongs to the LINK tier, never to a per-CU
//     compile error: another CU, or a library supplied later, may legitimately
//     provide the symbol.
//
// ★ WHY THIS ORACLE EXISTS AT ALL. Before it, a shipped descriptor was consulted
// ONLY when the source `#include`d its header (the semantic injector) or under
// `--resolve-library` (`collectShippedExternSymbolFormats`, which answers
// AVAILABILITY only — not library, not recipe, not link name). On an ordinary
// build a hand-written prototype consulted NO descriptor at all and fell through
// to a per-LANGUAGE default library guess. That guess was a SECOND OWNER of a fact
// the corpus already owns, and it was WRONG in the ways only a guess can be: it
// named one image for every symbol of every header, so `extern int printf(const
// char*, ...);` imported a bare `printf` from the LEGACY pe C runtime while the
// rest of the same program's stdio surface was correctly realized as UCRT shims —
// TWO C RUNTIMES in one three-line program, with no diagnostic at any stage. The
// guess is gone; this is what replaced it.
//
// ★ AGNOSTIC BY CONSTRUCTION. Every answer is DATA read off a descriptor row and
// selected by the ONE shared availability predicate `objectFormatInAvailabilitySet`
// — the SAME predicate the `#include` gate, `__has_include`, the macro splice and
// the semantic injector use, so this oracle cannot drift from them. There is no
// `if (format == …)`, no `if (arch == …)`, and no symbol name is special-cased.

// WHY a name has (or has not) a realization on the active object format. A CLOSED
// enum: every outcome is ENUMERATED and STATED, so none is an unenumerated
// fallthrough. Only `Realized` carries realization data.
enum class ShippedRealizationStatus : std::uint8_t {
    // No descriptor in the corpus declares this name. An ordinary program symbol,
    // a sibling-TU definition, or a typo — the LINK tier is the only tier that can
    // tell those apart, so the reference routes UNBOUND and link judges it
    // (K_SymbolUndefined when it is genuinely undefined AND referenced).
    Unknown = 0,
    // Declared by the corpus, but NOT on the active object format (the elf-only
    // `fdatasync` in a macho build). Routes UNBOUND for the same reason: binding it
    // to any image would link clean and die at LOAD. Never a compile error here.
    UnavailableForFormat,
    // ★ THE ARM THAT USED TO BE A FALLTHROUGH. Declared AND available on this
    // format, yet the row's `library` map names no image FOR this format —
    // `decodeLibraryMap` simply omits an absent key, so this outcome previously
    // existed only as "the map lookup missed". It is now a STATED arm: the platform
    // declares the symbol exists here but not where it lives, so there is nothing
    // to bind and the reference routes UNBOUND to the link tier. There is no
    // in-tree instance today; the arm is about the DESIGN being total, not about a
    // live bug (the eight POSIX descriptors once cited as evidence are top-level
    // gated ["elf","macho"] with zero pe-gated symbol rows, which makes their
    // absent `pe` key CORRECT and puts them in `UnavailableForFormat` above).
    //
    // ★★★ AND THE VERDICT IS UNBOUND, NOT A LOAD ERROR — decided on measurement,
    // D-FFI-DESCRIPTOR-KNOWN-NAME-HAS-NO-LIBRARY-FOR-FORMAT. Making a descriptor
    // that declares an available symbol with no image MALFORMED was the other
    // candidate closing, and it is wrong: ✔MEASURED, 125 rows across
    // ctype/math/memory/stdio/stdlib/string.json declare NO `availableObjectFormats`
    // — which this codebase reads as available on EVERY format, `wasm` and `spirv`
    // among them — while naming images only for elf/pe/macho, so a load-time
    // "available ⇒ must name an image" rule fails loud on the six most central C
    // descriptors the day it ships. Scoping it to EXPLICIT availability lists to
    // dodge that would be loud on the NARROWER claim and silent on the BROADER one.
    // And this reader is consulted for descriptors the user never `#include`d, so a
    // load error here either meets the "a descriptor that fails to read is SKIPPED"
    // contract below and does nothing, or turns one unrelated descriptor into every
    // program's build failure. C23 5.1.1.2 phase 8 owns this verdict: an unresolved
    // external reference is the LINK tier's to judge.
    //
    // ⚠ THE ARM IS DECIDED BY `shippedLibraryImageForFormat`, never by asking the
    // map whether it CONTAINS the format key. A key present naming the empty string
    // is a second spelling of this very outcome, and `contains` called it
    // `Realized` while every binder fold called it unbound. That spelling is now
    // refused at descriptor load and collapsed by the accessor.
    NoLibraryForFormat,
    // ★★★ D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF. Declared, available here, and the
    // platform states that the BODY IS SHIPPED rather than imported: no image
    // exports it on this format because no image HAS it (Windows has no POSIX
    // directory API in ucrtbase or kernel32), so DSS supplies the source and the
    // driver compiles it FOR THE TARGET as an ordinary extra translation unit.
    // The reference binds like any sibling-CU definition — it is NOT an import,
    // and emitting one would produce a binary the loader rejects at process
    // start. Carries `shippedSourceUnit`.
    ProvidedByShippedSource,
    // Fully realized: bind exactly as the `#include` path would have.
    Realized,
};

// The PLATFORM's realization of one extern name, already resolved for the ACTIVE
// (arch, object format, data model). Field-for-field the same realization the
// `#include` path carries on a `SuppressedShippedSymbol` — deliberately, so a
// hand-written declaration and an `#include`d one produce a BYTE-IDENTICAL import.
struct DSS_EXPORT ShippedSymbolRealization {
    ShippedRealizationStatus status = ShippedRealizationStatus::Unknown;
    // The row's per-object-format `library` map with the per-SYMBOL override
    // MERGED OVER it (symbol keys win; an omitted format inherits the
    // descriptor's) — the identical merge the semantic injector performs.
    std::unordered_map<std::string, std::string> library;
    std::string version;    // required ELF symbol version; EMPTY ⇒ unversioned
    std::string recipeId;   // `synthesize`; EMPTY ⇒ an ordinary library import
    std::string linkName;   // per-target link BASE name; EMPTY ⇒ the identifier
    // D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF: the CONFIG-ROOT-RELATIVE path of
    // the shipped source file whose compiled body provides this symbol on the
    // active format (`runtime/platform/pe/dirent.c`). NON-EMPTY iff
    // `status == ProvidedByShippedSource`; EMPTY for every other status.
    std::string shippedSourcePath;
    // The row's DECLARED signature, interned in the CALLER's interner (the
    // `signatureByDataModel` override for the active model already applied).
    // InvalidType unless `status == Realized`.
    TypeId      signature;
    bool        isFunction = true;   // ExternFunction vs ExternGlobal
};

// Resolve the platform realization of each requested NAME for the active target.
//
// Returns a map holding one entry per requested name whose status is anything
// other than `Unknown`; a name with NO corpus row is simply ABSENT (the caller
// treats absence as `Unknown` — unbound, link tier). Returns std::nullopt IFF the
// shippedLibs directory cannot be located (DSS_CONFIG_ROOT unset and no ancestor
// hit): the caller must then behave exactly as it did before this oracle existed
// and route unbound, never fail loud — a config-discovery miss is not a statement
// about the user's program.
//
// `activeFormat` nullopt (a direct-API / LSP / unit caller with no target) ⇒ EVERY
// name answers `Unknown`. Availability and the library map are BOTH per-format
// facts, so without a format there is no realization to state, and inventing one
// would be exactly the guess this oracle removes. (Same posture the macro-variant
// selection already takes under nullopt.)
//
// COST: the corpus INDEX (name → the descriptors declaring it) is built once and
// memoized; only the descriptors that actually declare a requested name are read,
// so a TU that hand-declares nothing reads NOTHING and a TU that hand-declares
// `popen`/`pclose` reads ONE descriptor. Descriptors are read through the SAME
// `readShippedLibDescriptor` the `#include` path uses — there is no second
// resolution grammar, so `variants` / `signatureByDataModel` / per-symbol
// `library` overrides cannot be resolved one way here and another way there.
//
// A descriptor that FAILS to read is SKIPPED (its names stay `Unknown` and route
// unbound → the link tier judges the reference LOUD). Deliberate: this oracle is
// consulted for names the user never `#include`d, so an unrelated descriptor's
// malformedness must not become this program's build failure. Descriptor health is
// owned by the tier that reads it for real (the `#include` path) plus the
// corpus-wide decode test — the `shippedHeaderAvailableForFormat` precedent.
//
// ── `reporter` — D-FFI-DUPLICATE-SYMBOL-ACROSS-DESCRIPTORS-SILENTLY-ORDER-RESOLVED
//
// ★★★ IT IS REQUIRED, NOT OPTIONAL, AND IT IS THE WHOLE REASON THIS PARAMETER
// EXISTS. Two descriptors may declare ONE name, both LIVE on this format, with
// DIFFERENT realizations; this function then picks the first in the corpus
// index's relPath sort — a DIFFERENT order from the `#include` path's include
// closure — so one program could bind two different C runtimes for one name
// depending only on whether it wrote `#include <string.h>` or a bare prototype.
// ✔MEASURED (P42, pe64, `objdump -p`, a `DSS_CONFIG_ROOT` copy with memory.json
// on msvcrt and string.json on ucrtbase): rc=0 and ZERO diagnostics, msvcrt bound.
// Before this parameter there was NOWHERE to say so from. `reporter` receives
// `F_ShippedCorpusInvariantBroken` — the SAME code, message and rule the
// `#include` path emits, produced by the same `ShippedTypeConsistency` — for
// exactly the names the caller ASKED about.
//
// ⛔ AN AMBIGUOUS NAME IS STILL ANSWERED, NEVER OMITTED. Dropping it from the map
// routes the reference unbound, and the link tier then reports an undefined
// symbol AGAINST THE USER'S PROGRAM — a true failure filed against an innocent
// subject. The map is unchanged; the DIAGNOSTIC is what changed, and it names the
// two descriptors.
//
// ⓘ THE CALLER MUST REFUSE. This function does not signal the conflict in its
// return value (nullopt is reserved for "the corpus could not be located", which
// is benign and must route unbound). Snapshot `reporter.errorCount()` around the
// call — the `tierClean` idiom this codebase already uses everywhere else.
[[nodiscard]] DSS_EXPORT
std::optional<std::unordered_map<std::string, ShippedSymbolRealization>>
realizeShippedExternSymbols(std::span<std::string const>      names,
                            TypeInterner&                     interner,
                            TypeRegistry&                     typeReg,
                            DiagnosticReporter&               reporter,
                            DataModel                         dataModel,
                            std::optional<std::string_view>   activeTarget,
                            std::optional<ObjectFormatKind>   activeFormat,
                            std::span<NamedTypeBinding const> namedTypes = {});

// EVERY symbol row of the descriptor that declares `name`, realized for the active
// target — i.e. the whole import surface that descriptor would contribute.
//
// ★ WHAT IT IS FOR, AND IT IS NOT A CONVENIENCE. A `synthesize` row is realized as
// a COMPILER-EMITTED BODY, and that body CALLS other rows of the SAME descriptor
// (the pe printf shim calls `__stdio_common_vfprintf` and `__acrt_iob_func`). On the
// `#include` path those cores arrive for free — the header's whole surface is
// injected. On the HAND-DECLARED path (C23 7.1.4p2) nothing else in the TU declares
// them, and the synth pass then correctly FAIL-LOUDS ("the UCRT core
// '__acrt_iob_func' is not imported by this module" — MEASURED). So realizing a
// recipe row means realizing the surface its recipe can reach into.
//
// ★ WHY THE WHOLE SURFACE AND NOT "THE CORES". The recipe→core mapping lives in the
// synth pass's per-recipe switch arms. Restating those core NAMES here would plant
// platform symbol literals in shared substrate — the agnosticism break this codebase
// keeps deleting — and would need editing every time a recipe lands. Instead the
// caller records the whole surface as NON-EAGER imports and the LINKER's existing
// reference gate prunes it to exactly the rows the emitted body referenced. The
// precision comes from a mechanism already in the tree, not from a list.
//
// Returns nullopt on config-discovery failure (same contract as
// `realizeShippedExternSymbols`); an EMPTY map when nothing declares `name`. Rows
// that are not `Realized` on this format are OMITTED — a companion that does not
// exist here must never become an import (the eager-import law's whole point).
// `name` ITSELF is included; the caller already has its realization and can skip it.
[[nodiscard]] DSS_EXPORT
std::optional<std::unordered_map<std::string, ShippedSymbolRealization>>
realizeShippedDescriptorSurfaceFor(std::string_view                  name,
                                   TypeInterner&                     interner,
                                   TypeRegistry&                     typeReg,
                                   DataModel                         dataModel,
                                   std::optional<std::string_view>   activeTarget,
                                   std::optional<ObjectFormatKind>   activeFormat,
                                   std::span<NamedTypeBinding const> namedTypes = {});

} // namespace ffi
} // namespace dss
