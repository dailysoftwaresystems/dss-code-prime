// D-PP-VA-SPECIAL-IDENTIFIER-IN-A-NONVARIADIC-BODY-REFUSED-ABOVE-THE-UNION
// witness (P54 lane `ob`).
//
// C23 6.10.5p5 is a CONSTRAINT: `__VA_ARGS__` and `__VA_OPT__` "shall occur only
// in the replacement-list of a function-like macro that uses the ellipsis
// notation". Lane `va` gave the violation a Warning everywhere OUTSIDE a
// `#define`; lane `vh` relaxed the two macro-PARAMETER positions and the
// `#define`/`#undef` NAME positions. THE LAST POSITION IN THE FAMILY IS THIS
// ONE -- either spelling inside a NON-VARIADIC replacement list, with no
// parameter of that name -- and DSS REFUSED IT OUTRIGHT: `#define OBJ
// __VA_ARGS__` was an Error from `handleDefine`'s catch-all guard and `#define
// OBJ __VA_OPT__` an Error from `validateVaOpt`. THE OPERATOR RULED 2026-09-02:
// ACCEPT AND WARN, the same treatment the other shapes got.
//
// ✔MEASURED 2026-09-02 (lane `ob`), each reference toolchain invoked
// SEPARATELY, compile-only, DEFAULT and STRICT columns, with the macro NEVER
// USED so that no reference's MEANING choice could masquerade as a refusal:
//
//   shape                        gcc 13.3.0  mingw 13.2.0  clang 18.1.3   cl 19.51
//   #define OBJ  __VA_ARGS__     warn rc 0   warn rc 0     SILENT rc 0    C5100 rc 0
//   #define OBJ  __VA_OPT__      warn rc 0   warn rc 0     warn rc 0      C5108 rc 0
//   #define OBJ  __VA_OPT__(x)   warn rc 0   warn rc 0     warn rc 0      C5108 rc 0
//   #define F(a) __VA_ARGS__     warn rc 0   warn rc 0     SILENT rc 0    C5100 rc 0
//   #define F(a) __VA_OPT__(a)   warn rc 0   warn rc 0     warn rc 0      C5108 rc 0
//   #define F(a) __VA_OPT__      warn rc 0   warn rc 0     ERROR  rc 1    C5108 rc 0
//
// STRICT turns every one of them into an error: `-pedantic-errors` for gcc,
// mingw gcc and clang, `/W4 /WX` (C2220) for cl. `DSS = (gcc ∪ clang ∪ MSVC) ∪
// ISO C` and THE DISJUNCTION DECIDES ACCEPTANCE, so refusing put DSS ABOVE the
// union; going silent would put it BELOW on the diagnostic axis, which is the
// gap lane `va` closed. A Warning is the only answer level on both axes at
// once, and the strict arm stays AVAILABLE rather than imposed: ✔MEASURED that
// this file under `--warnings-as-errors` exits rc 1 with EIGHT errors and zero
// warnings, which is the references' `-pedantic-errors` / `/W4 /WX` posture
// reproduced on demand.
//
// ⚠⚠ THE BRIEF'S PREMISE -- "all four references accept" -- IS FALSE FOR ONE
// MEMBER OF THE FAMILY AND THAT SHAPE IS IN THIS FILE ON PURPOSE. clang 18.1.3
// hard-errors `missing '(' following __VA_OPT__` on a BARE va-opt spelling in a
// FUNCTION-LIKE non-variadic body, and NOT on the same spelling in an
// object-like one -- so its rule is "inside a function-like replacement list
// this identifier introduces a construct", applied without regard to
// variadicity. The disjunction still says ACCEPT 3-1, so DSS accepts; but the
// union here is per-CONSTRUCT rather than per-reference, exactly as lane `vh`
// measured it from the other side.
//
// ⚠ THE MEANING, ✔MEASURED AND NOT ASSUMED: the token is an ORDINARY
// IDENTIFIER and passes through verbatim. Preprocessed with `-E`, `#define OBJ
// __VA_ARGS__` yields the token `__VA_ARGS__` on gcc, mingw gcc and clang, and
// the built program exits 0 on all three. TWO references lose code here and DSS
// follows neither: cl 19.51.36252 DELETES both spellings outright (`int = 42;`,
// `return == 42`), and clang ELIDES a USED `__VA_OPT__( ... )` even inside a
// non-variadic macro -- `#define H(a) __VA_OPT__(a)` invoked as `H(42)`
// disappears under clang while gcc and mingw gcc substitute the parameter and
// call the function. Same tie-break as `#pragma once` and as `#define
// __VA_OPT__ 42`: the reading that silently drops what the author wrote loses.
//
// ⚠⚠ ONLY THE TWO gcc REFERENCES BUILD THIS WHOLE FILE, and that is a property
// of a per-construct union rather than a weakness of the entry. ✔MEASURED
// 2026-09-02, each reference handed THIS FILE with only the shapes IT refuses
// removed:
//   * gcc 13.3.0 (WSL) and gcc 13.2.0 (mingw) build it UNCHANGED, exit 42, and
//     emit EIGHT warnings each -- the same count, on the same eight source
//     occurrences, that DSS emits and that `expectWarnings` declares below.
//     That agreement is the cross-check on the position list.
//   * clang 18.1.3 refuses TWO shapes, not one: `ADD_OPT` and `OPT_BARE_FN`,
//     both a BARE va-opt spelling in a FUNCTION-LIKE body. With both removed
//     and the `d` addend zeroed it builds and exits 33 (= 42 − 9), emitting
//     THREE warnings -- it is silent about `__VA_ARGS__` at default settings
//     and speaks only about `__VA_OPT__`.
//     ⚠ THIS REFUTED THIS FILE'S OWN FIRST DRAFT, which named one shape.
//   * cl 19.51.36252 cannot build it at all, and not because of the constraint:
//     its erasure of both spellings turns `int __VA_ARGS__ = 5;` into
//     `int = 5;` (C2513). Its vote on ACCEPTANCE is still with the others --
//     every 6.10.5p5 report it makes here is a C5100/C5108 WARNING.
//
// ★ 100% CONFIG DRIVEN: both spellings come from the active language document
// (`preprocess.variadicArgsName` / `preprocess.vaOptName`), never from a
// literal in `src/`. Pinned by
// Preprocessor.SpecialVariadicIdentifierConstraintIsConfigDriven, which rebinds
// the catch-all to `__REST__` and shows the constraint FOLLOW it, and by
// Preprocessor.NonVariadicBodyAcceptanceIsConfigDriven for THIS position.
//
// ★★ WHY `expectWarnings` AND NOT A SECOND INSTRUMENT. Lane `fw` landed that
// key in BOTH runners hours earlier in this same cycle, and it is the exact
// shape of this claim: the compile SUCCEEDS, the artifact is still built, still
// spawned and still checked, AND the declared codes were emitted at Warning
// severity at the declared positions. Before it, an entry could say "must be
// REFUSED with these diagnostics" (`expectDiagnostics`) or "must build and exit
// 42" and never both -- the sibling `preprocessor_va_special_identifier_
// ordinary_code`, written hours before the key existed, splits that claim
// across the unit suite and its exit code and says so in its own header.
//
// EXIT 42 = 5 + 7 + 6 + 9 + 15 + 0, six addends over six shapes, none removable
// without changing the answer.

/* ── THE CONTROL ───────────────────────────────────────────────────────────
   The ONE legitimate position. It must draw NOTHING, and the va-opt operator
   must still FIRE and still ELIDE -- accepting the spelling elsewhere must not
   cost the engine construct where it is legal. */
#define PICK(first, ...) (first __VA_OPT__(+) __VA_ARGS__)

/* Ordinary objects for the non-variadic bodies below to denote. Each declarator
   is itself a 6.10.5p5 violation in ordinary program text -- the position lane
   `va` closed -- so each warns here too, and the two rows' diagnostics are the
   same one code at different positions. */
int __VA_ARGS__ = 5;
int __VA_OPT__  = 7;

/* (A) the catch-all spelling in an OBJECT-LIKE replacement list. THIS IS THE
   SHAPE THE ROW WAS FILED ON, and it was a hard Error until this cycle. */
#define ARGS_OBJ __VA_ARGS__

/* (B) the va-opt spelling in an OBJECT-LIKE replacement list. Refused by a
   different code path (`validateVaOpt`) for a different stated reason, which is
   why both had to be retired to close one row. */
#define OPT_OBJ __VA_OPT__

/* (C) the catch-all in a NON-VARIADIC FUNCTION-LIKE replacement list. The macro
   HAS a parameter list, so this is not the object-like path. */
#define ADD_ARGS(a) ((a) + __VA_ARGS__)

/* (D) the BARE va-opt spelling in a NON-VARIADIC FUNCTION-LIKE replacement
   list -- one of the TWO shapes clang 18.1.3 hard-errors on (`missing '('
   following __VA_OPT__`), accepted here 3-1 by the disjunction. */
#define ADD_OPT(a) ((a) + __VA_OPT__)

/* (E) and (F) are DEFINED AND DELIBERATELY NEVER INVOKED: the DEFINITION is the
   entire claim, and it is the same way the references were probed. Both used to
   die inside `validateVaOpt` -- (E) on its `!def.isVariadic` arm and (F), had
   that arm been the only one removed, on `must be followed by '('`. The fix is
   an early return for a non-variadic macro rather than the deletion of one arm,
   because in a non-variadic body there is no va-opt-replacement for 6.10.5.1p3
   to constrain at all.
   ⚠ (F) IS THE SECOND SHAPE clang 18.1.3 REFUSES, (D) above being the first.
   Both stay in the file: the union is read construct by construct, and 3-1 for
   acceptance is still acceptance. */
#define OPT_CALL_SHAPE(x) __VA_OPT__(x)
#define OPT_BARE_FN(a) __VA_OPT__

int main(void) {
    /* The spelling denotes the ordinary object -- gcc's and clang's reading,
       not MSVC's erasure. */
    int const a = ARGS_OBJ;      /* 5 */
    int const b = OPT_OBJ;       /* 7 */
    int const c = ADD_ARGS(1);   /* 6 */
    int const d = ADD_OPT(2);    /* 9 */

    /* The control, at the point of use: non-empty variable arguments, so
       `__VA_OPT__(+)` yields `+` and the sum is 10 + 5. */
    int const e = PICK(10, 5);   /* 15 */

    /* The control again, ELIDING: no variable arguments, so `__VA_OPT__(+)`
       yields nothing and the value is `first` alone. */
    int const f = PICK(0);       /* 0 */

    return a + b + c + d + e + f;
}
