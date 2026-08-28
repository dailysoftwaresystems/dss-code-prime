/* c82 shell.c POSIX symbol-batch witness: the descriptor additions the
** sqlite shell's semantic frontier demanded, each exercised against REAL
** libc with a value-divergent check:
**
**   stdlib.json: strtoll (C99 long long parse — FLAT i64 on every DSS
**                target), realpath into a caller buffer (the NULL/malloc
**                form is blocked on D-LK-ELF-SYMBOL-VERSIONING — see below)
**   string.json: strdup (POSIX dup + independent storage)
**   unistd.json: symlink + readlink roundtrip, isatty (0 on a plain fd),
**                getuid, usleep, sleep(0)
**   sys/stat.json: chmod + stat mode readback (0640 -> S_IRUSR|S_IWUSR|S_IRGRP)
**
** exit 42 = strtoll("42") with every check contributing 0 when right.
** atexit is deliberately ABSENT: glibc exports only __cxa_atexit (atexit is
** a libc_nonshared static shim), so a libc.so.6-bound atexit import fails at
** LOAD — the descriptor honestly does not ship it (see stdlib.json).
**
** RED-ON-DISABLE: remove any of the descriptor entries -> S0001 undeclared
** (compile failure). gcc -D_DEFAULT_SOURCE cross-checked on linux: exits 42
** (the POSIX prototypes need _DEFAULT_SOURCE under -std=c17).
** Gated to the elf legs (the [elf,macho] descriptor gates; macho compiles
** but this manifest keeps run legs to linux + qemu like shipped_dirent).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

int main(void) {
  int rc = 0;
  long long v;
  char *dup;
  char *rp;
  char rbuf[4096];
  char lnk[64];
  long n;
  struct stat st;
  FILE *f;

  /* strtoll — C99, flat i64. */
  v = strtoll("42", 0, 10);
  if (v != 42) rc = rc + 100;

  /* strdup — independent storage, same bytes. */
  dup = strdup("c82");
  if (dup == NULL) rc = rc + 100;
  else {
    if (strcmp(dup, "c82") != 0) rc = rc + 100;
    free(dup);
  }

  /* realpath into a caller buffer. The NULL-resolved (malloc) form is
  ** deliberately NOT used: DSS's dynamic imports carry no symbol-version
  ** info yet, and an UNVERSIONED reference binds glibc's OLDEST version —
  ** realpath@GLIBC_2.2.5 EINVALs on a NULL buffer (witnessed; the @@2.3
  ** default accepts it). The buffer form is version-portable. The
  ** .gnu.version_r emission is the discovered D-LK-ELF-SYMBOL-VERSIONING
  ** frontier item (shell.c's realpath(zPath, NULL) run path needs it). */
  rp = realpath(".", rbuf);
  if (rp == NULL) rc = rc + 100;
  else if (rp[0] != '/') rc = rc + 100;

  /* isatty — an invalid fd is deterministically NOT a tty (returns 0). */
  if (isatty(-1) != 0) rc = rc + 100;

  f = fopen("c82_posix_tmp.txt", "wb");
  if (f == NULL) return 1;
  if (fputs("x", f) < 0) rc = rc + 100;
  if (fclose(f) != 0) rc = rc + 100;

  /* chmod + stat readback: 0640 = S_IRUSR|S_IWUSR|S_IRGRP. */
  if (chmod("c82_posix_tmp.txt", 0640) != 0) rc = rc + 100;
  if (stat("c82_posix_tmp.txt", &st) != 0) rc = rc + 100;
  if ((st.st_mode & 0777) != 0640) rc = rc + 100;

  /* symlink + readlink roundtrip. */
  (void)unlink("c82_posix_lnk");
  if (symlink("c82_posix_tmp.txt", "c82_posix_lnk") != 0) rc = rc + 100;
  n = readlink("c82_posix_lnk", lnk, sizeof(lnk) - 1);
  if (n <= 0) rc = rc + 100;
  else {
    lnk[n] = 0;
    if (strcmp(lnk, "c82_posix_tmp.txt") != 0) rc = rc + 100;
  }
  if (unlink("c82_posix_lnk") != 0) rc = rc + 100;
  if (unlink("c82_posix_tmp.txt") != 0) rc = rc + 100;

  /* getuid (any uid is valid — the CALL must resolve + return), usleep,
  ** sleep(0). */
  (void)getuid();
  if (usleep(1000) != 0) rc = rc + 100;
  if (sleep(0) != 0) rc = rc + 100;

  return rc + (int)v;   /* 42 */
}
