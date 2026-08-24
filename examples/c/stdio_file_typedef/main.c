/* c82 stdio semantic-surface witness (D-FFI-SHELL-STDIO-SEMANTIC-SURFACE):
**
**   1. `FILE` ships as a TYPEDEF (the opaque `struct "FILE" {}`, the dirent
**      DIR precedent) — so a BLOCK-scope `FILE *f;` declaration PARSES (the
**      shipped-typedef oracle steers declaration-vs-expression: pre-fix
**      `FILE *in;` parsed as a multiplication -> S0001 x2, the shell.c:24436
**      shape) and file-scope `static FILE *g;` resolves (pre-fix S0006).
**   2. `size_t`/`NULL` ride stdio.h per C89 7.19.1 (shell.c never includes
**      stddef.h; `size_t n` + `NULL` compares below fail pre-fix).
**   3. the printf FAMILY ships as VARIADIC externs: printf, fprintf,
**      sprintf, sscanf — and vfprintf takes a real `va_list` (the
**      descriptor spells `va_list`; the reader resolves it to the active
**      CC's exact type), exercised through a cli_printf-shaped forwarder —
**      shell.c's LOAD-BEARING output path — on every leg including the
**      AAPCS64 qemu leg (va_list is a 32-byte struct there).
**
** Flow: logf42(f, "%d %d", 40, 2) vfprintf's "40 2" into c82_stdio_tmp.txt;
** fread it back (size_t count), sscanf re-parses the two ints (40+2), and
** sprintf/strlen contribute strlen("40 2")==4 checked against the fread
** size_t. exit = 40 + 2 = 42 with every check contributing 0 when right.
**
** RED-ON-DISABLE: remove the stdio.json `typedefs` surface -> S0006/S0001
** on the FILE declarations; remove the `va_list` named-type thread -> the
** stdio.json read itself fails loud (F_ShippedLibUnsupportedType).
** gcc -std=c17 cross-checked: same source exits 42.
*/

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static FILE *gLog;   /* file-scope FILE* — the shell.c memtraceOut shape */

/* shell.c cli_printf shape: forward a va_list to vfprintf. */
static int logf42(FILE *out, const char *zFormat, ...) {
  va_list ap;
  int n;
  va_start(ap, zFormat);
  n = vfprintf(out, zFormat, ap);
  va_end(ap);
  return n;
}

int main(void) {
  FILE *f;                 /* BLOCK-scope declaration — the oracle steer */
  char buf[16];
  char msg[16];
  size_t nRead;            /* size_t via stdio.h (C89 7.19.1) */
  int a = 0;
  int b = 0;
  int rc = 0;

  f = fopen("c82_stdio_tmp.txt", "wb");
  if (f == NULL) return 1;             /* NULL via stdio.h */
  gLog = f;
  if (logf42(gLog, "%d %d", 40, 2) != 4) rc = rc + 100;
  if (fclose(f) != 0) rc = rc + 100;

  f = fopen("c82_stdio_tmp.txt", "rb");
  if (f == NULL) return 2;
  nRead = fread(buf, 1, sizeof(buf), f);
  if (fclose(f) != 0) rc = rc + 100;
  if (remove("c82_stdio_tmp.txt") != 0) rc = rc + 100;
  if (nRead != 4) rc = rc + 100;       /* "40 2" */
  buf[nRead] = 0;

  /* sprintf + strlen mirror the same text; sscanf re-parses it. */
  if (sprintf(msg, "%d %d", 40, 2) != 4) rc = rc + 100;
  if (strlen(msg) != nRead) rc = rc + 100;
  if (sscanf(buf, "%d %d", &a, &b) != 2) rc = rc + 100;

  printf("");                          /* variadic printf resolves + links */
  return rc + a + b;                   /* 40 + 2 = 42 */
}
