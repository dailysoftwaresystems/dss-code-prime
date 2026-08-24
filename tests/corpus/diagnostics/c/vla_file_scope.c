// VLA C1a (D-CSUBSET-VLA): a file-scope array with a runtime (non-constant)
// bound is NOT a variable-length array — a VLA needs automatic (block) storage.
// The scope gate leaves it S_NonConstantArrayLength (the pre-VLA behavior).
int n;
int g[n];
