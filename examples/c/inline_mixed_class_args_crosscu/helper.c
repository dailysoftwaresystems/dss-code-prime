// The sibling CompilationUnit that DEFINES the mixed int+FP-class callee. Under
// the shipped `release` pipeline the merged module optimizes POST-merge, so
// Inlining splices this body into main (from main.c) AFTER mir_merge re-encoded
// its Args. A merge-site flat-position wipe would resurface the mixed-class
// arg-drop miscompile HERE — the single-TU corpus can't reach this path
// (D-OPT-RELEASE-SYSV-MIXED-CLASS-REG-ARG-DROP F1: mir_merge threads position).
int isum(int a, double x, int b, double y, double z) {
    return (int)(x + y + z) + a + b;
}
