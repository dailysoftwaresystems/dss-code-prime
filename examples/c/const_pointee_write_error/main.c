// D-CSUBSET-POINTEE-CONST-ENFORCEMENT witness (negative / diagnostic).
//
// Writing through a const lvalue is a C 6.5.16.1 constraint violation: the left
// operand of an assignment shall be a MODIFIABLE lvalue, and 6.3.2.1p1 excludes
// a const-qualified type. Before P48 DSS enforced this for a PLAIN IDENTIFIER
// LHS only, so every shape below compiled in silence.
//
// ✔MEASURED 2026-09-01, each reference probed SEPARATELY at -O0 AND -O2, one
// self-contained translation unit per shape — UNANIMOUS HARD REJECT:
//   gcc 13.3.0 (-std=c2x)      "assignment of read-only location '*p'"
//                              / "assignment of read-only member 'v'"
//   clang 18.1.3 (-std=c23)    "read-only variable is not assignable"
//                              / "cannot assign to non-static data member 'v'
//                                 with const-qualified type 'const int'"
//   mingw-w64 gcc 13.2.0        as gcc
//   MSVC 19.51.36252 (/std:c17) "l-value specifies const object" (C2166)
// A unanimous refusal makes the diagnostic REQUIRED, and its Error severity is
// the references' own — not a choice.
//
// ── PART ONE, lane sm: writing through a const POINTEE / OBJECT ──────────────
// Four shapes, at three different levels of the qualifier spine:
//   *p        — a deref of a pointer-to-const             (spine level 1)
//   a[0]      — an element of a pointer-to-const          (spine level 1)
//   s->v      — a member of a pointer-to-const object     (object, level 1)
//   **pp      — two levels of indirection                 (spine level 2)
//
// ── PART TWO, lane cq: a field DECLARED const ────────────────────────────────
// The shape PART ONE deliberately left out, because its answer was missing in
// CONFIG rather than in code: c's `structField` / `unionField` declaration rows
// carried `volatileMarker` alone, so a member's own `const` was scanned by
// nothing and no field symbol was ever marked const. P48 added `constMarker`
// (and `restrictMarker`, which the qualifier spine's all-or-nothing rule makes
// inseparable from it) to both rows, which closed the pin with no change to the
// const-lvalue walk — it had been asking the right question all along.
//   q->cv        — a member declared `const int`
//   v->cv        — the same in a UNION, a SEPARATE config row
//   p->cp        — a member declared `int *const` (const on the POINTER, the
//                  c36 axis one level in)
//   o->in.cv     — a const member two member-steps down
//   o->cin.v     — a PLAIN member of a member declared `const struct` (C 6.5.2.3)
//   q->ca[0]     — an element of a member declared `const int[2]` (spine level 1)
//   q->cv += 1   — compound assignment, the same constraint
//
// The MUTABLE-pointer-to-const walk (`r++`), the four non-const writes and the
// three non-const FIELD writes are here as POLARITY CONTROLS: a check that fired
// on every deref / subscript / member — or a marker that answered `const` for
// every member — would satisfy every expectation above while refusing correct
// code, and this file would then still be "green" with a broken compiler. The
// `const int *` and `int *restrict` members matter most: `const char *zName;`
// members are everywhere in the sqlite corpus, and assigning the POINTER is
// legal in all four references.
//
// ⚠ ONE SHAPE OF THIS FAMILY IS DELIBERATELY NOT HERE, AND IT IS NOT A GAP. A
// member that is BOTH declared `const` AND a BIT-FIELD is the one shape on which
// the references SPLIT (gcc and mingw-w64 gcc warn and COMPILE; clang and MSVC
// refuse), so DSS warns and builds rather than erroring — a verdict this
// manifest cannot express, because `expectDiagnostics` carries a code and a
// position but no severity. It has its own RUNNABLE entry,
// `examples/c/const_bitfield_write_warns`, where the artifact executing IS the
// assertion that the program was still built.
//
// Front-end only (semantic tier), so any single target witnesses it.
//
// RED-ON-DISABLE: revert the widened LHS walk in semantic_analyzer.cpp
// (`constQualifiedLvalue` + its SE4 call) -> the PART ONE writes are silently
// accepted; delete `constMarker` from c.lang.json's `structField` / `unionField`
// rows -> the PART TWO writes are silently accepted. Either way the produced set
// shrinks and the EXACT SET EQUALITY this manifest asserts fails.
struct S { int v; };

struct Q { const int cv; int pv; const int ca[2]; };

union  V { const int cv; int pv; };

struct P { int *const cp; const int *pp; int *restrict rp; };

struct Inner { int v; };

struct Outer { struct Inner in2; const struct Inner cin; struct Q in; };

void write_through_pointer(const char *p) { *p = 'x'; }

void write_const_element(const int *a) { a[0] = 5; }

void write_member_of_const_object(const struct S *s) { s->v = 2; }

void write_through_double_pointer(const int *const *pp) { **pp = 1; }

// ── part two: a field DECLARED const ─────────────────────────────────────────
void write_const_declared_field(struct Q *q) { q->cv = 3; }

void write_const_declared_union_field(union V *v) { v->cv = 3; }

void write_const_pointer_field(struct P *p) { p->cp = 0; }

void write_nested_const_field(struct Outer *o) { o->in.cv = 5; }

void write_member_of_const_struct_field(struct Outer *o) { o->cin.v = 5; }

void write_const_array_field_element(struct Q *q) { q->ca[0] = 5; }

void compound_assign_const_field(struct Q *q) { q->cv += 1; }

// ── polarity controls: every one of these must stay CLEAN ────────────────────
void mutable_pointer_to_const_walks(const char *r) { r++; r += 2; }

void non_const_writes(char *p, int *a, struct S *s, struct S *b) {
    *p    = 'x';
    a[0]  = 5;
    s->v  = 2;
    b[1].v = 3;
}

void non_const_field_writes(struct Q *q, struct P *p, struct Outer *o, int *w) {
    q->pv  = 1;
    p->pp  = 0;
    p->rp  = w;
    o->in2.v = 4;
}

int main(void) { return 0; }
