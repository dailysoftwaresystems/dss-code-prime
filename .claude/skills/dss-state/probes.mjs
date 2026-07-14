// C-feature probe battery for the dss-state driver.
//
// Each probe is a tiny, UB-free C program with a deterministic exit code.
// The driver compiles each through the REAL dss-code-prime CLI for the host
// target and runs the produced binary. A probe therefore measures the WHOLE
// pipeline (lex → parse → semantic → HIR/MIR/LIR → asm → link → runtime),
// not just the grammar. Probes deliberately include features the compiler
// does NOT yet support — that is what makes the percentage meaningful and
// monotonically improvable.
//
// Style rules (learned from the examples corpus — keep new probes honest):
//  * declare-then-assign (`int x; x = 42;`) unless init-at-decl IS the probe
//  * never rely on Bool-arithmetic (`(a<b)*20` is a known semantic gap) —
//    use if/ternary accumulation instead
//  * one feature per probe; a failing probe should implicate ONE feature
//  * exit codes < 256 and meaningful (42 = pass convention; 7/9 = wrong path)
//
// `sqliteWeight` per category: rough share of what compiling the SQLite
// amalgamation (sqlite3.c, single TU, SQLITE_THREADSAFE=0) demands.
// Weights sum to 100; weight-0 categories don't count toward readiness.

export const CATEGORIES = {
  core_expr:     { label: 'Core expressions',                     sqliteWeight: 10 },
  integer_model: { label: 'Integer model (widths/UAC/64-bit)',    sqliteWeight: 6 },
  float:         { label: 'Floating point',                       sqliteWeight: 4 },
  control_flow:  { label: 'Control flow',                         sqliteWeight: 8 },
  functions:     { label: 'Functions & function pointers',        sqliteWeight: 10 },
  pointers:      { label: 'Pointers & memory',                    sqliteWeight: 12 },
  aggregates:    { label: 'Aggregates (struct/union/enum/array)', sqliteWeight: 20 },
  declarations:  { label: 'Declarations & storage classes',       sqliteWeight: 6 },
  preprocessor:  { label: 'Preprocessor',                         sqliteWeight: 20 },
  ffi:           { label: 'libc FFI (shipped headers)',           sqliteWeight: 4 },
  modern:        { label: 'C11/C23 modern surface',               sqliteWeight: 0 },
  // The §3.2 "named exclusions" of plan-23 — C23 features scoped OUT of the FC
  // arc's "C23 as mainstream compilers implement it" target. Tracked HERE as
  // loud red gaps (the DSS Axis foundation) instead of left invisible. Weight 0:
  // SQLite uses none, so readiness is unaffected — but the empirical-battery
  // denominator IS, on purpose. Each flips green the cycle its feature lands.
  c23_advanced:  { label: 'C23 advanced tier (atomics/complex/tgmath/…)', sqliteWeight: 0 },
};

// Canonical known-good program used for the cross-target emit/run matrix.
export const CANONICAL_SRC = 'int main() { return 42; }\n';

export const PROBES = [
  // ───────────────────────── core_expr ─────────────────────────
  { id: 'core_return_literal', cat: 'core_expr', expect: 42, src:
`int main() { return 42; }
` },
  { id: 'core_arith_mix', cat: 'core_expr', expect: 42, src:
`int main() { return 50 - 16 / 2; }
` },
  { id: 'core_parens', cat: 'core_expr', expect: 42, src:
`int main() { return (2 + 4) * 7; }
` },
  { id: 'core_modulo', cat: 'core_expr', expect: 42, src:
`int main() { return 142 % 100; }
` },
  { id: 'core_bitwise', cat: 'core_expr', expect: 42, src:
`int main() {
    int x;
    int y;
    x = (32 | 8) & 62;
    y = x ^ 0;
    return y + (~0 + 3);
}
` },
  { id: 'core_shifts', cat: 'core_expr', expect: 42, src:
`int main() { return (1 << 5) + (40 >> 2); }
` },
  { id: 'core_compare_chain', cat: 'core_expr', expect: 42, src:
`int main() {
    int s;
    s = 0;
    if (3 < 4)  { s = s + 20; }
    if (4 <= 4) { s = s + 10; }
    if (5 > 4)  { s = s + 8; }
    if (4 >= 5) { s = s + 99; }
    if (1 == 1) { s = s + 4; }
    if (1 != 1) { s = s + 99; }
    return s;
}
` },
  { id: 'core_logical_ops', cat: 'core_expr', expect: 42, src:
`int main() {
    int s;
    s = 0;
    if (1 && 1) { s = s + 40; }
    if (0 || 1) { s = s + 2; }
    if (0 && 1) { s = s + 99; }
    return s;
}
` },
  { id: 'core_logical_not', cat: 'core_expr', expect: 42, src:
`int main() {
    if (!0) { return 42; }
    return 7;
}
` },
  { id: 'core_ternary', cat: 'core_expr', expect: 42, src:
`int main() { return 1 ? 42 : 7; }
` },
  { id: 'core_comma_operator', cat: 'core_expr', expect: 42, src:
`int main() {
    int x;
    x = 0;
    return (x = 40, x + 2);
}
` },
  { id: 'core_unary_neg', cat: 'core_expr', expect: 42, src:
`int main() { return -(-42); }
` },
  { id: 'core_incdec', cat: 'core_expr', expect: 42, src:
`int main() {
    int i;
    i = 40;
    i++;
    ++i;
    return i;
}
` },
  { id: 'core_compound_assign', cat: 'core_expr', expect: 42, src:
`int main() {
    int x;
    x = 5;
    x += 40;
    x -= 3;
    x *= 2;
    x /= 2;
    x %= 100;
    return x;
}
` },
  { id: 'core_char_literal', cat: 'core_expr', expect: 42, src:
`int main() { return 'A' - 23; }
` },
  { id: 'core_sizeof', cat: 'core_expr', expect: 42, src:
`int main() { return (int)sizeof(int) * 10 + 2; }
` },
  { id: 'core_hex_bin_literal', cat: 'core_expr', expect: 42, src:
`int main() { return 0x20 + 0b1010; }
` },
  { id: 'core_digit_separator', cat: 'core_expr', expect: 42, src:
`int main() { return 1'042 - 1'000; }
` },
  // Adjacent string-literal concatenation (C 6.4.5p5) — `"a" "b"` ≡ "ab".
  // Pervasive in sqlite3.c error messages; its absence cascades hard
  // (50× "expected ')' — got '\"'" in the real amalgamation log).
  { id: 'core_string_concat', cat: 'core_expr', expect: 42, src:
`int main() { return ("AB" "CD")[3] - 26; }
` },

  // ──────────────────────── integer_model ────────────────────────
  { id: 'int_u32_wraparound', cat: 'integer_model', expect: 42, src:
`int main() {
    unsigned x;
    x = 4294967295u;
    x = x + 43u;
    return (int)x;
}
` },
  { id: 'int_mixed_sign_compare', cat: 'integer_model', expect: 42, src:
`int main() {
    if (-1 > 0u) { return 42; }
    return 7;
}
` },
  { id: 'int_long_long_arith', cat: 'integer_model', expect: 42, src:
`int main() {
    long long big;
    big = 4294967296ll;
    big = big * 2ll;
    if (big == 8589934592ll) { return 42; }
    return 7;
}
` },
  { id: 'int_wide_immediate', cat: 'integer_model', expect: 42, src:
`int main() {
    long long big;
    big = 1099511627776ll;
    return (int)(big >> 35) + 10;
}
` },
  { id: 'int_short_char_promote', cat: 'integer_model', expect: 42, src:
`int main() {
    short s;
    char c;
    s = 40;
    c = 2;
    return s + c;
}
` },
  { id: 'int_unsigned_div_highbit', cat: 'integer_model', expect: 42, src:
`int main() {
    unsigned x;
    x = 2147487747u;
    return (int)(x / 51130660u);
}
` },

  // ─────────────────────────── float ───────────────────────────
  { id: 'float_double_cast', cat: 'float', expect: 42, src:
`int main() { return (int)(0.5 + 41.75); }
` },
  { id: 'float_f32_arith', cat: 'float', expect: 42, src:
`int main() {
    float f;
    f = 21.0f;
    return (int)(f + 21.0f);
}
` },
  { id: 'float_hexfloat_literal', cat: 'float', expect: 42, src:
`int main() { return (int)(0x1.5p5); }
` },
  { id: 'float_compare', cat: 'float', expect: 42, src:
`int main() {
    double a;
    a = 1.5;
    if (a > 1.0) {
        if (a < 2.0) { return 42; }
    }
    return 7;
}
` },
  { id: 'float_nan_neq', cat: 'float', expect: 42, src:
`int main() {
    double z;
    double n;
    z = 0.0;
    n = z / z;
    if (n != n) { return 42; }
    return 7;
}
` },
  { id: 'float_div', cat: 'float', expect: 42, src:
`int main() { return (int)(84.9 / 2.0); }
` },
  { id: 'float_mul', cat: 'float', expect: 42, src:
`int main() {
    double a;
    double b;
    a = 21.0;
    b = 2.0;
    return (int)(a * b);
}
` },
  { id: 'float_sub', cat: 'float', expect: 42, src:
`int main() {
    double a;
    double b;
    a = 63.5;
    b = 21.5;
    return (int)(a - b);
}
` },

  // ──────────────────────── control_flow ────────────────────────
  { id: 'cf_if_else_chain', cat: 'control_flow', expect: 42, src:
`int main() {
    int v;
    v = 3;
    if (v == 1) { return 7; }
    else if (v == 3) { return 42; }
    else { return 9; }
}
` },
  { id: 'cf_while_loop', cat: 'control_flow', expect: 42, src:
`int main() {
    int n;
    int s;
    n = 6;
    s = 0;
    while (n != 0) {
        s = s + 7;
        n = n - 1;
    }
    return s;
}
` },
  { id: 'cf_for_loop', cat: 'control_flow', expect: 42, src:
`int main() {
    int i;
    int s;
    s = 0;
    for (i = 0; i < 21; i = i + 1) {
        s = s + 2;
    }
    return s;
}
` },
  { id: 'cf_for_incdec_update', cat: 'control_flow', expect: 42, src:
`int main() {
    int i;
    int s;
    s = 0;
    for (i = 0; i < 42; i++) {
        s = s + 1;
    }
    return s;
}
` },
  { id: 'cf_nested_loops', cat: 'control_flow', expect: 42, src:
`int main() {
    int i;
    int j;
    int s;
    s = 0;
    for (i = 0; i < 6; i = i + 1) {
        for (j = 0; j < 7; j = j + 1) {
            s = s + 1;
        }
    }
    return s;
}
` },
  { id: 'cf_break', cat: 'control_flow', expect: 42, src:
`int main() {
    int s;
    s = 0;
    while (1) {
        s = 42;
        break;
    }
    return s;
}
` },
  { id: 'cf_continue', cat: 'control_flow', expect: 42, src:
`int main() {
    int i;
    int s;
    s = 0;
    for (i = 1; i <= 12; i = i + 1) {
        if (i % 2 == 1) { continue; }
        s = s + i;
    }
    return s;
}
` },
  { id: 'cf_do_while', cat: 'control_flow', expect: 42, src:
`int main() {
    int x;
    x = 0;
    do {
        x = x + 1;
    } while (x < 42);
    return x;
}
` },
  { id: 'cf_switch', cat: 'control_flow', expect: 42, src:
`int main() {
    int v;
    v = 3;
    switch (v) {
        case 1: return 7;
        case 3: return 42;
        default: return 9;
    }
}
` },
  { id: 'cf_goto', cat: 'control_flow', expect: 42, src:
`int main() {
    goto done;
    return 7;
done:
    return 42;
}
` },
  { id: 'cf_empty_statement', cat: 'control_flow', expect: 42, src:
`int main() {
    ;
    return 42;
}
` },

  // ───────────────────────── functions ─────────────────────────
  { id: 'fn_helper_args', cat: 'functions', expect: 42, src:
`int add(int a, int b) {
    return a + b;
}

int main() { return add(40, 2); }
` },
  { id: 'fn_recursion', cat: 'functions', expect: 42, src:
`int fact(int n) {
    if (n <= 1) { return 1; }
    return n * fact(n - 1);
}

int main() { return fact(4) + 18; }
` },
  { id: 'fn_fnptr_call', cat: 'functions', expect: 42, src:
`int add(int a, int b) {
    return a + b;
}

int main() {
    int (*fp)(int, int);
    fp = add;
    return fp(40, 2);
}
` },
  { id: 'fn_fnptr_callback', cat: 'functions', expect: 42, src:
`int add(int a, int b) {
    return a + b;
}

int apply(int (*op)(int, int), int a, int b) {
    return op(a, b);
}

int main() { return apply(add, 40, 2); }
` },
  { id: 'fn_fnptr_deref_call', cat: 'functions', expect: 42, src:
`int add(int a, int b) {
    return a + b;
}

int main() {
    int (*fp)(int, int);
    fp = add;
    return (*fp)(40, 2);
}
` },
  { id: 'fn_prototype_forward', cat: 'functions', expect: 42, src:
`int add(int a, int b);

int main() { return add(40, 2); }

int add(int a, int b) {
    return a + b;
}
` },
  { id: 'fn_void_param', cat: 'functions', expect: 42, src:
`int answer(void) {
    return 42;
}

int main() { return answer(); }
` },
  { id: 'fn_unnamed_param', cat: 'functions', expect: 42, src:
`int second(int, int b) {
    return b;
}

int main() { return second(7, 42); }
` },
  { id: 'fn_static_function', cat: 'functions', expect: 42, src:
`static int hidden() {
    return 42;
}

int main() { return hidden(); }
` },
  { id: 'fn_variadic_definition', cat: 'functions', expect: 42, src:
`int first(int a, ...) {
    return a;
}

int main() { return first(42, 7, 9); }
` },
  { id: 'fn_multi_tu_link', cat: 'functions', expect: 42, files: {
      'main.c':
`extern int add5(int x);

int main() { return add5(37); }
`,
      'helper.c':
`int add5(int x) {
    return x + 5;
}
` } },
  // Prototype with UNNAMED mixed pointer+int params (sqlite3.c API decls:
  // `...sqlite3_column_database_name(sqlite3_stmt*,int);`). The amalgamation
  // choked at the `int` (P0009) — pin the bare (T*, int) prototype form.
  { id: 'fn_proto_unnamed_ptr_int', cat: 'functions', expect: 42, src:
`int store(int*, int);

int main() {
    int x;
    x = 0;
    return store(&x, 42);
}

int store(int* p, int v) {
    *p = v;
    return *p;
}
` },

  // ───────────────────────── pointers ─────────────────────────
  { id: 'ptr_deref_load', cat: 'pointers', expect: 42, src:
`int read_through(int* p) {
    return *p;
}

int main() {
    int x;
    x = 42;
    return read_through(&x);
}
` },
  { id: 'ptr_deref_store', cat: 'pointers', expect: 42, src:
`int main() {
    int x;
    int* p;
    x = 7;
    p = &x;
    *p = 42;
    return x;
}
` },
  { id: 'ptr_swap_through', cat: 'pointers', expect: 42, src:
`void swap(int* a, int* b) {
    int t;
    t = *a;
    *a = *b;
    *b = t;
}

int main() {
    int x;
    int y;
    x = 2;
    y = 40;
    swap(&x, &y);
    return x - y + 4;
}
` },
  { id: 'ptr_multilevel', cat: 'pointers', expect: 42, src:
`int main() {
    int x;
    int* p;
    int** pp;
    x = 42;
    p = &x;
    pp = &p;
    return **pp;
}
` },
  { id: 'ptr_arith_roundtrip', cat: 'pointers', expect: 42, src:
`int main() {
    int x;
    int* p;
    int* q;
    x = 42;
    p = &x;
    q = p + 1;
    q = q - 1;
    return *q;
}
` },
  { id: 'ptr_null_compare', cat: 'pointers', expect: 42, src:
`int main() {
    int* p;
    p = 0;
    if (p == 0) { return 42; }
    return 7;
}
` },
  { id: 'ptr_void_roundtrip', cat: 'pointers', expect: 42, src:
`int main() {
    int x;
    void* v;
    int* p;
    x = 42;
    v = &x;
    p = (int*)v;
    return *p;
}
` },
  { id: 'ptr_string_deref', cat: 'pointers', expect: 42, src:
`int main() {
    char* s;
    s = "hello";
    return *s - 62;
}
` },

  // ──────────────────────── aggregates ────────────────────────
  { id: 'agg_struct_member', cat: 'aggregates', expect: 42, src:
`struct Point { int x; int y; };

int main() {
    struct Point p;
    p.x = 40;
    p.y = 2;
    return p.x + p.y;
}
` },
  { id: 'agg_struct_arrow', cat: 'aggregates', expect: 42, src:
`struct Box { int v; };

int get(struct Box* b) {
    return b->v;
}

int main() {
    struct Box q;
    q.v = 42;
    return get(&q);
}
` },
  { id: 'agg_struct_byval_param', cat: 'aggregates', expect: 42, src:
`struct Pair { int a; int b; };

int take(struct Pair p) {
    return p.a + p.b;
}

int main() {
    struct Pair p;
    p.a = 40;
    p.b = 2;
    return take(p);
}
` },
  { id: 'agg_struct_return', cat: 'aggregates', expect: 42, src:
`struct Pair { int a; int b; };

struct Pair make() {
    struct Pair p;
    p.a = 40;
    p.b = 2;
    return p;
}

int main() {
    struct Pair p;
    p = make();
    return p.a + p.b;
}
` },
  { id: 'agg_union_member', cat: 'aggregates', expect: 42, src:
`union U { int i; };

int main() {
    union U u;
    u.i = 42;
    return u.i;
}
` },
  { id: 'agg_enum_constants', cat: 'aggregates', expect: 42, src:
`enum E { A = 40, B = 2 };

int main() { return A + B; }
` },
  { id: 'agg_array_local', cat: 'aggregates', expect: 42, src:
`int main() {
    int a[3];
    a[0] = 40;
    a[2] = 2;
    return a[0] + a[2];
}
` },
  { id: 'agg_array_global', cat: 'aggregates', expect: 42, src:
`int g[4];

int main() {
    g[1] = 40;
    g[3] = 2;
    return g[1] + g[3];
}
` },
  { id: 'agg_string_index', cat: 'aggregates', expect: 42, src:
`int main() { return "ABC"[1] - 24; }
` },
  { id: 'agg_designated_init', cat: 'aggregates', expect: 42, src:
`struct Point { int x; int y; };

int main() {
    struct Point p = { .x = 40, .y = 2 };
    return p.x + p.y;
}
` },
  { id: 'agg_struct_global', cat: 'aggregates', expect: 42, src:
`struct Pair { int a; int b; };

struct Pair gp = { 40, 2 };

int main() { return gp.a + gp.b; }
` },
  // Bitfields (C 6.7.2.1). sqlite3.c uses them throughout its parse-context
  // structs (`bft disableTriggers:1;`); the real amalgamation cascades 50×
  // "expected EndStatement" off this shape. Plain-type base to isolate the
  // bitfield grammar from typedef recognition.
  { id: 'agg_bitfield', cat: 'aggregates', expect: 42, src:
`struct Flags { unsigned a : 4; unsigned b : 28; };

int main() {
    struct Flags f;
    f.a = 10;
    f.b = 32;
    return f.a + f.b;
}
` },
  // volatile-qualified struct field (sqlite3.c: `volatile int isInterrupted;`
  // in struct Sqlite3). A walled `volatile` breaks the whole struct → its
  // closing `};` fails to parse. Distinct from the local-var `mod_volatile_local`.
  { id: 'agg_volatile_field', cat: 'aggregates', expect: 42, src:
`struct S { volatile int x; int y; };

int main() {
    struct S s;
    s.x = 40;
    s.y = 2;
    return s.x + s.y;
}
` },
  // Bitfield with a TYPEDEF base type (sqlite3.c: `typedef unsigned int bft;`
  // then `bft disableTriggers:1;` in struct Parse). Distinct from agg_bitfield
  // (plain `unsigned`), which already passes — this isolates whether the
  // amalgamation's 20999+ cascade is a real typedef-base-bitfield gap or just
  // downstream of the 18907 struct-close desync.
  { id: 'agg_bitfield_typedef', cat: 'aggregates', expect: 42, src:
`typedef unsigned int bft;

struct Flags { bft a : 4; bft b : 28; };

int main() {
    struct Flags f;
    f.a = 10;
    f.b = 32;
    return f.a + f.b;
}
` },
  // Anonymous/nested union member inside a struct (the sqlite3.c:18907 `};`
  // root signature — a struct whose `};` is orphaned because a nested
  // aggregate member consumed the closing brace). Anonymous members are an
  // FC16 item; this pins whether the desync reproduces in the small.
  { id: 'agg_nested_union_member', cat: 'aggregates', expect: 42, src:
`struct V { int tag; union { int i; char c; } u; };

int main() {
    struct V v;
    v.tag = 10;
    v.u.i = 32;
    return v.tag + v.u.i;
}
` },
  // Self-referential struct (a struct with a pointer to its own type) —
  // sqlite3.c: `struct sqlite3 { sqlite3 *pBlockingConnection; ... };` and
  // every linked-list/tree node. Closed by c24 (nominal composite typing,
  // D-CSUBSET-SELF-REFERENTIAL-STRUCT); this probe captures that win so the
  // battery stops being blind to it.
  { id: 'agg_self_referential_struct', cat: 'aggregates', expect: 42, src:
`struct Node { int v; struct Node* next; };

int main() {
    struct Node b;
    struct Node a;
    b.v = 2;
    b.next = 0;
    a.v = 40;
    a.next = &b;
    return a.v + a.next->v;
}
` },
  // TRULY ANONYMOUS (unnamed) union member — C11 6.7.2.1p13, accessed as if
  // its fields belong to the enclosing struct (`v.i`, not `v.u.i`). This is
  // the FC16 anonymous-member feature and the prime suspect for the
  // sqlite3.c:18907 `};` brace-desync root (named nested members already pass).
  { id: 'agg_anon_union_member', cat: 'aggregates', expect: 42, src:
`struct V { int tag; union { int i; char c; }; };

int main() {
    struct V v;
    v.tag = 10;
    v.i = 32;
    return v.tag + v.i;
}
` },

  // ─────────────────────── declarations ───────────────────────
  { id: 'decl_init_at_decl', cat: 'declarations', expect: 42, src:
`int main() {
    int x = 42;
    return x;
}
` },
  { id: 'decl_typedef_int', cat: 'declarations', expect: 42, src:
`typedef int myint;

int main() {
    myint x;
    x = 42;
    return x;
}
` },
  { id: 'decl_typedef_struct', cat: 'declarations', expect: 42, src:
`typedef struct { int x; } T;

int main() {
    T t;
    t.x = 42;
    return t.x;
}
` },
  { id: 'decl_multi_declarator', cat: 'declarations', expect: 42, src:
`int main() {
    int a, b;
    a = 40;
    b = 2;
    return a + b;
}
` },
  { id: 'decl_global_init', cat: 'declarations', expect: 42, src:
`int answer = 42;

int main() { return answer; }
` },
  { id: 'decl_const_local', cat: 'declarations', expect: 42, src:
`int main() {
    const int c = 42;
    return c;
}
` },
  { id: 'decl_local_static', cat: 'declarations', expect: 42, src:
`int bump() {
    static int n = 40;
    n = n + 1;
    return n;
}

int main() {
    bump();
    return bump();
}
` },
  { id: 'decl_extern_var_tu', cat: 'declarations', expect: 42, files: {
      'main.c':
`extern int shared;

int main() { return shared; }
`,
      'def.c':
`int shared = 42;
` } },
  { id: 'decl_string_global', cat: 'declarations', expect: 42, src:
`char* msg = "*hello";

int main() { return *msg; }
` },

  // ─────────────────────── preprocessor ───────────────────────
  { id: 'pp_include_shipped', cat: 'preprocessor', expect: 42, stdout: 'hello\n', src:
`#include <stdio.h>

int main() {
    puts("hello");
    return 42;
}
` },
  { id: 'pp_define_object', cat: 'preprocessor', expect: 42, src:
`#define ANSWER 42

int main() { return ANSWER; }
` },
  { id: 'pp_define_function', cat: 'preprocessor', expect: 42, src:
`#define ADD(a, b) ((a) + (b))

int main() { return ADD(40, 2); }
` },
  { id: 'pp_ifdef_else', cat: 'preprocessor', expect: 42, src:
`#define ON

#ifdef ON
int main() { return 42; }
#else
int main() { return 7; }
#endif
` },
  { id: 'pp_line_macro', cat: 'preprocessor', expect: 42, src:
`int main() {
    return __LINE__ + 40;
}
` },
  { id: 'pp_token_paste', cat: 'preprocessor', expect: 42, src:
`#define GLUE(a, b) a##b

int main() {
    int GLUE(an, swer);
    answer = 42;
    return answer;
}
` },

  // ─────────────────────────── ffi ───────────────────────────
  { id: 'ffi_putchar', cat: 'ffi', expect: 42, stdout: 'A\n', src:
`#include <stdio.h>

int main() {
    putchar(65);
    putchar(10);
    return 42;
}
` },
  { id: 'ffi_printf_vararg', cat: 'ffi', expect: 0, stdout: 'answer=42\n', src:
`extern int printf(const char* fmt, ...);

int main() {
    printf("answer=%d\\n", 42);
    return 0;
}
` },
  { id: 'ffi_exit', cat: 'ffi', expect: 42, src:
`#include <stdlib.h>

int main() {
    exit(42);
    return 7;
}
` },
  { id: 'ffi_strlen', cat: 'ffi', expect: 42, src:
`#include <string.h>

int main() { return (int)strlen("abcde") + 37; }
` },
  { id: 'ffi_strcmp', cat: 'ffi', expect: 42, src:
`#include <string.h>

int main() {
    if (strcmp("ab", "ab") == 0) { return 42; }
    return 7;
}
` },
  { id: 'ffi_malloc_free', cat: 'ffi', expect: 42, src:
`#include <stdlib.h>

int main() {
    int* p;
    int r;
    p = (int*)malloc(8);
    if (p == 0) { return 9; }
    *p = 42;
    r = *p;
    free(p);
    return r;
}
` },
  { id: 'ffi_sqrt', cat: 'ffi', expect: 42, src:
`#include <math.h>

int main() { return (int)sqrt(1764.0); }
` },
  { id: 'ffi_isdigit', cat: 'ffi', expect: 42, src:
`#include <ctype.h>

int main() {
    if (isdigit(55)) { return 42; }
    return 7;
}
` },
  { id: 'ffi_memcpy', cat: 'ffi', expect: 42, src:
`#include <string.h>

int main() {
    int a;
    int b;
    a = 42;
    b = 7;
    memcpy(&b, &a, 4);
    return b;
}
` },
  { id: 'ffi_fopen_write', cat: 'ffi', expect: 42, src:
`#include <stdio.h>

int main() {
    void* f;
    f = fopen("probe_out.txt", "w");
    if (f == 0) { return 9; }
    fputs("data", f);
    fclose(f);
    return 42;
}
` },

  // ───────────────────────── modern ─────────────────────────
  { id: 'mod_static_assert', cat: 'modern', expect: 42, src:
`static_assert(1, "ok");

int main() { return 42; }
` },
  { id: 'mod_generic_selection', cat: 'modern', expect: 42, src:
`int main() {
    return _Generic(1, int: 42, default: 7);
}
` },
  { id: 'mod_attr_bracket', cat: 'modern', expect: 42, src:
`[[maybe_unused]] int g = 7;

int main() { return 42; }
` },
  { id: 'mod_volatile_local', cat: 'modern', expect: 42, src:
`int main() {
    volatile int v;
    v = 42;
    return v;
}
` },
  { id: 'mod_restrict_param', cat: 'modern', expect: 42, src:
`int read_through(int* restrict p) {
    return *p;
}

int main() {
    int x;
    x = 42;
    return read_through(&x);
}
` },
  { id: 'mod_register_local', cat: 'modern', expect: 42, src:
`int main() {
    register int r;
    r = 42;
    return r;
}
` },

  // ───────── C23 conformance surface (added 2026-07-08) ─────────
  // Each probe isolates ONE C23 (or late-C99/C11) semantic the battery
  // did not previously exercise. A red probe here is a concrete, named
  // conformance gap — the honest denominator for "full C23".
  { id: 'c23_nullptr', cat: 'modern', expect: 42, src:
`int main() {
    int* p;
    p = nullptr;
    if (p == nullptr) { return 42; }
    return 7;
}
` },
  { id: 'c23_bool_keyword', cat: 'modern', expect: 42, src:
`int main() {
    bool t;
    bool f;
    t = true;
    f = false;
    if (t) { if (!f) { return 42; } }
    return 7;
}
` },
  { id: 'c23_digit_separators', cat: 'modern', expect: 42, src:
`int main() { return 1'000'042 - 1'000'000; }
` },
  { id: 'c23_typeof', cat: 'modern', expect: 42, src:
`int main() {
    int x;
    typeof(x) y;
    x = 42;
    y = x;
    return y;
}
` },
  { id: 'c23_typeof_unqual', cat: 'modern', expect: 42, src:
`int main() {
    const int x = 42;
    typeof_unqual(x) y;
    y = x;
    return y;
}
` },
  { id: 'c23_constexpr', cat: 'modern', expect: 42, src:
`int main() {
    constexpr int k = 42;
    return k;
}
` },
  { id: 'c23_auto_type', cat: 'modern', expect: 42, src:
`int main() {
    auto y = 42;
    return y;
}
` },
  { id: 'c23_bitint', cat: 'modern', expect: 42, src:
`int main() {
    _BitInt(32) x;
    x = 42;
    return (int)x;
}
` },
  { id: 'c23_alignof', cat: 'modern', expect: 42, src:
`int main() {
    if (alignof(int) == 4) { return 42; }
    return 7;
}
` },
  { id: 'c23_alignas', cat: 'modern', expect: 42, src:
`int main() {
    alignas(16) int x;
    unsigned long long addr;
    x = 42;
    addr = (unsigned long long)(&x);
    if (addr % 16 == 0) { return x; }
    return 7;
}
` },
  { id: 'c23_u8_char', cat: 'modern', expect: 42, src:
`int main() {
    unsigned char c;
    c = u8'*';
    return c;
}
` },
  { id: 'c23_enum_fixed_type', cat: 'modern', expect: 42, src:
`enum E : unsigned char { LO = 40, HI = 2 };

int main() {
    enum E a;
    enum E b;
    a = LO;
    b = HI;
    return a + b;
}
` },
  { id: 'c23_attr_nodiscard', cat: 'modern', expect: 42, src:
`[[nodiscard]] int compute(void) { return 42; }

int main() {
    int r;
    r = compute();
    return r;
}
` },
  { id: 'c23_attr_fallthrough', cat: 'modern', expect: 42, src:
`int main() {
    int x;
    x = 0;
    switch (1) {
        case 1: x = x + 10; [[fallthrough]];
        case 2: x = x + 32; break;
        default: x = 99;
    }
    return x;
}
` },
  { id: 'c23_empty_init', cat: 'modern', expect: 42, src:
`int main() {
    int z = {};
    int v = {42};
    return z + v;
}
` },
  { id: 'c23_elifdef', cat: 'modern', expect: 42, src:
`#define FEATURE_B
#ifdef FEATURE_A
int pick = 1;
#elifdef FEATURE_B
int pick = 42;
#else
int pick = 2;
#endif

int main() { return pick; }
` },
  { id: 'c23_has_include', cat: 'modern', expect: 42, src:
`#if __has_include(<stddef.h>)
int a = 40;
#else
int a = 0;
#endif
#if __has_include(<definitely_absent_zzz.h>)
int b = 100;
#else
int b = 2;
#endif

int main() { return a + b; }
` },
  { id: 'c11_thread_local', cat: 'modern', expect: 42, src:
`thread_local int counter;

int main() {
    counter = 42;
    return counter;
}
` },
  { id: 'c99_func_name', cat: 'modern', expect: 42, src:
`int main() {
    const char* fn;
    fn = __func__;
    if (fn[0] == 'm') { return 42; }
    return 7;
}
` },
  { id: 'c99_compound_literal', cat: 'modern', expect: 42, src:
`struct Pt { int x; int y; };

int main() {
    struct Pt p;
    p = (struct Pt){ 40, 2 };
    return p.x + p.y;
}
` },
  { id: 'c99_vla', cat: 'modern', expect: 42, src:
`int main() {
    int n;
    n = 6;
    int a[n];
    a[0] = 42;
    return a[0];
}
` },

  // ───── C23 advanced tier — plan-23 §3.2 named exclusions (added 2026-07-14) ─────
  // The features scoped OUT of "C23 as mainstream compilers implement it": the
  // substrate DSS Axis will stand on. Each is a loud red gap until its arc lands,
  // then flips green. Values are exact (no precision/edge sensitivity) so a probe
  // either cleanly PASSES (feature present) or cleanly REJECTS (feature absent) —
  // never a spurious miscompile. `_Atomic` qualifier is split from <stdatomic.h>
  // because the qualifier may be accepted long before real atomics + ordering.

  // _Atomic type qualifier (C11) — language-level; may go green before real atomics.
  { id: 'c23_atomic_qualifier', cat: 'c23_advanced', expect: 42, src:
`int main() {
    _Atomic int x;
    x = 42;
    return x;
}
` },
  // <stdatomic.h> + explicit memory ordering — the real "atomics with ordering" test.
  { id: 'c11_stdatomic_order', cat: 'c23_advanced', expect: 42, src:
`#include <stdatomic.h>

int main() {
    atomic_int x;
    atomic_store_explicit(&x, 42, memory_order_relaxed);
    return atomic_load_explicit(&x, memory_order_seq_cst);
}
` },
  // _Complex + <complex.h> — creal(40+2i)=40, cimag=2.
  { id: 'c99_complex_arith', cat: 'c23_advanced', expect: 42, src:
`#include <complex.h>

int main() {
    double _Complex z;
    z = 40.0 + 2.0 * I;
    return (int)creal(z) + (int)cimag(z);
}
` },
  // <tgmath.h> type-generic dispatch — sqrt on a float must select sqrtf; 42^2=1764.
  { id: 'c99_tgmath_generic', cat: 'c23_advanced', expect: 42, src:
`#include <tgmath.h>

int main() {
    float x;
    x = 1764.0f;
    return (int)sqrt(x);
}
` },
  // #embed (C23) — the companion byte file (aux, NOT compiled) holds '*' = 0x2A = 42.
  { id: 'c23_embed', cat: 'c23_advanced', expect: 42, aux: { 'embed_answer.bin': '*' }, src:
`int main() {
    static const unsigned char answer[] = {
#embed "embed_answer.bin"
    };
    return answer[0];
}
` },
  // long double — acceptance + the L suffix (20+22=42, exact in any format, so no
  // precision-divergence miscompile). Real 80/128-bit divergence is an ABI matter.
  { id: 'c_long_double', cat: 'c23_advanced', expect: 42, src:
`int main() {
    long double x;
    x = 20.0L;
    x = x + 22.0L;
    return (int)x;
}
` },
  // setjmp/longjmp — non-local control flow; longjmp(env,42) makes setjmp return 42.
  { id: 'c_setjmp_longjmp', cat: 'c23_advanced', expect: 42, src:
`#include <setjmp.h>

int main() {
    jmp_buf env;
    int r;
    r = setjmp(env);
    if (r == 0) { longjmp(env, 42); }
    return r;
}
` },
  // Inline asm — acceptance of the asm statement (empty template = host-agnostic,
  // no arch-specific instruction, no miscompile risk); real codegen is per-target.
  { id: 'c_inline_asm', cat: 'c23_advanced', expect: 42, src:
`int main() {
    int x;
    x = 42;
    __asm__ volatile ("");
    return x;
}
` },
  // <stdbit.h> (C23) — popcount(255) = 8, + 34 = 42.
  { id: 'c23_stdbit_popcount', cat: 'c23_advanced', expect: 42, src:
`#include <stdbit.h>

int main() {
    unsigned int x;
    x = 255u;
    return (int)stdc_count_ones(x) + 34;
}
` },
  // <threads.h> (C11) — PROMOTED into scope, lands before FC18. Deterministic
  // mutex round-trip (no thread scheduling) so it's a clean pass the cycle it lands.
  { id: 'c11_threads_mutex', cat: 'c23_advanced', expect: 42, src:
`#include <threads.h>

int main() {
    mtx_t m;
    mtx_init(&m, mtx_plain);
    mtx_lock(&m);
    mtx_unlock(&m);
    mtx_destroy(&m);
    return 42;
}
` },
];
