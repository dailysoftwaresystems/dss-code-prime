// C23 6.7.9p2 (D-CSUBSET-AUTO-TYPE-INFERENCE): the declarator of an
// initializer-inferred declaration shall be a PLAIN IDENTIFIER — pointer
// (`auto *p`), array, and function declarators are the WG14 v2-paper
// derived-declarator EXTENSION (the named deferral
// D-CSUBSET-AUTO-DERIVED-DECLARATOR) and reject loud
// S_AutoRequiresPlainIdentifier. The second declaration pins the
// diagnostic-SHAPE boundary the parser order relies on: `auto f(void);`
// parses into the headless rule (a fnSuffix on the name) and must be THIS
// code, never a silent prototype.
int main(void) {
    auto *p = 0;
    auto f(void);
    return 0;
}
