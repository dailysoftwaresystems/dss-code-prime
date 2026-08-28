// FC16 (D-CSUBSET-NORETURN): C11/C23 `noreturn` function-attribute SEMANTICS.
//
//  * `die`'s PROTOTYPE alone spells `_Noreturn`; the DEFINITION does not repeat
//    it — so the noreturn attribute must OR-MERGE from the proto into the
//    definition (the call in `compute` resolves to the definition). `die`
//    tail-calls the shipped `exit`, which stdlib.json marks noreturn — so this
//    ALSO witnesses shipped-library noreturn detection (the exit call in die's
//    body is wrapped too).
//  * `compute` is a NON-void function whose ONLY non-return path calls `die(N)`:
//    without noreturn semantics the fall-through would let `compute` reach its
//    closing `}` without returning -> H_VerifierFailure (no binary). The
//    synthesized `Unreachable` after the noreturn call makes that path
//    structurally terminate, so `compute` verifies clean.
//  * `main` returns `compute(0)`: 0 <= 100 -> die(42) -> exit(42) -> the process
//    exits 42.
//
// RED-ON-DISABLE (revert detection / OR-merge / the HIR wrap): `compute` fails
// non-void return completeness and no binary is produced.
//
// The `release` arm routes the reachable synthetic `Unreachable` through the
// shipped MIR optimizer (Mem2Reg/CSE/LICM/SimplifyCfg/Dce) — the required safety
// net for the first optimizer-visible `Unreachable` that is NOT behind an
// infinite loop.
#include <stdlib.h>

_Noreturn void die(int code);

void die(int code) {
    exit(code);
}

int compute(int x) {
    if (x > 100) {
        return x;
    }
    die(42);
}

int main() {
    return compute(0);
}
