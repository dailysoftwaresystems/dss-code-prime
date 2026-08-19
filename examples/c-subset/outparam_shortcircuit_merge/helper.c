/* Cycle P14. The cross-CU shape that `test_stmt_scanstatus` (sqlite's
 * test/test1.c) is built out of, reduced to two TUs and pinned here as a
 * POSITIVE guard — NOT a defect pin. ★ SAY THIS PLAINLY, because the file it
 * came from was written believing the opposite: P13/P14 read the pe64 corpus
 * abort at `scanstatus2-5.1` as a DSS miscompile on exactly this construct,
 * and ✔MEASURED 2026-08-19 it is not one. Every arm below exits 42 on DSS
 * (debug AND release) and on gcc (-O0 AND -O2); the real cause of that abort
 * is upstream and lives in `D-HARNESS-PE64-CORPUS-WINE-ABORT-SCANSTATUS2`.
 *
 * What this example is FOR, then, is the lowering shape itself, which is worth
 * a machine-checked witness on every leg and had none:
 *   - a local whose ADDRESS IS TAKEN and is written only through a pointer
 *     parameter of a call in the LEFT arm of a short-circuit `a || b`;
 *   - the value then read back and passed as ARGUMENT 1 of a 5-ARGUMENT call
 *     — on pe64 that puts arg5 in the shadow-space slot at [rsp+0x20] while
 *     args 1-4 ride rcx/rdx/r8/r9, which is the arm no 4-arg example covers;
 *   - across TWO TUs, so the build routes through the N>=2 whole-program MIR
 *     merge (`mir/merge/mir_merge.hpp`: the byte-identical single-CU path is
 *     kept for N==1) — a one-source spelling would silently not test it.
 * ⚠ DO NOT COLLAPSE `sources` TO ONE FILE, and do not drop an argument from
 * `consumer`: either edit keeps the example green while retiring the only two
 * properties it exists to hold.
 *
 * This TU owns the helpers; main.c owns the caller and main, so the calls
 * between them are cross-CU direct calls exactly as test1.c's are. */
struct V { int pad[34]; struct B *link; };   /* link at +0x88, the Vdbe::aOp shape */
struct B { long tag; };

static struct B b = { 42 };
static struct V a = { {0}, &b };

/* testStringToPointer's shape: a lookup that can legitimately return 0. */
void *registry_lookup(const char *s) {
    return s[0] != '0' ? (void *)&a : 0;
}

/* getStmtPointer's shape: the out-param write that the `||` left arm hides. */
int get_it(void *ctx, const char *s, void **out) {
    (void)ctx;
    *out = registry_lookup(s);
    return 0;
}

int getint(int *out) { *out = 3; return 0; }

/* The 5-argument callee: on pe64 arg5 arrives through the stack slot. */
int consumer(struct V *p, int i, int op, int flags, void *o) {
    (void)i; (void)op; (void)flags; (void)o;
    return p->link->tag == 42 ? 0 : 1;
}
