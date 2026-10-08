/* D-LK-ARCHIVE-SEARCH-FETCHES-A-MEMBER-FOR-A-WEAK-REFERENCE (P69 round 4): a static
 * link holds `hook` and `hookv` only as WEAK references, and the static library
 * it resolves against (hooklib.c, the target's `dependsOn`) has ONE member that
 * defines both. Whether the archive search FETCHES that member for a weak
 * reference is a property of the members' FORMAT, and the reference linkers of
 * each format agree on it (✔MEASURED 2026-10-07, probe-reference-cc meta-probes):
 *   * ELF — NO. The System V gABI: "The link editor does not extract archive
 *     members to resolve undefined weak symbols." GNU ld 2.42 and ld.lld 18 on
 *     x86_64, gcc + GNU ld on aarch64, -no-pie and -pie: both names stay
 *     unresolved, so both read as NULL and the program returns 42.
 *   * PE — NO, for a weak external that asks no library search (NOLIBRARY or
 *     ALIAS; DSS writes ALIAS): link.exe 14.51, lld-link 18, GNU ld's PE linker.
 *     42.
 *   * Mach-O — YES. ld64 (Apple clang 21, arm64 and x86_64) fetches the member as
 *     for a strong reference, so both names are defined: 7 + 7 + 10 = 24.
 * Until round 4 DSS's archive pull followed every weak reference as if it were
 * strong, so the ELF and PE links fetched the member and returned 24 where every
 * one of their reference linkers returns 42.
 *
 * Each check has its own code: 3 means the function and the datum disagreed — one
 * rule decides both, on every reference. */
extern int hook(void) __attribute__((weak));
extern int hookv __attribute__((weak));

int main(void) {
    int const haveFunction = hook ? 1 : 0;
    int const haveDatum = &hookv ? 1 : 0;
    if (haveFunction != haveDatum) return 3;
    if (!haveFunction) return 42;   /* not fetched: ELF, PE */
    return hook() + hookv + 10;     /* fetched: Mach-O */
}
