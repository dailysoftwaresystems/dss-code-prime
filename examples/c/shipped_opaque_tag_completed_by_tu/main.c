/* [[D-FFI-OPAQUE-TAG-HAS-NO-SPELLING]] — the runnable witness that a translation
 * unit may COMPLETE a struct tag a shipped descriptor declares OPAQUE.
 *
 * This is ordinary C and it is the entire point of an opaque handle: the header
 * says "there is a type called DIR, you may only hold pointers to it", and a TU
 * that knows better — a private shim, a test double, a reimplementation — may
 * supply its own definition. Every reference compiler accepts it. DSS used to
 * refuse it with `F_ShippedTypeIdentityConflict`, because the descriptor
 * vocabulary had no spelling for "opaque" and an EMPTY NAMED STRUCT was standing
 * in for one — and an empty named struct is a COMPLETE zero-byte type, so the
 * engine compared a 0-byte layout against the TU's and called it a conflict.
 *
 * ★ WHY BOTH TAGS ARE HERE. `DIR` is completed (the shape a windirent-style shim
 * uses) and `FILE` is left opaque and used only through a pointer (the shape
 * every ordinary program uses). One file, both halves: a fix that accepted the
 * completion but broke the pointer — or vice versa — passes neither.
 *
 * ⚠ WHAT THIS FILE DOES NOT CLAIM. It does not assert that the shipped `DIR` and
 * this `struct DIR` are the same object at runtime; nothing here calls opendir.
 * The property under test is a TYPE-SYSTEM one — that the completion is accepted
 * and the completed members are addressable at the offsets this TU declared —
 * so the arithmetic below reads members through the completed type only.
 *
 * ⚠ `volatile` on the seed keeps the release arm honest: without it the whole
 * computation folds to a constant and the optimized build stops exercising the
 * member offsets it is here to check.
 */

#include <dirent.h>
#include <stdio.h>

/* Completing the tag the descriptor declares opaque. Field layout is this TU's
 * business now; nothing shipped depends on it, which is exactly why completing
 * it is sound. */
struct DIR {
    int  marker;
    char tag[8];
};

extern int printf(const char *, ...);

int main(void) {
    volatile int seed = 1;
    int total = 0;

    /* The completed tag is a real, usable object type. */
    struct DIR d;
    d.marker = 10 * seed;
    d.tag[0] = 'x';
    d.tag[7] = 'z';
    total += d.marker;                                   /* 10 */
    total += (d.tag[0] == 'x') ? 6 : 0;                  /*  6 */
    total += (d.tag[7] == 'z') ? 6 : 0;                  /*  6 */

    /* A pointer to the completed tag behaves as an ordinary object pointer. */
    struct DIR *viaTag = &d;
    total += (viaTag == &d) ? 10 : 0;                    /* 10 */
    total += (viaTag->marker == 10) ? 5 : 0;             /*  5 */

    /* ⛔ DELIBERATELY ABSENT, and the omission is the interesting part:
     *      DIR *viaTypedef = &d;   total += (viaTypedef == viaTag);
     * That would assert the descriptor's `DIR` typedef names THIS completed tag.
     * ✔MEASURED and it is NOT a property the references have — on mingw the same
     * two lines are `warning: initialization of 'DIR *' from incompatible pointer
     * type 'struct DIR *'`, because mingw's `DIR` is a typedef to its OWN struct
     * and glibc's is `typedef struct __dirstream DIR`. In both, a user's
     * `struct DIR` is an UNRELATED type. Asserting the equality would have made
     * this example pin a DSS-only behaviour and call it conformance.
     * ⓘ That divergence is real and is recorded separately — DSS names the opaque
     * tag `DIR`, where every reference uses a reserved-namespace tag the user is
     * not expected to define, so completing `struct DIR` reaches the shipped
     * handle here and reaches nothing there. See
     * [[D-FFI-OPAQUE-TAG-USES-A-USER-REACHABLE-NAME]]. */

    /* The other half: a tag left OPAQUE is still perfectly usable through a
     * pointer, which must not have regressed while making completion legal. */
    FILE *stream = 0;
    total += (stream == 0) ? 5 : 0;                      /*  5 */

    /* 10 + 6 + 6 + 10 + 5 + 5 == 42 */
    if (total != 42) {
        printf("opaque-tag: expected 42, got %d\n", total);
        return 1;
    }
    printf("opaque-tag-completed\n");
    return total;
}
