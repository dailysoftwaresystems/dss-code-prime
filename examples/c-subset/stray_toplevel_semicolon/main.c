/* c81 (the sqlite shell.c CLI route): a stray FILE-SCOPE semicolon must
 * parse as a no-op external declaration. sqlite's shell.c defines
 * `#define SQLITE_EXTENSION_INIT1` EMPTY in the static-shell build, and two
 * use sites append `;` (base64.c/base85.c sections, logical 6809/7132) —
 * the expansion leaves a LONE `;` at file scope, which gcc/clang accept
 * (P0009 before c81: the topLevel alt list had no EndStatement arm; now the
 * statement-position `emptyStmt` rule is reused as a topLevel alt, lowering
 * to Skip exactly like an includeDirective). This file mirrors every shape:
 * the empty-macro expansion (shell.c's exact form), a bare `;` between
 * declarations, and a doubled `;;`. */
#define EXTENSION_INIT1     /* expands EMPTY — the shell.c shape */

EXTENSION_INIT1;            /* leaves a lone `;` at file scope */

static int base = 40;
;                           /* bare stray semicolon between declarations */

int two(void) { return 2; }
;;                          /* two in a row — each its own no-op decl */

int main(void) {
    return base + two();    /* 42 — the semicolons contributed nothing */
}
