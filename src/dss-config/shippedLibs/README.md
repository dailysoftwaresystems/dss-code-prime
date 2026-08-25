# Shipped-library FFI descriptors

This directory holds the **language-neutral FFI descriptors** for the standard
libraries the compiler ships against. A descriptor is the machine-readable
answer to "what functions does `<stdio.h>` provide, and what are their
signatures?" — the analogue of a C system header, but expressed once, in a
form every source language can consume.

When a c program writes `#include <stdio.h>` with **no** inline
`extern`, the angle-include resolver maps the header stem to the matching
`*.json` here (on the `semantics.shippedLibDirs` system search path), the
semantic analyzer injects the descriptor's symbols into scope before name
resolution, and the linker resolves them against the runtime image for the
active compilation target's object format — exactly like real C. See
`examples/c/shipped_include_puts/` (stdio) and
`examples/c/shipped_include_abs/` (stdlib) for end-to-end proofs.

**Model 3 (2026-06-09)** — descriptors are **platform-neutral**: ONE descriptor
per header (`<stem>.json`, at the header's own path — no per-platform
directories), carrying a
per-object-format `library` map (`{"pe":…,"elf":…,"macho":…}`). The active
target's object format selects its runtime image at `compile_pipeline`
resolution time (keyed by `objectFormatKindName`), so the same descriptor serves
every target. This dissolved the former per-platform-directory layout and the
`D-FFI-SHIPPED-LIB-PLATFORM-SELECT` deferral.

The universal reader is `src/ffi/shipped_lib_descriptor.{hpp,cpp}`. It is
**source/target/linker agnostic**: pure `nlohmann/json` + the single
`parseTypeFromText` codec, with no language/CPU/format identity branch.

## ★★ SCOPE — OS LIBRARIES ONLY. THIRD-PARTY LIBRARIES NEVER BELONG HERE.

This directory describes **the operating system's own surface** and nothing else:
libc/POSIX, Win32, libSystem, the platform's headers. That is DSS's *own*
vocabulary for a target platform, which is why it can ship it, version it, and
guarantee it on every host.

A third-party library — Tcl, zlib, OpenSSL, anything a *user* chose to depend on
— is **NOT** part of that surface and must never get a descriptor here. It is
supplied as a **per-leg compilation input**, declared by the build that consumes
it. DSS supports both dynamic and **static** third-party libraries through that
channel, so a project can compile against whatever it needs without the compiler
adopting responsibility for it.

**Why the rule is absolute, and not merely tidy.** A descriptor here is a promise
DSS makes about every target it supports. Admitting one third-party library makes
DSS's platform vocabulary responsible for someone else's release cycle, ABI and
symbol set — and it sets the precedent that the *next* dependency gets the same
treatment. The directory then stops meaning "the platform" and starts meaning
"whatever happened to be convenient", which is unfixable once it has begun.

⚠ **This rule was breached in proposal on 2026-08-04 and caught only because the
operator was reading.** The sqlite testfixture links Tcl (~102 symbols) and zlib
(8 symbols), and because those two lack descriptors, `--resolve-library` must read
a real binary's export table to learn their names — which is impossible for a
Darwin target on a non-Mac host. "Just give them descriptors" is the obvious fix,
it is wrong, and it was recommended by an agent *and* by the orchestrator before
being rejected. **The rule was nowhere in this README at the time, which is
precisely why it was proposed.** It is here now. See
`D-HARNESS-MACHO-LEG-INPUTS-UNOBTAINABLE-OFF-MAC` for the correct direction: a
per-leg declared library input, living with the build, not with the platform.

---

## Directory layout

**49 descriptors** today: **37** at the top level, plus **12** under `sys/` and
`malloc/`.

```
shippedLibs/
  stdio.json             <stdio.h>       — I/O
  stdlib.json            <stdlib.h>      — general utilities, alloc, conversion
  string.json            <string.h>      — byte-string / memory ops
  ctype.json             <ctype.h>       — character classification
  math.json              <math.h>        — floating-point math
  …                                      — 32 more C / POSIX / Win32 headers
  sys/stat.json          <sys/stat.h>    — the header's PATH is the descriptor's path
  sys/time.json          <sys/time.h>
  …                                      — 9 more under sys/ (11 in all), 1 under malloc/
```

NOT flat, but still platform-neutral (Model 3): one descriptor per header, each
carrying a per-object-format `library` map. The nesting that exists mirrors the
C **header namespace** — the resolver maps a header name to `<stem>.json`
verbatim, so `<sys/stat.h>` → `sys/stat.json` and `<malloc/malloc.h>` →
`malloc/malloc.json`. There are still no per-**platform** subdirectories: the
target's object format picks the runtime image from the map at resolution time.

---

## Descriptor schema

```json
{
  "header":   "stdio.h",        // REQUIRED — provenance: which C header
  "standard": "c89",            // optional — provenance: which language standard
  "library": {                  // per-object-format runtime image (Model 3)
    "pe":    "ucrtbase.dll",
    "elf":   "libc.so.6",
    "macho": "/usr/lib/libSystem.B.dylib"
  },
  "symbols": [
    { "name": "puts",
      "signature": "fn(ptr<char>) -> i32",
      "kind": "function",
      "linkage": "external" }
  ]
}
```

The table below is the **complete** closed key set the reader accepts at the
ROOT. Any other top-level key **fails loud**
(`F_ShippedLibDescriptorMalformed`, `(root)`), so a typo is never a
silently-ignored surface. Every nested surface is its own closed set on the
same terms — the `symbols` row below spells TWELVE of its THIRTEEN keys out in full (the
thirteenth, `realization`, has its own section further down),
because an entry key omitted from this document is one an author will never
know exists.

| Field       | Required | Meaning |
|-------------|----------|---------|
| `header`    | **yes**  | The header these symbols come from (`stdio.h`). This is the provenance answer to *"where does `puts` come from?"* — a descriptor that omitted it would defeat the purpose, so the reader **fails loud** (`F_ShippedLibDescriptorMalformed`) if it is missing or empty. |
| `standard`  | no       | The language standard the surface targets (`c89`, `c99`, …). Provenance only. |
| `library`   | no       | A per-OBJECT-FORMAT MAP (`"pe"`/`"elf"`/`"macho"` → runtime image). The active compilation target's object format selects its entry at `compile_pipeline` resolution (keyed by `objectFormatKindName`). **Optional**: a map MISSING the active format's key (or absent entirely) leaves the row UNBOUND, resolved at the LINK tier per C23 5.1.1.2 phase 8 — it does NOT inherit a per-language default, because UCRT-P4 (Decision 1) removed `externLibraryByFormat` outright as a second owner of a fact the corpus owns per SYMBOL. A key NOT in the object-format vocabulary (a typo like `"pee"`) **fails loud** (`F_ShippedLibDescriptorMalformed`) on read. **What the 30 descriptors that carry a map actually name today** (copy these, not the historical `msvcrt.dll`): 18 declare a `pe` entry — **16** `ucrtbase.dll` (the UCRT) and 2 `kernel32.dll` (`threads.json`, `windows.json`). **No descriptor names `msvcrt.dll` any more**: UCRT-P5 moved the last holdout, `setjmp.json`, once the facility was found in ucrtbase under the name `__intrinsic_setjmp` (that row pairs `library.pe` with a `linkName`, and the two must move together — see `tests/ffi/test_pe_crt_costate_binding.cpp`). Every `elf` entry is `libc.so.6` except `math.json`/`tgmath.json` (`libm.so.6`), and every `macho` entry is `/usr/lib/libSystem.B.dylib`. |
| `availableObjectFormats` | no | Per-target AVAILABILITY — which object formats this header EXISTS on (the sibling axis to `library`, which says which IMAGE per format). ABSENT/EMPTY = available on **every** format (C-standard headers omit it); a POSIX-only header carries `["elf","macho"]`, so `#include <sys/time.h>` **fails loud** for a windows-pe target and `__has_include` answers the per-target truth. Same object-format vocabulary as `library` — an unknown name **fails loud** on read. A `symbols` entry may carry its own `availableObjectFormats` to gate one symbol (see `stdio.json`'s `__stdinp` / `_wfopen`). |
| `includes`  | no       | The transitive sibling headers this header `#include`s in the real world (`inttypes.json` declares `["stdint.h"]`, mirroring C 7.8p1). Including the parent injects each declared sibling's surface too, walking the config-declared graph cycle-safely. Each entry is a header NAME resolved by the same `<stem>.json` convention as a source angle-include (`"sys/uio.h"` → `sys/uio.json`). ABSENT/EMPTY = no transitive edges. **`includes` does NOT count toward "declares something"** — an includes-only descriptor still fails loud. |
| `symbols`   | no\*     | The exported LINK surface (extern functions/objects). An entry accepts **THIRTEEN** keys and no more (✔MEASURED 2026-08-25 against the reader's own list; the twelve documented in this cell plus `realization`, which has its own section below) — an unknown one **fails loud** (`symbols[i]`) exactly as at the root. **`name`** (REQUIRED) — the undecorated C identifier; the linker-visible form is produced downstream by FF4 mangling. **`signature`** (REQUIRED) — a hir-text type string: a full `fn(…) -> …` FnSig for a function, or the value type for an object, decoded by the one shared `parseTypeFromText` codec. **`signatureByDataModel`** — a per-DATA-MODEL override map (`"LP64"`/`"LLP64"`/`"ILP32"` → signature string); the ACTIVE model's entry replaces `signature`, and EVERY declared override is parsed on every read, so a form that only Windows selects can never lurk malformed. An unknown model key fails loud. **`kind`** — `"function"` (default) or `"object"`; selects `ExternFunction` vs `ExternGlobal`. **`linkage`** — `"external"` (default) or `"weak"`; validated and carried (the synthesis path currently emits Strong for every shipped import). **`availableObjectFormats`** — per-SYMBOL availability, the symbol-granularity sibling of the header-level key. Load-bearing, not cosmetic: DSS imports EVERY declared shipped extern whether the program references it or not, so a name absent from the active format's runtime image must not be declared there or nothing links. **`noreturn`** — this extern never returns (`abort`, `exit`, `longjmp`, `thrd_exit`). A shipped extern has no user prototype to carry C11 `_Noreturn`, so the descriptor declares it and a direct call lowers to `Block{ call, Unreachable }`. **`returnsTwice`** — C11 7.13.1.1 (`setjmp`, `_setjmp`); rides to MIR as `MirInstFlags::ReturnsTwice`, which is what stops mem2reg promoting a live-across-`setjmp` local and stops the inliner taking the callee. **`synthesize`** — this symbol is NOT an import but a COMPILER-SYNTHESIZED body; the value is a recipe id from a CLOSED vocabulary and MUST equal `name` (the synth pass identifies each recipe by symbol name, so both an unknown id and a mismatch are rejected on read). Live use: `threads.json`'s `<threads.h>` shim (21 pe rows over kernel32, 21 macho rows over pthread/libSystem — the elf rows are ordinary glibc imports) and `stdio.json`'s five pe printf/scanf rows over the UCRT `__stdio_common_v*` cores, which `ucrtbase.dll` does not export as `printf`/`fprintf`/`sprintf`/`sscanf`/`vfprintf` at all. **`version`** — the ELF symbol VERSION this import must bind (`stdlib.json`'s `realpath` → `GLIBC_2.3` on x86_64-elf), so a reference does not misbind to a multi-versioned glibc symbol's OLDEST compat instance. A flat string, or per-target `variants` (`when` + `value`) since the version is genuinely per-target. Empty = unversioned; ELF-only semantics, carried and unused on PE/Mach-O. **`linkName`** — the UNDECORATED name the shipped library actually EXPORTS for this C identifier ON THIS TARGET, when it is not the identifier itself. Same shape as `version` (flat string, or per-target `variants` with `when` + `value`) and read by the same decoder. Live use: Darwin's modern 64-bit-inode ABI is reached through `$INODE64` asm-label aliases on **x86_64** and through the plain names on **arm64**, so `sys/stat.json`'s `stat`/`fstat`/`lstat`, `unistd.json`'s `statfs`/`fstatfs` and `dirent.json`'s `opendir`/`readdir` each carry a `when:{format:"macho",arch:"x86_64"}` arm; every other target matches nothing and keeps the identifier. ★ Write the BASE name only — `"fstat$INODE64"`, never `"_fstat$INODE64"`. Mach-O's leading underscore is a per-FORMAT fact the ENGINE composes (`ffi::linkNameFor` → `applyCMangling`), exactly as `version` composes `realpath` + `GLIBC_2.3` into `realpath@GLIBC_2.3` rather than making you spell it. ★ NOT the same thing as a user's C `__asm("x")` (which is verbatim and BYPASSES mangling); this is the INPUT to mangling, and a user `__asm` outranks it. ★ AUTHORING CHECK, and it REPLACES the older "verify the symbol is exported" rule for this class: an export check does NOT catch a wrong link name, because BOTH `_fstat` and `_fstat$INODE64` exist in libSystem's x86_64 slice. The check that works is **"does a real compiler for THIS target emit THIS name for THIS C identifier"** — compile a one-line TU with the platform toolchain and read the undefined symbol it emits. Getting this wrong does not fail loud: a descriptor SHADOWS the SDK header entirely, so the platform's own asm label never participates, the plain name resolves against the LEGACY implementation, and the program links, loads, and misbinds (MEASURED — it made `fstat` return `st_size == 0` and sqlite call every database "malformed"). **`library`** — a per-symbol OVERRIDE of the descriptor's map, same `{pe,elf,macho}` shape, MERGED over it (symbol keys win; an omitted format inherits). Counts, ✔MEASURED 2026-08-25 over the 49 shipped descriptors: `availableObjectFormats` 218 entries, `synthesize` 48, `linkName` 33, `noreturn` 6, `signatureByDataModel` 6, `returnsTwice` 2, `version` 1 — and the per-symbol `library` override is used by **no** descriptor at present. Its last user was `stdio.json`'s `__stdio_common_vsprintf` row, retired when this file's own `pe` default became `ucrtbase.dll`; it is documented because the reader accepts it and the next split-runtime symbol will need it, not because anything depends on it today. ⚠ The `linkName` 33 are NOT 33 Darwin renames: **25 are `pe` rows** where the UCRT's own C identifier simply carries a leading underscore or a width suffix (`io.json`'s `_write`/`_read`, `sys/stat.json`'s `_fstat64i32`, `time.json`'s `_time64`), **8 are the Mach-O ones** — `stat` `fstat` `lstat` `statfs` `fstatfs` `opendir` `readdir` (x86_64 only, `$INODE64`) and `realpath` (BOTH arches, `$DARWIN_EXTSN`). ★ Those eight are the COMPLETE Darwin divergence set over the current 236-identifier Mach-O import surface, ✔MEASURED symbol-by-symbol against Apple `cc` and recorded in `tests/ffi/data/darwin-link-names.tsv`; `ffi/test_darwin_link_name_oracle` reds if a NEW symbol is declared without that measurement. |
| `realization` | no | **The SIBLING AXIS TO `library`, and the answer to a question `library` cannot ask.** `library` says WHICH IMAGE a symbol is imported FROM, per object format. `realization` says whether it is imported **at all** — a per-OBJECT-FORMAT map (`"pe"`/`"elf"`/`"macho"`) whose value is an object `{"source": "<path>"}` naming a file **DSS ships and COMPILES FOR THE TARGET**, config-root-relative (i.e. relative to `src/dss-config/`). ABSENT (every descriptor but `dirent.json` today) ⇒ IMPORT, the default, byte-identical to the pre-ruling image. Same closed object-format key vocabulary as `library`, same `decodeLibraryMap`-shaped chokepoint, and the SAME merge rule — a per-**symbol** `realization` is merged OVER the descriptor's (symbol keys win; a format the symbol omits inherits). That is what makes it an EXTENSION of this axis rather than a second, parallel notion of where a body comes from: "where does this symbol's body come from, on this format" was already this map's established shape. ★ **THE ENGINE NEVER BRANCHES ON FORMAT** — it reads the declared realization and either emits an import or adds the source file to the build graph; there is no `if (format == "pe")` on the path. ★ **THE VALUE IS A PATH, NOT A UNIT NAME**, so the string in the descriptor is the string you can paste into `ls` and it greps in BOTH directions; an earlier draft named a unit and derived the path from a manifest, and both the manifest and the derivation were deleted as extra owners of facts the descriptor and the file tree already hold. **FOUR REFUSALS, all checked format-INDEPENDENTLY so an arm no current target selects cannot rot:** **R1** a realization naming a source that is not there ⇒ LOAD ERROR naming BOTH the descriptor row and the missing path (at descriptor read time — this is the one that can otherwise produce a build silently missing a body); **R2** a source file NO descriptor names ⇒ inert config, refused by the gate test (`tests/ffi/test_shipped_source_realization.cpp`) rather than at load, because without the deleted manifest the check costs a directory walk plus a corpus scan per compile while an inert `.c` can only waste disk; **R3** one format carrying BOTH a `library` image and a `source` ⇒ LOAD ERROR — two owners for one body is the defect, not a fallback, and preferring either silently is how a program links against an image that does not export the symbol and dies at LOAD; **R4** no HEADERS in the runtime tree, folded into R2 rather than given an extension check — a header is never a translation unit, so no realization can name one, so the unclaimed-file rule refuses it by construction. See the section below. |
| `constants` | no\*     | The header's object-like `#define` macro-CONSTANTS as NEUTRAL named integer constants (e.g. `CHAR_BIT`). Each entry: `name`, `value` (a JSON integer — the int64 BIT-PATTERN; for an unsigned `type` the uint64 value reinterpreted, so the full unsigned range round-trips), `type` (a hir-text INTEGER-SCALAR type, `i8`…`u128`), OR per-target `variants` (`when` + `value` + `type`). The semantic phase injects each as a compile-time constant that folds to a literal in VALUE and CONSTANT-EXPRESSION position (`int a[CHAR_BIT]`). A non-integer-scalar type, an out-of-range / negative-for-unsigned value, or an unknown key **fails loud**. A function-like macro belongs in `macros`; a float one in `floatConstants`. |
| `floatConstants` | no\* | The header's FLOATING-point macro-constants (`math.json` ships `INFINITY` and `HUGE_VAL`) — the `constants` surface is integer-ONLY, so a float there fails loud. Each entry: `name`, `value` (a **string** — JSON has no Infinity/NaN, so `"inf"`/`"+inf"`/`"-inf"` map to the IEEE-754 infinities and any other string is a finite literal parsed by the one float decoder), `type` (a FLOAT scalar, `f32`/`f64`). A finite literal that OVERFLOWS to ±inf **fails loud** — only the explicit `inf` tokens may produce an infinity. |
| `typedefs`  | no\*     | The header's `typedef`s as NEUTRAL type aliases (e.g. `size_t`). Each entry: `name`, and EITHER a flat `type` (any hir-text type) OR per-target `variants` (`when` + `type`). Injected as a type-position name. A builtin type of the same name wins. |
| `structs`   | no\*     | The header's `struct tag { … };` with NAMED fields (e.g. `struct timeval`), since the hir-text `struct "N" { T,… }` spelling carries field types positionally but no names. Each entry: `name` (the tag) and EITHER `fields` (each `name` + `type`, plus an optional explicit byte `offset` for a foreign OVERLAPPING layout — all-or-none within one struct) OR per-target `variants` (`when` + `fields`, as `fcntl.json`'s reordered `struct flock` needs). The tag lands in the TAG namespace with a field scope; offsets are DERIVED by natural alignment unless given. |
| `unions`    | no\*     | The header's `union tag { … };` with NAMED members. Entry keys are `name` + `fields` ONLY — members overlay at offset 0 by union semantics, so an explicit `offset` on a member is **rejected**, and (unlike `structs`) there is **no `variants`** key: a per-target union layout is not expressible today. |
| `macros`    | no\*     | The header's `#define`s that are NOT compile-time constants — a PREPROCESSOR substitution rather than a semantic injection. Resolving `#include <header.h>` splices each as a synthetic `#define` into the macro table, so it expands in the rest of the TU BEFORE parse. Each entry: `name`, plus EITHER a flat body (`replacement`; optional `params` and `variadic`) OR per-FORMAT `variants` (each `when: {format}` carrying its own `replacement`/`params`/`variadic`). Declaring both a body key and `variants` **fails loud**. `params` ABSENT = object-like (`#define X 1`); PRESENT — even `[]` — = function-like (`assert(e)`, `_IOWR(g,n,t)`); `variadic` marks a trailing `...` and requires `params`. `replacement` may be empty (a null macro `#define X`). Unlike the other `variants` axes, a macro's `when` is **format-only** — arch is not threaded into the preprocessor. A newline in any macro field **fails loud** (it would break the spliced directive). Examples: `assert.json` (function-like), `errno.json` + `stdio.json` (per-format), `sys/ioctl.json` (per-format function-like). |
| `$comment`  | no       | The repo-wide config-documentation convention: a human provenance note (which real headers the values were measured against, which deferral applies, why a divergence exists). Accepted and ignored — never consumed by lowering. |

\* A descriptor must declare **at least one** of `symbols` / `constants` /
`floatConstants` / `typedefs` / `structs` / `unions` / `macros` (a descriptor
that declares nothing **fails loud** — and note `includes` does *not* count). A
header may legitimately carry only `constants` (e.g. `<limits.h>` — all macros,
no link surface; see `limits.json`), only `typedefs`, or only `macros` (e.g.
`<assert.h>` — `assert` is a function-like macro with no link surface at all;
see `assert.json`). For `constants` / `typedefs` / `structs` / `unions` /
`macros` the check counts the JSON DECLARATION, not the post-selection
injection — so a **variants-only** descriptor that injects nothing on the current
target (or when read with no active format, as the provenance sweep does) is
still a valid declaration, not a false "declares nothing". Of those, the surfaces
that actually accept a `variants` key are `constants`, `typedefs`, `structs` and
`macros`; `unions` does not.

Because a C `.h` is C-syntax TEXT, shipping one would couple the
language-NEUTRAL config to C — so a header's constants + typedefs + tags live
here as neutral data injected by the semantic phase (Item 1, 2026-06-22).
`macros` is the one surface that must reach the PREPROCESSOR rather than the
semantic phase, so it alone is reconstructed as `#define` text — from neutral
fields, never by reading the real header.

The `signature` grammar is the IR type-text vocabulary documented in
[`docs/ir-type-text.md`](../../../docs/ir-type-text.md) — the same codec the
hir-text round-trip uses, so a descriptor signature and an IR dump speak one
language. C types map as: `int`→`i32`, `unsigned int`→`u32`, `size_t`→`u64`,
`void*`→`ptr<void>`, `char*`→`ptr<char>`, `char**`→`ptr<ptr<char>>`,
`FILE*`→`ptr<struct "FILE" {}>`, a function pointer→`ptr<fn(...) -> ...>`.

---

## ABI deltas — the `long`-width data-model split

The C type `long` (and `unsigned long`) is **not** the same width everywhere:
**LP64** (Linux + macOS) makes it 64-bit; **LLP64** (Windows) makes it 32-bit.
Six symbols across two headers bear a `long` and so are data-model-dependent:

| Symbol (header)        | `signature` — the LP64 (linux/macos) base form | `signatureByDataModel.LLP64` (windows) |
|------------------------|---------------------------------------------|----------------------------|
| `atol` (stdlib)        | `fn(ptr<char>) -> i64`                      | `… -> i32`                 |
| `strtol` (stdlib)      | `… -> i64`                                  | `… -> i32`                 |
| `strtoul` (stdlib)     | `… -> u64`                                  | `… -> u32`                 |
| `labs` (stdlib)        | `fn(i64) -> i64`                            | `fn(i32) -> i32`           |
| `fseek` offset (stdio) | `fn(ptr<…FILE…>, i64, i32) -> i32`          | `fn(ptr<…FILE…>, i32, …)`  |
| `ftell` (stdio)        | `… -> i64`                                  | `… -> i32`                 |

A descriptor is **platform-neutral** (Model 3), so all six live in ONE file:
each carries the **LP64 (i64/u64)** form as its base `signature` and declares
the Windows form in a per-symbol **`signatureByDataModel.LLP64`** override
(see the `symbols` row above). The active object format's `dataModel` picks
one — `pe` is LLP64, `elf`/`macho` are LP64 — and the reader parses BOTH on
every read, so the inactive arm cannot rot. This closed
**`D-LANG-PLATFORM-DEPENDENT-PRIMITIVE-WIDTH`** on 2026-06-10 (FC3 c1) together
with the `coreByDataModel` axis that resolves the `long` KEYWORD the same way.
Two tests hold the two halves: `ShippedLibDescriptor.ShippedStdlibSignaturesAreLp64`
pins the LP64 base form for all six, and `Fc3Descriptor.FseekOffsetFollowsTheDataModel`
reads the SHIPPED `stdio.json` twice and pins that `fseek`'s offset comes back
i64 under LP64 and i32 under LLP64 — the model every `pe` format declares. Every
other symbol uses fixed-width or model-invariant types (`int`, `double`,
`size_t`, pointers) and is width-identical everywhere.

---

## Type IDENTITY — the vocabulary tag and the `dataModel` selector

A DSS primitive interns on **(representation, vocabulary name)**. The name comes
from the LANGUAGE config (`typeSpecifiers[].name`), the representation from the
TARGET. `long`, `unsigned long`, `long long`, `unsigned long long` and
`long double` are NAMED entries; `int`, `short`, `unsigned`, `float`, `double`,
`char`, `bool` are deliberately ANONYMOUS (they must stay the anonymous
representative of their core, because integer promotion and enum-underlying
synthesis independently re-mint them).

That makes a bare core in a descriptor a **third thing**: `ptr<u32>` is a
pointer to the ANONYMOUS 32-bit unsigned, which matches neither `unsigned long*`
nor `unsigned int*`… it matches only itself. So **any descriptor type a user
would spell with a NAMED vocabulary entry must carry the hir-text tag**:

```jsonc
{ "name": "LPDWORD", "type": "ptr<u32 \"unsigned long\">" }   // Win32 DWORD* IS unsigned long*
{ "name": "ssize_t", "type": "i64 \"long\"" }                 // POSIX ssize_t IS long
```

Where the width genuinely denotes the anonymous representative (`int`,
`unsigned`, `short`, `BOOL`, `WORD`, `mode_t`, …) the type stays **untagged** —
tagging it would be the same lie in the other direction.

When the correct NAME is **data-model-dependent** — C's `size_t` IS
`unsigned long` on LP64 and `unsigned long long` on LLP64, `ptrdiff_t` IS `long`
/ `long long`, and every `<stdint.h>` 64-bit alias follows — a fixed tag cannot
express it. `when` therefore carries a third selector axis alongside
`arch`/`format`:

```jsonc
{ "name": "size_t", "variants": [
    { "when": { "dataModel": "LP64" },  "type": "u64 \"unsigned long\"" },
    { "when": { "dataModel": "LLP64" }, "type": "u64 \"unsigned long long\"" } ] }
```

The contract is the same MATCH-ALL-SPECIFIED / exactly-one-match rule the other
axes use, and the value is validated against the closed data-model vocabulary
(`LP64`/`LLP64`/`ILP32`) so a typo **fails loud** instead of silently never
matching. Keys compose: `{ "format": "macho", "dataModel": "LP64" }`.

### Two ENFORCED rules — `F_ShippedTypeIdentityConflict`

Descriptors are authored independently but intern into ONE lattice, and both
injection paths are **first-wins by name**. Two rules are therefore
machine-checked (`ffi::ShippedTypeConsistency`, run by the semantic phase just
before injection, plus the exhaustive `tests/ffi/test_shipped_type_consistency.cpp`
sweep over every descriptor × every shipped target):

1. **One name, one type.** Every declaration of a struct/union **tag** — a
   `structs` entry, an INLINE `struct "N" {…}` inside another type's text, or a
   repeat in a *second descriptor* — must resolve to a byte-identical type for a
   given target. Same for a **typedef** name. Only the first-injected tag gets a
   field scope, so a divergent second declaration interns a *second* type whose
   members are unreachable — an **include-order-dependent** `S000D member access
   requires a composite-typed operand`. This is exactly how `struct timeval`
   broke when `sys/time.json` was retagged and its `sys/resource.json` twin (both
   the `structs` entry AND the two inline `struct "timeval" {…}` field texts) was
   not.

2. **A tag must be producible on every target the descriptor ships on.** A
   vocabulary name's WIDTH belongs to the data model, so a FLAT tag is only legal
   when every format in `availableObjectFormats` shares one model. `i64 "long"`
   on a descriptor that also ships on `pe` is a **phantom** — LLP64 mints `long`
   as I32, so that pair matches no `_Generic` association and no pointer of that
   spelling. Give the entry per-format / per-`dataModel` `variants` instead (as
   `off_t` and `ssize_t` do).

---

## Variadic functions — encodable, and shipped where a consumer exists

The IR type-text `fn` grammar takes a **trailing `...`**, so a variadic
signature is spellable and a descriptor can carry one. Twelve symbol entries
across four descriptors do:

| Descriptor | Variadic symbols |
|------------|------------------|
| `stdio.json`     | `printf`, `fprintf`, `sprintf`, `sscanf`, `snprintf` — each authored twice: an `[elf,macho]` import row and a `[pe]` `synthesize` row. `snprintf` reached that shape LAST (TF-C119; its macho half 2026-08-05) — see the note below for the measurement that unblocked it, and for the closing instrument that turned out to be broken. |
| `fcntl.json`     | `open`, `fcntl` |
| `io.json`        | `_open` |
| `sys/ioctl.json` | `ioctl` |

What is still absent is **not a grammar limit** — it is the ordinary
need-driven rule this whole directory follows: a symbol ships when a real
consumer lands, because DSS eager-imports every declared extern. So
`scanf`, `fscanf`, `vsnprintf`, `vsprintf` and the rest of the
family are simply not authored yet. (`vfprintf` IS shipped — it is not
variadic; it takes a `va_list`.)

**`snprintf` SHIPPED 2026-08-05 (TF-C119)** — its consumer, the SQLite CLI,
landed. ⚠ Three corrections to what this paragraph used to say, kept because
each was load-bearing and each was wrong:

- It said the CLI "needs `snprintf` on the POSIX legs". ✔MEASURED: `snprintf`
  was missing on **all three** of `elf64-x86_64`, `elf64-aarch64` **and**
  `pe64-x86_64` — shipped nowhere, so the POSIX framing implied a specificity
  that did not exist.
- The tracking anchor's remediation hint named `__stdio_common_vsnprintf` as
  the UCRT backing symbol. ✔MEASURED with `objdump -p` over
  `C:/Windows/System32/ucrtbase.dll` (2,484 exports): **that symbol does not
  exist**, and neither does a bare `snprintf`. Adding it would have broken
  every pe binary's LOAD with `0xC0000139` under
  `D-FFI-DESCRIPTOR-EAGER-IMPORT`. The real core is `__stdio_common_vsprintf`
  (ordinal 117), already shipped for `sprintf`, so the pe arm is a
  `synthesize` shim adding **zero** new imports.
- It said macho was **deliberately not shipped**: the row was gated `["elf"]`
  because the libSystem export was INFERRED and never measured (the operator's
  Mac was unreachable and the build host carries no macOS SDK). That staging
  was the correct call at the time, and it is now **CLOSED**. ✔MEASURED
  2026-08-05 on the operator's real Mac over ssh (**macOS 26.5.2, arm64**) —
  with `SDK=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk`,
  `grep -rwq -e _snprintf $SDK/usr/lib/libSystem.B.tbd` reports **PRESENT**, as
  do `_vsnprintf _popen _pclose _fileno _sysctl _sysctlbyname _exit _fstat64`
  in the same pass. The row is `["elf","macho"]` and the darwin arm of
  `examples/c/shipped_snprintf_ucrt` is restored — in the SAME edit,
  because either half alone is red. ⚠ **The closing instrument this file and
  the anchor both pinned — `nm -gU /usr/lib/libSystem.B.dylib | grep
  ' _snprintf$'` — is BROKEN. Do not re-run it.** ✔MEASURED on that same host:
  `ls: /usr/lib/libSystem.B.dylib: No such file or directory`. Modern macOS
  keeps libSystem only in the dyld shared cache and ships no such file, so `nm`
  on that path returns nothing for **every** symbol and would have reported a
  **false ABSENT for all nine names above**. Under
  `D-FFI-DESCRIPTOR-EAGER-IMPORT` that is the *dangerous* direction of wrong:
  it breaks no build, it silently withholds a real export and makes the staging
  look permanently justified. The correct **file-based** instrument for "does
  libSystem export X on Darwin" is the SDK `.tbd` text stub — a real readable
  file — grepped `-w` for the **mangled** C name with its leading underscore
  (so `_snprintf` does not match `_vsnprintf`). ★ `popen`/`pclose` were held at
  `["elf"]` alongside it on the rule that this directory ships on a
  **consumer**, not on a confirmed export — the `.tbd` pass showed Darwin has
  both, and that alone was deliberately not enough. **THE CONSUMER HAS SINCE
  LANDED** (the macho64-arm64 SQLite CLI, where those two names were the last
  remaining descriptor errors on the 103-TU build), so the SAME rule now ships
  them: both rows read `["elf","macho"]`. ✔The export is MEASURED, not
  inferred — on the operator's Mac (macOS 26.5.2, arm64) by sqlite's own probe
  methodology, an `extern void f(void);` TU **compiled AND linked** against
  `/usr/bin/cc`: `popen` LINK-OK, `pclose` LINK-OK, with `_popen`/`_pclose`
  concurring in `$SDK/usr/lib/libSystem.B.tbd`.

**`D-FFI-DESCRIPTOR-VARIADIC-SIGNATURE`** is the tracking anchor. Its original
subject — the missing `fn` grammar marker — no longer exists; what remains under
it is per-symbol authoring as consumers appear. A name absent from a descriptor
still fails LOUD at the reference, never silently.

---

## Per-target library selection (Model 3)

The active c config (`src/dss-config/sources/c.lang.json`) sets
`"shippedLibDirs": ["shippedLibs"]` — the single neutral directory. There is no
per-platform directory to choose: every target reads the SAME descriptors, and
each descriptor's `library` MAP names the runtime image per object format. At
`compile_pipeline` resolution the active target's format (`objectFormatKindName`
→ `"pe"`/`"elf"`/`"macho"`) selects its entry. ⚠ A map missing that format is
**UNBOUND**, resolved at the LINK tier per C23 5.1.1.2 phase 8 — it does NOT
inherit a per-language default. This sentence used to say it inherited
`externLibraryByFormat[format]`, and that was **stale**: UCRT-P4 (Decision 1)
REMOVED that field outright rather than repointing it, because a per-language
default is a guess about a fact the descriptor corpus owns per SYMBOL. The
`library` row in the table above has always been correct; this paragraph was not,
and the two contradicted each other in one file. This is the same neutral
descriptor + per-format map design throughout — agnostic, no `if(format)` in
shared substrate, and it dissolved the former `D-FFI-SHIPPED-LIB-PLATFORM-SELECT`
deferral entirely (a single descriptor set serves all targets).

`examples/c/shipped_include_puts` proves it end to end: `#include
<stdio.h>` + `puts("hello")` links `puts` against ucrtbase.dll on Windows-PE
(msvcrt.dll until the TF-C111 CRT migration), libc.so.6 on Linux-ELF (x86_64 +
arm64), and libSystem on macOS-Mach-O — from the one `stdio.json`.

---

## Per-target REALIZATION — when DSS ships the body (`D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF`)

The `library` map above answers *which image*. It cannot answer *what if there is
no image* — and that case is not exotic. **Windows has no POSIX directory API.**
`opendir` / `readdir` / `closedir` are exported by nothing: not `ucrtbase.dll`,
not `kernel32.dll`, not any Windows component. Under the eager-import law
(`D-FFI-DESCRIPTOR-EAGER-IMPORT`) declaring them anyway produces a binary the
loader rejects at process start with `0xC0000139` — rc=0 from every compile
stage.

**DSS ships the source.** This directory is the DECLARATION half of a toolchain;
`src/dss-config/runtime/` is the IMPLEMENTATION half, and the `realization` map
is the single fact that binds the two:

```jsonc
// dirent.json
"library":     { "elf": "libc.so.6", "macho": "/usr/lib/libSystem.B.dylib" },
"realization": { "pe": { "source": "runtime/platform/src/dirent.c" } }
```

On `elf`/`macho` no key is present, the default (IMPORT) stands, and those arms
are byte-identical to what shipped before. On `pe` the driver adds
`runtime/platform/src/dirent.c` to the build graph as an **ordinary extra
translation unit compiled FOR THE TARGET**, and the linker resolves the
reference against that body exactly as it resolves a sibling CU's definition.

### Why this is the standard structure, not an invention

Every production toolchain splits at one line. The **compiler** synthesizes only
*stateless* glue — builtins, thunks, TLS sequences, import stubs, `va_arg`,
libcall lowering: inline-able, no heap, no cross-call state. A **runtime library
of compiled source** provides everything with state, allocation or nontrivial
control flow: `libgcc`, `compiler-rt`, `libmingwex`, `newlib`/`musl`/`glibc`.
`opendir` holds a handle across three calls, allocates, and does string surgery —
squarely the second category, and *literally* the libmingwex case: mingw-w64
implements these three functions as ordinary C in `mingw-w64-crt/misc/dirent.c`,
over the same Win32 primitives DSS uses.

gcc's literal answer here is "link libmingwex". **DSS cannot take it**: depending
on a host mingw-w64 breaks *build ANY target inside ANY host*
(`D-HARNESS-CROSS-HOST-ANY-TARGET`) — a Windows-targeting build on a Mac would
need a mingw sysroot — and it re-introduces exactly the third-party runtime
dependency the pe→UCRT migration ran to eliminate. So DSS ships its own. This is
the standard structure reached by the only route the constraints allow.

★ **THE SCOPE RULE ABOVE NEEDED NO WIDENING FOR THIS.** It already says *OS
LIBRARIES ONLY … libc/POSIX, Win32, libSystem*, and already promises *"which is
why it can ship it, version it, and GUARANTEE IT ON EVERY HOST"*. `opendir` is
POSIX; POSIX is named in scope; the every-host guarantee is already made. What
was missing was not permission but the mechanism that **honours** what the scope
section already says.

### The tree

```
src/dss-config/runtime/
  platform/                    <- the LOW-LEVEL tier: OS / libc / ABI gap-filling
    src/dirent.c               <- authored C
    dist/{debug,release}/...   <- generated objects; GITIGNORED, anchored path
```

`runtime/` is a **tier namespace**. `platform/` is the low-level tier; high-level
language runtimes (a CLR, a JVM) land as siblings under `runtime/managed/<lang>/`.
It is **not** `os` (too narrow — `strcasecmp` is libc, not OS, and the soft-float
helpers this tier will eventually hold are pure computation) and **not** `native`
(a word already load-bearing in this repo's test vocabulary). `src/` and `dist/`
make the tree self-describing: authored source in one, generated objects in the
other, nothing generated ever interleaved with anything authored.

⚠ **NO `.h` LIVES HERE, AND THIS TREE IS NEVER AN INCLUDE-SEARCH ROOT.** A
private header for the dirent unit would land at a path that IS the include path
`dirent.h` and would **shadow the descriptor the unit exists to consume** —
silently, producing exactly the struct-layout disagreement described next. R4
refuses it, and the driver builds a runtime CU with the system descriptor dirs
only, never the user's `-I` list.

⚠ **THE PATH CARRIES NO FORMAT SEGMENT.** An earlier draft used
`platform/<format>/…`; it was overruled because the descriptor already names the
file, so the path need not disambiguate anything — and a format segment would
force every format-INDEPENDENT unit (a `strcasecmp`, a soft-float helper) either
to be duplicated across format directories or to sit in one that lies about it. A
format-specific unit gets a distinct FILE NAME; the per-format `realization` map
is what routes each format to its own file.

### ★★★ The property this design has that a compiler-built IR body does not

**The layout agreement is checked by the compiler.** `struct dirent` is declared
ONCE — here, in `dirent.json` — and `runtime/platform/src/dirent.c` `#include`s
`<dirent.h>` to get it. There is no second copy of the layout to drift, and no
way to ship a runtime that disagrees with its own published ABI.

✔**MEASURED, not claimed**: renaming `d_name` in this file's `pe` `struct dirent`
variant makes `runtime/platform/src/dirent.c` **fail to compile**, with the error
positioned in the runtime source. The alternative — hand-building the body in the
compiler's IR builder — has no such check: a layout mismatch there lands in the
*silent wrong answer* class (a wrong directory listing, the copy-relocation
`environ` shape), which is precisely what this project refuses to ship.

### Language neutrality

★ **"A C file is language-specific" is a category error.** `libgcc` is C and
serves gcc's C, C++, Fortran, Ada, Go and D front ends. An implementation written
*in* C is not an implementation *for* C — it sits behind an ABI. The descriptor
stays language-neutral (it **is** the ABI declaration); any DSS language that can
call the ABI gets `opendir`, **DSS Axis included, with no Axis-side work**. The
reader stays agnostic with no identity branch because the realization is declared
in DATA and never branched on in code.

The language is not restated anywhere: the file's `.c` extension resolves it
through `fileExtensions`, the same mechanism every ordinary compile uses, and
`.c` is claimed by exactly one shipped language. An extension no language claims,
or one that two claim (`.s`/`.S`, claimed by both asm dialects), **fails loud**
rather than guessing — that ambiguity is a real future fork for a hand-written
assembly runtime unit, and it needs the *arch*, not the language.

Bootstrap is not circular: DSS's C front end compiles the runtime and every other
language consumes it — gcc's arrangement exactly — and it dogfoods the compiler on
its own runtime, which is a real correctness signal.

⚠ **ONE UNIT PER HEADER**, mirroring one-descriptor-per-header. Deliberately not a
monolithic `dss-libc.c`: a monolith overclaims (DSS ships the **gap** between what
a platform provides and what POSIX declares — mingw-w64 names its equivalent
`libmingwex`, *extensions*, for exactly that reason) and would make every pe binary
carry every future unit, or force dead-stripping to undo it.

`examples/c/shipped_dirent_readdir` proves it end to end, and it is a
differential over the realization axis rather than over the source: the SAME
`main.c` binds three libc imports on elf/macho and links a DSS-compiled body on
pe, and all four legs answer 42.

---

## Adding or extending a descriptor

0. **Check the scope rule first (see "SCOPE" above). Is this header part of the
   OPERATING SYSTEM's surface?** libc/POSIX, Win32, libSystem: yes. Tcl, zlib,
   OpenSSL, curl, or anything a *user* chose to depend on: **NO — stop here.**
   That library is a per-leg compilation input declared by the build that
   consumes it, not a descriptor. If you are here because a build cannot find a
   third-party library's symbols on some host, adding a descriptor is the wrong
   fix and will be rejected in review.
1. Pick the right `<header>.json` (create it if the header is new — at the path
   the HEADER has, so `<sys/foo.h>` → `sys/foo.json`; never a per-platform subdir).
2. Add the symbol with its hir-text signature (see `docs/ir-type-text.md` for
   the vocabulary). If it involves `long`, author the **LP64** (i64/u64) form as
   the base `signature` **and** the Windows form as a
   `signatureByDataModel.LLP64` override — both are parsed on every read, so
   neither arm can rot (see "ABI deltas" above).
3. Set `header` (required) and the per-format `library` map (`pe`/`elf`/`macho`
   — `ucrtbase.dll` for a pe libc surface, not `msvcrt.dll`); `standard` is
   optional provenance.
4. **★★★ ASK THE PLATFORM TWO QUESTIONS ABOUT EVERY NEW FUNCTION SYMBOL, ONCE
   PER FORMAT IN ITS `availableObjectFormats`. This step is not optional and it
   is not a review nicety** — DSS EAGER-IMPORTS every function a descriptor
   lists, whether a program calls it or not
   ([[D-FFI-DESCRIPTOR-EAGER-IMPORT]]), so one wrong name breaks the **LOAD of
   every binary that `#include`s this header**, not merely the callers. Both
   questions have shipped as real defects; neither answers the other.

   **(a) DOES THE NAME EXIST IN THE RUNTIME IMAGE?** An absent name is a load
   failure with no link error and no diagnostic naming the JSON — pe dies at
   `0xC0000139` STATUS_ENTRYPOINT_NOT_FOUND, elf/macho exit 127.

   | format | instrument (✔all three run 2026-08-25) | reading |
   |--------|-----------|---------|
   | `pe`   | `objdump -p /c/Windows/System32/ucrtbase.dll \| grep -w <name>` | the export table; `[ 299] _fstat64i32` = present |
   | `elf`  | `nm -D /lib/x86_64-linux-gnu/libc.so.6 \| grep -w <name>` | a WEAK `W` counts as PRESENT; only ABSENT breaks. Shows the VERSION too (`realpath@@GLIBC_2.3`) — see the `version` key |
   | `macho`| `grep -rwq -e _<name> "$(xcrun --show-sdk-path)/usr/lib/libSystem.B.tbd"` | the SDK **`.tbd` stub**, and the DECORATED `_` spelling |

   ⛔ **Do NOT reach for `nm -gU /usr/lib/libSystem.B.dylib` on macho.** ✔MEASURED
   2026-08-25 on macOS 26.5.2: **that file does not exist** — libSystem is
   dyld-shared-cache-only — so the command reports a **false ABSENT for every
   symbol**, which under the eager-import law is the dangerous direction (it
   "confirms" a wrong staging decision). ⚠ And it is easy to miss: `nm` alone
   exits 1, but in the natural `nm … | grep -w <name>` shape the PIPELINE exits
   0 and prints nothing — indistinguishable from a real ABSENT. The `.tbd` stub
   above is the working instrument, negative control included (`_snprintf`
   PRESENT, a fabricated name ABSENT).

   **(b) DOES A REAL COMPILER FOR THIS TARGET EMIT *THIS* NAME FOR *THIS* C
   IDENTIFIER?** ★ **(a) does NOT answer this, and that is the whole point.** An
   export check asks *"does this name exist"*; the defect that corrupted a real
   database was that the **wrong existing name** was declared. ✔MEASURED: on
   x86_64 **both** `_fstat` **and** `_fstat$INODE64` resolve in libSystem and
   they are **different functions** (`dlsym`, `same=0`) — so (a) passes on the
   wrong one, the build links, the image loads, and `fstat` writes 120 bytes into
   DSS's modern 144-byte `struct stat`, reports `st_size == 0`, and sqlite calls
   every database "malformed". Compile a one-line TU with the platform toolchain
   and read the undefined symbol, **per arch**:

   ```sh
   printf '#include <%s>\nvoid *p(void){return (void*)&%s;}\n' <header> <name> > t.c
   cc -arch x86_64 -fno-builtin -fno-stack-protector -w -c -o t.o t.c && nm -u t.o
   cc -arch arm64  -fno-builtin -fno-stack-protector -w -c -o t.o t.c && nm -u t.o
   ```

   Exactly one undefined symbol comes back. If it is not `_<name>`, the
   descriptor needs a `linkName` (see the schema table) — **UNDECORATED**, and
   per-target if the two arches disagree. ⚠ Two traps this probe has already
   sprung: `opendir`/`readdir` are renamed through `__DARWIN_ALIAS_I` and are
   **invisible to a header grep** — only compiling finds them; and a
   `<compile-failed>` means the probe header is wrong **for the platform**, since
   DSS's shipped headers are a deliberate SUPERSET (`statfs`/`fstatfs` live in
   DSS's `<unistd.h>` and in Darwin's `<sys/mount.h>`, `setlocale` in DSS's
   `<stdlib.h>` and Darwin's `<locale.h>`) — find the real header, never skip the
   symbol. Two of the eight names that diverge on Darwin are in that group.

   Record the macho answer in **`tests/ffi/data/darwin-link-names.tsv`**; a new
   Mach-O-visible function with no row there is RED
   (`ffi/test_darwin_link_name_oracle`), which is how this step is enforced
   rather than merely requested. `scripts/ssh-macos/ssh-macos.sh` reaches the
   operator's Mac.
5. `AllShippedDescriptorsDecode` will validate the new file decodes and every
   signature parses. Add an end-to-end corpus under `examples/c/` if it
   introduces a runtime-observable path not yet exercised.

A descriptor with a signature the codec cannot decode is a hard error
(`F_ShippedLibUnsupportedType`) — never a silently-skipped symbol.
