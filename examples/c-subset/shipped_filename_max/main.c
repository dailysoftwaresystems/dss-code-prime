/* c45 (D-FFI-STDIO-FILENAME-MAX) — FILENAME_MAX from <stdio.h> as a compile-time
 * constant (the next sqlite frontier after c43/c44 cleared the offsetof S001C).
 * sqlite: `#define SQLITE_MAX_PATHLEN FILENAME_MAX` (sqlite3.c:16681), used to size
 * path buffers -- a constant-expression context, so the array dim must FOLD. DSS's
 * shipped <stdio.h> (stdio.json) had function symbols but NO constants surface, so
 * FILENAME_MAX was undefined -> the array dim was non-constant -> S000B. Added
 * FILENAME_MAX to stdio.json's `constants` surface, PER-TARGET via `variants`
 * (glibc/elf 4096, Darwin/macho 1024, msvcrt/pe 260) -- the value is target-correct
 * (verified directly: pe->260, elf->4096). It folds at HIR + in const-expr position,
 * like <limits.h>'s CHAR_BIT.
 *
 * The corpus checks a target-INVARIANT range: a single exitCode cannot assert the
 * per-target value, and the corpus RUNS on multiple targets (pe host / elf WSL /
 * arm64 qemu) whose FILENAME_MAX differs. The EXACT per-target values live in the
 * descriptor + are verified by direct compile (pe yields 260, elf 4096). Here we
 * confirm FILENAME_MAX FOLDS to a sane positive buffer size on every target.
 *
 * RED-ON-DISABLE: revert the stdio.json constants addition -> FILENAME_MAX undefined
 * -> the array dim is non-constant -> S000B (would not compile). */
#include <stdio.h>

struct Path { char buf[FILENAME_MAX]; };   /* sqlite's path-buffer shape */

int main(void) {
    /* FILENAME_MAX folds to a per-target positive constant in [260, 4096]. */
    if (sizeof(struct Path) < 260)  return 1;   /* folded to a sane buffer size */
    if (sizeof(struct Path) > 4096) return 2;   /* not garbage / overflow */
    return 42;
}
