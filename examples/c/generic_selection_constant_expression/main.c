// P49 [[D-CSUBSET-GENERIC-SELECTION-IS-NOT-AN-INTEGER-CONSTANT-EXPRESSION]]:
// C23 6.5.1.1p3 — a generic selection IS an integer constant expression when the
// SELECTED assignment-expression is one, and the UNSELECTED associations are
// unevaluated. So `_Generic` is legal wherever C requires an ICE: a
// `_Static_assert` condition, an array declarator's length, an enumerator value.
//
// ★ THE EXTENT IS OBSERVED, NOT ASSUMED. Every array below is measured with
// `sizeof`, twice: once at COMPILE time through a `_Static_assert`, and once at
// RUN time into this program's exit code. Compiling is not the claim — an
// example that only builds proves the length folded to SOMETHING, not to 4.
//
// ★★ AND THE COMPILE-TIME HALF IS WHY `blockLocal` IS HERE. At block scope a
// non-constant length is a legal VLA, so the pre-fix compiler did not refuse
// `int a[_Generic(x, int: 4, default: 1)]` at all — it silently built a VLA and
// said nothing. The run-time check cannot see that (a 4-element VLA measures 4
// too); the `_Static_assert` can, because `sizeof` a VLA is not an integer
// constant expression.
//
// Data-model-independent: `int` is 4 bytes and `double` 8 on both LP64 and
// LLP64, and nothing here is size-sensitive to `long`, so every selection
// resolves identically on all four targets.
//
// Exit code 63 = every observation held; each bit names one, so a failure says
// WHICH selection went wrong rather than only that one did.

// The UNSELECTED-arm witness. Defined here so the program links; its whole point
// is that a call to it sits in a `default:` arm the controlling type does not
// pick, and must therefore never be evaluated for constness.
static int notAConstant(void) { return 1; }

int   controllingInt    = 0;
double controllingDouble = 0;

// 1. the typed arm wins
int fromIntArm[_Generic(controllingInt, int: 4, default: 1)];
_Static_assert(sizeof(fromIntArm) / sizeof(fromIntArm[0]) == 4, "int arm sizes 4");

// 2. the `default:` arm wins — makes the CONTROLLING TYPE load-bearing, which a
//    fix that always took the first association would fail.
int fromDefaultArm[_Generic(controllingDouble, int: 1, default: 9)];
_Static_assert(sizeof(fromDefaultArm) / sizeof(fromDefaultArm[0]) == 9, "default arm sizes 9");

// 3. the UNSELECTED arm holds a function call and must not poison the selection
int unselectedIsNotEvaluated[_Generic(controllingInt, int: 3, default: notAConstant())];
_Static_assert(sizeof(unselectedIsNotEvaluated) / sizeof(unselectedIsNotEvaluated[0]) == 3,
               "an unselected non-constant arm does not poison the fold");

// 4. a selection composes with surrounding arithmetic
int scaled[_Generic(controllingInt, int: 2, default: 1) + 3];
_Static_assert(sizeof(scaled) / sizeof(scaled[0]) == 5, "arithmetic over a selection");

// 5. a selection nests inside a selection
int nested[_Generic(controllingInt, int: _Generic(controllingInt, int: 6, default: 1), default: 1)];
_Static_assert(sizeof(nested) / sizeof(nested[0]) == 6, "a nested selection folds");

// 6. an ENUMERATOR value — a different const-expr consumer from an array length
enum Selected { SelectedValue = _Generic(controllingInt, int: 7, default: 1) };
_Static_assert(SelectedValue == 7, "an enumerator folds from a selection");

int main(void) {
    // The block-scope array, whose pre-fix failure was SILENT (a VLA).
    int blockLocal[_Generic(controllingInt, int: 4, default: 1)];
    _Static_assert(sizeof(blockLocal) / sizeof(blockLocal[0]) == 4,
                   "a block-scope selection length is constant, not a VLA");

    (void)notAConstant;
    // ⚠ ONE branch-free expression, deliberately. The `if`-per-observation shape
    // this replaced made the shipped `release` pipeline hit its fixpoint cap
    // (`X_OptFixpointTruncated`), so the optimized arm was witnessing a program
    // the pipeline had stopped short of converging on. Nothing about the
    // measurement changes: each bit still names exactly one observation.
    return   (((int)(sizeof(fromIntArm) / sizeof(fromIntArm[0])) == 4)                             << 0)
           + (((int)(sizeof(fromDefaultArm) / sizeof(fromDefaultArm[0])) == 9)                     << 1)
           + (((int)(sizeof(unselectedIsNotEvaluated) / sizeof(unselectedIsNotEvaluated[0])) == 3) << 2)
           + (((int)(sizeof(scaled) / sizeof(scaled[0])) == 5)                                     << 3)
           + (((int)(sizeof(nested) / sizeof(nested[0])) == 6)                                     << 4)
           + ((((int)(sizeof(blockLocal) / sizeof(blockLocal[0])) == 4) && SelectedValue == 7)     << 5);
}
