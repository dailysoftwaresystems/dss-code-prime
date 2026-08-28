/* c9 per-target __has_include witness — the UNAVAILABLE arm (the IDENTICAL source
 * as has_include_available, compiled for windows-pe).
 *
 * <sys/time.h> ships `availableObjectFormats:["elf","macho"]` (pe EXCLUDED), so
 * __has_include(<sys/time.h>) is FALSE on windows-pe → the #else branch → exit 7.
 * The pe binary RUNS on the windows CI leg, so the exit code is a real runtime
 * witness that the preprocessor evaluated __has_include per-target. */
#if __has_include(<sys/time.h>)
int main(void) { return 42; }   /* header available — elf / macho */
#else
int main(void) { return 7; }    /* header unavailable — windows-pe */
#endif
