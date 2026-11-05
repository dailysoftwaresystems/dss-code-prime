/* The LIBRARY half of weak_reference_and_the_archive_search (P69 round 4,
 * D-LK-ARCHIVE-SEARCH-FETCHES-A-MEMBER-FOR-A-WEAK-REFERENCE). The runner builds
 * this unit into a static library (the target's `dependsOn`); its one member
 * defines both names `main.c` holds only as WEAK references. Whether the static
 * link's archive search fetches this member for them is the members' format's
 * `archiveWeakReferenceSearch`: ELF and PE say no, Mach-O says yes. */
int hook(void) {
    return 7;
}

int hookv = 7;
