// D-C-ATTRIBUTE-CLAUSE-NAME-ADMITS-ONLY-IDENTIFIER-SO-A-KEYWORD-NAMED-ATTRIBUTE-IS-REFUSED
// (P32 lane A) witness: an attribute clause NAME spelled with a KEYWORD.
//
// `__const__` is not an identifier in this language — the keyword table maps
// `const`, `__const__` and `__const` all to `ConstKeyword` — so the attribute
// clause-name position, which admitted `Identifier` and nothing else, refused
// every one of them: `P0009 expected 'Identifier' or 'ParenClose' — got
// '__const__'`. ✔MEASURED (P31 lane G, re-measured P32 lane A at the pre-change
// HEAD): ONE `gcc -E -P` `_GNU_SOURCE` translation unit over
// stdlib/string/strings/math/byteswap/sched carries FIFTY occurrences of that
// single spelling, and neutralising it alone took the TU from 50 parse errors to
// 0 — it was the top parse blocker for real glibc headers.
//
// Every declaration below is written in the shape glibc actually writes,
// including the two-run form (`__attribute__ ((__nothrow__ , __leaf__))
// __attribute__ ((__const__))`) that `math.h` puts on `atan`.
//
// ★★ WHAT THIS EXAMPLE CAN AND CANNOT WITNESS, stated plainly because the
// distinction is the whole design of the row. A corpus example must BUILD, so it
// exercises the ACCEPTING half — and the accepting half is exactly what a
// GRAMMAR-ONLY widening also passes. Widen the grammar alone and every line here
// compiles while each keyword-named clause matches no effect row and VANISHES:
// accepted, then silently ignored, which is strictly worse than the loud refusal
// it replaced. The pins that can tell those two apart are REFUSALS, and they live
// in `tests/analysis/semantic/test_attribute_clause_name_token_class.cpp`: an
// UNMODELLED keyword-spelled name must reach the strict gate and fail loud while
// its MODELLED sibling does not. Read the two together.
//
// RED-ON-DISABLE for THIS file: revert the clause-name position in
// `src/dss-config/sources/c.lang.json` (`attrSpec` / `attrClauseTail` /
// `stdAttrItem`) from `{"tokenClass": "attributeClauseName"}` back to
// `"Identifier"` → the first prototype fails P0009 and the program no longer
// compiles (the runner reports a compile failure, not exit 42).
//
// ★ VERIFIED against BOTH references, `-std=c2x -Wall -Wextra`: gcc 13.3.0 and
// clang 19.1.7 both compile this file with ZERO errors and ZERO warnings, and
// both produce a binary that exits 42. Re-run that check if you touch this file:
// an attribute a real toolchain rejects would make the example prove the
// opposite of what it is for.
//
// Front-end feature (grammar + attribute-clause reading), target/format-
// agnostic: x86_64 (PE + ELF) and arm64 (ELF under qemu, Mach-O macos leg).

// (1) The bare glibc form. `const` here is the GNU function attribute — "depends
//     on nothing but its arguments, reads no memory" — which is exactly true of
//     this function, so a reference compiler accepts it without complaint.
int idem(int x) __attribute__ ((__const__));
int idem(int x) { return x; }

// (2) The keyword-named clause in a MULTI-CLAUSE run, and in the TRAILING
//     position — a different reader from the one that finds the first clause
//     (`collectAttrClauses`'s trailing-clause detector, not
//     `extractOneAttrClause`'s name scan). Widening only the first clause would
//     have made this line a parse error while (1) compiled.
int twice(int x) __attribute__ ((__nothrow__ , __leaf__ , __const__));
int twice(int x) { return x + x; }

// (3) The TWO-RUN form, which is what `math.h` writes on `atan`:
//     `extern double atan (double __x) __attribute__ ((__nothrow__ , __leaf__))
//      __attribute__ ((__const__));`
int thrice(int x) __attribute__ ((__nothrow__ , __leaf__)) __attribute__ ((__const__));
int thrice(int x) { return x + x + x; }

// (4) The C23 spelling reaches the SAME clause-name position through a different
//     grammar rule (`stdAttrItem`), and its FINAL segment is a keyword here.
//     ✔MEASURED: `[[gnu::const]]` is accepted with no diagnostic by gcc 13.3.0
//     AND clang 19.1.7 — unlike the un-namespaced `[[const]]`, which both accept
//     but both warn about (`attribute ignored`), so the namespaced form is the
//     one a warning-clean corpus file can carry.
[[gnu::const]] int quad(int x);
int quad(int x) { return x + x + x + x; }

int main(void) {
    int s = idem(20);        // 20
    s = s + twice(6);        // + 12 = 32
    s = s + thrice(2);       // +  6 = 38
    s = s + quad(1);         // +  4 = 42
    return s;                // 42
}
