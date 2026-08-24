/* c9 per-target __has_include witness — the AVAILABLE arm.
 *
 * <sys/time.h> ships `availableObjectFormats:["elf","macho"]`, so
 * __has_include(<sys/time.h>) is TRUE on the unix object-formats and FALSE on
 * windows-pe. The preprocessed token stream (and thus the exit code) therefore
 * differs PER TARGET — this is the per-target preprocessing the c9 restructure
 * delivers. This dir pins the available (#if-true) branch; the IDENTICAL source
 * in has_include_unavailable_pe pins the unavailable (#else) branch on pe. */
#if __has_include(<sys/time.h>)
int main(void) { return 42; }   /* header available — elf / macho */
#else
int main(void) { return 7; }    /* header unavailable — windows-pe */
#endif
