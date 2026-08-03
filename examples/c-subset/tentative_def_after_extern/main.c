// D-CSUBSET-TENTATIVE-DEFINITION-AFTER-EXTERN-DECL witness: a file-scope TENTATIVE
// definition (no initializer, no `extern`) that FOLLOWS an `extern` declaration of
// the same name must still emit ZERO-INITIALIZED storage (C 6.9.2p2 — a prior
// `extern` declaration does not stop a later no-initializer declaration from being
// a tentative definition). Before the fix, the extern "survived" the merge and the
// whole TU emitted an IMPORT and no storage, so the symbol was undefined and the
// link failed — the extern-in-a-header-then-define-in-a-.c pattern, which blocked
// essentially every multi-file C program (the sqlite amalgamation MASKED it; the
// full-source build's `undefined reference to sqlite3BuiltinFunctions` exposed it).
//
// This is distinct from `extern_definition_merge`, which pairs the extern with an
// INITIALIZED definition (`extern int g; int g = 30;` — a REAL definition, rank
// 1→3, which already worked). The bug was specifically the TENTATIVE definition
// (rank 1→2): no initializer, so nothing forced the definition to win the binding.
//
// Covers the sqlite shape — a STRUCT global and an INT global, each declared
// `extern` first (as a header would) then defined tentatively (as the .c does):
//   gp = {0,0} + accumulate 20,21 ; gi = 0 + accumulate 1  =>  20+21+1 = 42.
// Zero-initialization is OBSERVABLE (the accumulate reads the zero first) and the
// storage is REQUIRED (a missing definition fails to link, not just misbehaves).
//
// RED-ON-DISABLE: revert the defining-rank comparison in mergeOrCollideRedeclaration
// (back to keeping the prior binding whenever the new decl is non-defining) -> the
// extern survives -> an import row + no storage -> the link fails -> the runner
// reports a build/run failure instead of exit 42.
//
// Front-end feature (semantic merge + HIR tentative-global emission), target- and
// format-agnostic: x86_64 (PE + ELF) and arm64 (ELF under qemu, Mach-O macos leg).

typedef struct { int a; int b; } Pair;

extern Pair gp;                 // extern STRUCT declaration (as a header would give)
extern int  gi;                 // extern INT declaration

Pair gp;                        // TENTATIVE definition — zero-init storage, required
int  gi;                        // TENTATIVE definition — zero-init storage, required

int main(void) {
    gp.a += 20;                 // reads the zero-init struct field, then accumulates
    gp.b += 21;
    gi   += 1;                  // reads the zero-init int, then accumulates
    return gp.a + gp.b + gi;    // 20 + 21 + 1 = 42
}
