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
struct "Name" [opaque | [rec <H>] [packed]] { T [@off | ~align] [packed], ... }
union  "Name" [opaque | [rec <H>] [packed]] { T [packed], ... }
```

Examples:

```
struct "Point" { f32, f32 }
union "Value" { i64, f64, ptr<char> }
struct "Empty" { }
struct "FILE" opaque              // INCOMPLETE — no field list, and `{}` would be a different type
struct "Node" rec 1 { i32, ptr<rec 1> }   // self-referential
```

The name is a quoted string literal; members are comma-separated types until the closing `}`. The member *types* round-trip; member *names* are not part of the type record (they live in the `SemanticModel`).

**`opaque`** marks an **incomplete** composite (a tag declared but not defined). It is terminal — no field list follows — and it is *not* interchangeable with `{ }`, which is a legal **complete** zero-member composite. Both `struct` and `union` accept it.

**`rec <H>`** marks a composite whose own type graph reaches itself, and **`rec <H>` in type position** is the back-reference that closes the cycle. `H` is a handle for the *composite*, artifact-local and 1-based, and the same composite carries the same handle everywhere in one text — which is what lets a mutually recursive pair be written down, since one type then has two different spellings in one text. A back-reference resolves only against composites **currently open** at that point; a handle of `0`, a back-reference to a closed composite, and one handle carrying two different bodies are each refused by name.

**⚠ Inline composites and `rec <H>` are the STANDALONE form.** A `.dsshir` *module* (format v5 and later) defines every composite ONCE, in its `types` section, and names it everywhere else as **`type <H>`** — so in a module an inline `struct "N" {…}`, `union "N" {…}` or `rec <H>` is refused by name, and `type <H>` is the only composite spelling. A standalone type text — every FFI-descriptor signature `parseTypeFromText` decodes — has no `types` table, so it keeps every form in this section unchanged and refuses `type <H>` instead, by name, as module-only. See [`hir-text-format.md` §4.7 and §5.1](./hir-text-format.md) for the table, the cycle and mutual-recursion examples, and what a reader that re-interns must do about them.

**The MIR text follows the same rule since `.dssir` v2.** A `.dssir` module defines every composite ONCE, in a `types` section between the `dssir <version>` header and the `symbols` preamble, with the **same entry spelling** as the HIR table — `type <H> = struct|union "<name>" (opaque | [packed] [aligned N] [pack N] { <type> [@N | ~N] [bits N] [packed], … })` — and names it everywhere else as `type <H>`. Both tables are written and completed through one owner of what a definition carries (`src/core/types/type_lattice/composite_definition.hpp`), so the two tiers cannot drift on a layout channel. `.dssir` v1 spelled every composite inline at every use, with its field types only; a v1 text is refused by the version check, and an inline `struct "N" {…}` in a v2 module is refused by name.

**`packed`** (after the name) is the whole-composite packed flag; **`@<byteOffset>`** and **`~<align>`** after a field are explicit offsets and per-member alignment (all-or-none, and mutually exclusive with each other); a trailing **`packed`** on one field is the per-member packed attribute.

### 2.7 `enum`

A nominal enum with a quoted name and an **optional** underlying-type selector, a **fixed** underlying type, or a **chosen** compatible type:

```
enum "Name"
enum "Name" : <underlyingKeyword>
enum "Name" fixed <primitive>
enum "Name" chosen <primitive>
```

Examples:

```
enum "Color"              // underlying defaults to i32
enum "Flags" : u8         // underlying = u8
enum "Wide" fixed i64 "long"   // C23 `enum Wide : long` — the FIXED underlying type, `long`
enum "Small" fixed u8     // C23 `enum Small : unsigned char`
enum "Hue" chosen u32 "unsigned int"   // C `enum Hue { RED, GREEN }` — no fixed type; the language chose `unsigned int`
```

When the `: <keyword>` suffix is present it is a **primitive type keyword**, the same spelling the primitive table prints — never a `TypeKind` *ordinal*, which is version-fragile and is deliberately not serialized anywhere in this format. A keyword the table does not carry is refused by name, with the accepted set. The suffix is omitted when the underlying type is the `i32` default, and the reader's default matches. Enumerator *names* are not stored in the type record — only the nominal name and the underlying kind round-trip here.

**`fixed <primitive>`** spells an enumeration whose underlying type is FIXED (C23 6.7.3.3, `enum E : long`). The fixed type is part of the enumeration's identity — `enum E : long` and `enum E` are different types (C23 6.2.7p1), and a fixed `enum E : int` is not the plain `enum E` either — so it is written as the TYPE the clause named, with the primitive rule's optional vocabulary tag (`i64 "long"` is `long`; a bare `i64` is the anonymous 64-bit integer), and the underlying kind is that type's: no `:` follows. The `.dsshir` text carries the tag; the `.dssir` text, which carries no vocabulary tag on any primitive, writes `fixed i64` and reads back a fixed enumeration over the anonymous type. Added in `.dsshir` v6 and `.dssir` v3.

**`chosen <primitive>`** spells the integer type an enumeration WITHOUT a fixed underlying type is compatible with, when its language CHOSE one (C 6.7.2.2p4, C23 6.7.3.3p13 — C's choice rule is the `enumerationCompatibleTypes` ladders of `c.lang.json`). It is a different record from `fixed` with the same type — `enum E : unsigned int` and an `enum E` whose compatible type is `unsigned int` are different types (C23 6.2.7p1) — hence a different word; the kind, the tag and the `.dssir` spelling (`chosen u32`) follow `fixed`'s rules. An enumeration of a language that declares no choice keeps the plain `enum "N"` / `enum "N" : <kind>` form. Added in `.dsshir` v6 and `.dssir` v3, with `fixed`.

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
| struct / union (standalone text) | `struct "N" { T, ... }`, `union "N" { T, ... }`, `struct "N" opaque`, `struct "N" rec 1 { ptr<rec 1> }` |
| recursive back-reference (standalone text) | `rec <H>` (in type position — names an enclosing `rec <H>` composite) |
| composite reference (`.dsshir` module only) | `type <H>` (names entry `H` of the module's `types` section; refused by name in a standalone text) |
| enum | `enum "N"`, `enum "N" : u8`, `enum "N" fixed i64 "long"`, `enum "N" chosen u32 "unsigned int"` |
| function | `fn(T, ...) -> R`, `fn(...) -> R cc <name>` |
| extension | `ext "name" (T, ...)`, `ext "name" (...) [n, ...]` |
