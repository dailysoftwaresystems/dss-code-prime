// Plan 24 Stage 6 deep const-fold witness (see expected.json comment).
// The global g's initializer is a 150-deep nested const-expression folded
// by the HIR const-evaluator (evaluateConstant); the array a's dimension is
// a 150-deep nested const-expression folded by the CST const-evaluator
// (evaluateConstantCst). Both are now explicit work-stack drivers.
int g = ((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((10 - 4))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))));
int a[((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((8 + 1))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))];
int main(void) {
  return g + (int)sizeof(a);
}
