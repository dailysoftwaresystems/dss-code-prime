/* TF-C96 D-CSUBSET-PARAM-FN-TYPE-ADJUSTMENT witness (C 6.7.6.3p8): "A declaration of a parameter as 'function returning type' shall be adjusted to 'pointer to function returning type'."
** The FUNCTION twin of c82's D-CSUBSET-PARAM-ARRAY-ADJUSTMENT (C 6.7.6.3p7) —
** same chokepoint, same two resolution sites, different source type. It is what
** lets Darwin's BARE FUNCTION TYPEDEFS be used as parameters at all:
**     typedef int reader_t(int);     ... a function TYPE, not a pointer
**     void f(reader_t r);            ... r is int (*)(int) after adjustment
** Without the adjustment the parameter declarator is minted as a function
** PROTOTYPE in a position that admits none, and the TU dies at the declaration
** with S0018 — the same failure shape `D-CSUBSET-FN-TYPEDEF-PROTOTYPE` fixed for
** the file-scope `static Fn apply;` spelling, one scope down.
**
** THE REAL SITES, measured in the macOS SDK: `malloc/malloc.h:537,546,550`
** declare `memory_reader_t`, `vm_range_recorder_t` and `print_task_printer_t` as
** FUNCTION typedefs (not pointer typedefs), and `malloc_introspection_t`'s
** `enumerator` field at `malloc.h:553` takes two of them BARE as parameters —
** `(…, memory_reader_t reader, vm_range_recorder_t recorder)`. That is 6 of
** sqlite `mem1.c`'s residual diagnostics, all S0018, from this one rule.
**
** A COMPILE-ONLY witness would prove nothing here: the whole risk is that the
** adjusted parameter holds a real code address and that the CALL through it lands
** on the right function. So every form below is actually DISPATCHED at runtime:
**   * `viaTypedef(reader_t r, …)` — a parameter typed by the BARE FUNCTION
**     TYPEDEF, called through as `r(v)`;
**   * `viaInline(int g(int), …)`  — a parameter with an INLINE function type,
**     called through as `g(v)`; its body also pins that the parameter IS a
**     pointer (`sizeof(g) == sizeof(int *)` — an UN-adjusted function-typed `g`
**     has no sizeof at all, C 6.5.3.4p1);
**   * every one of the three is declared by a PROTOTYPE above main and DEFINED
**     below it, so the adjustment must mint the SAME FnSig at the prototype and
**     at the definition — an asymmetry makes the two declarations incompatible
**     (S0022 / S0003) and the TU fails to COMPILE, it does not merely mis-run;
**   * the ARGUMENT side both ways: a plain function NAME (which decays, C
**     6.3.2.1p4) and an explicit `&func`, into the SAME `reader_t` parameter slot;
**   * `spin` passes its own adjusted parameter ONWARD into another function-typed
**     parameter — the adjusted value must satisfy an adjusted param.
**
** VALUE-DIVERGENT, and the two callees are deliberately distinguishable at every
** input used, so a dispatch that lands on the wrong one moves the exit code:
**   viaTypedef(addSeven, g_x=5) = 12 ; viaInline(twice, g_y=6)   = 12
**   viaTypedef(&twice,   g_x=5) = 10 ; spin(&addSeven, g_n=2, g_z=1) = 8
**   12 + 12 + 10 + 8 = 42.  (mis-dispatching arm 3 to addSeven -> 44; arm 2 to
**   addSeven -> 43; arm 1 to twice -> 40.)
**
** ANTI-FOLD: `viaTypedef` is invoked with TWO DIFFERENT callees, so it cannot be
** specialized to one; every operand arrives from a MUTABLE GLOBAL (runtime-opaque
** — the c11_atomic device); and `spin` is SELF-RECURSIVE, its own SCC, so the
** inliner refuses it and `r(v)` stays a genuine indirect call through a register
** in the baseline AND the release arm. Nothing here folds to 42 without the
** parameter actually holding, and the call actually reaching, the right function.
**
** RED-ON-DISABLE: revert the FnSig arm in the parameter-adjustment helper (the
** ONE chokepoint every parameter path routes through, the same one that already
** implements the p7 array adjustment) -> the function-typed parameter
** declarators are no longer rewritten to Ptr<FnSig> and each is minted a
** prototype -> `S0018 function prototype declarations are not supported here` ->
** the example no longer COMPILES (a compile failure, not a wrong exit). NOTE the
** fix belongs at that chokepoint and NOT at the S0018 rejection site: suppressing
** the error there would leave the symbol typed FnSig — a silent miscompile, which
** is exactly what the runtime dispatch below would catch.
**
** Front-end feature (semantic adjustment): source-, target- and format-agnostic,
** so all four shipped targets run it — arm64 ELF under qemu, Mach-O on the
** macos-latest leg — and the release arm re-witnesses it under the optimizer.
** gcc/clang -std=c17 cross-checked: the same source exits 42.
*/

typedef int reader_t(int);        /* a function TYPE typedef (NOT a pointer) */

/* The two callees the dispatch has to tell apart. */
static int addSeven(int x) { return x + 7; }
static int twice(int x)    { return x * 2; }

/* Mutable globals = runtime-opaque operands (defeats folding the round-trips). */
int g_x = 5;
int g_y = 6;
int g_z = 1;
int g_n = 2;

/* PROTOTYPES — each parameter carries a FUNCTION type. The definitions are
** below main, so the prototype is the ONLY declaration main type-checks its
** arguments against; prototype and definition must adjust IDENTICALLY. */
static int viaTypedef(reader_t r, int v);
static int viaInline(int g(int), int v);
static int spin(reader_t r, int n, int v);

int main(void) {
    int a = viaTypedef(addSeven, g_x);   /* bare NAME decays -> addSeven(5) = 12 */
    int b = viaInline(twice, g_y);       /* bare NAME, inline fn-type param -> 12 */
    int c = viaTypedef(&twice, g_x);     /* explicit &func, SAME slot  -> twice(5) = 10 */
    int d = spin(&addSeven, g_n, g_z);   /* param passed ONWARD -> addSeven(1) = 8 */
    return a + b + c + d;                /* 12 + 12 + 10 + 8 = 42 */
}

/* The bare-function-TYPEDEF parameter, called through. */
static int viaTypedef(reader_t r, int v) {
    return r(v);
}

/* The INLINE function-type parameter, called through — plus the structural pin
** that the parameter is a POINTER. Every shipped target is LP64/LLP64 and its
** function pointers are the same width as its object pointers (the identity
** `D-LANG-VOIDPTR-FN-CONVERT` already relies on), so `sizeof(int *)` is the
** portable spelling of the expected width. */
static int viaInline(int g(int), int v) {
    if (sizeof(g) != sizeof(int *)) return 900;
    return g(v);
}

/* Self-recursive (its own SCC -> never inlined), and it hands its OWN adjusted
** parameter to another function-typed parameter: the call-site half of the
** adjustment, applied to an already-adjusted value. */
static int spin(reader_t r, int n, int v) {
    if (n) { return spin(r, n - 1, v); }
    return r(v);
}
