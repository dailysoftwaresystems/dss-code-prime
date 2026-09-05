// D-MIR-CALLCTX-REFERENCE-HELD-ACROSS-A-NESTED-LOWERING-DUPLICATES-THE-ARGUMENT
// (P61, lane ml): the RUNNABLE witness that a BY-VALUE AGGREGATE ARGUMENT is
// evaluated EXACTLY ONCE -- C 6.5.2.2p10.
//
// *** WHY THIS FILE EXISTS, AND WHY THE CORPUS ALREADY HERE COULD NOT SEE IT.
// A by-value aggregate argument is materialized by `emitByValueStructCallArg`
// in `src/mir/lowering/hir_to_mir.cpp`, which calls `lowerLvalueAddress(arg)`
// FIRST and pushes the resulting call operands AFTER. When the argument is
// itself an aggregate-returning CALL, that lowering re-enters the expression
// driver and grows the driver's `callCtxs` accumulator. P61 hoisted `callCtxs`
// from a driver LOCAL to a MEMBER of the one shared driver, so the growth could
// REALLOCATE it -- and the per-call context reference the helpers held across
// the lowering dangled. The argument index never advanced in the LIVE context,
// the argument pump re-entered on the SAME argument, and the argument
// expression was LOWERED TWICE. Nothing was refused. The program compiled,
// linked and ran; its only symptom was a side effect running one time too many.
// MEASURED at that state through `dsscp`: `op(mk())` with a counting `mk`
// exited 5 where gcc 13.3.0 (`-std=c2x -O0`) exited 4.
//
// The by-value-struct-argument shapes already in this corpus -- `op(mk(3u,0u))`
// in `examples/c/c_int128_float_conv` among them -- pass PURE launderers. Two
// evaluations of a pure argument give the identical value and the identical
// exit code, so those examples are green in BOTH directions. Only an argument
// with an OBSERVABLE SIDE EFFECT discriminates, and that is the whole design of
// this file: every argument below is a call that INCREMENTS A GLOBAL COUNTER,
// and every term asserts the counter reads exactly 1.
//
// *** THREE AGGREGATE SHAPES, BECAUSE THE ABI TAKES THREE DIFFERENT ROUTES and
// a fix to one is not a fix to the others:
//   n1: a 16-byte struct -- InRegisters (two GPR eightbyte pieces) under SysV,
//       ByReference under Win64, so one source reaches both `appendByValueArg`
//       branches across the shipped targets.
//   n2: a 32-byte struct -- MEMORY class on every shipped target, so it takes
//       the by-reference / stack-carrier arm rather than the piece-load arm.
//   n3: `unsigned __int128` -- the WIDE INTEGER route into the same
//       `isByValueClass` classification (D-CSUBSET-BITINT-C2-WIDE), which is a
//       different arm again and would be a separate defect if only one is fixed.
//
// *** AND ONE SCALAR CONTROL, n4. Identical nesting, scalar argument: it reaches
// the call frame through the `ScalarPending` path and never enters the by-value
// synthesis at all, so it was correct before the fix and after it. If the
// control term ever fails, the fixture broke -- not the rule the other three
// terms state. Without it, "the example went red" would be equally consistent
// with "calls are broken generally".
//
// ANTI-FOLD: every counter is a mutable global, and every value derives from the
// mutable global `gseed`, so no front-end arm can pre-compute the answer and
// report green without having lowered the calls.
//
// *** WHICH TERM ACTUALLY DISCRIMINATES, MEASURED RATHER THAN ASSUMED -- and it
// is ONE of the three, not all three. Reverting the fix and running this file
// gives exit 32, i.e. exactly one ten-point term flips. The reason is the shape
// of the accumulator: a `std::vector`'s capacity only GROWS, so the FIRST nested
// by-value argument in a translation unit is the one whose push reallocates;
// after it the vector already has room and the reference survives. (MEASURED
// separately on a two-function file: `op(mk())` alone exited 5 against gcc's 4.)
// So the three aggregate shapes here are NOT three independent detectors of this
// defect -- they are three ABI ROUTES held against future regressions, and only
// whichever runs first detects THIS one. That is also exactly why the defect was
// invisible: any by-value struct call earlier in the file hides it.
//
// (i) AND WHAT THE `release` ARM DOES AND DOES NOT ADD, MEASURED. The runner
// compares an optimized arm against the BASELINE's actual exit code, not against
// 42 -- with the fix reverted the release arm reported PASS at exit 32 while the
// baseline arm reported FAIL. So the release arm pins that the shipped pipeline
// AGREES with the baseline; it is not a second, independent witness of the
// value. The baseline arm and the MIR-tier pin are the discriminating ones.
//
// REFERENCE ORACLE, each probed SEPARATELY on this exact file: gcc 13.3.0 (WSL)
// and clang 18.1.3 compile and RUN it to 42 at -O0 and -O2; mingw-w64 gcc 13.2.0
// compiles and RUNS it to 42 at -O0 and -O2. All accept it, so it is a legal
// program DSS must accept and must give this answer for.
//
// exit = 10 + 10 + 10 + 6 + 6 = 42.

unsigned long long gseed = 1u;   /* mutable: nothing here folds */

/* n1 -- 16 bytes: InRegisters (SysV) / ByReference (Win64). */
struct Pair { unsigned long long lo; unsigned long long hi; };
int n1 = 0;
static struct Pair mk1(void)
{
    n1 += 1;
    struct Pair p;
    p.lo = gseed;
    p.hi = 2u;
    return p;
}
static unsigned long long op1(struct Pair v) { return v.lo + v.hi; }

/* n2 -- 32 bytes: MEMORY class on every shipped target. */
struct Quad { unsigned long long a; unsigned long long b;
              unsigned long long c; unsigned long long d; };
int n2 = 0;
static struct Quad mk2(void)
{
    n2 += 1;
    struct Quad q;
    q.a = gseed;
    q.b = 2u;
    q.c = 3u;
    q.d = 4u;
    return q;
}
static unsigned long long op2(struct Quad v) { return v.a + v.b + v.c + v.d; }

/* n3 -- the wide-integer route into the same by-value classification. */
int n3 = 0;
static unsigned __int128 mk3(void)
{
    n3 += 1;
    return ((unsigned __int128)gseed << 64) | (unsigned __int128)5u;
}
static unsigned long long op3(unsigned __int128 v)
{
    return (unsigned long long)(v >> 64) + (unsigned long long)v;
}

/* n4 -- the SCALAR CONTROL. Never enters the by-value synthesis. */
int n4 = 0;
static unsigned long long mk4(void) { n4 += 1; return gseed + 6u; }
static unsigned long long op4(unsigned long long v) { return v + 1u; }

int main(void)
{
    int rc = 0;

    unsigned long long const v1 = op1(mk1());
    unsigned long long const v2 = op2(mk2());
    unsigned long long const v3 = op3(mk3());
    unsigned long long const v4 = op4(mk4());

    if (n1 == 1) rc += 10;   /* 16-byte struct arg: evaluated ONCE */
    if (n2 == 1) rc += 10;   /* 32-byte struct arg: evaluated ONCE */
    if (n3 == 1) rc += 10;   /* __int128 arg:       evaluated ONCE */
    if (n4 == 1) rc += 6;    /* CONTROL: scalar arg, evaluated ONCE */

    /* and the VALUES are right, not merely the counts -- a "fix" that dropped
       the second materialization's operands instead of the second EVALUATION
       would satisfy the counters and corrupt the arguments. */
    if (v1 == 3u && v2 == 10u && v3 == 6u && v4 == 8u) rc += 6;

    return rc;
}
