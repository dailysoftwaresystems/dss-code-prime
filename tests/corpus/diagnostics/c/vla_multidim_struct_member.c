// VLA C3 (D-CSUBSET-VLA, MINOR-9, and that row is CLOSED since 2026-09-01): a
// MULTI-dimensional VLA is a valid BLOCK-scope object, while the same shape as a STRUCT
// MEMBER is refused by a constraint the closed row never deferred and never could — ISO C
// forbids a variably-modified struct member, so gcc, clang and MSVC refuse it too.
// ⇒ this pin OUTLIVES the row it cites.
// P69 (lane `cs`): the refusal is now the constraint's own — a member's bound must be an
// integer constant expression (C 6.7.2.1p9), so the first bound that does not fold is
// refused BY NAME (S_NonConstantArrayLength), the 1-D vla_struct_member.c's code. It
// used to fire through the struct composition path (S_FlexibleArraySoleMember), because
// a member's runtime bound was read as a flexible array member — beside another member
// that reading was ACCEPTED. This pins that the array-of-VLA-in-a-struct reject is
// PRESERVED for the multi-dim shape (fails loud, never a silent wrong layout).
int n;
int m;
struct S { int a[n][m]; };
