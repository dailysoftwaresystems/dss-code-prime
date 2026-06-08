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
resolution, and the linker resolves them against the platform runtime — exactly
like real C. See `examples/c-subset/shipped_include_puts/` (stdio) and
`examples/c-subset/shipped_include_abs/` (stdlib) for end-to-end proofs.

The universal reader is `src/ffi/shipped_lib_descriptor.{hpp,cpp}`. It is
**source/target/linker agnostic**: pure `nlohmann/json` + the single
`parseTypeFromText` codec, with no language/CPU/format identity branch.

---

## Directory layout

```
shippedLibs/
  <platform>/            e.g. windows-x86_64, linux-x86_64, macos-arm64
    stdio.json           <stdio.h>   — I/O
    stdlib.json          <stdlib.h>  — general utilities, alloc, conversion
    string.json          <string.h>  — byte-string / memory ops
    ctype.json           <ctype.h>   — character classification
    math.json            <math.h>    — floating-point math
```

Each `<platform>` directory is a self-contained ABI surface. The platform name
is `<arch>-<os>`-shaped so a future target-driven selector can pick the right
directory from the target triple.

---

## Descriptor schema

```json
{
  "header":   "stdio.h",        // REQUIRED — provenance: which C header
  "standard": "c89",            // optional — provenance: which language standard
  "library":  "msvcrt.dll",     // the runtime that exports these symbols
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
| `library`   | no       | The runtime image that exports the symbols (provenance + the eventual link target — today the linker resolves against the format's default runtime via `externLibraryByFormat`). **Optional** in the reader: when absent/empty the lowering inherits the language's per-format default. Every shipped descriptor here sets it by convention, but a descriptor MAY omit it. |
| `symbols`   | yes      | The exported surface. Each entry: `name`, `signature` (a hir-text type string), `kind`, `linkage`. |

The `signature` grammar is the IR type-text vocabulary documented in
[`docs/ir-type-text.md`](../../../docs/ir-type-text.md) — the same codec the
hir-text round-trip uses, so a descriptor signature and an IR dump speak one
language. C types map as: `int`→`i32`, `unsigned int`→`u32`, `size_t`→`u64`,
`void*`→`ptr<void>`, `char*`→`ptr<char>`, `char**`→`ptr<ptr<char>>`,
`FILE*`→`ptr<struct "FILE" {}>`, a function pointer→`ptr<fn(...) -> ...>`.

---

## ABI deltas — why a signature differs per platform

The C type `long` (and `unsigned long`) is **not** the same width everywhere,
so the descriptors are **not** byte-identical across platforms. Two data models
are in play:

| Data model | Platforms here          | `long` / `unsigned long` |
|------------|-------------------------|--------------------------|
| **LP64**   | `linux-x86_64`, `macos-arm64` | 64-bit → `i64` / `u64` |
| **LLP64**  | `windows-x86_64`        | 32-bit → `i32` / `u32` |

This changes the encoded signature of every function that takes or returns a
`long`:

| Symbol (header)        | LP64 (linux/macos)                    | LLP64 (windows)                       |
|------------------------|---------------------------------------|---------------------------------------|
| `atol` (stdlib)        | `fn(ptr<char>) -> i64`                | `fn(ptr<char>) -> i32`                |
| `strtol` (stdlib)      | `… -> i64`                            | `… -> i32`                            |
| `strtoul` (stdlib)     | `… -> u64`                            | `… -> u32`                            |
| `labs` (stdlib)        | `fn(i64) -> i64`                      | `fn(i32) -> i32`                      |
| `fseek` offset (stdio) | `fn(ptr<…FILE…>, i64, i32) -> i32`    | `fn(ptr<…FILE…>, i32, i32) -> i32`    |
| `ftell` (stdio)        | `… -> i64`                            | `… -> i32`                            |

The `linux-x86_64` and `macos-arm64` surfaces are signature-identical (both
LP64); they differ only in `library` (`libc.so.6` vs `libSystem.B.dylib`).

Functions whose C signatures use fixed-width or model-invariant types
(`int`, `double`, `size_t`, pointers) are identical across all three platforms.

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

## Platform selection — current status

The active c-subset config (`src/dss-config/sources/c-subset.lang.json`) sets
`"shippedLibDirs": ["shippedLibs/windows-x86_64"]`, so today the
**`windows-x86_64`** surface is the one live on the search path. The
`linux-x86_64` and `macos-arm64` surfaces are authored and validated (every
descriptor is proven to decode by `tests/ffi/test_shipped_lib_descriptor.cpp ::
AllShippedDescriptorsDecode`) but await **target-driven selection** — choosing
the directory from the compilation target's triple instead of a fixed config
string. That wiring is pinned as **`D-FFI-SHIPPED-LIB-PLATFORM-SELECT`**.

The fixed platform string lives in **config** (`*.lang.json`), never in engine
code — the engine reads `shippedLibDirs` generically — so this is an honest
staged deferral, not a hardcode in shared substrate.

---

## Adding or extending a descriptor

1. Pick the right `<platform>/<header>.json` (create it if the header is new).
2. Add the symbol with its hir-text signature (see `docs/ir-type-text.md` for
   the vocabulary). Mind the ABI delta if it involves `long`.
3. Set `header` (required) and `library`; `standard` is optional provenance.
4. `AllShippedDescriptorsDecode` will validate the new file decodes and every
   signature parses. Add an end-to-end corpus under `examples/c-subset/` if it
   introduces a runtime-observable path not yet exercised.

A descriptor with a signature the codec cannot decode is a hard error
(`F_ShippedLibUnsupportedType`) — never a silently-skipped symbol.
