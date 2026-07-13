// VLA C1a (D-CSUBSET-VLA): pinned at FILE scope. `sizeof(x)` of a VALUE is a
// pre-existing const-eval gap (it does not fold in the integer-const path), so a
// file-scope array with that bound stays S_NonConstantArrayLength (a file-scope
// array needs a constant bound; it is NOT a VLA). A block-scope `int a[sizeof(x)]`
// would become a VLA (accepted, fails at the LIR C1b boundary).
const int x = 7;
int a[sizeof(x)];
