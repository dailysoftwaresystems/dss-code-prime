/* P69 fold 3f, lane p69/xa: the FOREIGN units of tests/link/test_written_name_binding.cpp -- objects that WRITE a
 * name the platform's link spells another way (the shipped corpus' `linkerAlias` route on pe: `write` stands for
 * the POSIX names the UCRT exports only as `_<name>`), or a name no image of the platform exports. The program's
 * `main` is the test's own C unit: `int say(void); int main(void) { return say(); }`.
 *
 * One unit per macro:
 *   WRITE_HDR            `write` through the platform's own <io.h>, which declares it beside `_write` without
 *                        renaming it: exits 42 where `write` reaches the C runtime's `_write`
 *   ALTERNATENAME_WRITE  `write` with a fallback of the unit's own, `/alternatename:write=my_write`, beside the
 *                        platform's alias (oldnames.lib's `write` -> `_write`): 42 where the alias stands, 1 where
 *                        the unit's `my_write` (7) is called
 *   IOB_FUNC             `__iob_func`, the msvcrt runtime's stream accessor, which no UCRT image exports
 *
 * HOW EACH OBJECT IS BUILT (probe-reference-cc runs 20261117-134551-cb87f606 on windows-x86_64-release and run 20261117-134619-3841ae20 on
 * windows-x86_64-debug), from this file's text, each twice in two directories and compared byte for byte,
 * recording no host path:
 *   cl /nologo /c /O2 /GS- /Brepro /experimental:deterministic /pathmap:<its dir>=X:\dss-fixture /MT|/MD
 *      /D_CRT_NONSTDC_NO_WARNINGS /D<MACRO> probe.c   (written_name_<unit>_x86_64_pe_cl_<MT|MD>_O2.obj)
 *   gcc -c -O2 -ffile-prefix-map=<its dir>=. -D<MACRO> probe.c   (MinGW-w64 gcc;
 *      written_name_<unit>_x86_64_pe_gcc_O2.obj)
 * Each unit is also linked and RUN by its own reference beside the `main` above; the answers, the sizes and the
 * md5s: the test's header.
 */
#if defined(WRITE_HDR)
#include <io.h>
int say(void) { return write(1, "", 0) == 0 ? 42 : 1; }
#elif defined(ALTERNATENAME_WRITE)
#pragma comment(linker, "/alternatename:write=my_write")
int write(int, const void *, unsigned);
int my_write(int fd, const void *buf, unsigned n) { (void)fd; (void)buf; (void)n; return 7; }
int say(void) { return write(1, "", 0) == 0 ? 42 : 1; }
#elif defined(IOB_FUNC)
extern void *__iob_func(void);
int say(void) { return __iob_func() != 0 ? 42 : 1; }
#else
#error "name the unit: -DWRITE_HDR, -DALTERNATENAME_WRITE or -DIOB_FUNC"
#endif
