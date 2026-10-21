/* SOURCE of the committed fixtures `tests/link/data/custom_section_<isa>_<format>.{o,obj}` (P69 fold 2, lane
 * p69/xa; D-LK-COFF-READER-REFUSES-A-CUSTOM-NAMED-SECTION).
 *
 * WHAT IT IS. A definition in a section of the program's own naming, one object per format, all written by ONE
 * compiler so that the fixtures differ in format alone. Every reference linker keeps such a section as its own.
 * DSS: the ELF reader carries it (a section no row names is classified by its flags) and the program reads 42; the
 * COFF and Mach-O readers have no row for it and refuse the object BY NAME, `K_ObjectSectionNotModelled` -- the
 * object is well-formed, which the code it was refused under until P69 (`F_CorruptedBinary`) denied.
 *
 * REBUILD (clang 19.1.5; no path of the builder is recorded in any object):
 *   clang --target=<triple> -O0 -c -DSECTION='"<name>"' custom_section.source.c -o <fixture>
 *     x86_64-linux-gnu         dssdata          custom_section_x86_64_elf.o
 *     aarch64-linux-gnu        dssdata          custom_section_aarch64_elf.o
 *     x86_64-pc-windows-msvc   dssdata          custom_section_x86_64_pe.obj
 *     arm64-apple-macos11      __DATA,dssdata   custom_section_arm64_macho.o
 *     x86_64-apple-macos10.13  __DATA,dssdata   custom_section_x86_64_macho.o
 *   The COFF object is built with `-mno-incremental-linker-compatible` besides: without it the object's header
 *   records the time it was built, and no two builds are the same bytes.
 * Each object's md5 is recorded where the fixtures are read (`tests/link/test_common_symbols.cpp`, the suite
 * `ObjectSectionsNoRowNames`); no guard enforces it.
 */
__attribute__((section(SECTION))) int dss_custom_datum = 42;
int dss_custom_read(void) { return dss_custom_datum; }
