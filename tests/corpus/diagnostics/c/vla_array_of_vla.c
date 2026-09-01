// VLA C3 (D-CSUBSET-VLA, which is CLOSED since 2026-09-01): a FIXED-OUTER multi-dim VLA
// `int a[5][n]` RUNS at BLOCK scope (`array(vlaArray,5)` — a runtime row stride), while a
// FILE-scope one is refused by a constraint that closed row never deferred and never could
// — ISO C gives a VLA AUTOMATIC storage duration only, so gcc, clang and MSVC refuse this
// program too (C 6.7.6.2p2). ⇒ this pin OUTLIVES the row it cites, and a later reader must
// not clear it by re-opening anything.
// The top type here is a fixed Array (NOT isVlaArray), so this pins the gate-5
// `typeContainsVla` routing that funnels the fixed-outer shape into the VLA constraint
// validator, which fails it loud — ✔RE-MEASURED 2026-09-01 through the shipped CLI:
// S_NonConstantArrayLength, "a file-scope array requires a constant length".
// Red-on-regression for the transitive routing gate.
int n;
int a[5][n];
