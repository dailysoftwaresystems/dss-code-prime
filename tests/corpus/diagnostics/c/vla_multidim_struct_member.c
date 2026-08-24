// VLA C3 (D-CSUBSET-VLA, MINOR-9): a MULTI-dimensional VLA is now a valid BLOCK-scope
// object, but it is STILL rejected as a STRUCT MEMBER (ISO C forbids a variably-modified
// struct member). After the C3 lift the member type BUILDS (vlaArray(vlaArray(int)))
// instead of tripping the old S_VlaMultiDimUnsupported at type-construction, so the
// reject now fires via the struct composition path — S_FlexibleArraySoleMember (the
// sole-member facet, same code the 1-D vla_struct_member.c pins), NOT 0xE052. This pins
// that the array-of-VLA-in-a-struct reject is PRESERVED for the multi-dim shape (fails
// loud, never a silent wrong layout) and pins the code that ACTUALLY fires.
int n;
int m;
struct S { int a[n][m]; };
