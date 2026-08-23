/* LANE-EX — the RUN witness for `<unistd.h>` ON pe, and it is a witness over
 * the RE-EXPORT axis rather than over the source: ONE main.c, four legs, no
 * `#ifdef _WIN32` arm anywhere. On elf/macho every name below is a symbol row
 * in `unistd.json` itself; on pe that descriptor declares NO symbols at all and
 * instead carries two `includes` edges GATED to the format —
 *   { "header": "io.h",      "when": { "format": "pe" } }
 *   { "header": "process.h", "when": { "format": "pe" } }
 * — exactly as mingw's own <unistd.h> is a thin re-export of <io.h>, <process.h>
 * and <getopt.h>. So the SAME call text binds a libc import on elf/macho and a
 * ucrtbase import reached through a gated edge on pe, and all four legs answer
 * 42 with byte-identical stdout.
 *
 * ★ WHAT EACH NAME PROVES ON pe, because "it compiled" is not the claim:
 *   getpid   — the process.h edge, ALONE. It lives in `process.json`, is not in
 *              `unistd.json` on any format's pe arm, and no other header in this
 *              TU declares it. Delete that one edge and this file is the only
 *              thing that breaks (MEASURED: `S0001: getpid` on pe64, elf64 still
 *              green — see expected.json).
 *   open/write/close/read/lseek/isatty/access/unlink/rmdir/chdir/getcwd
 *            — the io.h edge. `open`/`close`/`read` arrive as io.json MACROS onto
 *              `_open`/`_close`/`_read`; the other eight are symbol rows with a
 *              `linkName` onto the underscored ucrtbase export. Both spellings
 *              are exercised on purpose — they are two different resolution
 *              paths through the same edge.
 *   SEEK_SET/SEEK_CUR/SEEK_END, F_OK/R_OK/W_OK, STD{IN,OUT,ERR}_FILENO
 *            — declared by `unistd.json` ITSELF on pe (its own `constants`, flat
 *              across formats). They are the half of the pe surface that does
 *              NOT travel over an edge, so they discriminate "the descriptor is
 *              available on pe" from "the edges fire".
 *
 * ★ EVERY CALL IS VALUE-CHECKED AGAINST SOMETHING ONLY A LIVE BINDING CAN
 * PRODUCE — a mis-bound name that still links would have to produce the right
 * ANSWER, not merely a plausible one: the bytes written come back through a
 * SECOND descriptor path (write on one fd, read on another), the file's length
 * is read back through lseek(SEEK_END), the read cursor is confirmed through
 * lseek(SEEK_CUR), and access() is checked in BOTH directions (0 for a path
 * that exists, non-0 for one that does not, and then non-0 for the same path
 * after unlink) so a stub that always answers 0 fails.
 *
 * ⚠ THREE TRAPS THIS SOURCE AVOIDS DELIBERATELY, each MEASURED on this surface:
 *   - `isatty` is called on a VALID fd. UCRT's `_isatty` on an invalid fd trips
 *     the invalid-parameter handler -> `__fastfail` -> exit 0xC0000409 with no
 *     output, which reads exactly like a struct-shape miscompile.
 *   - the isatty assertion is `== 0` (a regular file is not a character device)
 *     and NEVER `== 1`: MSVC documents `_isatty` as returning a NONZERO value
 *     for a character device, not specifically 1.
 *   - `X_OK` is NOT passed to access(). UCRT's `_access_s` validates
 *     `(mode & ~6) == 0`, so mode 1 is EINVAL-or-worse there. The constant is
 *     declared on pe (it is a real mingw <io.h> value) but exercising it would
 *     make this example a Windows-crash test, not a re-export witness.
 *
 * ⚠ THE OUTPUT CARRIES NO NEWLINE, ON PURPOSE. fd 1 is a TEXT-mode stream on
 * pe, so a '\n' would reach the harness as "\r\n" on Windows and "\n" on
 * linux/darwin and force a per-target `expectedStdout` override — i.e. the
 * manifest would stop asserting that the four legs agree, which is the whole
 * point of a one-source example. */
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

int main(void) {
    char cwd0[512];
    char cwd1[512];
    char buf[8];
    int  fd;

    /* process.h edge on pe; unistd.json's own row on elf/macho. */
    if (getpid() <= 0) return 10;

    /* 384 == 0600 == _S_IREAD|_S_IWRITE: the one mode literal that means the
     * same thing to POSIX and to the Microsoft CRT (the shipped_posix_batch
     * precedent). */
    fd = open("dss_unistd_reexport.tmp", O_WRONLY | O_CREAT | O_TRUNC, 384);
    if (fd < 0) return 11;
    if (write(fd, "DSSU", 4) != 4) return 12;
    if (close(fd) != 0) return 13;

    fd = open("dss_unistd_reexport.tmp", O_RDONLY);
    if (fd < 0) return 14;
    if (isatty(fd) != 0) return 15;              /* a regular file is never a tty */
    if (lseek(fd, 0, SEEK_END) != 4) return 16;  /* the 4 bytes written above */
    if (lseek(fd, 1, SEEK_SET) != 1) return 17;
    buf[0] = 'x'; buf[1] = 'x'; buf[2] = 'x'; buf[3] = 'x';
    if (read(fd, buf, 3) != 3) return 18;
    buf[3] = 0;
    if (strcmp(buf, "SSU") != 0) return 19;      /* the bytes survived the round trip */
    if (lseek(fd, 0, SEEK_CUR) != 4) return 20;  /* read advanced the cursor 1 -> 4 */
    if (close(fd) != 0) return 21;

    if (access("dss_unistd_reexport.tmp", F_OK) != 0) return 22;
    if (access("dss_unistd_reexport.tmp", R_OK) != 0) return 23;
    if (access("dss_unistd_reexport.tmp", W_OK) != 0) return 24;
    if (access("dss_unistd_no_such_file", F_OK) == 0) return 25;   /* negative control */

    if (unlink("dss_unistd_reexport.tmp") != 0) return 26;
    if (access("dss_unistd_reexport.tmp", F_OK) == 0) return 27;   /* unlink really removed it */

    if (rmdir("dss_unistd_no_such_dir") == 0) return 28;           /* negative control */

    if (getcwd(cwd0, 512) == 0) return 29;
    if (cwd0[0] == 0) return 30;
    if (chdir(".") != 0) return 31;
    if (getcwd(cwd1, 512) == 0) return 32;
    if (strcmp(cwd0, cwd1) != 0) return 33;      /* chdir(".") is a no-op round trip */

    if (STDIN_FILENO != 0 || STDERR_FILENO != 2) return 34;
    if (write(STDOUT_FILENO, "unistd-reexport-ok", 18) != 18) return 35;

    return 42;
}
