// D-C-LINKAGE-SPECIFIER-LOOKUP-IS-POSITION-BLIND-AND-NOT-DUNDER-NORMALIZED
// (P42 lane V), CU B: the STRONG definitions that cu_a.c's dunder-spelled weak
// declarations must resolve to. Deliberately carries NO attributes at all - if a
// `__weak__` over there is parsed but its binding never reaches the symbol,
// these definitions collide with cu_a.c's and the LINK fails, which is what
// makes every weak row in cu_a.c exit-code discriminating rather than a parse
// check.
int wfun(void) { return 12; }
int wd_lead    = 5;
int wd_tail    = 5;
int wp(void)   { return 8; }
int e_weak     = 9;
