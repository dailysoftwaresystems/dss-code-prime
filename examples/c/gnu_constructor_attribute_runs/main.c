// D-C-GNU-CONSTRUCTOR-ATTRIBUTE-IS-WARNED-AND-IGNORED-NOT-RUN — the runnable
// witness that `__attribute__((constructor))` and `((destructor))` are HONOURED
// rather than accepted and dropped.
//
// ★★ WHAT WAS WRONG. For one cycle DSS parsed the attribute, emitted
// `warning[H_UnknownLinkageSpecifier]: 'constructor' is not a recognized linkage
// specifier`, and threw it away: this exact program exited 0 under DSS and 42
// under mingw-w64 gcc 13.2.0. Accepting the SYNTAX was the correct front-end
// verdict — every reference accepts vocabulary it does not model — but
// `constructor` is vocabulary the references IMPLEMENT, so acceptance was never
// agreement.
//
// ★★ THE EXIT CODE IS BUILT COMMUTATIVELY ON PURPOSE, AND CHANGING THAT WOULD
// PIN A FACT NO REFERENCE AGREES ON. ⚠⚠ ORDER AMONG EQUAL-PRIORITY
// CONSTRUCTORS IS NOT PORTABLE — ✔MEASURED 2026-08-28: two bare same-priority
// constructors run 1,2 under Linux gcc 13.3.0 and clang 18.1.3 and **2,1** under
// mingw-w64 gcc 13.2.0. `bump_a` and `bump_b` below therefore both `+= 10`, so
// either order gives the same total. If you are tempted to "improve" this into a
// sequence assertion, that is the trap: it would assert an order the references
// themselves disagree about.
//
// ★★ WHAT *IS* PORTABLE, AND IS ASSERTED: DISTINCT PRIORITIES ORDER, AND THE BARE
// FORM RUNS LAST. ✔MEASURED, unanimous across gcc 13.3.0 (`-std=c2x`), clang
// 18.1.3 (`-std=c23`) and mingw-w64 gcc 13.2.0, each function printing its own
// tag so stdout order IS run order:
//     c101 c102 cBARE | MAIN | dBARE d102 d101
// So constructors run ASCENDING by priority with the unprioritized form LAST, and
// destructors run that same sequence BACKWARD. `seq` below is the witness for the
// constructor half (it must read 123) and the printed stdout is the witness for
// the destructor half.
//
// ★ THE PRIORITY ARGUMENT IS PART OF THE UNION, NOT AN EXTRA. An implementation
// that parsed the attribute but ignored `(101)` would run this program's
// initializers in an order all three references disagree with, and `seq` would
// not read 123.
//
// ⚠ THE RELEASE ARM IS NOT DECORATION — IT PINS A SEPARATE DEFECT. A static
// initializer is called from NOWHERE in the program text, so module-level dead-
// code elimination sees an unreferenced internal-linkage function and deletes it.
// Every constructor here is `static`. Without the DCE root clause this example
// passes in `debug` (whose pipeline is `Identity`) and fails in `release` — which
// is exactly the split a release arm exists to catch.
//
// ⚠ AND THE DESTRUCTORS ARE OBSERVED THROUGH STDOUT because they run AFTER `main`
// returns: there is no exit code left to influence by then. Each flushes, rather
// than trusting a runtime flush at exit — DSS's entry ends in a raw process-exit
// request, so an unflushed buffer would simply be lost and the assertion would be
// vacuous rather than red.

#include <stdio.h>

static int acc = 0;
static int seq = 0;

/* Distinct priorities: this ordering IS portable and is asserted through `seq`. */
__attribute__((constructor(101))) static void first(void)  { seq = seq * 10 + 1; acc += 20; }
__attribute__((constructor(102))) static void second(void) { seq = seq * 10 + 2; acc += 1; }
/* Bare = unprioritized, which every reference runs AFTER every prioritized one. */
__attribute__((constructor))      static void last(void)   { seq = seq * 10 + 3; acc += 1; }

/* Two bare constructors of EQUAL priority — commutative on purpose (see above). */
__attribute__((constructor)) static void bump_a(void) { acc += 10; }

/* ★★ `bump_b` CALLS SOMETHING, AND THAT IS THE WHOLE REASON IT DOES. The entry
   trampoline materializes argc/argv into the convention's ARGUMENT registers
   before its prologue, and those registers are CALLER-saved — so a constructor
   that CALLS anything destroys them unless the trampoline parks them across the
   call. Pure-arithmetic constructors never touch those registers, so an example
   built only from them cannot see the defect: ✔MEASURED, removing the park left
   this example GREEN until this call was added.
   `fputs("", stdout)` loads the first two argument registers (the string and the
   stream) and writes NOTHING, so the asserted stdout is unchanged. */
__attribute__((constructor)) static void bump_b(void) {
    acc += 10;
    (void)fputs("", stdout);
}

/* The after-entry channel, distinct priorities, walked in reverse. */
__attribute__((destructor(101))) static void bye_101(void) { fputs("d101 ", stdout); fflush(stdout); }
__attribute__((destructor(102))) static void bye_102(void) { fputs("d102 ", stdout); fflush(stdout); }
__attribute__((destructor))      static void bye_bare(void) { fputs("dBARE ", stdout); fflush(stdout); }

/* ★★ `main` TAKES ARGUMENTS ON PURPOSE, AND THAT IS A SECOND DEFECT'S WITNESS.
   The entry trampoline materializes argc/argv into the calling convention's
   ARGUMENT registers BEFORE its prologue (their stack offsets are defined against
   the untouched process-entry SP), and those registers are CALLER-saved. The
   before-entry initializer calls sit between that materialization and the call to
   `main` — so unless the trampoline parks the values in callee-saved registers
   across them, `main` receives whatever the last constructor left behind. A
   `main(void)` cannot see that at all, which is exactly why the first draft of
   this example missed it.
   The checks mirror `examples/c/wmain_argc`: this runner invokes the artifact
   with no extra arguments, so argc == 1 and argv[0] is the program path. Each
   failure has its OWN exit code so a clobber reds with a name. */
int main(int argc, char **argv) {
    /* 20 + 1 + 1 + 10 + 10 == 42, and only if every constructor ran exactly once.
       `seq != 123` means the priority ordering was not honoured, which is a
       DIFFERENT defect from "nothing ran" — so it gets its own exit code rather
       than folding into the 42/0 answer. */
    if (seq != 123) return 7;
    if (argc != 1) return 9;
    if (!argv) return 10;
    if (!argv[0]) return 11;
    if (!argv[0][0]) return 12;
    return acc;
}
