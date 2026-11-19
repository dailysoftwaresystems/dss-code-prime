// VLA C1a (D-CSUBSET-VLA, MINOR-1): a struct member with a runtime bound is NOT
// a VLA (the `!allowFlexibleArray` gate excludes struct fields), and it is NOT a
// flexible array member either: a member cannot have a variably modified type
// (C 6.7.2.1p9), so its bound must be an integer constant expression and one that
// does not fold is refused BY NAME at the bound (S_NonConstantArrayLength). Until
// P69 it took the flexible-member path instead, and failed loud here only because
// it was the SOLE member — beside another member it became a flexible member.
int n;
struct S { int a[n]; };
