// FORM 3 — the pragma inside a TAKEN conditional. It must FIRE, because the
// line executes (C 6.10p1). This is the live twin of the dead-branch case the
// unit suite pins: `#pragma once` inside `#if 0` must NOT fire, and all four
// references agree on both halves. Testing only the dead half would leave a
// "never fire from inside any conditional" implementation looking correct.
#if 1
#pragma once
#endif

#define C_VALUE 9

int once_c_value(void) { return C_VALUE; }
