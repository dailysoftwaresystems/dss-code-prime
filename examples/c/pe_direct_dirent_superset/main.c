/* <direct.h>, <dirent.h> and <sys/stat.h> on pe are the REFERENCES' surface — no
 * more, no less (P68 round 9, the coordinator's ruling (a) on
 * D-CONFIG-SYS-STAT-OVER-DECLARES-S-ISLNK-ON-PE).
 *
 * This example used to WITNESS a declared superset deviation: on pe DSS made
 * <direct.h> pull <dirent.h>, <dirent.h> pull <windows.h>, and <sys/stat.h> define
 * `S_ISLNK(m)` as `(0)` — three edges NEITHER Windows reference has (✔MEASURED
 * 2026-09-23: `DIR` from <direct.h> is rc=1 under mingw-w64 gcc 13.2.0 and rc=2
 * under MSVC 14.51; <dirent.h> gives no `DWORD` under mingw and does not exist
 * under MSVC; neither defines `S_ISLNK` or `S_IFLNK`). They were removed: a program
 * built with DSS on Windows must see what it would see with a Windows reference.
 * The directory name is kept as the record of what it once witnessed.
 *
 * So the file now pins the ABSENCE at preprocessing time (`#error`, which no
 * implicit declaration can satisfy), and uses at run time what pe really has: the
 * directory API through <dirent.h> (mingw-w64 ships it; DSS realizes it from
 * shipped source), <direct.h>'s own `_getcwd`, and the `<sys/stat.h>` mode tests
 * both references define. The elf/macho legs take the POSIX arm, where
 * `S_ISLNK`/`S_IFLNK` exist and are the real formula.
 *
 * The exit is 42 only when every check holds, else the sum of the failing ones. */

#if defined(_WIN32)
#  include <direct.h>
#endif
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>

#if defined(_WIN32)
#  if defined(S_ISLNK) || defined(S_IFLNK)
#    error "no Windows reference defines S_ISLNK or S_IFLNK (mingw-w64 13.2.0, MSVC 14.51)"
#  endif
#  if !defined(S_ISREG) || !defined(S_ISDIR) || !defined(S_IFMT) || !defined(S_IRWXG)
#    error "mingw-w64's <sys/stat.h> defines S_ISREG, S_ISDIR, S_IFMT and S_IRWXG"
#  endif
#  if !defined(_S_IREAD) || !defined(S_IREAD)
#    error "both Windows references define _S_IREAD and S_IREAD"
#  endif
#else
#  if !defined(S_ISLNK) || !defined(S_IFLNK) || !defined(S_ISUID) || !defined(S_ISVTX)
#    error "POSIX defines S_ISLNK, S_IFLNK, S_ISUID and S_ISVTX (glibc, Darwin)"
#  endif
#endif

int main(void) {
    int f = 0;
    DIR *d;
#if defined(_WIN32)
    char cwd[512];
    if (_getcwd(cwd, 512) == 0) return 64;
    d = opendir(cwd);
#else
    d = opendir(".");
#endif
    if (d == 0) return 128;
    if (readdir(d) == 0) f = f + 256;
    if (closedir(d) != 0) f = f + 512;

    if (S_ISREG(32768) == 0) f = f + 1;
    if (S_ISDIR(16384) == 0) f = f + 2;
    if (S_ISDIR(32768) != 0) f = f + 4;
    if (S_IRWXG != 56 || S_IRWXO != 7) f = f + 8;
#if defined(_WIN32)
    if (_S_IREAD != 256 || S_IREAD != 256) f = f + 16;
#else
    if (S_ISLNK(40960) == 0 || S_ISLNK(32768) != 0) f = f + 1024;
    if (S_IFLNK != 40960) f = f + 2048;
    if (S_ISUID != 2048 || S_ISGID != 1024 || S_ISVTX != 512) f = f + 16;
#endif

    if (f != 0) return f;
    return 42;
}
