/* D-LK-SQLITE-MACHO-UNDEFINED-LIBSYSTEM-SYMBOLS — the RUNTIME witness for the
 * twelve Darwin/libSystem externs that anchor closed. The descriptor unit
 * tests (tests/ffi RealUnistdJsonDarwinFsSysctlMachoOnly and
 * RealStdlibJsonMallocZoneMachoOnly — the structural twins of this file) pin
 * the DECLARATIONS; the sqlite testfixture that consumes them is NOT in the
 * ctest gate. This example is the gate-resident proof that every one of
 * the twelve actually BINDS through the real libSystem stub and EXECUTES with
 * correct semantics — declaration-only coverage would let a wrong signature, a
 * wrong import, or a broken out-param ABI ship green.
 *
 *   unistd.json : flock, statfs, fstatfs, sysctl, sysctlbyname
 *                 (sqlite os_unix.c + mem1.c + test1.c, the DEFAULT macho
 *                  path — not the opt-in AFP cluster)
 *   stdlib.json : malloc_default_zone, malloc_create_zone, malloc_set_zone_name,
 *                 malloc_size, malloc_zone_malloc, malloc_zone_free,
 *                 malloc_zone_realloc  (sqlite mem1.c SQLITE_SYSTEM_MALLOC
 *                 Darwin arm)
 *
 * EVERY RESULT IS LOAD-BEARING. Each call's return feeds either a guard with
 * its own exit code (so a wrong value is diagnosed, not merely "not 42") or
 * the final exit arithmetic. A call whose result were ignored would prove only
 * that the import resolved, never that it works.
 *
 * HERMETIC BY CONSTRUCTION — invariants only, never environment specifics:
 *   - The page size is never compared to a literal. THREE independent sources
 *     (sysctl by MIB, sysctlbyname by name, sysconf) must AGREE, and the value
 *     must be positive and a power of two. True on 4 KiB x86_64 and 16 KiB
 *     arm64 alike.
 *   - No ncpu / disk-size / path-layout assertion of any kind.
 *   - The only file created is /tmp/dss_libsystem_witness_<pid>.tmp, unlink'd
 *     IMMEDIATELY after open (the fd stays valid; the name is gone even on an
 *     abnormal exit). getpid() makes it collision-free against a concurrent
 *     run. That same removed name then doubles as the GUARANTEED-nonexistent
 *     path for the statfs negative — no guessed "surely this is missing" path.
 *   - Zone hygiene: every block taken from either zone is malloc_zone_free'd.
 *     The created zone itself is NOT destroyed because malloc_destroy_zone is
 *     not among the twelve shipped names; a live zone stays reachable from the
 *     malloc runtime's zone table, so this leaks no unreachable memory.
 *
 * NOT SHIPPED, SO SPELLED LOCALLY (measured from the real SDK, xcrun
 * --show-sdk-path): LOCK_EX/LOCK_NB/LOCK_UN = 2/4/8 (sys/file.h) and
 * CTL_HW/HW_PAGESIZE = 6/7 (sys/sysctl.h). These are call ARGUMENTS, not
 * results — nothing is asserted about them beyond what the calls do with them.
 * sizeof(struct statfs) measured 2168 on this SDK; the 8192-byte buffer keeps
 * ~3.7x headroom, and the descriptors deliberately model `struct statfs *` as
 * ptr<void> (no layout is read here, exactly as sqlite's consumers do not).
 *
 * THE size_t* OUT-PARAM IS PROVEN, NOT ASSUMED. sysctl gets an oldlen of
 * sizeof(pgBuf)=16 over a buffer that really is 16 bytes, so the SHRINK to the
 * bytes actually produced is a genuine write-back through the size_t* — an
 * oldlen pre-set to the answer would have proven only "did not clobber". The
 * shrink is asserted as an INVARIANT (`lenMib < sizeof(pgBuf)`), never as the
 * literal 4: MEASURED, the MIB node reports 4 while sysctlbyname("hw.pagesize")
 * reports 8 when offered room (the legacy int node vs. the long node), so a
 * hardcoded 4 would be a latent OS-version flake. sysctlbyname is instead given
 * an exact 4-byte oldlen, the form every real consumer uses. Both data buffers
 * are pre-poisoned to -1, so a call that writes nothing trips a guard.
 *
 * WHAT THE EXIT ARITHMETIC PINS THAT THE GUARDS DO NOT (i.e. what no pass can
 * fold): the guards on the three FAILURE returns are `!= 0`, which proves
 * nonzero but NOT the value — the exit sum pins each at exactly -1. The sysctl
 * out-param shrink, the two page-size DIFFERENCES, and malloc_size(NULL) are
 * likewise deliberately UNGUARDED, asserted here and nowhere else. Every
 * operand of the final expression is an opaque extern return, so nothing
 * reduces to a literal.
 *
 * TWO OF THE TWELVE RETURN void — malloc_set_zone_name and malloc_zone_free —
 * so no result of theirs can reach the exit code, and none is faked. They are
 * witnessed by POST-CONDITIONS instead: the custom zone must still allocate
 * correctly AFTER being named (set_zone_name replaces the zone's name storage;
 * a botched ABI there corrupts the zone), and BOTH zones must still allocate
 * correctly AFTER a free.
 *
 * RED-ON-DISABLE: delete any of the twelve rows from
 * src/dss-config/shippedLibs/unistd.json or stdlib.json → S0001 undeclared
 * identifier at its call, no binary; these descriptors SHADOW the real SDK
 * headers totally, so the rows are the ONLY possible source of the names.
 * Break any semantic and the guard exit code (1..31) names the exact call. */
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#define LOCK_EX     2      /* SDK sys/file.h — exclusive advisory lock        */
#define LOCK_NB     4      /* SDK sys/file.h — do not block                   */
#define LOCK_UN     8      /* SDK sys/file.h — unlock                         */
#define CTL_HW      6      /* SDK sys/sysctl.h — the "hw" top-level MIB node  */
#define HW_PAGESIZE 7      /* SDK sys/sysctl.h — hw.pagesize                  */

#define STATFS_BUF  8192   /* >> sizeof(struct statfs) (measured 2168)        */
#define BLK_SMALL   100    /* first zone block                                */
#define BLK_BIG     4096   /* realloc'd size                                  */
#define BLK_ZONE    77     /* block taken from the CREATED zone               */

int main(void) {
    char   sbuf[STATFS_BUF];
    char   path[64];
    char   digits[16];
    char  *pfx;
    int    mib[2];
    int    pgBuf[4];
    int    pgMib, pgName;
    long   pgConf;
    size_t lenMib, lenName;
    int    rcMib, rcName;
    int    rcRoot, rcGone, rcFd, rcBadFd;
    int    rcLockEx, rcLockUn, rcLockBad;
    int    fd, pid, i, n, v;
    void  *dz, *cz, *p, *q, *cp, *again, *zagain;
    size_t szP, szQ, szCp, szNull;
    unsigned char seed;

    /* ---- sysctl(2) by MIB: hw.pagesize ---------------------------------- */
    mib[0]   = CTL_HW;
    mib[1]   = HW_PAGESIZE;
    pgBuf[0] = -1;                     /* poison: unwritten trips guard 4     */
    lenMib   = sizeof(pgBuf);          /* 16 bytes offered, 16 bytes real     */
    rcMib    = sysctl(mib, 2, pgBuf, &lenMib, 0, 0);
    pgMib    = pgBuf[0];
    if (rcMib != 0)                     return 1;   /* the MIB form succeeds  */
    if (lenMib < sizeof(int))           return 2;   /* produced >= an int     */
    if (lenMib > sizeof(pgBuf))         return 3;   /* never past the buffer  */
    if (pgMib <= 0)                     return 4;   /* a page size is positive*/
    if ((pgMib & (pgMib - 1)) != 0)     return 5;   /* ...and a power of two  */
    /* That lenMib SHRANK — the actual write-back proof — is asserted only by
     * the exit sum, so no pass can fold it away. */

    /* ---- sysctlbyname(3): the same node, addressed by name --------------- */
    pgName  = -1;                      /* poison: unwritten trips guard 8     */
    lenName = sizeof(pgName);          /* the exact-fit form real callers use */
    rcName  = sysctlbyname("hw.pagesize", &pgName, &lenName, 0, 0);
    if (rcName != 0)                    return 6;   /* the name form succeeds */
    if (lenName != sizeof(pgName))      return 7;   /* wrote exactly an int   */
    if (pgName <= 0)                    return 8;   /* the data out-param too */

    /* Cross-source agreement, no literal anywhere: the MIB spelling, the name
     * spelling and sysconf must all report the SAME page size. Both
     * differences are unguarded and land in the exit sum. */
    pgConf = sysconf(_SC_PAGESIZE);
    if (pgConf <= 0)                    return 9;

    /* ---- a private, immediately-anonymous temp file ---------------------- */
    pid = getpid();                 /* opaque: the file name cannot be folded */
    pfx = "/tmp/dss_libsystem_witness_";
    i   = 0;
    while (pfx[i] != 0) { path[i] = pfx[i]; i = i + 1; }
    v = pid;
    if (v <= 0) v = 1;
    n = 0;
    while (v > 0) { digits[n] = (char)('0' + (v % 10)); n = n + 1; v = v / 10; }
    while (n > 0) { n = n - 1; path[i] = digits[n]; i = i + 1; }
    path[i] = '.'; path[i + 1] = 't'; path[i + 2] = 'm';
    path[i + 3] = 'p'; path[i + 4] = 0;

    fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0)                         return 10;  /* /tmp must be writable  */
    if (unlink(path) != 0)              return 11;  /* name gone, fd still ok */

    /* ---- statfs(2) / fstatfs(2): one positive and one negative each ------ */
    rcRoot = statfs("/", sbuf);                     /* "/" is always mounted  */
    if (rcRoot != 0)                  { close(fd); return 12; }
    rcGone = statfs(path, sbuf);                    /* the name just unlinked */
    if (rcGone == 0)                  { close(fd); return 13; }
    rcFd = fstatfs(fd, sbuf);                       /* the live fd            */
    if (rcFd != 0)                    { close(fd); return 14; }
    rcBadFd = fstatfs(-1, sbuf);                    /* EBADF                  */
    if (rcBadFd == 0)                 { close(fd); return 15; }

    /* ---- flock(2): take, release, and fail on a bad fd ------------------- */
    rcLockEx = flock(fd, LOCK_EX | LOCK_NB);
    if (rcLockEx != 0)                { close(fd); return 16; }
    rcLockUn = flock(fd, LOCK_UN);
    if (rcLockUn != 0)                { close(fd); return 17; }
    rcLockBad = flock(-1, LOCK_EX | LOCK_NB);       /* EBADF                  */
    if (rcLockBad == 0)               { close(fd); return 18; }
    if (close(fd) != 0)                            return 19;

    /* ---- the Darwin zone allocator -------------------------------------- */
    dz = malloc_default_zone();
    if (dz == 0)                        return 20;

    p = malloc_zone_malloc(dz, BLK_SMALL);
    if (p == 0)                         return 21;
    szP = malloc_size(p);
    if (szP < BLK_SMALL)                return 22;  /* usable >= requested    */

    /* Seed from the pid so no pass can precompute the pattern. The check
     * after the realloc recomputes the SAME formula — it asserts SURVIVAL,
     * never a particular byte value. */
    seed = (unsigned char)(pid ^ 0x5A);
    for (i = 0; i < BLK_SMALL; i = i + 1) {
        ((unsigned char *)p)[i] = (unsigned char)(i * 7 + seed);
    }

    q = malloc_zone_realloc(dz, p, BLK_BIG);
    if (q == 0)                         return 23;
    szQ = malloc_size(q);
    if (szQ < BLK_BIG)                  return 24;  /* grew, and knows it     */
    for (i = 0; i < BLK_SMALL; i = i + 1) {
        if (((unsigned char *)q)[i] != (unsigned char)(i * 7 + seed)) return 25;
    }

    malloc_zone_free(dz, q);            /* void — witnessed by the next alloc */
    again = malloc_zone_malloc(dz, BLK_SMALL);
    if (again == 0)                     return 26;  /* default zone survived  */
    malloc_zone_free(dz, again);

    /* malloc_size of a non-allocation is 0 — proves the call INSPECTS rather
     * than echoing its argument. Unguarded: the exit sum is its assertion. */
    szNull = malloc_size(0);

    cz = malloc_create_zone(0, 0);      /* 0 start_size = the zone's default  */
    if (cz == 0)                        return 27;
    if (cz == dz)                       return 28;  /* genuinely a NEW zone   */

    malloc_set_zone_name(cz, "dss-libsystem-witness-zone");  /* void */

    /* The post-condition that witnesses set_zone_name: the zone must still be
     * fully usable after its name storage was replaced. */
    cp = malloc_zone_malloc(cz, BLK_ZONE);
    if (cp == 0)                        return 29;
    szCp = malloc_size(cp);
    if (szCp < BLK_ZONE)                return 30;
    malloc_zone_free(cz, cp);
    zagain = malloc_zone_malloc(cz, BLK_ZONE);
    if (zagain == 0)                    return 31;  /* created zone survived  */
    malloc_zone_free(cz, zagain);

    /* Every operand below is an opaque libSystem return, and the four things
     * the guards left unpinned — the sysctl out-param SHRINK, both page-size
     * deltas, malloc_size(NULL), and the exact -1 of the three failure
     * returns — are asserted HERE and nowhere else. */
    return (int)(lenMib < sizeof(pgBuf))            /*  1 = out-param shrank  */
         + (pgMib - pgName)                         /*  0 = MIB == by-name    */
         + (int)(pgConf - (long)pgName)             /*  0 = sysconf agrees    */
         + (int)szNull                              /*  0 = malloc_size(NULL) */
         + (rcMib + rcName + rcRoot + rcFd)         /*  0 = four successes    */
         + (rcLockEx + rcLockUn)                    /*  0 = lock + unlock     */
         - (rcGone + rcBadFd + rcLockBad)           /* +3 = three exact -1s   */
         + 38;                                      /*  1 + 3 + 38 = 42       */
}
