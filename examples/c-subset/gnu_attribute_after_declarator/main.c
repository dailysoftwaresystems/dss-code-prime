// D-CSUBSET-GNU-ATTRIBUTE (TF-C62) witness: GNU `__attribute__((...))` in the
// AFTER-DECLARATOR position, with the widened argument grammar. Before TF-C62 the
// after-declarator position failed loud (`P0009 got '__attribute__'`) and the
// argument grammar accepted only `name` / `name("string")` — so `aligned(16)`
// (number), `format(printf,1,2)` (multi-arg), `__nonnull__((1))` (nested), and a
// multi-clause `a, b` run all failed. This is the shape every real C header uses
// on its prototypes — glibc: `extern int remove(const char*) __attribute__((
// __nothrow__, __leaf__));` ; Tcl: `Tcl_Panic(const char*,...) TCL_FORMAT_PRINTF(
// 1,2);`. The attributes are parse-and-ignore hints here (they do not change the
// program's behavior), so the point is that the program COMPILES and RUNS with
// them present, exactly as C requires.
//
// Covers, all in the after-declarator position:
//   * bare            `__attribute__((noreturn))`      on a real diverging function
//   * multi-arg       `__attribute__((format(printf,1,2)))`
//   * multi-clause    `__attribute__((__nothrow__, __leaf__))`
//   * nested-paren    `__attribute__((__nonnull__((1))))`
//   * number arg      `__attribute__((aligned(4)))`    on an object declarator
//
// The arithmetic (each call accumulates) yields 42; the attributes must not alter
// it. RED-ON-DISABLE: revert the after-declarator `attrSpec` slot in
// c-subset.lang.json (initDeclarator) → the first prototype fails P0009 and the
// program no longer compiles (the runner reports a compile failure, not exit 42).
//
// Front-end feature (grammar + declaration lowering), target/format-agnostic:
// x86_64 (PE + ELF) and arm64 (ELF under qemu, Mach-O macos leg).

// A locally-DEFINED variadic with a multi-arg `format` attribute (no extern, so
// nothing to link). It just returns its first arg; the point is that the
// format(printf,1,2) argument grammar PARSES on a real declarator.
int firstv(int n, ...) __attribute__((format(printf, 1, 2)));
int firstv(int n, ...) { return n; }

int add(int a, int b) __attribute__((__nothrow__, __leaf__));
int add(int a, int b) { return a + b; }

int deref(const int *p) __attribute__((__nonnull__((1))));
int deref(const int *p) { return *p; }

int v __attribute__((aligned(4))) = 20;

int main(void) {
    int twelve = 12;
    int s = add(v, twelve);      // 20 + 12 = 32   (v is the aligned global)
    int ten = 10;
    s = s + deref(&ten);         // 32 + 10 = 42
    s = s + firstv(0, 1, 2);     // + 0 = 42        (format-attributed variadic)
    return s;                    // 42
}
