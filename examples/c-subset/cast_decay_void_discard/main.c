// FC3.5 sweep-c3 runtime witness for TWO cast residuals:
//
//   * D-CSUBSET-CAST-ARRAY-DECAY — `(char*)"hi"` decays the
//     Array<Char> string literal to Ptr<Char> INSIDE the explicit
//     cast (C 6.3.2.1p3: the operand undergoes array-to-pointer
//     conversion before the cast applies). Pre-sweep this fired
//     S_InvalidCast. The decayed pointer feeds the shipped `puts`
//     FFI — "hi" on stdout proves the cast produced the REAL rodata
//     address end-to-end (semantic → HIR decay Cast → MIR GlobalAddr
//     → riprel/ADRP materialization → the C runtime printed it).
//
//   * D-CSUBSET-CAST-VOID-DISCARD — `(void)puts(...)` is C's
//     evaluate-and-discard idiom (6.3.2.2): the operand call is
//     EVALUATED (the captured "hi" output IS the observable proof —
//     a discard that skipped evaluation would print nothing) and its
//     int result is discarded with no Cast node (mapCast has no void
//     arm; the lowering drops the value as a statement effect).
//
// Expected: prints "hi" + newline to captured stdout, exits 42. A
// regression in the decay (S_InvalidCast → compile rc != 0), in the
// discard (likewise), or in evaluation order (no output) flips the
// harness RED on every target arm.

#include <stdio.h>

int main() {
    (void)puts((char*)"hi");
    return 42;
}
