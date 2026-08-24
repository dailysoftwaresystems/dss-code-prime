/* Cluster G — shipped <assert.h> macro injection (the FIRST consumer of the
 * descriptor `macros` surface). `assert` is a FUNCTION-LIKE MACRO, not a typed
 * symbol — when the preprocessor resolves `#include <assert.h>`, it splices a
 * synthetic `#define assert(e) ((void)0)` into the synth buffer (from
 * shippedLibs/assert.json), so assert(expr) expands BEFORE parse.
 *
 * The assert here is FALSE (x == 99 with x == 42). A real (active) assert would
 * ABORT; the no-op `((void)0)` evaluates NOTHING (C 7.2p1 NDEBUG behavior — `e`
 * is not even evaluated) and control flow continues unchanged, so the program
 * returns 42. exit 42 is the witness that (a) `#include <assert.h>` resolved to
 * the descriptor, (b) `assert` was injected as a function-like macro, (c) its
 * replacement tokens are span-valid in the final synth buffer, and (d) the
 * expansion is a true no-op (a false assert does not alter the result).
 *
 * RED-ON-DISABLE: without the macro injection, `assert` is an undeclared
 * identifier → the compile fails (no binary). */
#include <assert.h>

int main(void) {
    int x = 42;
    assert(x == 99);   /* FALSE — a no-op; an active assert would abort */
    return x;          /* 42 */
}
