/* c12 D-PP-IF-CHAR-CONSTANT runtime witness.
 *
 * Character constants in the `#if` constant-expression (C 6.10.1p4 + 6.4.4.4):
 * a `'A'` token in `#if` is an INT whose value is the execution-charset code
 * (ASCII: 'A' == 65). The `#if` Pratt evaluator (pp_if_eval.cpp) gained a
 * char-literal primary: the `'` opener + coalesced body token, body decoded via
 * the SHARED `decodeCharLiteralBody` (named/octal/hex escapes). Two guards:
 *   - `'A' == 65`     TRUE on ASCII  → selects the real `main`.
 *   - `'A' == '\301'` FALSE on ASCII → the SQLite EBCDIC guard, exercising an
 *                     OCTAL escape (`\301` == 0301 == 193 != 65).
 *
 * RED-ON-DISABLE: remove the char-literal primary from pp_if_eval's
 * parsePrimary and `#if 'A' == 65` fails (P0013 "unexpected token in #if
 * expression: '"). The OCTAL escape additionally needs the shared
 * decodeEscapedBytes octal arm (`'\301'` would otherwise fail loud). */
#if 'A' == '\301'
int main(void) { return 7; }      /* EBCDIC — not taken on an ASCII host */
#else
#if 'A' == 65
int main(void) { return 42; }     /* taken: ASCII 'A' == 65 */
#else
int main(void) { return 9; }
#endif
#endif
