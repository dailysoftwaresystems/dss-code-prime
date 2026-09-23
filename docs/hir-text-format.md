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
| `1` | DSS **rejected** the source (or the write failed). Diagnostics are on **stderr**. **No artifact is written.** |
| `2` | DSS accepted the source, but the artifact it produced **did not survive a round trip through our own reader**. A compiler defect — please report it. **No artifact is written.** |

There is no third state. In particular there is no "we emitted HIR but something later failed",
because there *is* no later: that is the reason `--emit-hir` is a mode rather than a modifier on
`--compile`. As a modifier, a link failure would have produced a non-zero exit standing beside a
perfectly good artifact, and you could not tell that apart from a rejection.

`2` is not a third state either — it is a **refinement of the non-zero row**, and the binary
contract is unchanged: **`0` ⇔ a readable artifact exists; non-zero ⇔ nothing was written.** Branch
on `rc != 0` and you are correct. The split exists because the two ask *you* for different things:
`1` means fix the program, `2` means the program was fine and we could not serialize it — nothing
you can change in your source will help, so do not retry, report it.

### 2.1.1 What `parses back` costs, and why it is not optional

Before writing anything, `--emit-hir` **parses its own artifact and re-emits it, and refuses to
write unless the re-emission is byte-identical.** The third clause of the `0` row is therefore a
measurement on every invocation rather than a promise.

**It is a re-emit and a byte compare, never merely "it parses"**, and that distinction is load-
bearing rather than belt-and-braces. A spelling the reader mis-reads can parse perfectly cleanly: a
`lit float` past 2^64 was read back as `0.0` with **no diagnostic at all**, which every
"did it parse?" check in this repository passed. Byte identity is the only predicate that can see a
value the reader reconstructed *differently*, because it compares what the reader rebuilt against
what the writer meant, one token at a time.

This is not hypothetical. Three shipped writer spellings could not be read back by the shipped
reader — one **aborted the reading process**, one was refused for a token the lexer did not have,
and one silently returned the wrong value — and all three exited `0`. They were found by a person
round-tripping the corpus by hand. Nothing was checking, because the format had a writer, a reader,
and **no consumer of the reader**.

You pay for it in wall clock, and the amount is small: the front end (preprocess, parse, semantic
analysis, HIR lowering) dominates the mode, and reading a `.dsshir` back is text work over what it
just produced. There is **no flag to turn it off**, deliberately — a check a consumer opts into is a
check that is off in the run that matters, and the failure mode it prevents is not a wrong exit
code, it is a file we told you was good that kills your process when you read it.

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

The two lines that differ, verbatim from the two artifacts:

```
x86_64:elf64-x86_64-linux (LP64)
  function %1 : fn(i64 "long") -> i64 "long" {
      var %5 : i32 = @loc(buf 3, 674..675) cast [syn] : i32 (@loc(buf 3, 674..675) ref %4 : i64 "long")

x86_64:pe64-x86_64-windows-exec (LLP64)
  function %1 : fn(i32 "long") -> i32 "long" {
      var %5 : i32 = @loc(buf 3, 904..905) ref %4 : i32
```

(The spans differ too, and that is also the target: each target's predefine prologue is a different
length, and a span indexes the preprocessed text — §4.3.)

Under LLP64 the `Cast` **node is absent** — `long` and `int` are the same representation, so the
conversion is an identity retag with nothing to represent. The *node set of the tree* changes with
the target, not merely the spelling of its types. Run `--emit-hir` once per target, to a different
path each time.

`--language` is optional exactly as it is for `--compile`, and "optional" means something narrower
than it sounds: omitted, each target supplies **the source language it declares as its own assembly
dialect**. That is the right answer for a `.s`, and the wrong one for everything else — a `.c` with
no `--language` is refused by name:

```
error[D_UnknownFileExtension]: no source language for '/tmp/ht-doc/doc_example.c': no --language was given, so target 'x86_64' selected its declared defaultAssemblyLanguage 'asm-x86_64-att' — which claims .s, .S and not '.c'. Pass --language <name> to name the source language explicitly.
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
dsshir 5
producer "0.5.0+nogit20260919T193847Z.5"
buffers {
  buf 1 "/tmp/ht-doc/doc_example.c"
  buf 2 "<built-in>"
  buf 3 "/tmp/ht-doc/doc_example.c" synthesized from 1
}
types {
  type 1 = struct "Point" {i32, i32}
}
symbols {
  %1 "Point"
  %2 "sum_fields"
  %3 "p"
}
module "C" {
  @loc(buf 3, 608..638)
  type_decl %1 : type 1
  @loc(buf 3, 641..698)
  function %2 : fn(ptr<type 1>) -> i32 {
    @loc(buf 3, 669..671)
    param %3 : ptr<type 1>
    @loc(buf 3, 673..698)
    block {
      @loc(buf 3, 677..696)
      return @loc(buf 3, 684..695) binop Add : i32 (@loc(buf 3, 684..688) member #0 : i32 (@loc(buf 3, 684..688) deref [syn] : type 1 (@loc(buf 3, 684..685) ref %3 : ptr<type 1>)), @loc(buf 3, 691..695) member #1 : i32 (@loc(buf 3, 691..695) deref [syn] : type 1 (@loc(buf 3, 691..692) ref %3 : ptr<type 1>)))
    }
  }
}
```

Note what is *not* there: no type ids, no symbol ids, no pointers into a compiler's memory. Every
identity in the file is defined by the file — `%3` by the `symbols` section, `buf 3` by `buffers`,
and `type 1`, the struct, by `types`, where its fields are spelled exactly once however many times
the module mentions it.

---

## 4. The grammar

Whitespace-insignificant. **LF only** — the writer never emits `\r`, and a `.dsshir` carries no
legitimate carriage return. UTF-8. Strings are double-quoted with `\"` and `\\` escapes.

```
file       := "dsshir" INT "producer" STR preamble module
preamble   := ext_kinds? ext_ops? intrinsics? buffers? types? symbols?   (non-empty sections only)
types      := "types" "{" ("type" INT "=" composite)* "}"
composite  := ("struct" | "union") STR
              ( "opaque"
              | "packed"? ("aligned" INT)? ("pack" INT)? "{" field ("," field)* "}" )
field      := type ("@" INT | "~" INT)? ("bits" INT)? "packed"?
module     := "module" flags? STR "{" decl* "}"
decl       := function | global | type_decl | extern_function
            | extern_global | import_group | ext_node | error
stmt       := block | if | while | do | for | switch
            | break | continue | return | expr | var | assign
            | unreachable | ext_node | error
return     := "return" flags? ("void" | expr)
expr       := lit | ref | call | intrinsic | binop | unop | cast | member
            | index | swizzle | construct | ternary | logical_and
            | logical_or | sizeof | alignof | addressof | deref | typeref
            | rmw | ext_node | error
rmw        := "rmw" flags? SYM ":" type "(" expr "," expr ")"
              (an indivisible read-modify-write: the object's lvalue, then the
               update, which reads the observed value as `ref SYM`; yields the
               value the object held before the replacement that took effect)
type       := (see docs/ir-type-text.md — one grammar, one decoder), where a
              COMPOSITE is always `"type" INT`, naming an entry of `types` (§4.7)
```

⚠ **A value-less return is `return void`, and `return` followed by anything that does not begin an
expression is refused.** The value is the one optional child in this grammar that sits at the END of
its statement and can open with `@`, and so can the next statement's `@loc` block — so the absence
has to be spelled. v4 wrote a bare `return` and its reader took a following `@` for the start of the
value: on `if (x) return; g();` it read `g();` as the return's operand, and `--emit-hir` refused its
own artifact (✔measured on sqlite's `test/speedtest1.c`, P68). No look-ahead can settle it instead,
because `error` and `ext_node` open a statement *and* render inline as an expression. The same
reader refuses a STATEMENT in any expression slot (an operand, a condition, a value, an initializer)
by name, rather than building one there.

```
      @loc(buf 3, 646..663)
      if (@loc(buf 3, 650..651) binop Ne : bool (@loc(buf 3, 650..651) ref %3 : i32, lit [syn] int 0 : i32))
        @loc(buf 3, 653..660)
        return void
      @loc(buf 3, 663..666)
      expr @loc(buf 3, 663..666) call : void (@loc(buf 3, 663..664) ref %1 : fn() -> void)
```

Sections are emitted **only when non-empty**, in the order shown. The two header lines are
**always** emitted, including for an empty module.

### 4.1 `dsshir <version>` — the format version

The first line. **Currently `5`.** A reader that does not understand the version must **refuse**,
not guess. Our own parser does exactly that: a version it does not know is a hard
`H_TextVersionMismatch` error and no module is produced.

### 4.2 `producer <string>` — the compiler revision

The second line, and **mandatory**: a file without it is malformed, and our parser refuses it. The
value is the compiler's build stamp:

```
0.5.0                                        version only (clean tree, no git)
0.5.0+g90e0014fd50d                          …plus the commit it was built from
0.5.0+g90e0014fd50d.dirtyd20789119c8c91de    …plus a digest of uncommitted changes
0.5.0+nogit20260919T193847Z.5                no git or no work tree
```

(The third and fourth are stamps two real builds printed — a work tree with uncommitted changes, and
a copy of the sources with no `.git`; the first two are the same stamp's forms on a clean tree.)

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
  buf 1 "/tmp/ht-doc/doc_example.c"
  buf 2 "<built-in>"
  buf 3 "/tmp/ht-doc/doc_example.c" synthesized from 1
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
name*, so without this marker `buf 3 "/tmp/ht-doc/doc_example.c"` would read as a real file and you
would index the wrong text. `from <handle>` names the main origin buffer the synthesis started from.

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

DSS's node set is **open**: a language or domain can register HIR kinds beyond the 54 core ones (see
§6). When a module uses them, they are declared by name in these sections and referenced as
`ext_node` in the body. A reader that has covered every core kind can still meet one of these, and
should refuse it **by name** rather than skip it.

### 4.6 `@loc(buf H, START..END)` — source spans

Attached inline before a node. `H` is a `buffers` handle; `START`/`END` are byte offsets, end
exclusive, **in the coordinate system of that buffer** — read §4.3 before using them.

### 4.7 `types { … }` — every composite, defined once (new in v5)

```
types {
  type 1 = struct "Point" {i32, i32}
}
```

Every `struct` and `union` the module mentions — in a signature, a declaration, an expression's type,
or another composite's fields — is **defined once** here, under an artifact-local handle, and every
mention in the body **names it**: `type_decl %1 : type 1`, `param %3 : ptr<type 1>`. A composite is
never spelled inline in a module; `struct "N" { … }` appears only as the right-hand side of an entry.
This is the device every reference IR measured uses — clang's named `%struct.Point = type { i32, i32 }`
defined once at module scope, gcc's `@N` handles in its raw tree dumps, MSVC's CodeView type indices —
and the same one this file already uses for symbols (`%N`) and buffers (`buf N`).

- **`H` is 1-based and artifact-local**, minted at the composite's first mention in text order: the
  body first, then each definition in handle order (a definition's own fields can mention a composite
  nothing before it did, which takes the next handle). It is a function of the module alone, so the
  same input and revision give the same numbers (§8). Entries are written in handle order.
- **An entry may name a handle defined after it**, and that is the ordinary case, not an edge: it is
  how a cycle — `type 1 = struct "Node" {i32, ptr<type 1>}` — and mutual recursion are written (§5.1).
  A reader needs the heads (`type H = struct|union "name"`) of the whole section before it can build
  the fields; ours scans them first, exactly as an LLVM reader creates a named type before its body.
- **Key your own interning on the handle, never on the text of the body.** Two composites that share
  a name and a field list — the same `struct S { int v; }` declared in two scopes — are two types and
  arrive as two entries with identical bodies. Folding them by content would merge them.
- **An incomplete composite is an entry too** — for `struct Handle; struct Handle *open_handle(void);`
  the table holds `type 1 = struct "Handle" opaque` and the prototype is `fn() -> ptr<type 1>`. It
  has no field list, and it must not be read as `{}`, which is a legal *complete* zero-member
  composite.
- **The layout travels in the entry** (§5.2): `packed`, `aligned N` and `pack N` after the name,
  and after each field `@N` (an explicit offset) or `~N` (a member alignment), `bits N` (a bit-field
  width) and `packed` (a per-member packed).

Refused, each by name: a mention of a handle the section does not define; `type 0` or a handle past
32 bits; one handle defined twice; an entry that is not a `struct` or a `union`; an `aligned` / `pack`
value that is not a representable power of two (or is `0`, which the writer never spells); layout
markers that cannot be combined (explicit offsets with member alignments, with `packed`, or with
bit-field widths); and, anywhere in the module, an inline `struct "N" { … }` / `union …` or a v3
`rec <H>` — a module has exactly one spelling of each composite.

---

## 5. Self-containment: what a reader can reconstruct from the bytes alone

This is the property the format is built around, so here is exactly how far it goes.

**Guaranteed to travel:**

- **Every type the module references — each composite ONCE.** A `struct` or `union` is defined in the
  `types` section with its field types (`type 1 = struct "Point" {i32, i32}`) and every use names it
  (`ptr<type 1>`) — never as an opaque name, and never as an id into a compiler's memory: the handle is
  the file's own, bound by the file (§4.7). Every other type is spelled structurally where it is used
  (`arr<i32, 4>` — a count, not a pointer; `fn(ptr<type 1>) -> i32`). **Every layout attribute the
  compiler gives a composite travels too** — packing (whole-composite and per-member), explicit field
  offsets, per-member alignment, bit-field widths, a whole-composite `aligned(N)` and a `#pragma pack`
  cap (§5.2) — because a type whose layout you cannot reconstruct is not one you can state a property
  about:

  ```c
  struct __attribute__((packed)) P { char a; int b; };
  struct A { char a; int b __attribute__((aligned(8))); };
  ```
  ```
  types {
    type 1 = struct "P" packed {char, i32}
    type 2 = struct "A" {char ~0, i32 ~8}
  }
  …
    type_decl %1 : type 1
    type_decl %2 : type 2
  ```
  (`packed` is the whole-composite marker, `~N` a per-member alignment, `@N` an explicit offset.)
- **Every source-level name**, in `symbols`.
- **Every literal value**, inline (`lit int 42 : i32`, `lit str "hi" : arr<char, 3>`).
- **The format version and the producer revision.**
- **Source spans**, with their buffer named and classified.

**Documented boundaries — these do *not* travel, deliberately**, plus one channel that is carried in a
different place than you might look for it:

| | |
|---|---|
| **enumerator names and values** | `enum "Color"` carries the enum's name, and its underlying width when that diverges from the default (`enum "E" : u8`). It does **not** carry `Red = 0, Green = 1`: enumerators are folded to literals at every use, so the *values* are in the body while the *names* are not. |
| **`const` and `restrict`** | Not interned — they never affect layout or codegen, so they are not part of type identity and cannot ride a type. (`volatile` and `_Atomic` *are* carried.) |
| **`__attribute__((aligned(N)))` on a *typedef*** | Carried, as **`aligned<T, N>`** at the use — the `arr<T, N>` shape: the decorated type first, the byte count second. ⚠ Do not confuse it with the WHOLE-COMPOSITE `aligned N` of a `types` entry (§5.2): that one is part of the composite's definition, this one is a *decoration* on whatever type the alias names. Written by cycle P66 (lane `al`), when an over-aligned type alias became representable at all. It rides the same transparent skin as `volatile`/`_Atomic` (one record, distinct interned identity, `kind()`/`operands()`/`scalars()` see through it), so a type carrying both spells as `aligned<volatile<i32>, 8>` and the reader merges them back into **one** record — the round trip is an identity, not a nesting that grows per hop. It is a **decoration, not a derivation level**: it does not touch the declarator spine the way `ptr<…>` and `arr<…>` do. `sizeof` is unaffected (`aligned<i32, 8>` is still 4 bytes wide, aligned 8) — which is why both gcc and clang refuse an *array* of such an alias, and so does DSS. ⚠ **P66 (lane `ag`): `N` may be WEAKER than the decorated type's natural alignment** — `aligned<i32, 2>` is a real, round-trippable type whose layout alignment is 2, because gcc, clang, mingw-w64 gcc and aarch64-gcc all lower a typedef's alignment (measured; the whole-composite channel does *not*). So the byte count is the *answer*, not a floor, and a reader must not "correct" it upward. |
| **aliasing information** | HIR has none. |
| **undefined behaviour** | Not represented: no poison, no `nsw`/`nuw`, no overflow flags. Constant folding wraps. Integer-overflow soundness is entirely the reader's problem. |

### 5.1 Composites: defined once, referenced everywhere (v5); cycles are ordinary

```c
struct Node { int v; struct Node *next; };   /* the ordinary linked list */
```
```
types {
  type 1 = struct "Node" {i32, ptr<type 1>}
}
…
  type_decl %1 : type 1
```

This type's graph is **cyclic** — the second field points at the struct itself. v2 spelled types
structurally and had no way to close the loop, so it **refused** the file; v3 added a `rec <H>`
back-reference so the cycle could be written inside the one inline spelling. **v5 makes the cycle an
ordinary case**: a composite is written once, in the `types` table, and the pointer that closes the
loop is `ptr<type 1>` — a reference like any other. Nothing is ever expanded at a use, so nothing can
expand forever, and no marker says "this one is recursive".

Mutual recursion is just two entries that name each other:

```c
struct A { int x; struct B *b; };
struct B { int y; struct A *a; };
```
```
types {
  type 1 = struct "A" {i32, ptr<type 2>}
  type 2 = struct "B" {i32, ptr<type 1>}
}
…
  type_decl %1 : type 1
  type_decl %2 : type 2
```

**Why v5 moved away from spelling a composite at every use.** Structural-at-every-use makes an
artifact's size (type mentions) × (the reachable type graph): every typed node re-spells everything its
type reaches, and real C's graphs are large and densely connected. ✔MEASURED (P68, WSL x86_64, a
Release `dsscp`, retired instructions): on the first 1.18 MB of the sqlite amalgamation, `--emit-hir`
under v4 ran **276 G instructions and died `std::bad_alloc`**, where the full compile of the same file
is 3.24 G; under v5 it runs **3.35 G** and writes a 396 KB artifact that reads back byte for byte. The
whole 9.57 MB amalgamation, which v4 could not finish in 12 minutes, emits under v5 in **60.1 G**
(the full compile: 73.1 G), 35.8 MB, 493 composites in its table. The cost of the type text is now
proportional to the mentions plus the graph, never their product — our suite pins that as a COUNT of
type nodes spelled (`HirTextTypeSpellingCost.*` in `tests/hir/test_hir_text.cpp`).

**`rec <H>` is subsumed in a module, and survives in a standalone type text.** A `.dsshir` module
refuses `rec` (and any inline `struct "N" { … }`) by name — it has exactly one spelling per composite.
A STANDALONE type string — the FFI-descriptor signatures `parseTypeFromText` decodes — has no table to
reference, so it keeps the inline forms and the `rec <H>` back-reference unchanged
([`ir-type-text.md`](./ir-type-text.md) §2.6), and it refuses `type <H>`, by name, as module-only.

Rules a reader can rely on:

- Every composite the body mentions has **exactly one** entry, and every mention of it — in the body
  and in other entries — is its `type <H>`.
- Entries are numbered 1, 2, 3 … in the order the writer first mentions them, and written in that
  order. A reference to a handle defined LATER in the section is ordinary; resolve the whole section's
  heads before building its fields (§4.7).
- **Key your interning on the handle, not on the entry's text.** Two entries with the same name and
  the same body are two types (two scopes' `struct S { int v; }`) and must stay two.
- `opaque` is an entry with no field list (an incomplete composite), never `{}` — `{}` is a legal
  complete zero-member composite, and confusing the two is a silent size change.

**Refused, loudly:** a shape this codec cannot spell arrives as a diagnostic naming the type, and
`--emit-hir` writes no file (§2.1) — never as a file with a `?` in it.

For scale — ✔MEASURED (P68 round 8, WSL x86_64, a Release `dsscp`), over this repository's own C
example corpus: **881 files**, every `.c` under `examples/c`, one `--emit-hir` run each at the first
target its example declares: **856 emit at exit 0**, **25** are front-end rejections of sources DSS
does not accept (the corpus's deliberate negative cases), and **none** is refused by the round-trip
self-check. Against the v4 writer over the same 881 files no outcome moved; 238 artifacts got smaller
(the largest shrink: to 2.7% of its v4 size), 614 are the same size (no composite at all) and 4 grew
by at most 47 bytes (the table's own framing, for a struct mentioned once); the corpus's total went
from 15.3 MB to 12.2 MB. Those are small examples — real client code is where large type graphs live,
and there the difference is the sqlite one above, not a percentage.

### 5.2 Every layout attribute of a composite travels (v5)

A `types` entry spells every layout channel DSS gives a composite. Until v5, three of them — and a
fourth nobody had listed — had no spelling and were dropped on a round trip **silently** (the file
re-parsed cleanly and re-emitted the same bytes, because the re-emission spelled the stripped type
exactly as the first emission had):

```c
struct Flags { unsigned a : 3, b : 5; };
struct Wide { char c; int i; } __attribute__((aligned(16)));
#pragma pack(4)
struct Capped { char c; long long x; };
#pragma pack()
```
```
types {
  type 1 = struct "Flags" {u32 bits 3, u32 bits 5}
  type 2 = struct "Wide" aligned 16 {char, i32}
  type 3 = struct "Capped" pack 4 {char, i64 "long long"}
}
```

| attribute | spelling | where |
|---|---|---|
| **whole-composite packed** | `packed` | after the name |
| **whole-composite alignment** (`__attribute__((aligned(N)))` on the definition) | `aligned N` | after the name — v5 |
| **member-alignment cap** (`#pragma pack(N)`) | `pack N` | after the name — v5 |
| **explicit field offset** | `@N` | after a field (all fields or none) |
| **member alignment** (`_Alignas` on a member) | `~N` | after a field (all or none; `~0` = none) — on a UNION member too, v5 |
| **bit-field width** | `bits N` | after a field — v5 (`bits 0` is the unnamed packing break) |
| **per-member packed** | `packed` | after a field |

The fourth is the one no one had listed: a member alignment on a **union** member. The compiler
completes unions with it; v4's union spelling omitted `~N`. ✔MEASURED (P68 round 8): for
`union U { char c; _Alignas(8) int i; };` — which gcc 13.3 and clang 18.1.3 both lay out as sizeof 8,
alignof 8 — the v4 writer emitted `union "U" {char, i32}` at exit 0, so the alignment was simply not
in the file; v5 emits `union "U" {char ~0, i32 ~8}`. The completeness is now held at compile time:
the definition writer and reader are pinned to the signature of the one interner function that
completes a composite, so a new layout channel stops the build until the format spells it.

**What this is *not*:** none of this changes what DSS emits as code — every one of these attributes
was always honoured in a real compile; what changed is that a reader of the `.dsshir` now gets them.

---

## 6. The node-kind inventory

```
dsscp --dump-hir-kinds
```

Needs neither `--language` nor `--target`: the core node set is a property of the compiler, not of a
program. Output:

```
dsshir-kinds 1
dsshir-format-version 5
producer "0.5.0+nogit20260919T193847Z.5"
core-kind-count 54
kind "Module" position non-expr typed no symbol no
kind "Function" position non-expr typed yes symbol yes
...
kind "Literal" position expr keyword "lit" typed yes symbol no
kind "Ref" position expr keyword "ref" typed yes symbol yes
...
kind "ReadModifyWrite" position expr keyword "rmw" typed yes symbol yes
stmt-keyword-count 28
stmt-keyword "block"
...
stmt-keyword "var"
...
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
   what it does not recognise and rebuilds a `Node` with no `next`. **v4** added the `rmw` expression
   (an indivisible read-modify-write of an `_Atomic` object, §4): a v3 reader meeting it has no rule
   for the keyword — and one that skipped it would rebuild `x += v` as the separate load and store
   whose lost updates the node exists to prevent. **v5** moved every composite into the `types`
   section, referenced as `type <H>` (§4.7, §5.1); spelled every layout channel of a composite (`aligned
   N`, `pack N`, `bits N`, and `~N` on a union member — §5.2); and spelled a value-less return
   `return void` (§4). A v4 reader meets `types`, `type`, `aligned`, `pack`, `bits` and `void` with no
   rule for any of them — and the last one matters most: a v4 reader that skipped `void` would be back
   to reading the next statement as the return's value.
3. **We do not maintain a compatibility window.** This build understands **one** version and refuses
   every other, in both directions — a v1 file is refused by a v2 parser just as a v3 file is. One
   grammar, no conditional parsing, no "mostly works".
4. **Expect it to move while the language surface grows.** HIR grows as DSS grows, and C++ is the
   arc that will grow it most. Until C++ compilation lands, treat a version bump as a normal event
   and plan to re-emit rather than to migrate files.
5. **After C++ lands the format is expected to stabilise** and bumps to become rare.

**What is stable today** (as stable as anything in a `5`): the mode and its exit-code contract, the
two header lines and their order, the section order, the type syntax (shared with FFI descriptors,
see [`ir-type-text.md`](./ir-type-text.md) — a module differs only in naming every composite through
its `types` table), and the self-containment rules in §5.

**What will grow:** the node set (new `HirKind`s as language features land — use `--dump-hir-kinds`),
the type forms, and the attribute vocabulary.

**Our recommendation:** pin the version, refuse on mismatch, and re-emit from source rather than
migrating stored artifacts. Store the `producer` string with any verdict you publish so it stays
re-derivable.

---

## 8. Determinism

**Same input + same producer revision ⇒ byte-identical output.** Nothing in the emitter reads a
clock, a path, an address, or an unordered container's iteration order. Symbol handles are assigned
in pre-order of first encounter; composite (`type`) handles in order of first mention in the text;
buffer handles by sorted internal id; the one caller-supplied collection whose order is not the
caller's business is sorted by the emitter rather than trusted.

⚠ This was measured, not assumed — and the first measurement **failed**. The artifact originally
printed the compiler's internal `BufferId`, which is a process-global counter: one unchanged source
emitted twice in one process produced `buf 321` and then `buf 344`. Artifact-local handles (§4.3)
are the fix.

Changing the compiler changes the `producer` string, and may change the bytes. That is the point of
carrying the revision.

---

## 9. Reading a file back

Our own parser is `dss::parseHir` ([`src/hir/hir_text.hpp`](../src/hir/hir_text.hpp)). It

- re-interns every type into a fresh interner — each `types` entry as ONE composite keyed on its
  handle, so two entries with identical bodies stay two types,
- re-registers extension kinds / ops / intrinsics from the preamble,
- rebuilds the symbol names, buffer table, literal pool and per-node side-tables,
- and runs **`HirVerifier` on load** — on a module it ACCEPTED. A file the reader refuses is refused
  by the reader's own diagnostics alone: the tree its error recovery built is never verified, so no
  verifier line follows the one that names the cause. (Verifying such a tree used to kill the
  process outright on a malformed signature; measured and fixed in P68.) Among what the verifier
  refuses: a function whose declared type is not a signature, or whose signature leaves its result
  or a parameter unresolved (`fn() -> invalid`) — one diagnostic per declaration, naming every hole.
  `ok` is the verifier's own verdict, not a count of what the reporter stored: a module it refuses
  fails every read, even a second read into a reporter that drops the repeat as a duplicate, and no
  `--suppress` list can silence a refusal it makes (every error code it emits is protected).

`emitHir(parseHir(emitHir(h)))` is byte-identical, and the checked-in fixtures are pinned on that
property — as is **every program in `examples/c`**, each emitted at its own declared target and
required to parse back and re-emit byte for byte on every run of our test suite. That corpus walk
exists because the property was previously asserted by nobody: the format had a writer, a reader,
and no consumer of the reader, which is how three unreadable writer spellings shipped at once
(§2.1.1). `--emit-hir` now runs the same check on its own output before it writes.

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
| the verifier `parseHir` runs on an accepted module (declared signatures included) | `src/hir/hir_verifier.{hpp,cpp}` |
| the CLI mode and its exit-code contract | `src/program/cli_args.{hpp,cpp}`, `src/program/program.cpp` (`Program::emitHirText`) |
| the front-end stage stop | `src/program/compile_pipeline.{hpp,cpp}` (`buildCuHir`) |
| checked-in fixtures | `tests/hir/corpus/*.dsshir`, `tests/hir/lowering_goldens/*.dsshir` |
| the codec, as tests (composites, cycles, layout channels, the spelling-work count, `return void`) | `tests/hir/test_hir_text.cpp` |
| the consumer contract, as tests | `tests/program/test_emit_hir_mode.cpp` |
| the shared type grammar (standalone texts keep inline composites and `rec <H>`) | [`ir-type-text.md`](./ir-type-text.md) |
