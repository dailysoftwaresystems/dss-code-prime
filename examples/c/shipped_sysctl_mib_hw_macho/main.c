/* D-FFI-DARWIN-SYSCTL-CONSTANTS-EMPTY — the gate-resident RUNTIME witness that
 * the Darwin sysctl MIB constants shipped by src/dss-config/shippedLibs/sys/
 * sysctl.json address the kernel nodes their names claim.
 *
 * WHY IT EXISTS. sqlite's src/test1.c:9100-9104 addresses hw.availcpu / hw.ncpu
 * by MIB ARRAY — `nm[0] = CTL_HW; nm[1] = HW_AVAILCPU;` — so the macho
 * testfixture could not compile until those three constants existed. Shipping
 * them makes it compile; it does NOT make them right. A wrong MIB number still
 * compiles, because this descriptor SHADOWS the SDK's <sys/sysctl.h> totally
 * (src/core/types/include_path_resolve.hpp: Descriptor beats Source) even on a
 * native Mac, so there is no second opinion anywhere in the build. It would
 * simply query a different kernel node and return a plausible number. A value
 * table cannot check itself either: asserting CTL_HW == 6 beside a row that
 * says 6 proves only that both were typed the same way. So the check has to
 * ask something that does not read this repo — the kernel.
 *
 * THE ONE INCLUDE IS DELIBERATE. This TU includes <sys/sysctl.h> and NOTHING
 * else, which is what makes it a test of the descriptor rather than of the
 * corpus: every name below — the two functions and every constant — can only
 * have come from that one file. The sibling witness
 * shipped_flock_statfs_sysctl_zone_macho reaches sysctl through <unistd.h> and
 * #defines CTL_HW / HW_PAGESIZE in its own source, so it proves the CALL works
 * and proves nothing at all about these constants. This one covers exactly that
 * hole. No <stdio.h>, no <stdlib.h>: the verdict is the exit code, and `size_t`
 * is spelled `unsigned long` so that no second header is needed (every macho64
 * format declares dataModel LP64, so the two are the same 8-byte type; the
 * descriptor's own signatures say ptr<u64>).
 *
 * HOW EACH CONSTANT IS PINNED, and every claim below is MEASURED on the
 * operator's Mac (macOS 26.5.2 build 25F84, arm64, both -arch arm64 and -arch
 * x86_64 under Rosetta, 2026-08-06) rather than reasoned about:
 *
 *   CTL_HW + HW_NCPU     the MIB form and sysctlbyname("hw.ncpu") must return
 *                        the SAME integer. Both report length 4. A wrong CTL_HW
 *                        or a wrong HW_NCPU lands on a different node, and the
 *                        two forms disagree (or the MIB call returns -1).
 *   CTL_HW + HW_MEMSIZE  the WIDTH DISCRIMINATOR, and the strongest check here.
 *                        hw.memsize is CTLTYPE_QUAD: the kernel writes back a
 *                        length of 8 where every int node writes 4. So a
 *                        HW_MEMSIZE that pointed at any int-typed sibling would
 *                        be caught by the LENGTH alone, independently of the
 *                        value it returned. Machine-independent: it is a
 *                        property of the node's type, not of the hardware.
 *   CTL_HW + HW_BYTEORDER  MIB and name form must agree (both length 4).
 *   CTL_HW + HW_PAGESIZE the VALUES must agree, and the value must be a
 *                        positive power of two. ★ The LENGTHS deliberately are
 *                        NOT compared for this node: MEASURED, the MIB form
 *                        reports 4 while sysctlbyname("hw.pagesize") reports 8
 *                        when offered room — the same asymmetry the sibling
 *                        witness recorded — so the name form is read as a
 *                        64-bit cell and narrowed. The page size itself is
 *                        never compared to a literal: the same physical Mac
 *                        reports 16384 to an arm64 binary and 4096 to an
 *                        x86_64 one, so any hardcoded page size would be an
 *                        arch flake waiting to happen.
 *   CTL_HW + HW_AVAILCPU rc 0, a positive count, and availcpu <= ncpu.
 *                        ⚠ STATED, NOT HIDDEN — this is the WEAKEST pin in the
 *                        file, and the reason is a measurement, not laziness:
 *                        THERE IS NO NAME-ADDRESSABLE hw.availcpu NODE.
 *                        sysctlbyname("hw.availcpu") returns -1 on both arches
 *                        while the MIB {CTL_HW, HW_AVAILCPU} succeeds, so the
 *                        cross-check used for every other constant here is
 *                        simply unavailable for this one. What follows is that
 *                        on a machine where availcpu == ncpu (the common case;
 *                        this Mac reports 10 and 10) a HW_AVAILCPU wrongly
 *                        equal to HW_NCPU would NOT be caught here. Its guard
 *                        against that is the compile-time value pin in
 *                        tests/ffi RealSysSysctlJsonMibConstantDomains plus the
 *                        four-instrument measurement recorded in the
 *                        descriptor's own comment.
 *
 * ⛔ TWO NEGATIVE PROBES WERE DESIGNED, MEASURED, AND THROWN AWAY — recorded so
 * neither is re-proposed. (1) "an id past HW_MAXID must fail": FALSE. {CTL_HW,
 * HW_MAXID + 100} = {6,128} returns rc 0 with an 8-byte payload on both arches
 * — Apple has live hw nodes above the classic contiguous range, so HW_MAXID
 * bounds the NAMED ids and not the valid ones. (2) "{CTL_HW, HW_MAXID} must
 * fail": TRUE today (rc -1, both arches) but it asserts that Apple will never
 * fill slot 28, which is an OS-version flake with a long fuse. The negative
 * used instead is a name that cannot ever exist, which needs no such bet.
 *
 * HERMETIC. No core count, no memory size, no page size, no path is ever
 * compared to a literal — only nodes against each other, plus bounds that hold
 * on any Mac (counts positive, availcpu <= ncpu, page size a power of two,
 * the quad node wider than an int node). Nothing is written to disk.
 *
 * THE EXIT ARITHMETIC, 42, and why no pass can fold it to a constant: the
 * GUARDS below prove only liveness and ordering — nonzero, positive, wider,
 * failed — while the SUM is the only place the EXACT values are pinned. The
 * two kernel-written lengths (4 for an int node, 8 for the quad node), the
 * exact -1 of the absent-name failure, and all four cross-form deltas being
 * exactly 0 are asserted ONLY in the return expression. Every operand is either
 * an opaque extern return or a cell the kernel wrote through an out-param, so
 * the optimizer cannot see any of them. 4 + 8 - 1 + 31 = 42.
 *
 * RED-ON-DISABLE: delete any constant row this file names from
 * src/dss-config/shippedLibs/sys/sysctl.json → error[S0001] at its use, no
 * binary (the descriptor is the only possible source — there is no other
 * include here). Perturb a VALUE instead of deleting it and the build still
 * succeeds: the run then fails, either at a guard (1..23, each naming its call)
 * or on the exit code, which is the whole reason this example exists rather
 * than a declaration-only test. The `release` arm re-runs every call under the
 * full optimizer. ⓘ The x86_64-macho arm was compile-verified and Rosetta-run
 * verified while authoring, but is NOT declared: it would make the example
 * depend on Rosetta being installed on whatever runs the darwin leg. */
#include <sys/sysctl.h>

#define DSS_ABSENT_NODE "dss.no.such.sysctl.node"   /* can never exist         */

int main(void) {
    int           nm[2];
    int           ncpuMib, ncpuName, availMib, pgMib, byteMib, byteName;
    long long     memMib, memName, pgName;
    unsigned long lenNcpuMib, lenNcpuName, lenAvail, lenPgMib, lenPgName;
    unsigned long lenMemMib, lenMemName, lenByteMib, lenByteName, lenAbsent;
    int           rcNcpuMib, rcNcpuName, rcAvailMib, rcPgMib, rcPgName;
    int           rcMemMib, rcMemName, rcByteMib, rcByteName, rcAbsent;

    /* ---- hw.ncpu: MIB form, then the same node addressed by NAME --------- */
    nm[0]       = CTL_HW;
    nm[1]       = HW_NCPU;
    ncpuMib     = -1;                    /* poison: unwritten trips guard 3   */
    lenNcpuMib  = sizeof(ncpuMib);
    rcNcpuMib   = sysctl(nm, 2, &ncpuMib, &lenNcpuMib, 0, 0);
    if (rcNcpuMib != 0)                  return 1;
    if (lenNcpuMib == 0)                 return 2;   /* nonzero; sum pins 4   */
    if (ncpuMib < 1)                     return 3;

    ncpuName    = -1;
    lenNcpuName = sizeof(ncpuName);
    rcNcpuName  = sysctlbyname("hw.ncpu", &ncpuName, &lenNcpuName, 0, 0);
    if (rcNcpuName != 0)                 return 4;
    if (lenNcpuName == 0)                return 5;
    /* ncpuMib == ncpuName is pinned ONLY by the sum — see the header note.   */

    /* ---- hw.availcpu: MIB only. There is no name form (MEASURED). -------- */
    nm[1]       = HW_AVAILCPU;
    availMib    = -1;
    lenAvail    = sizeof(availMib);
    rcAvailMib  = sysctl(nm, 2, &availMib, &lenAvail, 0, 0);
    if (rcAvailMib != 0)                 return 6;
    if (availMib < 1)                    return 7;
    if (availMib > ncpuMib)              return 8;   /* available <= total    */

    /* ---- hw.pagesize: values must agree; LENGTHS legitimately differ ----- */
    nm[1]       = HW_PAGESIZE;
    pgMib       = -1;
    lenPgMib    = sizeof(pgMib);
    rcPgMib     = sysctl(nm, 2, &pgMib, &lenPgMib, 0, 0);
    if (rcPgMib != 0)                    return 9;
    if (lenPgMib == 0)                   return 10;
    if (pgMib < 1)                       return 11;
    if ((pgMib & (pgMib - 1)) != 0)      return 12;  /* a power of two        */

    pgName      = -1;                    /* 8-byte cell: the name form fills 8 */
    lenPgName   = sizeof(pgName);
    rcPgName    = sysctlbyname("hw.pagesize", &pgName, &lenPgName, 0, 0);
    if (rcPgName != 0)                   return 13;

    /* ---- hw.memsize: the WIDTH discriminator (CTLTYPE_QUAD, length 8) ---- */
    nm[1]       = HW_MEMSIZE;
    memMib      = -1;
    lenMemMib   = sizeof(memMib);
    rcMemMib    = sysctl(nm, 2, &memMib, &lenMemMib, 0, 0);
    if (rcMemMib != 0)                   return 14;
    if (lenMemMib <= lenNcpuMib)         return 15;  /* wider than an int node */
    if (memMib < 1)                      return 16;

    memName     = -1;
    lenMemName  = sizeof(memName);
    rcMemName   = sysctlbyname("hw.memsize", &memName, &lenMemName, 0, 0);
    if (rcMemName != 0)                  return 17;
    if (lenMemName <= lenNcpuMib)        return 18;

    /* ---- hw.byteorder: a third independent MIB-vs-name agreement --------- */
    nm[1]       = HW_BYTEORDER;
    byteMib     = 0;                     /* poison: unwritten trips guard 21  */
    lenByteMib  = sizeof(byteMib);
    rcByteMib   = sysctl(nm, 2, &byteMib, &lenByteMib, 0, 0);
    if (rcByteMib != 0)                  return 19;
    if (lenByteMib == 0)                 return 20;
    if (byteMib == 0)                    return 21;

    byteName    = 0;
    lenByteName = sizeof(byteName);
    rcByteName  = sysctlbyname("hw.byteorder", &byteName, &lenByteName, 0, 0);
    if (rcByteName != 0)                 return 22;

    /* ---- the negative: a name that cannot exist must FAIL ---------------- */
    lenAbsent   = sizeof(byteName);
    rcAbsent    = sysctlbyname(DSS_ABSENT_NODE, &byteName, &lenAbsent, 0, 0);
    if (rcAbsent == 0)                   return 23;  /* failed; sum pins -1   */

    /* Guards proved liveness and ordering. The exact values live only here.  */
    return (int)lenNcpuMib                /*  4  kernel-written, int node     */
         + (int)lenMemMib                 /*  8  kernel-written, quad node    */
         + rcAbsent                       /* -1  the absent-name failure      */
         + (ncpuMib  - ncpuName)          /*  0  CTL_HW + HW_NCPU agree       */
         + (pgMib    - (int)pgName)       /*  0  CTL_HW + HW_PAGESIZE agree   */
         + (byteMib  - byteName)          /*  0  CTL_HW + HW_BYTEORDER agree  */
         + (int)(memMib != memName)       /*  0  CTL_HW + HW_MEMSIZE agree    */
         + rcNcpuMib + rcNcpuName + rcAvailMib + rcPgMib + rcPgName
         + rcMemMib  + rcMemName  + rcByteMib + rcByteName   /* 0 each        */
         + 31;
}
