// FC1 (V2-4.X, 2026-06-10): `%` requires INTEGER operands (C23 6.5.5
// constraint). A float-typed remainder has no MIR realization — the
// HIR→MIR lowering rejects HirOpKind::Rem on F64 with a positioned
// H_UnsupportedLoweringForKind (fail-loud; never a silent fmod or a
// silent integer truncation). The clean SEMANTIC-tier S_* operand
// constraint (shared with shifts/bitwise) lands with FC3's operand-
// constraint vocabulary — see plan-23 FC3.
//
// EXPRESSION STATEMENT (not `return 1.5 % 2`): the complete semantic-tier
// expression typer (D-SEMANTIC-SUBTREETYPE-TRANSPARENT-WRAPPERS ✅) now
// types `1.5 % 2` as F64, so a return/init/arg context would surface the
// F64-vs-int type MISMATCH first and mask the operator rejection. The bare
// statement keeps this example pinned on the float-% LOWERING rejection
// itself (the precise operand-constraint diagnostic stays FC3 future work).
int main() {
    1.5 % 2;
    return 0;
}
