// FORM 1 — the bare idiom: `#pragma once` on the first line, no macro guard at
// all. MEASURED: 18 of the 21 macOS 26 SDK headers that carry `#pragma once`
// (all of AppleArchive/*) have NO macro guard, so this is the shape that
// actually ships, not a reduced one.
#pragma once

#define A_VALUE 10

// ★ A REAL DEFINITION, DELIBERATELY — this is what makes the example a WITNESS
// rather than a restatement. If the include-once dedup fails, this function is
// defined twice and the build fails loudly. A header full of only `#define`s
// would be re-splicable with no observable consequence at all.
int once_a_value(void) { return A_VALUE; }
