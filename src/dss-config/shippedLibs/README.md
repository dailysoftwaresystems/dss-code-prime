# Shipped-library FFI descriptors

This directory holds the **language-neutral FFI descriptors** for the standard
libraries the compiler ships against. A descriptor is the machine-readable
answer to "what functions does `<stdio.h>` provide, and what are their
signatures?" — the analogue of a C system header, but expressed once, in a
form every source language can consume.

When a c-subset program writes `#include <stdio.h>` with **no** inline
`extern`, the angle-include resolver maps the header stem to the matching
`*.json` here (on the `semantics.shippedLibDirs` system search path), the
semantic analyzer injects the descriptor's symbols into scope before name
resolution, and the linker resolves them against the runtime image for the
active compilation target's object format — exactly like real C. See
`examples/c-subset/shipped_include_puts/` (stdio) and
`examples/c-subset/shipped_include_abs/` (stdlib) for end-to-end proofs.

**Model 3 (2026-06-09)** — descriptors are **platform-neutral**: ONE descriptor
per header (a flat `<stem>.json`, no per-platform directories), carrying a
per-object-format `library` map (`{"pe":…,"elf":…,"macho":…}`). The active
target's object format selects its runtime image at `compile_pipeline`
resolution time (keyed by `objectFormatKindName`), so the same descriptor serves
every target. This dissolved the former per-platform-directory layout and the
`D-FFI-SHIPPED-LIB-PLATFORM-SELECT` deferral.

The universal reader is `src/ffi/shipped_lib_descriptor.{hpp,cpp}`. It is
**source/target/linker agnostic**: pure `nlohmann/json` + the single
`parseTypeFromText` codec, with no language/CPU/format identity branch.

---

## Directory layout

```
shippedLibs/
  stdio.json             <stdio.h>   — I/O
  stdlib.json            <stdlib.h>  — general utilities, alloc, conversion
  string.json            <string.h>  — byte-string / memory ops
  ctype.json             <ctype.h>   — character classification
  math.json              <math.h>    — floating-point math
```

FLAT and platform-neutral (Model 3): one descriptor per header, each carrying a
per-object-format `library` map. There are no per-platform subdirectories — the
target's object format picks the runtime image from the map at resolution time.

---

## Descriptor schema

```json
{
  "header":   "stdio.h",        // REQUIRED — provenance: which C header
  "standard": "c89",            // optional — provenance: which language standard
  "library": {                  // per-object-format runtime image (Model 3)
    "pe":    "msvcrt.dll",
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

| Field       | Required | Meaning |
|-------------|----------|---------|
| `header`    | **yes**  | The header these symbols come from (`stdio.h`). This is the provenance answer to *"where does `puts` come from?"* — a descriptor that omitted it would defeat the purpose, so the reader **fails loud** (`F_ShippedLibDescriptorMalformed`) if it is missing or empty. |
| `standard`  | no       | The language standard the surface targets (`c89`, `c99`, …). Provenance only. |
| `library`   | no       | A per-OBJECT-FORMAT MAP (`"pe"`/`"elf"`/`"macho"` → runtime image). The active compilation target's object format selects its entry at `compile_pipeline` resolution (keyed by `objectFormatKindName`). **Optional**: a map MISSING the active format's key (or absent entirely) inherits the language's `externLibraryByFormat[format]` default for that format. A key NOT in the object-format vocabulary (a typo like `"pee"`) **fails loud** (`F_ShippedLibDescriptorMalformed`) on read. |
| `symbols`   | yes      | The exported surface. Each entry: `name`, `signature` (a hir-text type string), `kind`, `linkage`. |

The `signature` grammar is the IR type-text vocabulary documented in
[`docs/ir-type-text.md`](../../../docs/ir-type-text.md) — the same codec the
hir-text round-trip uses, so a descriptor signature and an IR dump speak one
language. C types map as: `int`→`i32`, `unsigned int`→`u32`, `size_t`→`u64`,
`void*`→`ptr<void>`, `char*`→`ptr<char>`, `char**`→`ptr<ptr<char>>`,
`FILE*`→`ptr<struct "FILE" {}>`, a function pointer→`ptr<fn(...) -> ...>`.

---

## ABI deltas — the `long`-width data-model split (deferred)

The C type `long` (and `unsigned long`) is **not** the same width everywhere:
**LP64** (Linux + macOS) makes it 64-bit; **LLP64** (Windows) makes it 32-bit.
Six symbols across two headers bear a `long` and so are data-model-dependent:

| Symbol (header)        | LP64 (linux/macos) — the form authored here | LLP64 (windows) — deferred |
|------------------------|---------------------------------------------|----------------------------|
| `atol` (stdlib)        | `fn(ptr<char>) -> i64`                      | `… -> i32`                 |
| `strtol` (stdlib)      | `… -> i64`                                  | `… -> i32`                 |
| `strtoul` (stdlib)     | `… -> u64`                                  | `… -> u32`                 |
| `labs` (stdlib)        | `fn(i64) -> i64`                            | `fn(i32) -> i32`           |
| `fseek` offset (stdio) | `fn(ptr<…FILE…>, i64, i32) -> i32`          | `fn(ptr<…FILE…>, i32, …)`  |
| `ftell` (stdio)        | `… -> i64`                                  | `… -> i32`                 |

Because a descriptor is now **platform-neutral** (Model 3), these six carry a
SINGLE authored form — the **LP64 (i64/u64)** form, correct for the runnable
linux/macos targets. The Windows LLP64 (i32/u32) form is **latently deferred**:
it is UNEXERCISED by any corpus/test, and is tracked by
**`D-LANG-PLATFORM-DEPENDENT-PRIMITIVE-WIDTH`** (a `long` whose width depends on
the data model). When that anchor lands a per-target primitive-width model, the
neutral `long` will resolve to i32 on Windows and i64 on Unix automatically; the
test `ShippedLibDescriptor.ShippedStdlibSignaturesAreLp64` guards the current
form. Every other symbol uses fixed-width or model-invariant types
(`int`, `double`, `size_t`, pointers) and is width-identical everywhere.

---

## What is deliberately NOT here — variadic functions

`printf`, `fprintf`, `scanf`, `sscanf`, `snprintf`, and the rest of the
variadic surface are **excluded**. The IR type-text `fn(...)` grammar has no
variadic marker yet, so a variadic signature is currently unencodable — and the
reader fails loud rather than mint a wrong fixed-arity stand-in. This is pinned
as **`D-FFI-DESCRIPTOR-VARIADIC-SIGNATURE`** (trigger: add a variadic marker to
the `fn` grammar, then these descriptors gain their `printf`-family symbols).
Until then, a program needing `printf` declares it with an inline `extern` of
the concrete arity it calls.

---

## Per-target library selection (Model 3)

The active c-subset config (`src/dss-config/sources/c-subset.lang.json`) sets
`"shippedLibDirs": ["shippedLibs"]` — the single neutral directory. There is no
per-platform directory to choose: every target reads the SAME descriptors, and
each descriptor's `library` MAP names the runtime image per object format. At
`compile_pipeline` resolution the active target's format (`objectFormatKindName`
→ `"pe"`/`"elf"`/`"macho"`) selects its entry; a map missing that format inherits
the language's `externLibraryByFormat[format]` default. This is the same neutral
descriptor + per-format map design throughout — agnostic, no `if(format)` in
shared substrate, and it dissolved the former `D-FFI-SHIPPED-LIB-PLATFORM-SELECT`
deferral entirely (a single descriptor set serves all targets).

`examples/c-subset/shipped_include_puts` proves it end to end: `#include
<stdio.h>` + `puts("hello")` links `puts` against msvcrt.dll on Windows-PE,
libc.so.6 on Linux-ELF (x86_64 + arm64), and libSystem on macOS-Mach-O — from
the one `stdio.json`.

---

## Adding or extending a descriptor

1. Pick the right `<header>.json` (create it if the header is new — flat, no
   per-platform subdir).
2. Add the symbol with its hir-text signature (see `docs/ir-type-text.md` for
   the vocabulary). If it involves `long`, author the **LP64** (i64/u64) form
   and note the `D-LANG-PLATFORM-DEPENDENT-PRIMITIVE-WIDTH` deferral (see above).
3. Set `header` (required) and the per-format `library` map (`pe`/`elf`/`macho`);
   `standard` is optional provenance.
4. `AllShippedDescriptorsDecode` will validate the new file decodes and every
   signature parses. Add an end-to-end corpus under `examples/c-subset/` if it
   introduces a runtime-observable path not yet exercised.

A descriptor with a signature the codec cannot decode is a hard error
(`F_ShippedLibUnsupportedType`) — never a silently-skipped symbol.
