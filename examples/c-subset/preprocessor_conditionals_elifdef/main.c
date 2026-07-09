// D-PP-ELIFDEF-ELIFNDEF (C23 6.10.1) runtime witness for `#elifdef`.
//
// `#elifdef X` == `#elif defined(X)`. FEATURE_A is NOT defined (so `#ifdef
// FEATURE_A` is a DEAD group) and FEATURE_B IS defined, so the `#elifdef
// FEATURE_B` branch MUST be taken -> PICK = 42, NOT the `#else` (PICK = 2).
//
//   #ifdef FEATURE_A    -> false (dead)   #define PICK 1
//   #elifdef FEATURE_B  -> TRUE, TAKEN    #define PICK 42   <-- the answer
//   #else                                 #define PICK 2
//
// THE PRE-FIX SILENT MISCOMPILE: an unrecognized `#elifdef` is silently
// consumed inside the dead `#ifdef FEATURE_A` group (no diagnostic), control
// falls through to `#else`, and PICK becomes 2 -> the program exits 2 with NO
// compile or link error. That clean wrong-exit (not a loud failure) is exactly
// the silent-miscompile class this pin guards.
//
// Fold-resistance: PICK reaches `identity` as a FUNCTION ARGUMENT, so the
// baseline (unoptimized) arm keeps a live runtime call -- the return is not
// const-folded to a single immediate at exit. The release arm runs the shipped
// pipeline over the same source.
//
//   FEATURE_B defined -> PICK = 42 -> identity(42) -> exit 42

#define FEATURE_B

#ifdef FEATURE_A
#define PICK 1
#elifdef FEATURE_B
#define PICK 42
#else
#define PICK 2
#endif

int identity(int v) { return v; }

int main(void) {
    return identity(PICK);
}
