// D-PP-VA-SPECIAL-IDENTIFIER-OUTSIDE-REPLACEMENT-LIST witness (P54 lane va).
//
// C23 6.10.5p5 is a CONSTRAINT: `__VA_ARGS__` and `__VA_OPT__` "shall occur only
// in the replacement-list of a function-like macro that uses the ellipsis
// notation". DSS used to enforce it ONLY inside a `#define`; in ORDINARY PROGRAM
// TEXT both spellings passed as plain identifiers in complete silence.
//
// ★★ THIS EXAMPLE IS RUNNABLE ON PURPOSE, and that is the whole content of the
// boundary decision rather than a convenience. The unit suite
// (Preprocessor.SpecialVariadicIdentifier*) pins THAT the diagnostic is emitted
// and that it is a WARNING; only an EXECUTING artifact can pin that the program
// is still BUILT and still computes the right answer. ⚠ An `expectDiagnostics`
// manifest could not say this: ✔MEASURED by the sibling
// const_bitfield_write_warns, the runner treats any manifest carrying that key
// as an EXPECT-ERROR entry, asserts the compile was REJECTED, and never runs the
// artifact — the exact verdict this shape must NOT draw.
//
// ✔MEASURED 2026-09-02, each reference toolchain invoked SEPARATELY,
// compile-only, over the whole position axis below:
//   gcc 13.3.0 (WSL, -std=c2x)      warning, rc 0  |  -pedantic-errors: error
//   gcc 13.2.0 (mingw-w64)          warning, rc 0  |  -pedantic-errors: error
//   clang 18.1.3 (WSL, -std=c2x)    __VA_OPT__: warning (-Wvariadic-macros),
//                                   rc 0; __VA_ARGS__: SILENT at default,
//                                   warning under -Wpedantic
//                                                  |  -pedantic-errors: error
//   MSVC 19.51.36252 (/std:c17)     C5100 / C5108 warning, rc 0
//                                                  |  /W4 /WX: error C2220
// EVERY reference ACCEPTS the translation unit at its DEFAULT settings and every
// one errors only in its strict mode. `DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO C` and
// THE DISJUNCTION DECIDES ACCEPTANCE, so refusing outright would put DSS ABOVE
// the union — rejecting a program all three compile — while staying silent left
// it BELOW the union on the diagnostic axis. A Warning is the only answer level
// on both axes at once, and `--warnings-as-errors` reproduces the references'
// strict arm for anyone who wants it. ISO C agrees rather than forcing the other
// reading: 5.1.1.3 requires a DIAGNOSTIC for a constraint violation, and 4p3
// permits a conforming implementation to translate the program after issuing
// one.
//
// ⚠ THE ROW'S OWN HEADLINE WAS REFUTED BY THIS MEASUREMENT. It recorded
// "clang-18/clang-19/gcc-13 reject", which is true ONLY under
// `-pedantic-errors`; at default settings all four references compile. Had that
// been taken at face value the fix would have been a REFUSAL, and this example
// would not build.
//
// ⚠⚠ THREE OF THE FOUR REFERENCES BUILD *THIS FILE* AND RUN IT TO 42; MSVC DOES
// NOT, AND THE REASON IS A MEANING DIVERGENCE RATHER THAN A VERDICT ON THE
// CONSTRAINT. ✔MEASURED 2026-09-02: gcc 13.3.0 (WSL), gcc 13.2.0 (mingw-w64) and
// clang 18.1.3 each compile this source at default settings and the built
// program exits 42 — byte-for-byte the same answer DSS gives. cl 19.51.36252
// treats both spellings as BUILT-IN MACROS THAT EXPAND TO NOTHING (it prints
// "note: in expansion of macro '__VA_ARGS__'" and then `error C2513: 'int': no
// variable declared before '='`), so `int __VA_ARGS__ = 20;` becomes `int = 20;`
// there. MSVC's own diagnostic FOR THE CONSTRAINT is still only a WARNING
// (C5100 / C5108) and it exits 0 on every shape where the erasure does not
// break the syntax — so its ACCEPTANCE vote is with the others, and only what
// the identifier MEANS differs. DSS follows gcc and clang: the spelling is an
// ordinary identifier. That is the behaviour DSS already had; this change adds
// the diagnostic without moving the meaning.
//
// THE POSITION AXIS, every one of which warns and still compiles here:
//   (1) a file-scope DECLARATOR name, both spellings
//   (2) a STRUCT MEMBER name, both spellings
//   (3) a LABEL
//   (4) an ARGUMENT of a variadic macro invocation — an argument list is not a
//       replacement list
//   (5) a `#if` CONTROLLING EXPRESSION (an undefined identifier folds to 0)
// and the two that must stay COMPLETELY silent:
//   (6) the ONE legitimate position — both spellings inside a variadic
//       function-like macro's replacement list
//   (7) an ELIDED `#if 0` branch. ✔MEASURED: all four references are silent
//       there too; a skipped group is only parsed far enough to track nesting
//       (C 6.10p1), so diagnosing would be a warning DSS invents alone.
//
// RED-ON-DISABLE, AND THE TWO DIRECTIONS ANSWER DIFFERENT QUESTIONS — both were
// run, neither is asserted from the armchair.
//   * REMOVE direction (delete the `specialVariadicSpelling` call from
//     `Preprocessor::run`'s live-token path and the
//     `scanLineForSpecialVariadicIdentifiers` call at the top of
//     `handleDirective`): ✔MEASURED — the four `Preprocessor.SpecialVariadic*` /
//     `VaArgsNameAsMacroName*` unit tests go red BY NAME while THIS EXAMPLE
//     STAYS GREEN. ⚠ That is correct and deliberate, not a hole: deleting a
//     warning leaves the program perfectly buildable, so an entry whose
//     assertion is "exit 42" cannot see it. The diagnostics are pinned by the
//     unit suite; this entry pins something the unit suite cannot.
//   * REFUSE direction (flip this diagnostic's severity to Error, i.e. "fix" the
//     constraint by rejecting): ✔MEASURED — BOTH runners fail this entry with
//     `compile rc == 0 (rc=1)` and quote `error[P_PreprocessorDirective]` for
//     both spellings. Exit 42 becomes unreachable, which is exactly the claim
//     this entry exists to hold.
// Object md5 pinned and asserted MOVED then RETURNED in both transcripts.
//
// ★ 100% CONFIG DRIVEN: both spellings are read from the active language
// document (`preprocess.variadicArgsName` / `preprocess.vaOptName`), never from
// a literal in `src/`. Pinned by
// Preprocessor.SpecialVariadicIdentifierConstraintIsConfigDriven, which rebinds
// the catch-all to `__REST__` and shows the constraint FOLLOW it.
//
// EXIT 42 = 20 + 15 + 4 + 3, and every addend rides a different position on the
// axis, so no single one of them can be dropped without changing the answer.

/* (6) THE LEGITIMATE POSITION — both spellings inside a VARIADIC function-like
   macro's replacement list. This must draw nothing at all, in this file and
   under `--warnings-as-errors`. */
#define PICK(first, ...) (first __VA_OPT__(+) __VA_ARGS__)

/* (7) An ELIDED branch: the violation inside it is unreachable, so it is not
   diagnosed. */
#if 0
int __VA_ARGS__ = 999;
int __VA_OPT__  = 999;
#endif

/* (1) File-scope declarator names. Both spellings are ordinary identifiers
   here — the constraint is violated, the program is still translated. */
int __VA_ARGS__ = 20;
int __VA_OPT__  = 15;

/* (2) Struct member names. */
struct Slots {
    int __VA_ARGS__;
    int __VA_OPT__;
};

/* (5) A `#if` controlling expression. The spelling is not a defined macro, so
   it folds to 0 and the dead arm is not taken — the point is the diagnostic on
   the CONTROLLING LINE, not the branch. */
#if __VA_ARGS__
#error the catch-all identifier is not a defined macro, so this arm is dead
#endif

int main(void) {
    struct Slots s;
    s.__VA_ARGS__ = 4;
    s.__VA_OPT__  = 3;

    /* (6) again, at the point of use: PICK's variable arguments are non-empty,
       so `__VA_OPT__(+)` yields `+` and the sum is 0 + 0 = 0. Silent. */
    int const legit = PICK(0, 0);

    /* (4) A variadic macro's ARGUMENT is not its replacement list: the spelling
       below is the ordinary file-scope variable declared above, passed by
       value, and PICK's variable-argument part is empty so it contributes
       nothing. Silent would be wrong; refusing would be wronger. */
    int const viaArg = PICK(__VA_ARGS__) - 20;

    /* (3) A label. */
    goto __VA_ARGS__;
__VA_ARGS__:
    return __VA_ARGS__ + __VA_OPT__ + s.__VA_ARGS__ + s.__VA_OPT__ + legit
           + viaArg;
}
