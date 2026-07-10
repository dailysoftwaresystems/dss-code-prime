// C23 6.7.9p2 (D-CSUBSET-AUTO-TYPE-INFERENCE): an initializer-inferred
// declaration shall declare EXACTLY ONE declarator — `auto a = 1, b = 2;`
// is a constraint violation (S_AutoRequiresSingleDeclarator, unsuppressable:
// a suppressed reject would let Pass 2's initializer backfill type each
// declarator independently — the exact multi-declarator form the
// constraint forbids).
int main(void) {
    auto a = 1, b = 2;
    return a + b;
}
