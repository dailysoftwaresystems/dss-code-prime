// D-PP-VA-SPECIAL-IDENTIFIER-NAME-POSITIONS-REFUSED-ABOVE-THE-UNION witness
// (P54 lane vh).
//
// C23 6.10.5p5 makes `__VA_ARGS__` and `__VA_OPT__` legal ONLY in a variadic
// function-like macro's replacement list. Lane `va` shipped the missing WARNING
// for every other position. It also left FOUR shapes REFUSED that every
// reference accepts -- `#define <vaOptName>`, `#undef <vaOptName>`, and either
// spelling as a macro PARAMETER NAME -- because relaxing them needed a ruling
// about what such a program MEANS. THE OPERATOR RULED (2026-09-02): ACCEPT, AND
// HONOUR THE DEFINITION.
//
// ★★ THIS EXAMPLE EXISTS BECAUSE THE UNIT SUITE CANNOT PIN "THE ARTIFACT RUNS".
// Every arithmetic contribution below is unreachable unless a relaxed shape both
// COMPILES and MEANS what the ruling says, so a compile-only entry -- or an
// `expectDiagnostics` manifest, which ✔MEASURED via the sibling
// const_bitfield_write_warns makes the runner assert the compile was REJECTED
// and never run the artifact -- would pass the moment the program merely built.
// Exit 42 is reachable only if all nine hold at once.
//
// ✔MEASURED 2026-09-02, each reference toolchain invoked SEPARATELY -- gcc
// 13.3.0 (WSL), gcc 13.2.0 (mingw-w64 native), clang 18.1.3 (WSL), cl
// 19.51.36252 -- acceptance probed with the defined name NEVER USED, so no
// reference's MEANING choice could be mistaken for a refusal:
//
//   shape                     gcc    mingw   clang   cl      union
//   #define __VA_ARGS__ 42    rc 0   rc 0    rc 0    rc 0    ACCEPT
//   #define __VA_OPT__  42    rc 0   rc 0    rc 0    rc 0    ACCEPT
//   #undef  __VA_ARGS__       rc 0   rc 0    rc 0    rc 0    ACCEPT
//   #undef  __VA_OPT__        rc 0   rc 0    rc 0    rc 0    ACCEPT
//
// Acceptance is UNANIMOUS, so `DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO C` forbids
// refusing. On MEANING the split is 3-1: gcc, mingw gcc and clang HONOUR the
// definition (`_Static_assert(__VA_OPT__ == 42)` passes and the built program
// exits 0 on all three); cl answers C4117 "macro name '__VA_OPT__' is reserved,
// '#define' ignored" and DISCARDS it, and ignores the `#undef` too. DSS follows
// the three -- and the tie-break is not the head-count but the failure class:
// MSVC's alternative SILENTLY DROPS CODE THE AUTHOR WROTE, which is the one
// failure class this project refuses outright. Same reasoning that decided
// `#pragma once`.
//
// ⚠⚠ NO SINGLE REFERENCE BUILDS THIS WHOLE FILE, AND THAT IS A PROPERTY OF THE
// UNION RATHER THAN A WEAKNESS OF THE ENTRY. The bar is a per-CONSTRUCT
// disjunction: every shape below is accepted by at least one reference, but no
// one reference accepts all of them, so a composite witness is necessarily
// DSS-only. ✔MEASURED 2026-09-02 rather than asserted -- each reference was
// handed THIS FILE with ONLY the shapes IT refuses removed, and each then
// produced exactly the arithmetic this file's remaining claims predict:
//
//   reference            shape removed                    exit   = 42 minus
//   gcc 13.3.0 (WSL)     SHADOW_ARGS                       36     6
//   gcc 13.2.0 (mingw)   SHADOW_ARGS                       36     6
//   clang 18.1.3         OPT_PARAM + SHADOW_OPT            32     2 + 8
//   cl 19.51.36252       (every `#define`/`#undef` of      --     n/a
//                         either spelling is DISCARDED)
//
// So the eight or nine claims each reference CAN express agree with DSS to the
// integer. gcc and mingw gcc refuse only `SHADOW_ARGS`, and their reason is an
// implementation artefact rather than a reading of the standard -- they name
// their own variadic parameter `__VA_ARGS__` internally, so an explicit
// parameter of that spelling collides ("duplicate macro parameter"); clang and
// cl accept the identical line and bind the parameter, which is why the shadow
// rule stays. clang refuses any replacement-list `__VA_OPT__` not followed by
// `(`, which is its own reading and does not narrow the union.
//
// cl is the one whose refusal is a MEANING divergence rather than a shape
// verdict, and it is RULED. Pinned in the suite by
// Preprocessor.SpecialVariadicNameMeaningDivergesFromMsvcByRuling so a later
// cycle applying the disjunction by reflex has to come and change a test that
// says why.
//
// ★★ THE SHADOW RULE, and the measurement that chose it. Accepting the
// parameter name means one spelling can name both a declared parameter and an
// engine-provided construct in the same replacement list. THE DECLARED PARAMETER
// WINS. ✔MEASURED on the only shape where the two readings differ observably --
// `#define F(__VA_OPT__, ...) (__VA_OPT__ (0))` -- gcc, mingw gcc and cl all
// substitute the PARAMETER and then fail downstream on `7 (0)` ("called object
// is not a function" / C2064), a type error in the user's program rather than a
// preprocessing refusal, while only clang reads the operator. 3-1 for the
// parameter. DSS answers `S_NotCallable` on the same source: the same reading as
// the majority, reported at the same tier.
//
// ⚠ RELAXING DID NOT GO SILENT. All four shapes remain 6.10.5p5 constraint
// violations and every reference diagnoses them, so DSS still emits the Warning
// lane `va` added -- per OCCURRENCE, and `--warnings-as-errors` reproduces the
// references' `-pedantic-errors` / `/W4 /WX` arm. Going silent would have put
// DSS below the union on the diagnostic axis, which is the gap `va` closed.
//
// ★ 100% CONFIG DRIVEN: every spelling below is read from the active language
// document (`preprocess.variadicArgsName` / `preprocess.vaOptName`), never from
// a literal in `src/`. Pinned by
// Preprocessor.SpecialVariadicNamePositionsAreConfigDriven, which rebinds
// `vaOptName` to `DSS_OPT` and shows all three arms FOLLOW it.
//
// EXIT 42 = 5 + 7 + 4 + 2 + 6 + 8 + 3 + 3 + 4, and every addend rides a
// different one of the nine claims, so none can be dropped without changing the
// answer.

/* (1)(2) THE DEFINITIONS ARE HONOURED. Before the ruling `#define __VA_OPT__`
   was refused outright (P_PreprocessorOperatorNameNotDefinable, an Error and a
   member of kUnsuppressableCodes); `#define __VA_ARGS__` already warned and
   applied. Both are now ordinary object-like macros. */
#define __VA_ARGS__ 5
#define __VA_OPT__  7

/* (3)(4) EITHER SPELLING AS A MACRO PARAMETER NAME, in a NON-variadic
   function-like macro. Both used to be refused; both now bind. */
#define ARGS_PARAM(__VA_ARGS__) ((__VA_ARGS__) + 1)
#define OPT_PARAM(__VA_OPT__)   ((__VA_OPT__) + 1)

/* (5)(6) THE SHADOW RULE inside a VARIADIC macro, where the parameter and the
   engine construct claim the same spelling. The PARAMETER wins in both:
   SHADOW_ARGS yields its FIRST argument, not the variadic tail, and SHADOW_OPT
   yields its first argument rather than being read as a va-opt introducer. */
#define SHADOW_ARGS(__VA_ARGS__, ...) (__VA_ARGS__)
#define SHADOW_OPT(__VA_OPT__, ...)   (__VA_OPT__)

/* (7)(8) THE ENGINE CONSTRUCTS STILL WORK despite the definitions above. Both
   are matched by CONFIG TEXT inside a variadic replacement list and never
   through the program's macro table, so `#define __VA_OPT__ 7` cannot reach
   them. If it could, PICK(2, 1) would splice `7 (+)` and `5` into the
   expansion instead of computing 3. */
#define PICK(first, ...) (first __VA_OPT__(+) __VA_ARGS__)

/* Declared here, DEFINED after the `#undef` pair below -- claim (9) needs the
   operator to be used on the far side of the `#undef`, and C23 has no implicit
   function declaration to fall back on. */
int tail(void);

int main(void) {
    /* (1)(2) the honoured definitions, read from ORDINARY PROGRAM TEXT. */
    int const a = __VA_ARGS__;          /* 5  */
    int const b = __VA_OPT__;           /* 7  */

    /* (3)(4) the parameter names bind. */
    int const c = ARGS_PARAM(3);        /* 4  */
    int const d = OPT_PARAM(1);         /* 2  */

    /* (5)(6) the shadow rule: the declared parameter beats the catch-all and
       beats the va-opt introducer. A catch-all reading would make `e` 99. */
    int const e = SHADOW_ARGS(6, 99);   /* 6  */
    int const f = SHADOW_OPT(8, 99);    /* 8  */

    /* (7)(8) the va-opt operator still FIRES and still ELIDES. */
    int const g = PICK(2, 1);           /* 3  */
    int const h = PICK(3);              /* 3  */

    /* (9) rides in `tail()`, on the far side of the `#undef` pair. */
    return a + b + c + d + e + f + g + h + tail();
}

/* (9) THE `#undef` HALF TAKES EFFECT. Both names are removed -- if either
   `#undef` were merely diagnosed and dropped, the `#ifdef` below would be true
   and this file would not translate. And the va-opt OPERATOR must survive the
   `#undef` untouched, which is what `tail()` returns. */
#undef __VA_ARGS__
#undef __VA_OPT__

#ifdef __VA_ARGS__
#error the #undef of the catch-all spelling must REMOVE the definition
#endif
#ifdef __VA_OPT__
#error the #undef of the va-opt spelling must REMOVE the definition
#endif

#define PICK2(first, ...) (first __VA_OPT__(+) __VA_ARGS__)

int tail(void) {
    return PICK2(2, 2);                 /* 4  */
}
