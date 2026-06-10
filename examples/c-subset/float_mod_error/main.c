// FC1 (V2-4.X, 2026-06-10): `%` requires INTEGER operands (C23 6.5.5
// constraint). A float-typed remainder has no MIR realization — the
// HIR→MIR lowering rejects HirOpKind::Rem on F64 with a positioned
// H_UnsupportedLoweringForKind (fail-loud; never a silent fmod or a
// silent integer truncation). The clean SEMANTIC-tier S_* constraint
// (shared with shifts/bitwise) lands with FC3's operand-constraint
// vocabulary — see plan-23 FC3.
int main() {
    return 1.5 % 2;
}
