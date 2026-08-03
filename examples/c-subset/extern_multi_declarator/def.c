// The sibling TU: defines every object that main.c multi-declarator-extern-
// declares. The LK11 cross-CU merge collapses main.c's file-scope and block-scope
// extern imports onto these definitions (each object load reaches its global).
int fa = 5;
int fb = 7;
int base = 3;
int *fp = &base;
int farr[2] = { 4, 6 };
int bc = 8;
int bd = 9;
