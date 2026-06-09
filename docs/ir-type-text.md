# DSS Code Prime — IR Type-Text Format

> **For contributors reading or writing IR text.** This is the canonical textual syntax the IR (HIR / MIR / LIR text formats) uses for every type, and the exact grammar the shared decoder `dss::parseTypeFromText` accepts. About 10 minutes; reference-style.
>
> Companions: [`tree-model.md`](./tree-model.md), [`language-config-spec.md`](./language-config-spec.md). Authoritative grammar: `src/hir/hir_text.cpp::parseType()` + `parseTypeListUntil` + the primitive table `primName`/`primFromName`.

---

## 1. One decoder, no drift

There is exactly **one** type-text decoder in the codebase: `dss::parseTypeFromText` (declared in [`src/hir/hir_text.hpp`](../src/hir/hir_text.hpp), implemented by the internal `parseType()` in `src/hir/hir_text.cpp`). Every consumer of the textual type syntax routes through it, so the grammar below is authoritative for all of them:

- **The HIR/MIR/LIR text round-trip.** The textual IR formats print types via `primName(...)` / the structural printers, and parse them back via `parseType()`. Print → parse is a fixed point for every form here.
- **The shipped-library FFI descriptor.** Each symbol's `signature` field in a `shippedLibs/<platform>/<lib>.json` descriptor is an IR type-text string, decoded by this same `parseTypeFromText`. See the [Shipped-library FFI descriptor](./language-config-spec.md#12-shipped-library-ffi-descriptor) section of the config spec.

Because both surfaces share the single decoder, the type syntax cannot drift between the IR dumps and the FFI descriptors — there is no second grammar to keep in sync.

```cpp
#include "hir/hir_text.hpp"

// Parse a standalone type string into a TypeId in the caller's interner.
TypeId t = dss::parseTypeFromText("fn(ptr<char>) -> i32", interner, typeReg, reporter);
```

`parseTypeFromText` interns the result into the **caller-provided** `TypeInterner` (and `TypeRegistry`, for `ext` extension kinds); the returned `TypeId` belongs to that interner's CU. On malformed or unknown text it returns `InvalidType` and emits at least one `Error`-severity diagnostic — it never returns a partially built type silently. The input must be a **single, complete** type: trailing tokens after one type are reported as an error.

---

## 2. Type forms

Every form below is exactly what `parseType()` accepts. The parser dispatches on a leading keyword (an identifier), so every type starts with one of these words.

### 2.1 `invalid`

The invalid/sentinel type.

```
invalid
```

Decodes to `InvalidType`. Round-trips the "no type" sentinel — e.g. an extern function with no parsed signature.

### 2.2 Primitives

A bare primitive keyword. The accepted names are exactly those in the `primName` / `primFromName` tables:

| Name | Kind | Name | Kind |
|---|---|---|---|
| `bool` | boolean | `char` | character |
| `i8` | signed 8-bit | `byte` | raw byte |
| `i16` | signed 16-bit | `u8` | unsigned 8-bit |
| `i32` | signed 32-bit | `u16` | unsigned 16-bit |
| `i64` | signed 64-bit | `u32` | unsigned 32-bit |
| `i128` | signed 128-bit | `u64` | unsigned 64-bit |
| `f16` | float 16-bit | `u128` | unsigned 128-bit |
| `f32` | float 32-bit | `void` | void / no value |
| `f64` | float 64-bit | | |
| `f128` | float 128-bit | | |

```
i32        // a 32-bit signed integer
char       // the character type
void       // the void type
```

There is no separate `f80` or platform-word keyword — the set above is the complete primitive vocabulary.

### 2.3 Single-element wrappers — `ptr` `ref` `nullable` `optional` `slice`

Each wraps exactly one element type in angle brackets:

```
ptr<T>          // pointer to T
ref<T>          // reference to T
nullable<T>     // a nullable T (the null-able pointer-shaped form)
optional<T>     // an optional T
slice<T>        // a slice (pointer + length) of T
```

Examples:

```
ptr<char>          // pointer to char  (the C `char*`)
ref<i32>           // reference to i32
nullable<ptr<u8>>  // nullable pointer-to-byte
slice<f32>         // slice of f32
```

> **Note — `fnptr<T>` is recognized but not constructible.** The parser accepts the `fnptr<...>` spelling syntactically but then emits a malformed-type diagnostic ("`fnptr<>` is not constructible in this interner") and returns `InvalidType`. Use `ptr<fn(...) -> ...>` for a function pointer.

### 2.4 Sized numeric/array forms — `vec` `mat` `arr`

These take an element type plus integer dimension(s):

```
vec<T, n>          // SIMD vector of n lanes of T
mat<T, r, c>       // matrix with r rows, c columns of T
arr<T, n>          // array of n elements of T
```

Examples:

```
vec<f32, 4>        // 4-lane f32 vector
mat<f32, 4, 4>     // 4x4 f32 matrix
arr<i32, 8>        // array of 8 i32
```

The dimensions are integer literals (parsed via `takeInt`); `vec`/`arr` take one, `mat` takes two (rows, then columns).

### 2.5 `tuple`

A heterogeneous tuple of zero or more element types in angle brackets:

```
tuple<T, ...>
```

Examples:

```
tuple<i32, f64>           // a pair
tuple<ptr<char>, i32, bool>
tuple<>                   // the empty tuple
```

Elements are comma-separated and parsed until the closing `>`.

### 2.6 `struct` and `union`

A nominal aggregate with a quoted name and a brace-delimited list of member types:

```
struct "Name" { T, ... }
union "Name" { T, ... }
```

Examples:

```
struct "Point" { f32, f32 }
union "Value" { i64, f64, ptr<char> }
struct "Empty" { }
```

The name is a quoted string literal; members are comma-separated types until the closing `}`. The member *types* round-trip; member *names* are not part of the type record (they live in the `SemanticModel`).

### 2.7 `enum`

A nominal enum with a quoted name and an **optional** underlying-type selector:

```
enum "Name"
enum "Name" : <underlyingOrdinal>
```

Examples:

```
enum "Color"              // underlying defaults to i32
enum "Flags" : 7          // underlying = the TypeKind whose ordinal is 7
```

When the `: <ordinal>` suffix is present, the integer is read as a `TypeKind` ordinal and used as the enum's underlying scalar type (it must be a valid `TypeKind` ordinal; otherwise the default `i32` is kept). Enumerator *names* are not stored in the type record — only the nominal name and the underlying `TypeKind` round-trip here.

### 2.8 `fn` — function signatures

A function signature: a parenthesized, comma-separated parameter-type list, an `->` arrow, a single result type, and an **optional** calling-convention suffix:

```
fn(params...) -> result
fn(params...) -> result cc <name>
```

Examples:

```
fn() -> void                        // no params, returns void
fn(i32, i32) -> i32                 // (i32, i32) -> i32
fn(ptr<char>) -> i32                // pointer-to-char -> i32  (C `int puts(const char*)`)
fn(i32) -> i64 cc sysv              // explicit System V calling convention
```

- **Params** are parsed until the closing `)` (zero or more, comma-separated).
- **Result** is a single type after `->`.
- **Calling convention** is optional. When absent, the signature defaults to `CcSysV`. When present, the `cc` keyword is followed by a calling-convention name (resolved via `callConvFromName`; an unknown name is reported as malformed and falls back to `CcSysV`).

> **Variadic limitation.** `parseType`'s `fn` arm does **not** currently parse a variadic marker. It always interns the signature as non-variadic (it calls the three-argument `fnSig(params, result, cc)` form; the interner's variadic flag is never set from text). There is no accepted spelling for a variadic `fn(...)` in the type-text grammar today — a C-style variadic prototype like `printf` cannot be expressed as a variadic signature through this decoder.

### 2.9 `ext` — extension types

A language-specific nominal extension type (see [`language-config-spec.md` §10.1 `typeExtensions`](./language-config-spec.md#101-typeextensions--per-language-extension-type-kinds)): a quoted name, a parenthesized list of type arguments, and an **optional** bracketed list of integer scalars:

```
ext "name" (args...)
ext "name" (args...) [scalars...]
```

Examples:

```
ext "TSQL::RowType" ()
ext "TSQL::Varchar" () [255]
ext "Lang::Thing" (i32, ptr<char>) [1, -2, 3]
```

- **Args** are comma-separated *types* parsed until `)`.
- **Scalars** are an optional `[...]` list of integer literals (each may be negated with a leading `-`), comma-separated until `]`.

The name is registered into the `TypeRegistry` as an extension kind, then interned as an extension type carrying the args and scalars.

---

## 3. Worked example

```
fn(ptr<char>) -> i32
```

Reading left to right:

- `fn(` opens a function signature's parameter list.
- `ptr<char>` — one parameter: a pointer to `char`.
- `)` closes the parameter list.
- `-> i32` — the result type is `i32`.
- No `cc` suffix → the default `CcSysV` calling convention.

This is the IR type-text for the C signature `int puts(const char *)`: a function taking a pointer-to-`char` and returning `i32`. It is exactly the string the shipped `stdio.json` FFI descriptor uses for `puts` (see the [Shipped-library FFI descriptor](./language-config-spec.md#12-shipped-library-ffi-descriptor) section).

---

## 4. Quick reference

| Form | Example |
|---|---|
| invalid | `invalid` |
| primitive | `i32`, `char`, `void`, `bool`, `f64`, `u128`, `byte`, … |
| pointer / reference | `ptr<T>`, `ref<T>` |
| nullable / optional | `nullable<T>`, `optional<T>` |
| slice | `slice<T>` |
| vector / matrix / array | `vec<T, n>`, `mat<T, r, c>`, `arr<T, n>` |
| tuple | `tuple<T, ...>` |
| struct / union | `struct "N" { T, ... }`, `union "N" { T, ... }` |
| enum | `enum "N"`, `enum "N" : <ordinal>` |
| function | `fn(T, ...) -> R`, `fn(...) -> R cc <name>` |
| extension | `ext "name" (T, ...)`, `ext "name" (...) [n, ...]` |
