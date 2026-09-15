/*
 * [[D-C-PE64-CORPUS-BLOCKED-BY-AN-UNDECLARED-WIN32-CALL-AND-A-TYPE-DSS-SPLITS-THAT-MINGW-ALIASES]]
 *
 * The two DECLARATION gaps that kept `pe64-x86_64` the last poisoned leg of the
 * SQLite matrix, as one runnable program. Both live in upstream sqlite source
 * this project may not touch, so the witness reproduces the SHAPE of each call
 * site rather than the file:
 *
 *   (1) `src/os_win.c` builds its aSyscall[] table with
 *       `{ "GetDriveTypeW", (SYSCALL)GetDriveTypeW, 0 }` and then asks
 *       `osGetDriveTypeW(zRoot)==DRIVE_REMOTE`. Neither the kernel32 entry
 *       point nor the winbase.h manifest constant was declared, so both were
 *       `S_UndeclaredIdentifier` — they were the ONLY two in the whole
 *       testfixture build.
 *
 *   (2) `bld/shell.c` `openChrSource` declares `struct __stat64 x` and calls
 *       `_fstat64(_fileno(rv), &x)`. Both references reach ONE type through
 *       `#define __stat64 _stat64` (mingw-w64 `_mingw_stat64.h`; the UCRT's
 *       `sys/stat.h`, marked "For legacy compatibility"), while DSS declared
 *       TWO struct tags with identical members — and a descriptor struct is
 *       interned by NAME plus field types, so identical members did NOT make
 *       the call site compile. The remedy is the reference's own mechanism: a
 *       format-gated `macros` row, not a second declaration.
 *
 * ★ WHAT EACH CELL CAN AND CANNOT DISCRIMINATE, stated rather than implied.
 * A missing declaration is not a wrong answer — it is a REFUSED compile, so
 * removing either config row makes this example fail to BUILD, which is the
 * red-on-disable direction a fixture must synthesize. The exit-code arithmetic
 * below is aimed at the OTHER failure available here: a declaration that is
 * present but WRONG. `driveFamilyIsTheSdkEnumeration` reds on any wrong
 * constant value; `legacyTagIsOneTypeNotTwo` reds if the alias ever resolved to
 * some other record. The two cells marked TAUTOLOGICAL-BY-DESIGN read a real
 * call whose result depends on the machine this runs on (which drive letters
 * exist; whether fd 0 is a valid handle), so their VALUE is pinned to a
 * tautology on purpose and their whole contribution is that the call TYPE-CHECKS
 * and the process survives it.
 *
 * Every non-pe target compiles the `#else` arm — `windows.json` is
 * `availableObjectFormats:[pe]` and the `__stat64` macro carries
 * `when:{format:pe}`, so the guarded include is never resolved off pe and the
 * elf `struct stat64` (a genuinely different LFS record) is never touched.
 * All four targets exit 42, by two different routes.
 */
#if defined(_WIN32)
#include <windows.h>
#include <sys/stat.h>
#endif

#if defined(_WIN32)

/* GAP 1, the CONSTANT half. winbase.h defines these as one closed enumeration;
 * the Windows SDK 10.0.26100.0 `um/winbase.h` and mingw-w64's `winbase.h` agree
 * value for value. A wrong value here moves the exit code off 42. */
static int driveFamilyIsTheSdkEnumeration(void) {
    return DRIVE_UNKNOWN == 0 && DRIVE_NO_ROOT_DIR == 1 && DRIVE_REMOVABLE == 2
           && DRIVE_FIXED == 3 && DRIVE_REMOTE == 4 && DRIVE_CDROM == 5
           && DRIVE_RAMDISK == 6;
}

/* GAP 1, the CALL half, in os_win.c's exact shape: a `UINT` result compared
 * against the bare `int` constant with no cast, which is what makes this a test
 * of the TYPE MAPPING (`UINT` -> bare u32, `LPCWSTR` -> ptr<u16>) and not only
 * of the name resolving. TAUTOLOGICAL-BY-DESIGN: which drive types exist is a
 * property of the machine, so only the range is asserted. Every documented
 * return is DRIVE_UNKNOWN..DRIVE_RAMDISK. */
static int driveTypeCallTypeChecksAndReturnsInRange(void) {
    WCHAR root[4];
    UINT kind;
    int isRemote;
    root[0] = 'C';
    root[1] = ':';
    root[2] = '\\';
    root[3] = 0;
    kind = GetDriveTypeW(root);
    isRemote = (GetDriveTypeW(root) == DRIVE_REMOTE); /* os_win.c's own line */
    return kind <= (UINT)DRIVE_RAMDISK && (isRemote == 0 || isRemote == 1);
}

/* GAP 2. The two spellings must be ONE type, so a pointer crosses in BOTH
 * directions with no cast. Two look-alike tags would refuse this even with
 * byte-identical members — which is exactly what the tree did before the
 * alias landed. */
static int legacyTagIsOneTypeNotTwo(void) {
    struct __stat64 viaLegacy;
    struct _stat64 viaUcrt;
    struct _stat64 *ucrtPointsAtLegacy = &viaLegacy;
    struct __stat64 *legacyPointsAtUcrt = &viaUcrt;
    return ucrtPointsAtLegacy != 0 && legacyPointsAtUcrt != 0
           && sizeof(struct __stat64) == sizeof(struct _stat64);
}

/* GAP 2, shell.c's call site. TAUTOLOGICAL-BY-DESIGN: whether descriptor 0 is
 * a stat-able handle depends on how the process was launched, so the result is
 * read and its range pinned rather than its value. The witness is that
 * `&record` — a `struct __stat64 *` — reaches a parameter declared
 * `struct _stat64 *`. */
static int legacyTagReachesTheUcrtEntryPoint(void) {
    struct __stat64 record;
    int rc = _fstat64(0, &record);
    return rc == 0 || rc != 0;
}

#endif /* _WIN32 */

int main(void) {
#if defined(_WIN32)
    int cells = driveFamilyIsTheSdkEnumeration()
                + driveTypeCallTypeChecksAndReturnsInRange()
                + legacyTagIsOneTypeNotTwo()
                + legacyTagReachesTheUcrtEntryPoint();
    /* Four cells, each 1. One wrong cell moves the code off 42 by 7 and cannot
     * land back on it. */
    return 42 + (cells - 4) * 7;
#else
    return 42;
#endif
}
