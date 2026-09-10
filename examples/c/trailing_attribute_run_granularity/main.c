// P56 (D-CSUBSET-TRAILING-ATTRIBUTE-RUN-IS-READ-AT-THE-WRONG-GRANULARITY): a
// trailing attribute run belongs to exactly one of three things — the
// DECLARATION (every declarator), THIS DECLARATOR (the entity just named), or
// the TYPE — and ONE config key used to answer for all three. Each consumer read
// it at whichever grain it happened to want, which put DSS ABOVE the union on
// one construct and BELOW it on two others AT THE SAME TIME.
//
// ★★★ WHAT THIS PROGRAM WITNESSES, AND WHY IT IS A WARNING SET RATHER THAN AN
// EXIT CODE ALONE. The grain decides WHICH ENTITY a fact lands on. For
// `deprecated` that fact is observable as a diagnostic AT THE USE SITE, so the
// witness is the EXACT SET of `S_DeprecatedSymbolUsed` reports plus a program
// that still runs: every use below is deliberate, and a use that is NOT listed
// in `expected.json` is a symbol that must NOT carry the flag. Both directions
// are therefore pinned by one artifact — an over-application shows up as an
// unexpected warning, a drop as a missing one.
//
// ★★ THE MEASURED AXIS, AND IT IS NOT THE ONE THE ROW ASSUMED. ✔MEASURED
// 2026-09-03, four attributes (`deprecated`, `nodiscard`, `maybe_unused`,
// `noreturn`) x two declarator shapes, EACH REFERENCE PROBED SEPARATELY: the
// answer for a C23 `[[...]]` run written after a declarator is constant per
// SHAPE and does not vary by attribute.
//
//   int  x       [[deprecated]];   gcc 13.3.0 warns at the USE · clang 18.1.3
//                                  warns at the USE · MSVC 19.51 /std:clatest
//                                  C4996 at the USE            => the ENTITY
//   int  arr[3]  [[deprecated]];   gcc `'deprecated' attribute ignored` ·
//                                  clang `error: cannot be applied to types`
//                                  (rc=1) · MSVC `C4649 attributes are ignored
//                                  in this context`            => the TYPE
//   void f(void) [[deprecated]];   the same three verdicts      => the TYPE
//
// C23 6.7.6p1 puts the sequence written after an IDENTIFIER declarator on the
// declared entity, while 6.7.6.2p1 and 6.7.6.3p1 put the one written after an
// array- or function-declarator on the array or function TYPE.
//
// ⚠⚠ AND THE GNU SPELLING HAS NO SUCH SPLIT — ✔MEASURED, `__attribute__((...))`
// confers on the ENTITY after an identifier, an array declarator, a function
// declarator and a function-pointer declarator alike, in gcc AND clang. (MSVC
// implements no `__attribute__` in any position — C2061 at /std:c11, /std:c17
// AND /std:clatest — so it ABSTAINS on the GNU spelling; an abstention is not
// agreement.) That is why the grain is declared PER RULE in `c.lang.json`
// rather than once for the whole after-declarator position.
//
// ★★★ THE REASON THIS COULD NOT BE FIXED BY NARROWING ONE LOOP, WHICH IS WHAT
// THE ROW'S CLOSING WORK INSISTS ON. ✔MEASURED at the base commit through the
// shipped CLI, DSS conferred on BOTH `void f(void) [[deprecated]];` (above the
// union — no reference confers) and `int x [[deprecated]];` (correct — all three
// confer), out of the SAME over-broad "fold the whole trailing run" read.
// Excluding `stdAttr` from the entity roots fixes the first and BREAKS the
// second; including it does the reverse. Only a grain that can depend on the
// declarator gets both right.
//
// ★ WHAT THE REFERENCES DO WITH THIS FILE, stated exactly rather than claimed
// clean, because one of them REFUSES it and that refusal is the point.
// ✔MEASURED 2026-09-03:
//   • gcc 13.3.0 `-std=c2x -Wall -Wextra` compiles it and the binary exits 42.
//     Its diagnostics are the five `-Wdeprecated-declarations` notes this file
//     intends, plus `-Wattributes` "attribute ignored" for each C23 run that
//     appertains to a type — which is gcc SAYING, in its own words, that those
//     two runs confer nothing.
//   • MSVC 19.51 `/std:clatest` accepts the same constructs, with `C4996` at the
//     intended uses and `C4649 attributes are ignored in this context` for the
//     type-appertaining runs. It refuses the GNU spellings (C2061), so it
//     abstains on those.
//   • clang 18.1.3 REFUSES the file: `error: 'deprecated' attribute cannot be
//     applied to types` (rc=1) for `void depFn(void) [[deprecated]];` and for
//     `depArr`. Under the standing ruling that the disjunction decides
//     ACCEPTANCE, two references that accept make the construct acceptable — and
//     clang's refusal is a THIRD vote that it must not CONFER, which is what
//     this file pins.
//
// RED-ON-DISABLE (REMOVE direction): in `src/dss-config/sources/c.lang.json`,
// change `stdAttr`'s `appertainsTo` in `declarators.afterDeclaratorAttrRules`
// from `declaratorUnlessTypeDerived` to `declarator` and this example FAILS IN
// BOTH RUNNERS with two UNEXPECTED `S_DeprecatedSymbolUsed` reports (`depFn` and
// `depArr`), while `trailing_noreturn_attribute` — whose spellings are all GNU —
// stays green.
#include <stdlib.h>

int accumulated = 0;

// ── (1) C23, IDENTIFIER declarator: appertains to the ENTITY. ────────────────
// All three references confer. Its use below MUST warn.
int depObj [[deprecated]];

// ── (2) C23, ARRAY declarator: appertains to the array TYPE. ─────────────────
// gcc ignores it, clang refuses the program, MSVC ignores it with C4649. Its use
// below must NOT warn — and the array is genuinely used, so a missing warning is
// the assertion and not an absence of evidence.
int depArr[3] [[deprecated]];

// ── (3) C23, FUNCTION declarator: appertains to the function TYPE. ───────────
// The construct the row was opened on. Called below; must NOT warn.
void depFn(void) [[deprecated]];

// ── (4) THE CONTROL THAT KEEPS (2) AND (3) NON-VACUOUS. ──────────────────────
// The GNU spelling in the SAME two shapes DOES confer, in gcc and clang alike.
// If the trailing run had simply stopped being read, these two would go silent
// and the example would red here rather than passing for the wrong reason.
int gnuArr[3] __attribute__((deprecated));
void gnuFn(void) __attribute__((deprecated));

// ── (5) GRANULARITY WITHIN ONE DECLARATION — the sibling must stay clean. ────
// ✔MEASURED, gcc and clang both warn at a use of `depFirst` and are both silent
// at a use of `keptSibling`. Leaking leftward would be a fact on a symbol the
// programmer never annotated.
int depFirst __attribute__((deprecated)), keptSibling;

// ── (6) A TRAILING DECLARATION-LEVEL SLOT — the LAST declarator only. ────────
// `typedefTrailingAttrRun` is written after the alias list, and ✔MEASURED gcc
// and clang both give `typedef int A, B __attribute__((deprecated));` to **B**
// alone. Before the split this run had no grain to be read at, so the whole slot
// conferred on nobody: DSS was BELOW the union here.
typedef int KeptAlias, DeprecatedAlias __attribute__((deprecated));

// ── (7) …and the LEADING typedef run still reaches EVERY alias. ─────────────
// The control for (6): it proves (6)'s negative is about the slot's GRAIN and
// not about a typedef scan that stopped seeing attributes.
typedef __attribute__((deprecated)) int LeadDepA, LeadDepB;

void depFn(void) {
    accumulated += 1;
}

void gnuFn(void) {
    accumulated += 2;
}

// Uses of the ENTITY-conferring spellings — each of these MUST warn.
int useEntityConferring(void) {
    int total = depObj;                 // (1) C23 identifier shape  -> warns
    gnuFn();                            // (4) GNU function shape    -> warns
    total += gnuArr[0];                 // (4) GNU array shape       -> warns
    total += depFirst;                  // (5) the decorated slot    -> warns
    DeprecatedAlias b = 4;              // (6) the LAST alias        -> warns
    LeadDepA la = 5;                    // (7) leading reaches all   -> warns
    return total + b + la;
}

// Uses of the TYPE-appertaining and undecorated spellings — each of these must
// be SILENT. The exact-set semantics of `expectWarnings` is what turns each of
// these lines into a live negative assertion rather than a comment.
int useNonConferring(void) {
    int total = depArr[0];              // (2) C23 array shape   -> silent
    depFn();                            // (3) C23 function shape-> silent
    total += keptSibling;               // (5) the sibling       -> silent
    KeptAlias a = 6;                    // (6) the FIRST alias   -> silent
    return total + a;
}

int main(void) {
    depObj = 1;
    depArr[0] = 2;
    gnuArr[0] = 3;
    depFirst = 4;
    keptSibling = 5;

    if (useEntityConferring() != 1 + 3 + 4 + 4 + 5) {
        return 1;
    }
    if (useNonConferring() != 2 + 5 + 6) {
        return 2;
    }
    // depFn() and gnuFn() each ran exactly once inside the two helpers.
    if (accumulated != 3) {
        return 3;
    }
    return 42;
}
