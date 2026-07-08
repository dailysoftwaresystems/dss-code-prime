// C11/C23 6.4.3 (FF1/FF2): `U"\uD800"` names a UTF-16 surrogate half
// (U+D800..U+DFFF) - not a Unicode scalar value. The compile FAILS LOUD with the
// dedicated H_InvalidUniversalCharacterName (unsuppressable), NEVER a silent
// CESU-8 / wrong code unit. Red-on-disable: drop the FF1 surrogate reject in
// decodeEscapedBytes and this compiles emitting overlong/CESU-8 bytes.
int main(void) {
    unsigned int *p = (unsigned int *)U"\uD800";
    return p[0];
}
