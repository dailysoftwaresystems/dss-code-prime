/* D-C-FLOAT-LITERAL-OVERFLOW-REFUSED-INSTEAD-OF-YIELDING-INFINITY, the WARNING
 * half (P54 lane `fw`). The golden pins the SPAN of every range warning, which
 * is what no unit test does: a diagnostic whose code is right and whose caret
 * points at the wrong token is invisible to a count assertion.
 *
 * ⓘ NO `long double` HERE ON PURPOSE. This harness calls `analyze()` with no
 * long-double axis (LongDoubleFormat::None), under which an `L` literal is
 * correctly S_LongDoubleFormatUndeclared and not a range question at all. The
 * per-axis coverage is in SemanticAnalyzerC.LongDoubleLiteralOverflowFollowsTheDeclaredAxis.
 */

/* Warn: the correctly-rounded value is +inf. */
static const double over = 1e400;
static const double just_over = 1.7976931348623159e308;
static const double hex_over = 0x1p+99999;

/* Warn through the NARROWING door: 1e40 is an ordinary double and only
 * overflows once it is rounded to binary32. */
static const float f_over = 1e40f;

/* Silent: all four references accept these without a word. `dbl_max` is ONE ULP
 * below `just_over` above, which is the only pair separating "reports overflow"
 * from "reports anything large"; `subnormal` is an ordinary representable value.
 */
static const double dbl_max = 1.7976931348623157e308;
static const double subnormal = 1e-320;
static const float f_max = 3.40282347e38f;

int main(void) { return 0; }
