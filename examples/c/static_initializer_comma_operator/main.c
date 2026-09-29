// P68 round 13 (lane `cs`, the static-initializer item) — the COMMA operator in a static
// initializer: C 6.6p3 excludes it from a constant expression, and clang (even at
// -pedantic-errors) and MSVC build it anyway, so c.lang.json names the form
// (`semantics.staticInitializers.otherConstantForms: commaOperator`) and DSS admits it WITH the
// warning the constraint asks for (S_StaticInitializerUsesTheCommaOperator). The discarded
// operand must itself be a constant; the value is the last operand's.

int a[2] = { 7, 42 };

static int last = (1, 42);                          // an integer
int *second = (0, &a[1]);                           // an address

int main(void) { return (last == 42 && *second == 42) ? 42 : 1; }
