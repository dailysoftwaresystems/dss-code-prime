// VLA C1a (D-CSUBSET-VLA, MINOR-1): a struct member with a runtime bound is NOT
// a VLA (the `!allowFlexibleArray` gate excludes struct fields). It takes the
// existing FAM/incomplete-array path and fails loud there (a sole flexible array
// member) — never a silent nested-VLA member.
int n;
struct S { int a[n]; };
