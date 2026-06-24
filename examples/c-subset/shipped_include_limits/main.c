// Shipped <limits.h> constants via the NEUTRAL JSON descriptor (Item 1).
//
// `#include <limits.h>` resolves to `limits.json` on the shippedLibDirs system
// path; the semantic phase injects each `#define` macro-constant (CHAR_BIT, …)
// as a named integer constant. NO C `.h` ships in the config — the shipped
// header content is language-agnostic JSON, injected and folded to a literal.
//
// `CHAR_BIT` (= 8) is used in BOTH positions a real header constant must work:
//   * a CONSTANT-EXPRESSION — the array dimension `int buf[CHAR_BIT]` (routes
//     through the const-eval engine's direct-value arm), and
//   * a VALUE — the index `buf[CHAR_BIT - 1]` and the returned expression.
//
// Why it is a STRONG witness:
//   * If the constant injection silently no-ops, `CHAR_BIT` is an undeclared
//     identifier → a COMPILE ERROR, never a silent wrong exit.
//   * If the array-dimension fold regressed, `int buf[CHAR_BIT]` fails loud
//     (S_NonConstantArrayLength) — the MF-1 case the const-eval arm closes.
//   * If the constant folds to the wrong value, the exit code flips.
//
// Expected: exit 42 on every target (40 + 2).

#include <limits.h>

int main() {
    int buf[CHAR_BIT];            // const-expr dimension → 8 elements
    buf[0]            = 40;
    buf[CHAR_BIT - 1] = 2;        // index 7 (value position)
    return buf[0] + buf[CHAR_BIT - 1];
}
