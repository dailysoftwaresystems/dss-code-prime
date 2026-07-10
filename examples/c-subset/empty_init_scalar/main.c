// FC17.5 (D-CSUBSET-EMPTY-INITIALIZER, C23 6.7.10): the `{}` empty
// initializer + the scalar brace initializer, END TO END on all four targets:
//
//   * `int z = {};`          — C23 6.7.10p11: empty initializer ZERO-initializes
//                              a plain scalar;
//   * `int v = {35};`        — 6.7.10p12: a scalar brace init with exactly ONE
//                              expression initializes with that expression
//                              (byte-identical to `= 35`);
//   * `int cl = (int){7};`   — the SCALAR compound literal (C 6.5.2.5p9), the
//                              flip of the pre-FC17.5 aggregate-only fail-loud
//                              pin — same scalar brace-init arm via the one
//                              lowerBraceInit funnel;
//   * `int *p = {};`         — the empty initializer on a POINTER zero-fills to
//                              the null pointer (p == 0 must hold);
//   * `struct Pair s = {};`  — the AGGREGATE `{}` zero-fill that ALREADY worked
//                              (the regression guard: the new scalar arm must
//                              not perturb the aggregate route);
//   * `int arr[3] = {};`     — same regression guard for the array route.
//
// exit = z(0) + v(35) + cl(7) + (p==0 ? 0 : 100) + s.a(0) + s.b(0)
//        + arr[0](0) + arr[2](0) = 42.
// RED-on-revert: without the scalar brace-init arm, `{}`/`{35}`/`(int){7}` on
// a scalar all fail to compile (the pre-FC17.5 aggregate-only gate).

struct Pair { int a; int b; };

int main(void) {
    int z = {};              // 0  (empty initializer, C23 6.7.10p11)
    int v = {35};            // 35 (single-expression scalar brace init)
    int cl = (int){7};       // 7  (scalar compound literal)
    int *p = {};             // null pointer
    struct Pair s = {};      // {0, 0} (aggregate zero-fill regression guard)
    int arr[3] = {};         // {0, 0, 0}
    return z + v + cl + (p == 0 ? 0 : 100) + s.a + s.b + arr[0] + arr[2];
}
