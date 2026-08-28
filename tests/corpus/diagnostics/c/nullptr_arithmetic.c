// C23 §6.5.6 (D-CSUBSET-NULLPTR): `nullptr` (nullptr_t) is NOT an arithmetic
// operand. `nullptr + 1` is a constraint violation → the fail-loud operator gate
// emits S_NullptrInvalidOperand at the `nullptr` operand. WITHOUT the gate the HIR
// lowering (nullptr → integer-0 null constant) would silently compile this as
// `0 + 1`. The `void*` return type accepts the (cascade) NullptrT result via the
// isAssignable nullptr arm, so this file pins exactly ONE diagnostic — the gate's.
void *bad(void) {
    return nullptr + 1;
}
