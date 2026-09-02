// The FIRST of the two sibling TUs. Both declare the SAME two names with the
// SAME C 6.2.2p4/p5 inheritance shape, which is the whole point: each name is
// internal to its own TU, so the two copies are distinct objects and the link
// must succeed.

// FUNCTION half — 6.2.2p5 routes a plain function declaration through p4, and p4
// gives it the linkage of the visible prior declaration. The definition below
// carries NO `static` token; it is internal anyway.
static int tuValue(void);
int tuValue(void) { return 20; }

// OBJECT half — the same inheritance, on an INITIALIZED definition. This is the
// ordering that actually reaches the emission arm: the initialized definition
// OUTRANKS the `static` tentative in the redeclaration merge and becomes the
// surviving, emitting declaration, so it is the one whose linkage had to be
// read off the survivor record rather than off its own (absent) `static`.
static int tuSlot;
int tuSlot = 1;

int leftTotal(void) { return tuValue() + tuSlot; }   // 20 + 1 = 21
