// The sibling TU. Every declaration here wears a KEYWORD as an attribute clause
// name, and every one of them must be INERT — the attribute is warned about and
// ignored, exactly as gcc and clang do.

// If this conferred internal linkage, `sharedCounter` would leave the object and
// main.c could not bind its `extern`. Both references keep it exported.
__attribute__((static)) int sharedCounter = 21;

// The dunder disguise of the same thing. `__static__` never reached the keyword's
// entry even before the fix (this scan does not dunder-normalize, unlike its HIR
// twin), so this declarator is the CONTROL for the one above: both must behave
// identically, and if a future change adds normalization here without the
// position skip, this is the arm that moves.
__attribute__((__static__)) int alsoExported = 0;

void bumpTwice(void) {
    // If the attribute conferred STATIC STORAGE this local would be promoted to
    // a hidden module global and keep its value across calls; it must not. It is
    // an ordinary automatic, freshly zeroed on entry, so each call adds exactly
    // 2 to the shared object and never accumulates in the local.
    __attribute__((static)) int step = 0;
    step = step + 2;
    sharedCounter = sharedCounter + step;
    (void)alsoExported;
}
