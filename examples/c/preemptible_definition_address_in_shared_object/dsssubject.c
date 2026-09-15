/* [[D-MIR-DYLIB-SELF-CALL-BYPASSES-WEAK-COALESCING]], the ADDRESS half -- THE
 * LIBRARY, and the corpus coverage that did not exist for it on any format.
 *
 * THE SHAPE THE ROW IS ABOUT LIVES IN A SHARED OBJECT, not in an executable.
 * `preemptible_definition_address_control` already pins the EXECUTABLE
 * direction on all four legs (an exec is always its own winner, so every
 * address there must stay direct). Nothing in `examples/` built a SHARED
 * OBJECT that takes the address of its own preemptible definition -- so the
 * routing this row installs, and the walker paths that realize it, had no
 * corpus arm at all. This file is that arm, and it carries BOTH forms of the
 * address in ONE translation unit:
 *
 *   (1) THE RUNTIME FORM  -- `&w` taken inside a function. Under a
 *       `direct-plt` dispatch the reference's own VA is a call thunk, so the
 *       lowering mints a SECOND symbol naming the slot that holds the
 *       loader-resolved address, and the format walker binds it to the slot it
 *       already mints for that reference (an ELF `.got` entry, a Mach-O
 *       `__DATA_CONST __got` one).
 *
 *   (2) THE STATIC-INITIALIZER FORM -- `int (*p_w)(void) = w;` at FILE SCOPE.
 *       There the address is not lowered at all: it is a data-item relocation,
 *       and leaving it BAKED gives one identifier two addresses INSIDE ONE
 *       IMAGE -- `p_w` would name this library's own body while `&w` named the
 *       loader's winner. That is what `self_consistent()` below asserts, and it
 *       needs no rival image to do it.
 *
 * WHY 42 ON EVERY LEG THOUGH THE ROUTING DIFFERS PER FORMAT. The declared set
 * differs -- `["global","weak"]` on the two `elf64-*-linux-dyn` documents,
 * `["weak"]` on the two `macho64-*-darwin-dylib` ones, ABSENT on pe64 (Windows
 * has no symbol interposition) -- so `w` is routed on three of the four arms
 * and direct on the fourth, and `st` is direct on all four. The ANSWER is the
 * same either way, and that is the point: what this example asserts is not
 * WHICH mechanism was chosen but that every mechanism yields ONE address for
 * one identifier. A build that routes one form and bakes the other reds here
 * with no reference toolchain in the picture.
 *
 * ⚠⚠ AND THAT CUTS BOTH WAYS -- READ THE GREEN CORRECTLY. pe64 is the one arm
 * of the four that RUNS (the loader finds a DLL beside the exe; the other three
 * would need LD_LIBRARY_PATH / an rpath / an LC_RPATH, which the corpus runners
 * cannot thread -- and no dynamic-library example anywhere in `examples/` runs
 * on anything but pe64, so this is a runner boundary rather than a weakness of
 * this entry). pe64 is ALSO the one arm whose format document declares NO
 * `preemptibleDefinitionBindings`. So on the only arm that executes,
 * `self_consistent()` compares two BAKED addresses and CANNOT FAIL. Its green
 * says the library links, loads and answers 42; it does not say the routing
 * works. Three things do say that, and they are named here so the next reader
 * does not have to re-derive them: the unit case
 * `MachoCoalescingScopeReference.
 * AStaticInitializerNamingAPreemptibleDefinitionIsLoaderResolved`; the darwin
 * hardware run; and a manual ELF witness -- ✔MEASURED 2026-09-07, build these
 * two sources for `x86_64:elf64-x86_64-linux-{dyn,exec}` with
 * `--resolve-library`, and under Linux the exe exits 127 with the `.so` merely
 * beside it (DT_NEEDED is a bare soname and there is no DT_RUNPATH) and exits
 * 42 under `LD_LIBRARY_PATH=.`, on the arm where `w` really is routed.
 * ⚠ Do NOT read "the darwin arm goes red at BUILD" as covering everything: it
 * is true of the ADDRESS-SLOT half (an unbound slot symbol cannot be emitted)
 * and FALSE of the STATIC-INITIALIZER half below -- dropping that fold emits a
 * valid image that is simply WRONG, at rc 0, which no compile-only arm sees.
 *
 * WHY THE BODY IS NOT `return 42;`: `expected.json` declares a `release` arm
 * with `mustDifferFromBaseline: true` on each dependency, which reds unless the
 * optimized LIBRARY really differs from the baseline one. `mix` is an inlinable
 * static helper inside a loop -- work the shipped release pipeline visibly does
 * -- so the difference is real rather than incidental. */

/* (1) WEAK / DEFAULT VISIBILITY -- the subject. Every loader that coalesces
 * anything coalesces this binding, which is why it is the one member both
 * darwin dylib documents declare. */
__attribute__((weak)) int w(void) { return 11; }

/* (2) `static` -- module-private, in no image's dynamic export set, so no
 * loader can replace it under any format. THE IN-OBJECT CONTROL: its address
 * must stay this object's own on every arm, and calling through that pointer
 * must reach this object's body. Without it, "the two forms agree" would be
 * equally consistent with "this build routes every address", which would cost a
 * load through memory to reach a definition nothing can replace. */
static int st(void) { return 7; }

static int mix(int x) { return x * 3 + (x >> 1) - 2; }

/* THE STATIC-INITIALIZER FORM of the very same identifier, at FILE SCOPE. */
int (*p_w)(void) = w;
int (*p_st)(void) = st;

int (*take_w(void))(void)  { return w; }
int (*take_st(void))(void) { return st; }
int (*read_p_w(void))(void)  { return p_w; }
int (*read_p_st(void))(void) { return p_st; }

int lib_mix_sum(int n) {
    int acc = 0;
    for (int i = 0; i < n; ++i) { acc += mix(i) + st(); }
    return acc;
}

/* Runs INSIDE the shared object, so the identity it checks is the one the row
 * protects: one identifier, one function, across every use form in one image.
 * Returns 42 when every property holds, and a DISTINCT code per property
 * otherwise so a failure names itself instead of reporting "not 42". */
int self_consistent(void) {
    int (*runtime_w)(void)  = take_w();
    int (*initial_w)(void)  = read_p_w();
    int (*runtime_st)(void) = take_st();
    int (*initial_st)(void) = read_p_st();

    if (runtime_w == 0)          return 51;
    if (initial_w == 0)          return 52;
    /* THE ASSERTION THIS EXAMPLE EXISTS FOR: the runtime address-take and the
     * file-scope initializer of ONE identifier are ONE pointer. */
    if (runtime_w != initial_w)  return 53;
    if (runtime_w() != 11)       return 54;
    if (initial_w() != 11)       return 55;
    /* The in-object control, both forms again. */
    if (runtime_st == 0)         return 56;
    if (runtime_st != initial_st) return 57;
    if (runtime_st() != 7)       return 58;
    if (runtime_w == runtime_st) return 59;
    /* And the release arm's own subject, so the optimized library is executed
     * rather than merely compared. mix(0..4) = -2,1,5,8,12 -> 24; plus 7 five
     * times -> 59. */
    if (lib_mix_sum(5) != 59)    return 60;
    return 42;
}
