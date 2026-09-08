# The `.dsshir` HIR Text Format

> **For a consumer outside this repository.** You run `dsscp`, you read a file. You do not link
> against `libdsscp`, you do not construct a `SemanticModel`, and you do not need a C++ toolchain
> that matches ours. This document is the contract for that file.
>
> Companion: [`ir-type-text.md`](./ir-type-text.md) is the authoritative grammar for the **type**
> syntax used below, and it applies verbatim here. Authoritative implementation:
> [`src/hir/hir_text.hpp`](../src/hir/hir_text.hpp) (contract) and `src/hir/hir_text.cpp`
> (emitter + parser, one file, one grammar).

---

## 1. What it is, in one paragraph

`.dsshir` is the textual form of **HIR** — DSS's high-level intermediate representation. HIR is
**language-neutral and typed**: control flow is still *structured* (`if` / `while` / `do` / `for`
are first-class nodes with separable parts, not a CFG), every expression carries a resolved type,
and conversions are explicit `cast` nodes. It sits above MIR (SSA over a CFG) and below the
language's own syntax tree.

A `.dsshir` file is **one module** — one translation unit, compiled for **one target**.

---

## 2. Getting one

```
dsscp --emit-hir <path> [--language <name>] --target <spec> <file>...
dsscp --emit-hir -      ...                                          # `-` writes to stdout
```

`--emit-hir` is a **mode**, mutually exclusive with `--compile` / `--transpile` / `--directory` /
`--project` / `--lsp`. It runs the front end — preprocess, parse, semantic analysis, HIR lowering —
and **stops**. No MIR, no optimizer, no code generation, no link, no object file.

### 2.1 The exit-code contract

This is the part to depend on:

| exit code | meaning |
|---|---|
| `0` | DSS **accepted** the source. The artifact is written, complete, and parses back. |
| non-zero | DSS **rejected** the source (or the write failed). Diagnostics are on **stderr**. **No artifact is written.** |

There is no third state. In particular there is no "we emitted HIR but something later failed",
because there *is* no later: that is the reason `--emit-hir` is a mode rather than a modifier on
`--compile`. As a modifier, a link failure would have produced a non-zero exit standing beside a
perfectly good artifact, and you could not tell that apart from a rejection.

**A translation unit that would not link is a normal input.** A single function in isolation, a call
to an undefined symbol, no `main` at all — all emit at exit 0. Nothing downstream of HIR runs, so
nothing downstream of HIR can object.

Diagnostics on stderr are **prose**, and they are ours. Carry them verbatim and attribute them to
DSS; do not parse them. (Structured JSON diagnostics do not exist yet — see §9.)

### 2.2 It takes a target, and exactly one

HIR is **target-dependent**, so `--target` is required and a second `--target` is refused
(`AmbiguousEmitHirTarget`). This is not pedantry. Measured, same source, two data models:

```c
long identity(long v) { return v; }
int  narrow(long v) { int n = v; return n; }
```

| | `x86_64:elf64-x86_64-linux` (LP64) | `x86_64:pe64-x86_64-windows-exec` (LLP64) |
|---|---|---|
| `identity` | `fn(i64 "long") -> i64 "long"` | `fn(i32 "long") -> i32 "long"` |
| `int n = v;` | `var %5 : i32 = cast [syn] : i32 (ref %4 : i64 "long")` | `var %5 : i32 = ref %4 : i32` |

Under LLP64 the `Cast` **node is absent** — `long` and `int` are the same representation, so the
conversion is an identity retag with nothing to represent. The *node set of the tree* changes with
the target, not merely the spelling of its types. Run `--emit-hir` once per target, to a different
path each time.

`--language` is optional exactly as it is for `--compile`, and "optional" means something narrower
than it sounds: omitted, each target supplies **the source language it declares as its own assembly
dialect**. That is the right answer for a `.s`, and the wrong one for everything else — a `.c` with
no `--language` is refused by name:

```
error[D_UnknownFileExtension]: no source language for 'foo.c': no --language was given, so target
'x86_64' selected its declared defaultAssemblyLanguage 'asm-x86_64-att' — which claims .s, .S and
not '.c'. Pass --language <name> to name the source language explicitly.
```

**Pass `--language c`.** Value flags also accept the `=` form (`--emit-hir=out.dsshir`).

### 2.3 What else affects the artifact, and what does not

`--define`, `-I` / `--include-dir` and `--target` all reach the front end and therefore change the
HIR. `--config=debug|release` does **not**: it selects an optimizer pipeline, and no optimizer runs
before HIR — measured, the two configs produce a byte-identical artifact for the same input.
`--stack-reserve`, `--force-git-cache` and `--schema-dir` are refused in this mode, because they act
on things it does not produce (an image, a manifest, an LSP server); a flag that would be silently
discarded is refused rather than accepted.

---

## 3. A complete example

```c
struct Point { int x; int y; };

int sum_fields(struct Point *p) {
  return p->x + p->y;
}
```

```
dsshir 3
producer "0.0.2+gcd1331ebc3ef.dirty922bae8971de8a1b"
buffers {
  buf 1 "/work/doc_example.c"
  buf 2 "<built-in>"
  buf 3 "/work/doc_example.c" synthesized from 1
}
symbols {
  %1 "Point"
  %2 "sum_fields"
  %3 "p"
}
module "C" {
  @loc(buf 3, 443..473)
  type_decl %1 : struct "Point" {i32, i32}
  @loc(buf 3, 476..533)
  function %2 : fn(ptr<struct "Point" {i32, i32}>) -> i32 {
    @loc(buf 3, 504..506)
    param %3 : ptr<struct "Point" {i32, i32}>
    @loc(buf 3, 508..533)
    block {
      @loc(buf 3, 512..531)
      return @loc(buf 3, 519..530) binop Add : i32 (...)
    }
  }
}
```

Note what is *not* there: no type ids, no symbol ids, no pointers into a compiler's memory. Every
identity in the file is defined by the file.

---

## 4. The grammar

Whitespace-insignificant. **LF only** — the writer never emits `\r`, and a `.dsshir` carries no
legitimate carriage return. UTF-8. Strings are double-quoted with `\"` and `\\` escapes.

```
file       := "dsshir" INT "producer" STR preamble module
preamble   := ext_kinds? ext_ops? intrinsics? buffers? symbols?   (non-empty sections only)
module     := "module" flags? STR "{" decl* "}"
decl       := function | global | type_decl | extern_function
            | extern_global | import_group | ext_node | error
stmt       := block | if | while | do | for | switch
            | break | continue | return | expr | var | assign
            | unreachable | ext_node | error
expr       := lit | ref | call | intrinsic | binop | unop | cast | member
            | index | swizzle | construct | ternary | logical_and
            | logical_or | sizeof | alignof | addressof | deref | typeref
            | ext_node | error
type       := (see docs/ir-type-text.md — one grammar, one decoder)
```

Sections are emitted **only when non-empty**, in the order shown. The two header lines are
**always** emitted, including for an empty module.

### 4.1 `dsshir <version>` — the format version

The first line. **Currently `3`.** A reader that does not understand the version must **refuse**,
not guess. Our own parser does exactly that: a version it does not know is a hard
`H_TextVersionMismatch` error and no module is produced.

### 4.2 `producer <string>` — the compiler revision

The second line, and **mandatory**: a file without it is malformed, and our parser refuses it. The
value is the compiler's build stamp:

```
0.0.2                                        version only (clean tree, no git)
0.0.2+gcd1331ebc3ef                          …plus the commit it was built from
0.0.2+gcd1331ebc3ef.dirty922bae8971de8a1b    …plus a digest of uncommitted changes
0.0.2+nogit20260907T091455Z.7                no git or no work tree
```

It contains no whitespace, so the whole stamp is one token.

`producer ""` is **legal and meaningful**: it says *"this artifact is not attributed to a compiler
revision."* It is what hand-written fixtures and unit-test modules carry. **Every artifact
`--emit-hir` writes carries a real stamp** — if you receive `producer ""` from something claiming to
be `dsscp` output, do not trust its provenance.

The distinction matters: an **absent** field is a malformed file (refused), an **empty** field is a
positive statement of non-attribution (accepted). We did not want a reader to silently turn "I
cannot tell who wrote this" into "nobody claims to have written this."

### 4.3 `buffers { … }` — source files, by artifact-local handle

```
buffers {
  buf 1 "/work/doc_example.c"
  buf 2 "<built-in>"
  buf 3 "/work/doc_example.c" synthesized from 1
}
```

Each row binds a **handle** to a source name. The handles are **artifact-local ordinals** starting
at 1 — deliberately *not* the compiler's internal buffer ids, which are a process-global counter and
would make the same input produce different bytes depending on how many other files the process had
opened first (see §8).

⚠ **`synthesized from <handle>` is the load-bearing part.** It marks a buffer that is the C
preprocessor's **synthesized text**, not a file on disk. Its byte offsets index the *preprocessed*
program — after the predefine prologue, after every `-D`, with every `#include` spliced in — and
they will not index the original file. The synthesized buffer is *constructed with the main source's
name*, so without this marker `buf 3 "/work/doc_example.c"` would read as a real file and you would
index the wrong text. `from <handle>` names the main origin buffer the synthesis started from.

A language with no preprocess pass produces no synthesized buffer; there, every row is plain and the
offsets index the named file directly.

### 4.4 `symbols { … }` — source-level names

```
symbols {
  %1 "Point"
  %2 "sum_fields"
  %3 "p"
}
```

Every `%N` in the module body is bound here to the name it had in the source. State properties about
`p`, not about `%3`. The handle number is positional and artifact-local; it is stable within one
file and means nothing across files.

An empty name (`%5 ""`) is a compiler-generated symbol with no source spelling (a synthetic temp, an
unnamed parameter).

### 4.5 `ext_kinds` / `ext_ops` / `intrinsics` — the open half

DSS's node set is **open**: a language or domain can register HIR kinds beyond the 53 core ones (see
§6). When a module uses them, they are declared by name in these sections and referenced as
`ext_node` in the body. A reader that has covered every core kind can still meet one of these, and
should refuse it **by name** rather than skip it.

### 4.6 `@loc(buf H, START..END)` — source spans

Attached inline before a node. `H` is a `buffers` handle; `START`/`END` are byte offsets, end
exclusive, **in the coordinate system of that buffer** — read §4.3 before using them.

---

## 5. Self-containment: what a reader can reconstruct from the bytes alone

This is the property the format is built around, so here is exactly how far it goes.

**Guaranteed to travel:**

- **Every type the module references, structurally.** A `struct` spells its field types inline
  (`struct "Point" {i32, i32}`), at *every* use site including through a pointer
  (`ptr<struct "Point" {i32, i32}>`) — never as an opaque name or an id. Unions and array extents
  likewise (`union "Bits" {i32, f32}`, `arr<i32, 4>` — a count, not a pointer). **Three ABI-relevant
  layout attributes travel too** — packing (whole-composite and per-member), explicit field offsets,
  and per-member alignment — because a type whose layout you cannot reconstruct is not one you can
  state a property about. ⚠ Three others do **not**; see §5.2 before computing any size or offset:

  ```c
  struct __attribute__((packed)) P { char a; int b; };
  struct A { char a; int b __attribute__((aligned(8))); };
  ```
  ```
  type_decl %1 : struct "P" packed {char, i32}
  type_decl %2 : struct "A" {char ~0, i32 ~8}
  ```
  (`packed` is the whole-composite marker, `~N` a per-member alignment, `@N` an explicit offset.)
- **Every source-level name**, in `symbols`.
- **Every literal value**, inline (`lit int 42 : i32`, `lit str "hi" : arr<char,3>`).
- **The format version and the producer revision.**
- **Source spans**, with their buffer named and classified.

**Documented boundaries — these do *not* travel.** Most are deliberate; the layout-attribute row is
a **gap**, and it is marked as one rather than dressed up as a decision:

| | |
|---|---|
| **enumerator names and values** | `enum "Color"` carries the enum's name, and its underlying width when that diverges from the default (`enum "E" : u8`). It does **not** carry `Red = 0, Green = 1`: enumerators are folded to literals at every use, so the *values* are in the body while the *names* are not. |
| **`const` and `restrict`** | Not interned — they never affect layout or codegen, so they are not part of type identity and cannot ride a type. (`volatile` and `_Atomic` *are* carried.) |
| **bit-field widths, `__attribute__((aligned(N)))` on a composite, and a `#pragma pack(N)` cap** | ⚠ Three ABI-relevant layout attributes the type grammar has no spelling for. They are dropped **silently** on a round trip — see §5.2. |
| **aliasing information** | HIR has none. |
| **undefined behaviour** | Not represented: no poison, no `nsw`/`nuw`, no overflow flags. Constant folding wraps. Integer-overflow soundness is entirely the reader's problem. |

### 5.1 Cyclic composites: `rec <H>` (new in v3)

```c
struct Node { int v; struct Node *next; };   /* the ordinary linked list */
```

This type's graph is **cyclic** — the second field is `ptr<struct Node>` whose operand is the struct
itself. Because the format spells a type structurally, a walk with no back-reference form would
expand it forever, so **v2 refused it**: exit non-zero, no file. That made every list, tree,
intrusive container and parent pointer in real C unemittable.

**v3 spells it.** A composite whose own type graph reaches itself carries a `rec <H>` marker
directly after its name, and the point where the graph closes spells `rec <H>` in type position:

```
type_decl %1 : struct "Node" rec 1 {i32, ptr<rec 1>}
```

**`H` is a handle for the composite, not for the spelling.** It is artifact-local (1, 2, 3 … in the
order the writer first reaches each recursive composite) and every mention of the same composite in
the same file uses the same number — the same device `symbols`/`%N` and `buffers`/`buf N` already
use. That matters as soon as recursion is mutual, because one type then has **two** spellings in one
file:

```c
struct A { int x; struct B *b; };
struct B { int y; struct A *a; };
```
```
type_decl %1 : struct "A" rec 1 {i32, ptr<struct "B" rec 2 {i32, ptr<rec 1>}>}
type_decl %2 : struct "B" rec 2 {i32, ptr<struct "A" rec 1 {i32, ptr<rec 2>}>}
```

Standing inside `A`, `B` is `struct "B" rec 2 {i32, ptr<rec 1>}`; standing alone it is the expanded
form. Both are `B`, and the handle `2` is what says so. **If you rebuild types from this format, key
your own interning on the handle, not on the text of the composite** — two spellings of one type
would otherwise become two types, and nothing about the bytes would tell you.

Rules a reader can rely on:

- `rec <H>` in type position always names a composite that is **currently open** at that point —
  an enclosing one. Our parser refuses anything else rather than binding it to a closed composite.
- Handles are 1-based. `rec 0` is refused.
- `rec` and `opaque` never appear together: `opaque` means the composite is *incomplete* and has no
  field list, so nothing can close a cycle through it.
- A handle spelled twice must describe the same composite both times. Two different bodies under one
  handle is refused, by name.

**Still refused, loudly:** a shape this codec cannot spell arrives as a diagnostic naming the type,
and `--emit-hir` writes no file (§2.1) — never as a file with a `?` in it. The composite *shape* is
now fully representable; §5.2 lists three composite *layout* attributes that are still dropped, and
those are dropped **silently**, which is why they are called out rather than left to be discovered.

For scale: over this repository's own C example corpus — **840 files**, one `--emit-hir` run each —
**791 emit at exit 0** (v2: 790, with the one refusal being exactly this), and 49 are front-end
rejections of sources DSS does not accept at all (the corpus's deliberate negative cases). But those
are small examples; real client code is where lists and trees live, so the change is much larger
than 1-in-840 suggests.

### 5.2 Three composite layout attributes that do *not* travel

A composite spells its packing, its explicit field offsets (`@N`) and its per-member alignment
(`~N`), and those are enough to reconstruct most layouts. Three ABI-relevant attributes are **not**
in the grammar and are dropped when the file is read back:

| attribute | how you write it in C | what a re-read gives you |
|---|---|---|
| **bit-field widths** | `struct S { unsigned a : 3, b : 5; };` | two full-width members — a different size |
| **whole-composite alignment** | `struct S { … } __attribute__((aligned(16)));` | the natural alignment |
| **member-alignment cap** | `#pragma pack(4)` around the definition | no cap |

⚠ **This is a silent loss, not a refusal**, and it is stated here for that reason: the emitted file
is well-formed and re-parses cleanly, so nothing in the bytes tells you a layout attribute was
dropped. If you compute sizes or offsets from a `.dsshir`, treat a `struct` carrying any of these
three as unreliable until the format carries them.

**What this is *not*:** the *values* laid out by the compiler are unaffected — this is a limitation
of the text tier only, and every one of these attributes is honoured in a real compile. Nothing here
changes what DSS emits as code.

---

## 6. The node-kind inventory

```
dsscp --dump-hir-kinds
```

Needs neither `--language` nor `--target`: the core node set is a property of the compiler, not of a
program. Output:

```
dsshir-kinds 1
dsshir-format-version 2
producer "0.0.2+gcd1331ebc3ef.dirty922bae8971de8a1b"
core-kind-count 53
kind "Module" position non-expr typed no symbol no
kind "Function" position non-expr typed yes symbol yes
kind "Literal" position expr keyword "lit" typed yes symbol no
kind "Ref" position expr keyword "ref" typed yes symbol yes
...
stmt-keyword "var"
stmt-keyword "param"
...
extension-kind-base 256
```

Per kind:

- **`position expr keyword "<kw>"`** — the writer spells this kind in expression position, with that
  `.dsshir` keyword. **`position non-expr`** — it does not. (Not "statement": `Module` is neither,
  and `Error`/`Extension` render inline in expressions while owning *statement* keywords.)
- **`typed yes|no`** — whether the node must carry a resolved result type.
- **`symbol yes|no`** — whether it carries a symbol id (a `%N`).

`stmt-keyword` rows publish the statement vocabulary as a **set** rather than a kind→keyword map,
because on that side it is not a function: one `VarDecl` spells `var` or `param` by position, one
`CaseArm` spells `case` or `default` by content.

`extension-kind-base` is where the open half begins (§4.5).

**Arity is not reported**, and will not be: it is not a static property of a kind here. A `Call` has
one callee plus N arguments, a `Block` has N statements, a `ConstructAggregate` has one slot per
field. Child counts are spelled by the text itself.

Every field above is projected from the *same* tables the emitter and parser dispatch on, so the
inventory cannot describe a compiler other than the one that would write your file. Compare it
against your coverage at **build time** and refuse an uncovered construct by name.

---

## 7. Versioning policy

**Yes, `.dsshir` is versioned explicitly, and yes, a format change is breaking during the C++
window.** Concretely:

1. **The version is a single integer on the first line.** There is no minor version and no feature
   negotiation. Pin the number you support and refuse anything else.
2. **We bump it for any change a reader of the previous version would misread — including
   *additions*.** Adding a required construct is a breaking change: v2 added the mandatory `producer`
   line and the optional `buffers` section, and a v1 reader meeting a v2 file would take `producer`
   for the start of the module. It got a bump for exactly that reason. **v3** added the `rec <H>`
   composite marker and the `rec <H>` type head (§5.1): a v2 reader meeting
   `struct "Node" rec 1 {i32, ptr<rec 1>}` would meet a token it has no rule for — which is the
   correct outcome, and precisely why the number has to move. The alternative is a reader that skips
   what it does not recognise and rebuilds a `Node` with no `next`.
3. **We do not maintain a compatibility window.** This build understands **one** version and refuses
   every other, in both directions — a v1 file is refused by a v2 parser just as a v3 file is. One
   grammar, no conditional parsing, no "mostly works".
4. **Expect it to move while the language surface grows.** HIR grows as DSS grows, and C++ is the
   arc that will grow it most. Until C++ compilation lands, treat a version bump as a normal event
   and plan to re-emit rather than to migrate files.
5. **After C++ lands the format is expected to stabilise** and bumps to become rare.

**What is stable today** (as stable as anything in a `3`): the mode and its exit-code contract, the
two header lines and their order, the section order, the type syntax (shared with FFI descriptors,
see [`ir-type-text.md`](./ir-type-text.md)), and the self-containment rules in §5.

**What will grow:** the node set (new `HirKind`s as language features land — use `--dump-hir-kinds`),
the type forms, and the attribute vocabulary.

**Our recommendation:** pin the version, refuse on mismatch, and re-emit from source rather than
migrating stored artifacts. Store the `producer` string with any verdict you publish so it stays
re-derivable.

---

## 8. Determinism

**Same input + same producer revision ⇒ byte-identical output.** Nothing in the emitter reads a
clock, a path, an address, or an unordered container's iteration order. Symbol handles are assigned
in pre-order of first encounter; buffer handles are assigned by sorted internal id; the one
caller-supplied collection whose order is not the caller's business is sorted by the emitter rather
than trusted.

⚠ This was measured, not assumed — and the first measurement **failed**. The artifact originally
printed the compiler's internal `BufferId`, which is a process-global counter: one unchanged source
emitted twice in one process produced `buf 321` and then `buf 344`. Artifact-local handles (§4.3)
are the fix.

Changing the compiler changes the `producer` string, and may change the bytes. That is the point of
carrying the revision.

---

## 9. Reading a file back

Our own parser is `dss::parseHir` ([`src/hir/hir_text.hpp`](../src/hir/hir_text.hpp)). It

- re-interns every type into a fresh interner,
- re-registers extension kinds / ops / intrinsics from the preamble,
- rebuilds the symbol names, buffer table, literal pool and per-node side-tables,
- and runs **`HirVerifier` on load**.

`emitHir(parseHir(emitHir(h)))` is byte-identical, and the whole corpus of checked-in fixtures is
pinned on that property.

You do not have to use it — the grammar above is the contract, not our implementation — but if you
do link against us, `parseHir` is the supported entry point and `result->ok` is the verdict.

**Not available yet:** structured (JSON) diagnostics. Diagnostics are prose on stderr. If you need
them machine-readable, ask — nothing about the current design prevents it.

---

## 10. Where this lives in the repository

| | |
|---|---|
| contract + the `HirTextContext`/`HirParseResult` surfaces | [`src/hir/hir_text.hpp`](../src/hir/hir_text.hpp) |
| emitter, parser, and the one type-text grammar | `src/hir/hir_text.cpp` |
| the CLI mode and its exit-code contract | `src/program/cli_args.{hpp,cpp}`, `src/program/program.cpp` (`Program::emitHirText`) |
| the front-end stage stop | `src/program/compile_pipeline.{hpp,cpp}` (`buildCuHir`) |
| checked-in fixtures | `tests/hir/corpus/*.dsshir`, `tests/hir/lowering_goldens/*.dsshir` |
| the consumer contract, as tests | `tests/program/test_emit_hir_mode.cpp` |
