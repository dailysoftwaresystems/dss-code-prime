// TF-C89 (D-CSUBSET-SHIPPED-TYPEDEF-POSITION-BLIND-SUPPRESSION)
// — the TYPEDEF sibling of `bare_proto_shipped_redecl`, and the shape
// that was silently deleting SQLite's own integer vocabulary on macOS.
//
// A TU BOTH `#include <stdint.h>` (whose shipped descriptor injects `int16_t`)
// AND, LOWER DOWN, redeclares that very name the way a platform header does:
// `typedef short int16_t;` — byte-for-byte what the macOS SDK's
// `<sys/_types/_int16_t.h>` contributes, and legal C11 (6.7p3: a typedef name
// may be redefined to denote the same type it already denotes).
//
// The USE that matters is ABOVE the redeclaration. Goal-2's "a user declaration
// of the name wins" skip used to key on a WHOLE-TU, POSITION-BLIND name set, so
// the LOWER redeclaration deleted the shipped `int16_t` for the WHOLE unit —
// while the user's own typedef is POSITION-SENSITIVE. Everything above it then
// resolved to NOTHING, even though C 6.2.1p7 ("the scope of an identifier
// declared by a declarator begins just after the completion of its declarator")
// puts the ENCLOSING declaration in scope for exactly that region. In SQLite
// that killed `i16`, `i8`, `LogEst` and `ynVar` — 80 of the 123 `S0006` on the
// arm64 macho corpus leg.
//
// WHY THIS IS RUNTIME-OBSERVABLE, not just "it compiles": the wrong repair
// would be to let the type resolve to SOMETHING and move on. So the aliases on
// BOTH sides of the redeclaration are witnessed at run time to be exactly 16
// bits WIDE and exactly SIGNED: a `volatile` seed of 40000 crosses the cast at
// run time, and 40000 truncated into 16 signed bits is -25536. A resolution
// that landed on int (or on an unsigned 16-bit type) changes that number and
// flips the exit code. `sizeof` pins the width from the compile-time side too,
// so the two faces of the same fact must agree.
//
// CLANG GROUND TRUTH (MEASURED 2026-07-29, arm64-darwin host):
//   clang -std=c11 -Wall -Wextra -isysroot $(xcrun --show-sdk-path) main.c
//   is CLEAN (rc 0, no warnings) and the native binary prints
//   `est16-both-sides` and exits 42 — byte-for-byte what DSS produces here.
//
// RED-ON-DISABLE (either arm of the fix, verified during the cycle):
//   * restore the typedef injection's goal-2 skip to `userDeclaredNames` —
//     the shipped `int16_t` is deleted, `typedef int16_t Est;` fails
//     error[S0006] "got int16_t", no artifact;
//   * restore `resolveTypeNodeImpl`'s alias walk to plain `ScopeTree::lookup` —
//     the not-yet-typed user binding stops the walk before the shipped
//     declaration and the SAME error[S0006] fires.

#include <stdint.h>
#include <stdio.h>

// The USE, ABOVE the redeclaration — this is the line that used to die.
typedef int16_t Est;

// The platform's own declaration of the same name, LOWER in the same TU.
typedef short int16_t;

// The name must keep resolving BELOW the redeclaration too (there the user's
// own declaration is the one in scope) — both sides, one contract.
typedef int16_t EstBelow;

static volatile int seed = 40000;

int main(void) {
    Est      above = (Est)seed;
    EstBelow below = (EstBelow)seed;
    // Width, from the compile-time face.
    if (sizeof(Est) != 2u)      { puts("width-above-wrong"); return 1; }
    if (sizeof(EstBelow) != 2u) { puts("width-below-wrong"); return 2; }
    // Width AND signedness, from the runtime face: 40000 in 16 signed bits.
    if (above != -25536)        { puts("wrap-above-wrong");  return 3; }
    if (below != -25536)        { puts("wrap-below-wrong");  return 4; }
    puts("est16-both-sides");
    return 42;
}
