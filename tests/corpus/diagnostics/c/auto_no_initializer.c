// C23 6.7.9p2 (D-CSUBSET-AUTO-TYPE-INFERENCE): an initializer-inferred
// declaration REQUIRES an initializer — there is nothing to infer from.
// `auto T;` is the diagnostic-SHAPE pin for the parser-order boundary: the
// only shape both decl rules could take (`<specifiers> Ident ;`) is invalid
// C either way, and it must surface as THIS inference-tier code
// (S_AutoRequiresInitializer, unsuppressable), never a silent no-op or an
// unrelated parse error.
int main(void) {
    auto T;
    return 0;
}
