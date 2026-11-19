void note(int);
/* PROTOTYPED FIRST: the unit numbers `wc` before `wa` and `wb`, and defines it last. */
__attribute__((constructor)) void wc(void);
__attribute__((constructor)) void wa(void) { note(21); }
__attribute__((constructor)) void wb(void) { note(22); }
__attribute__((constructor)) void wc(void) { note(23); }
