// The SECOND sibling TU. Byte-for-byte the same declaration shapes as left.c
// with DIFFERENT values, so the exit code reports WHICH copy each call reached:
// 55 means each TU used its own, 42 means both collapsed onto left.c's.

static int tuValue(void);
int tuValue(void) { return 30; }

static int tuSlot;
int tuSlot = 4;

int rightTotal(void) { return tuValue() + tuSlot; }   // 30 + 4 = 34
