// C11/C23 6.7.11 + 6.6 ([[D-C-STATIC-ASSERT-REFUSES-A-LONG-DOUBLE-COMPARISON]]):
// a FLOATING-POINT comparison is a constant expression every reference toolchain
// folds, in a static assertion and in every other integer-constant-expression
// position. ✔MEASURED separately on gcc 13.3.0, clang 18.1.3, mingw-w64 gcc
// 13.2.0 and MSVC 19.51: all four accept every line in this file, and gcc RUNS it
// to exit 42.
//
// ★ WHY THE EXIT CODE IS THE INSTRUMENT AND NOT THE STATIC ASSERTIONS. An
// assertion only proves the fold produced SOMETHING non-zero; a fold that
// answered the WRONG truth value would still compile the file. So the array
// bound and the bit-field width below are built out of enumerators whose values
// ARE the folded comparisons, and `main` returns a number derived from both:
// fold any one of them the wrong way and the program exits with a different
// number rather than passing quietly.
//
// ★ THE COMPARISONS LIVE IN THE ENUMERATORS, and the array bound is then built
// from ENUM CONSTANTS. That is not an accident of style: ✔MEASURED, writing the
// float comparison directly in the array bound makes gcc 13.3.0 warn "variably
// modified 'table' at file scope" and clang 18.1.3 warn "variable length array
// folded to constant array as an extension" — both still accept it, but a corpus
// example should pin the shape the references call clean, and an enumerator
// position is a strict integer constant expression on all four.
//
// ⚠ THE FALSE DIRECTION IS PINNED IN THE UNIT SUITE, NOT HERE, for the obvious
// reason: a corpus example that must FAIL to compile has no exit code. See
// `StaticAssertFalseFloatComparisonStillFailsLoud` in
// tests/analysis/semantic/test_semantic_analyzer_c.

_Static_assert(0.1L > 0.0L, "a long double comparison is a constant expression");
_Static_assert(0.1  > 0.0,  "a double comparison is a constant expression");
_Static_assert(0.1f > 0.0f, "a float comparison is a constant expression");
_Static_assert(1.0 + 2.0 == 3.0, "float arithmetic folds, then compares");
_Static_assert(1.0L / 4.0L == 0.25L, "long double arithmetic folds, then compares");

// The `f` suffix must carry BINARY32 precision, not the host double the decoder
// naturally produces: 0.1 rounded to binary32 and widened back is a DIFFERENT
// number from 0.1 at binary64. All four references agree this assertion holds.
_Static_assert(0.1f != 0.1, "an f-suffixed literal is a binary32 value");

enum Ordering {
    kGreater  = (0.1L > 0.0L),        /* 1 */
    kLess     = (0.1L < 0.0L),        /* 0 */
    kSum      = (1.0 + 2.0 == 3.0),   /* 1 */
    kNarrowed = (0.1f != 0.1)         /* 1 */
};

struct Packed { unsigned lane : kGreater + kSum + 1; };   /* 3 bits */

static int table[40 + kGreater + kSum + kNarrowed - kLess - 1];   /* 42 */

int main(void) {
    // Block scope reaches the same door through a different visitor path.
    _Static_assert(0.25 == 1.0 / 4.0, "block scope folds identically");
    struct Packed p;
    p.lane = 3;   /* fits 3 bits; a narrower field would truncate and move the exit */
    return (int)(sizeof table / sizeof table[0]) + (int)p.lane - 3;
}
