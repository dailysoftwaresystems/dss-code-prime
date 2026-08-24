/* R1 crash-fix red-on-disable: a malformed string escape (`\q`) leaves the
 * sizeof operand InvalidType (the semantic string arm cannot decode it). The
 * enclosing cast must then FAIL LOUD (H_UnsupportedLoweringForKind), NOT crash.
 * Pre-fix, lowerCast called interner.kind() on the InvalidType operand →
 * `TypeInterner::get: TypeId out of range` abort. Revert the lowerCast
 * `if (!operand.type.valid()) return operand;` guard and this CRASHES instead
 * of emitting the diagnostic — the runner then fails to find H_UnsupportedLoweringForKind. */
int main(void) {
    return (int)sizeof("\q");
}
