// FC1 (V2-4.X, 2026-06-10): `%` modulo end-to-end — the
// D-CSUBSET-MOD-OP-CODEGEN runtime witness.
//
// Runtime TRUNCATED-remainder discriminator (C23 6.5.5: the result's
// sign follows the DIVIDEND; (a/b)*b + a%b == a with / truncating
// toward zero). Function-arg operands defeat ConstFold under the
// baseline pipeline, so a real divide instruction computes each
// remainder at runtime (x86: cqo+idiv capturing RDX; arm64: the
// sdiv+mul+sub expansion):
//
//   m = mod(-7,  2) = -1    (floored semantics would give +1)
//   p = mod( 7, -2) = +1    (floored gives -1)
//   q = 0b101 = 5; q %= 3 → 2   (binary literal + the newly-live `%=`
//                                compound, which has targeted Rem since
//                                its config row landed but only now has
//                                a codegen path)
//
//   exit = m + 43 + q - 2 + 2*(p - 1)
//     truncated:        -1 + 43 + 2 - 2 + 2*( 0) = 42
//     floored:           1 + 43 + 2 - 2 + 2*(-2) = 40
//     quotient-flip (role contract broken — mod returns the quotient,
//       so m = trunc(-7/2) = -3, p = trunc(7/-2) = -3, q = trunc(5/3) = 1):
//                       -3 + 43 + 1 - 2 + 2*(-4) = 31
//     unsigned-divide routing (FLAG 1): mod(-7,2) under DIV is a huge
//       positive remainder → far from 42.
//
// Every known wrong-semantics class lands away from 42; the runner
// asserts exit code 42 exactly on all four target arms.
int mod(int a, int b) {
    return a % b;
}

int main() {
    int m = mod(-7, 2);
    int p = mod(7, -2);
    int q = 0b101;
    q %= 3;
    return m + 43 + q - 2 + 2 * (p - 1);
}
