/* The RELOCATABLE half of weak_reference_in_a_relocatable_link (P69 round 5,
 * D-LK-ARCHIVE-SEARCH-FETCHES-A-MEMBER-FOR-A-WEAK-REFERENCE). The runner links
 * this unit into a relocatable object beside hooklib.c's static library (this
 * `dependsOn` entry's own `dependsOn`), whose member defines `hook`. `hook` is
 * a WEAK reference here: an ELF or PE relocatable link fetches nothing for it
 * and writes it back as a weak undefined name, a Mach-O one fetches the member. */
extern int hook(void) __attribute__((weak));

int f(void) {
    return hook ? hook() : 0;
}
