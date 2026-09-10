// D-COMPILER-INPUT-PROPORTIONAL-RECURSION-RESIDUE-UNCONVERTED-AND-UNCAPPED (P60,
// lane rc): a RUNNABLE witness that the parser's two former host recursions --
// the speculation drive and the paren/postfix-body atom re-entry -- are gone,
// and that the ceilings that guarded them rose to what the references WORK at.
//
// Two shapes, each ONE PAST an old ceiling and well inside the new one:
//   * 1300 nested parentheses  -- past the old `parser.maxExpressionDepth` of
//     1024 (now 16384), the shape that cost the parser one host frame per `(`;
//   * 400 nested `(int)` casts  -- past the old `parser.maxSpeculationDepth` of
//     320 (now 2048), the shape that cost it eight host frames per level.
// Before P60 `dsscp` REFUSED this file (a positioned `P_ExpressionTooDeep` /
// `P_MaxSpeculationDepth`), so there was no binary to run; rc 42 proves both
// that the nests PARSE and that they mean what C says they mean -- a paren nest
// and an int cast chain are the identity, so a dropped or duplicated level would
// change the sum.
//
// MEASURED reference matrix, each probed SEPARATELY (see the c.lang.json
// `$parserComment`): gcc 13.3.0 compiles 16384 parens and 65536 casts;
// mingw-w64 gcc 13.2.0 2048 parens / 32768 casts; MSVC 19.51 1400 parens and
// 2985 casts, refusing LOUDLY (C1026) beyond; clang 18.1.3 refuses more than
// 256 brackets LOUDLY and crashes past 1349 casts. A reference that refuses
// loudly casts no vote; gcc, mingw-w64 AND MSVC all compile THIS file, so it is a
// legal program DSS must accept.
//
// ANTI-FOLD: both nests start from MUTABLE GLOBALS, so no front-end arm can
// fold either away and report green without having parsed it. The `release`
// arm additionally proves the result survives Mem2Reg/CSE/LICM/DCE with the
// same exit code.
//
// exit = 12 + 30 = 42.

int g12 = 12;
int g30 = 30;

// 1300 nested parentheses around a mutable global.
static int parens(void)
{
    return
        ((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((g12))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))));
}

// 400 nested casts around a mutable global.
static int casts(void)
{
    return
        (int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)(int)g30;
}

int main(void)
{
    return parens() + casts();
}
