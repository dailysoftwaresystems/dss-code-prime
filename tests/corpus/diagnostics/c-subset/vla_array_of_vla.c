// VLA C3 (D-CSUBSET-VLA): a FIXED-OUTER multi-dim VLA `int a[5][n]` now RUNS at BLOCK
// scope (`array(vlaArray,5)` — a runtime row stride) — but a VLA has AUTOMATIC storage
// duration only (C 6.7.6.2p2), so a FILE-scope one is still rejected. The top type here
// is a fixed Array (NOT isVlaArray), so this pins the gate-5 `typeContainsVla` routing
// that funnels the fixed-outer shape into the VLA constraint validator, which fails it
// loud (S_NonConstantArrayLength). Red-on-regression for the transitive routing gate.
int n;
int a[5][n];
