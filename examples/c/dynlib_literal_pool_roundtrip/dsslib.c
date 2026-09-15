// ★★★ THE SHARED LIBRARY THAT CARRIES COMPILER-SYNTHESIZED DEFINITIONS —
// [[D-MIR-DYLIB-SELF-CALL-BYPASSES-WEAK-COALESCING]], the corpus gap that let a
// BLOCKING regression ship green.
//
// WHY THIS FILE EXISTS, stated as the measurement that produced it. Before it,
// the whole corpus built exactly ONE dynamic library from C source
// (`dynlib_resolve_roundtrip`), and NEITHER of that example's two sources
// contained a single `"` character. So no corpus example ever asked a
// `-dyn`/`-dll`/`-dylib` build to place a string literal, a floating-point
// constant or `__func__` — and when the preemptible-definition routing began
// judging every module global by its binding and visibility ALONE, the
// compiler-synthesized literal-pool globals (minted `Global`/`Default` with NO
// on-binary name) were judged preemptible, found nameless, and REFUSED.
// ✔MEASURED 2026-09-06: `const char *greet(void){ return "hello, world"; }` was
// rc 1 at `elf64-x86_64-linux-dyn` and `elf64-aarch64-linux-dyn` and rc 0 at
// every format declaring no preemptible binding set — a shared library
// containing a string literal did not compile, while the whole suite was green.
//
// ★ SO THE ASSERTION THAT MATTERS HERE IS THAT THE LIBRARY BUILDS AT ALL, on a
// format that DECLARES a preemptible binding set. Every `dependsOn` arm below is
// a `-dyn`/`-dll`/`-dylib` build of this file, and the two ELF `-dyn` arms are
// the ones that go RED the moment the name precondition in
// `symbolIsPreemptibleDefinition` is removed. The exit-code half then proves the
// three synthesized kinds were materialized CORRECTLY rather than merely
// accepted — a refusal is loud, but a literal pointing at the wrong bytes is
// not.
//
// THE THREE SYNTHESIZED DEFINITION KINDS, one exported entry point each:
//   * a STRING LITERAL       -> the literal pool global (`Global`/`Default`, no name)
//   * `__func__`             -> the same pool, minted from a different producer
//   * a FLOATING CONSTANT    -> the promoted float global (same binding pair)
// and, because the two halves interact, a static table holding the address of an
// EXPORTED definition beside a `static` sibling as the in-object control.

// The string-literal form. Both references compile this into a bare
// pc-relative address of a module-private pool object; DSS now does too.
const char *dss_greet(void) { return "hello, world"; }

// `__func__` reaches the same pool through a different producer, so a fix that
// only taught the string-literal path would still refuse here.
const char *dss_where(void) { return __func__; }

// The FLOATING CONSTANT form. `1.5` and `1000.0` are promoted to pool globals
// exactly as the string is. The parameter is deliberately an `int` and the
// return an `int` — the floats stay INSIDE the library, so this asserts the
// synthesized float definitions rather than a double-across-the-image-boundary
// calling convention, which is a different subject with its own coverage. It is
// also why the argument is not a constant: a folded expression would delete the
// very definitions this example exists to place.
int dss_scaled_times_1000(int x) {
    double const d = (double)x;
    double const r = -d * 1.5;
    return (int)(r * 1000.0);
}

// The in-object CONTROL for the table below: a `static` definition is in no
// image's dynamic export set, so no loader can replace it under any format.
static const char *dss_private(void) { return "private"; }

typedef const char *(*Str0)(void);

// A STATIC INITIALIZER holding the address of an EXPORTED (and, on the ELF dyn
// documents, preemptible) definition beside the `static` control. On a format
// declaring a preemptible binding set the first entry must be resolved by the
// loader and the second must stay prelinked — and C 6.2.2p2 requires the
// comparison below to hold whichever image wins, because one identifier names
// one function across the whole program.
static Str0 const dss_table[2] = { dss_greet, dss_private };

// 1 iff the library's own view of `&dss_greet` agrees with its table entry.
int dss_table_matches_greet(void) { return dss_table[0] == dss_greet ? 1 : 0; }

// 1 iff the `static` control still resolves to its own body — the arm that
// makes the line above mean "preemptible definitions are routed" rather than
// "every address in this library happens to agree".
int dss_table_matches_private(void) { return dss_table[1] == dss_private ? 1 : 0; }
