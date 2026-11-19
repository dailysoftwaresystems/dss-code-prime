/* The LIBRARY half of weak_reference_in_a_relocatable_link (P69 round 5,
 * D-LK-ARCHIVE-SEARCH-FETCHES-A-MEMBER-FOR-A-WEAK-REFERENCE): the one member of
 * the static library the relocatable link of f.c resolves against. It defines
 * the name f.c holds only as a WEAK reference. */
int hook(void) {
    return 23;
}
