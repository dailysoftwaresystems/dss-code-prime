// FC17.5 (D-CSUBSET-AUTO-TYPE-INFERENCE): the ★C3 inference rejections — each
// initializer type that cannot become a declared object type fails loud
// S_AutoInferenceInvalid (unsuppressable; without the arm's reject the Pass-2
// initializer backfill would silently adopt Void / the unresolved self-reference):
// `auto v = voidFn();` is void, no object type; `auto x = x;` resolves to the
// symbol being declared, whose type is what is being inferred.
// ⚠ `auto p = nullptr;` WAS A THIRD ROW HERE and is GONE with its golden line: it
// stopped being malformed when [[D-CSUBSET-NULLPTR-T-DECLARABLE]] landed, and both
// gcc 13.3.0 (-std=c2x) and clang 18.1.3 (-std=c23) compile and run it. Its
// positive replacement is `SemanticAnalyzerC.AutoInfersTheNullPointerConstantsType`.
void voidFn(void) { }
int main(void) {
    auto v = voidFn();
    auto x = x;
    return 0;
}
