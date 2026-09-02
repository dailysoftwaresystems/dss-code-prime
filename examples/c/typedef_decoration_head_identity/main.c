/* P45 lane td — [[D-CSUBSET-TYPEDEF-HEAD-DECORATION-TYPE-HIJACK]], the RUNNABLE
 * witness for the sweep.
 *
 * `resolveTypeNodeImpl` picks a type-position node's head by
 * FIRST-CHILD-THAT-RESOLVES-WINS, and its token arm resolves ANY identifier
 * through the scope chain as a possible type alias. So an identifier a
 * decoration drags into a type-resolved child list can win the race against the
 * real head, and the declaration silently takes the WRONG TYPE in a program that
 * compiles clean. Every typedef below decorates its head with an attribute whose
 * NAME (or whose second clause's name) is ALSO a typedef of `int` declared just
 * above it — the exact collision that makes the hijack observable.
 *
 * ★ WHAT THIS EXAMPLE CAN WITNESS THAT THE UNIT PINS CANNOT. The pins in
 * `tests/analysis/semantic/test_type_head_hijack_sweep.cpp` stop at the semantic
 * model — they read the resolved TypeId. This file carries the answer all the
 * way to an EXIT CODE through the real backend, so a hijack that survived
 * lowering would change the number rather than merely a table entry. It is an
 * ACCEPT-direction witness by construction (a corpus example must build), which
 * is why the REFUSAL half — the tempting wrong fix being rejected loudly — lives
 * in the unit suite as `WrongFixInsideTheHeadIsRefusedLoudly`. Read the two
 * together; neither alone separates "guarded" from "accidentally still right".
 *
 * ★★ `long long` AND `int`, NEVER `long` AND `int`. `sizeof(long)` is 8 under
 * LP64 (linux, darwin) and 4 under LLP64 (windows pe64), so `long` vs `int` is
 * NOT a discriminator on every target this example runs on — it would make the
 * windows arm pass through the very hijack it claims to guard. `long long` is 8
 * and `int` is 4 on all four targets below.
 *
 * ⛔ THE TEMPTING WRONG FIX, recorded so it is never re-proposed: putting the
 * attribute run INSIDE the head rule (`typedefHeadFull`), "where the type head
 * already is". It looks right and it fails SILENTLY. The decoration belongs in a
 * SIBLING slot — the declaration row's `specifierPrefix` — which is what
 * `specifierPrefixChild` hands to the alignas / noreturn / attribute scans.
 *
 * RED-ON-DISABLE for THIS file — a 2x2, ✔MEASURED through the shipped CLI at
 * `x86_64:pe64-x86_64-windows-exec`, not argued. The config axis is the wrong
 * fix: move the attribute run out of `typedefDeclSpecifiers` and into
 * `typedefHeadFull` in `src/dss-config/sources/c.lang.json`. The engine axis is
 * the ambiguous-head guard in `resolveTypeNodeImpl`.
 *
 *     shipped config + guard   -> builds, EXIT 42          (the shipped state)
 *     shipped config + NO guard-> builds, EXIT 42          (guard is dormant)
 *     wrong fix     + NO guard -> BUILDS CLEAN, EXIT 2     ← the silent miscompile
 *     wrong fix     + guard    -> refuses, S_InvalidTypeSpecifierCombination
 *
 * The third row is the defect this row exists to prevent, reproduced on demand;
 * the fourth is the fix. The second row is what makes the guard safe to ship —
 * it changes nothing about a well-formed grammar.
 *
 * ✔REFERENCE CONTROL, each probed SEPARATELY, `-std=c2x -fsyntax-only` on the
 * same collision shape: gcc 13.3.0 (WSL), clang 18.1.3 (WSL) and mingw-w64 gcc
 * 13.2.0 all ACCEPT it and all keep the head type — `_Static_assert(sizeof(T) ==
 * sizeof(long long))` and `_Static_assert(sizeof(T) != sizeof(int))` both hold.
 * MSVC 19.51 (`cl /c /std:c17`) does not speak `__attribute__`, and its own
 * `__declspec(align(16))` spelling of the same question also keeps the head
 * type. Three of four references accept, so under
 * `DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO C` DSS must accept it too — and all four
 * agree on what it MEANS.
 */

int io(int x) { return x; }

/* ── the collisions: each decoration's name is a live typedef of `int` ─────── */

typedef int may_alias;
typedef int unused;

/* ⚠ THE DECORATION IS `may_alias` / `unused`, DELIBERATELY NOT `aligned(16)`,
 * AND THE REASON IS NOT COSMETIC. DSS refuses `__attribute__((aligned(N)))` on
 * a TYPEDEF with `S_AlignasInvalidContext` — "the alias resolves to the same
 * type as its aliasee" — which is a SEPARATE, pre-existing, deliberate refusal
 * carried by its own row ([[D-CSUBSET-ALIGNAS-TYPEDEF-PARAM-PARSE]], ⏳ gated:
 * C11/C23 6.7.5p2 makes an alignment specifier on a typedef a constraint
 * violation). ✔MEASURED through the shipped CLI on this very file: with
 * `aligned(16)` the example does not COMPILE, so it could witness nothing at
 * all. `may_alias` and `unused` have no layout sink, so the only question this
 * file asks is the one it is for — does the decoration's NAME displace the
 * head's TYPE. */

/* Slot (1): the decoration AFTER the `typedef` keyword — the
 * `typedefDeclSpecifiers` prefix. This is the row's own canonical case. */
typedef __attribute__((may_alias)) long long AfterKeywordT;

/* Slot (2): the decoration BETWEEN head and declarator — the first
 * `typedefAttrRun`. */
typedef long long __attribute__((may_alias)) MidT;

/* Slot (3): the decoration AFTER the declarator — the second
 * `typedefAttrRun`. */
typedef long long TrailingT __attribute__((may_alias));

/* A SECOND clause whose name collides, reached from inside the attribute's own
 * clause run rather than from its leading name. Both clause names are live
 * `typedef int`s, so either winning the head is observable. */
typedef __attribute__((may_alias, unused)) long long TwoClauseT;

/* The leading position (before the `typedef` keyword), which also rides the
 * specifier prefix. */
__attribute__((unused)) typedef long long LeadingT;

/* The decoration names must still name `int` everywhere else — the guard must
 * not have achieved its result by breaking the typedefs themselves. */
typedef may_alias StillIntT;

int main(void) {
    /* 8 each if the head survived; 4 each if a decoration hijacked it. */
    int a = io((int)sizeof(AfterKeywordT));
    int b = io((int)sizeof(MidT));
    int c = io((int)sizeof(TrailingT));
    int d = io((int)sizeof(TwoClauseT));
    int e = io((int)sizeof(LeadingT));

    /* 4 — the borrowed names are unharmed. */
    int f = io((int)sizeof(StillIntT));
    int g = io((int)sizeof(may_alias));

    int score = 0;
    score += (a == 8) ? 8 : 0;
    score += (b == 8) ? 8 : 0;
    score += (c == 8) ? 8 : 0;
    score += (d == 8) ? 8 : 0;
    score += (e == 8) ? 8 : 0;
    score += (f == 4) ? 1 : 0;
    score += (g == 4) ? 1 : 0;
    return score;   /* 5*8 + 2*1 = 42 */
}
