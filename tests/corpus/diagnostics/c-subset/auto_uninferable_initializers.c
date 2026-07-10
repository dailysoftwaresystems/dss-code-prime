// FC17.5 (D-CSUBSET-AUTO-TYPE-INFERENCE): the ★C3 inference rejections —
// each initializer type that cannot become a declared object type fails
// loud S_AutoInferenceInvalid (unsuppressable; without the arm's reject the
// Pass-2 initializer backfill would silently adopt Void / NullptrT / the
// unresolved self-reference):
//   • `auto v = voidFn();`  — void, no object type;
//   • `auto p = nullptr;`   — nullptr_t is semantic-tier-only
//                             (D-CSUBSET-NULLPTR-T-DECLARABLE);
//   • `auto x = x;`         — the name resolves to the symbol being
//                             declared, whose type is what is being inferred.
void voidFn(void) { }
int main(void) {
    auto v = voidFn();
    auto p = nullptr;
    auto x = x;
    return 0;
}
