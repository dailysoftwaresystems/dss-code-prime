// C23 6.7.10 (D-CSUBSET-INFERRED-AUTO-REFUSES-THE-MULTI-DECLARATOR-FORM-THE-STANDARD-NAMES-A-COMMON-EXTENSION):
// an initializer-inferred declaration may carry SEVERAL declarators, and they
// all share ONE deduced type. What is a violation is a DISAGREEMENT between
// them (S_AutoDeclaratorsInferDifferentTypes, unsuppressable: a suppressed
// reject falls through to Pass 2's initializer backfill, which would type each
// declarator independently and ship a declaration with two types).
int main(void) {
    auto a = 1, b = 2.5;
    return a + (int)b;
}
