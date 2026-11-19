/* A parameter declared by its type alone, followed by an attribute run:
 * `int add(int __attribute__((unused)), int);`. The run stands where a declarator
 * would, and it is not one. Before P69's fold f26c DSS read it as the parameter's
 * declarator, so the prototype lost the parameter's type: H_TypeUnresolved on the
 * prototype alone, S_IncompatibleRedeclaration beside its definition. gcc 13.3.0,
 * clang 18.1.3, mingw-w64 gcc 13.2.0 and Apple clang 21.0.0 run each of these
 * shapes (lane cs's probe np, one shape per cell).
 *
 * A run that changes the type reaches the nameless parameter too: `mode(QI)` makes
 * it `signed char`, which the definition agrees with, and a call converts 298 to
 * it (298 - 256 = 42).
 *
 * Exit 42 = every check held; another code names the first check that did not. */

static int add(int __attribute__((unused)), int);
static int add(int a, int b) { return a + b; }

static int both(int __attribute__((__unused__)) __attribute__((unused)), int);
static int both(int a, int b) { return a - b; }

static int narrow(int __attribute__((mode(QI))));
static int narrow(signed char c) { return c; }

int main(void) {
    if (add(40, 2) != 42) return 1;
    if (both(50, 8) != 42) return 2;
    if (narrow(298) != 42) return 3;
    return add(both(30, 10), narrow(278));  /* 20 + 22 */
}
