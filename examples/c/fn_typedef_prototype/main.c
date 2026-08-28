// D-CSUBSET-FN-TYPEDEF-PROTOTYPE witness: a declaration `T x;` where T is a
// FUNCTION-TYPE typedef declares x as a function PROTOTYPE (C 6.7 / 6.9.1p2) — the
// exact shape of SQLite test_thread.c's `static Tcl_ObjCmdProc sqlthread_proc;`.
// Pre-fix DSS minted the bare `static Fn apply;` as an OBJECT and rejected it
// (S0018 "function prototype declarations are not supported here"); the later
// definition then collided (S0002 "redeclared symbol"). The fix recognizes a
// function-typed bare declarator as a prototype that MERGES with its definition
// (the static gives internal linkage; the definition supplies the body).
//
// `Fn` is a function TYPE, NOT a pointer (note: `int Fn(int)`, not `int (*Fn)(int)`
// — a `Fn *fp;` would be a data object, correctly NOT a prototype). `apply` and
// `twice` are declared via bare `static Fn ...;` prototypes and are forward-CALLED
// in main BEFORE their definitions appear — precisely how test_thread registers
// `sqlthread_proc` above its body. Value-divergent so any miscompile misses 42:
//   apply(20) = 21, twice(21) = 42  =>  exit 42 ; any bug => 7.
//
// RED-ON-DISABLE: revert the Pass-1 `maybeFnTypedefProto` candidate + the Pass-1.5
// FnSig upgrade -> the bare `static Fn apply;` is minted an OBJECT -> S0018 at the
// prototype + S0002 at the definition -> the program no longer COMPILES (the runner
// reports a compile failure instead of exit 42).
//
// Front-end (semantic + HIR) feature, fully target-agnostic — the emitted code is
// two ordinary functions and their calls. Runs on x86_64 (PE + ELF) and arm64
// (ELF qemu, Mach-O macos leg); the release arm re-witnesses under the optimizer.

typedef int Fn(int);      // a function TYPE typedef (NOT a pointer)

static Fn apply;          // bare function-typedef declarator -> a PROTOTYPE
static Fn twice;          // a second prototype (independent declaration)

int main(void) {
    int a = apply(20);    // 21  (apply defined below)
    int b = twice(a);     // 42  (twice defined below)
    return (a == 21 && b == 42) ? 42 : 7;
}

static int apply(int x) { return x + 1; }   // satisfies `static Fn apply;`
static int twice(int x) { return x * 2; }   // satisfies `static Fn twice;`
