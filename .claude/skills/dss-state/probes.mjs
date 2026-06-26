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
];
