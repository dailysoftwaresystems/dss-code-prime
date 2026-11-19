/* SOURCE of the committed fixture `tests/link/data/defined_at_an_offset_x86_64_elf_gcc.o` (P69, lane p69/xa; the
 * re-review of fold 3b, MINOR 3; D-C-TENTATIVE-DEFINITIONS-OF-ONE-NAME-IN-TWO-UNITS-REFUSED).
 *
 * WHAT IT IS. A definition of `w` that sits at offset 1 of a 64-aligned `.data`: `pad` asks 64 and comes first, so
 * the section is 64-aligned and `w` is aligned to 1. Beside a unit's `_Alignas(64) char w;` the common yields to
 * this definition, which is LESS aligned than the common's unit relies on -- said by every link that measures the
 * definition at the alignment it has (the smaller of its section's and its offset's), and missed by one that reads
 * the section's alignment as the definition's.
 *
 * REBUILD (gcc 13.3.0, Ubuntu 24.04, x86_64; run in this directory, so no path of the builder is recorded):
 *   gcc -O0 -c defined_at_an_offset.source.c -o defined_at_an_offset_x86_64_elf_gcc.o
 * The object's md5 is recorded where it is read (`tests/link/test_tentative_definitions_as_commons.cpp`, the case
 * `ADefinitionAtAnOffsetIsAlignedAsItsOffsetAllows`); no guard enforces it.
 */
char pad __attribute__((aligned(64))) = 1;
char w = 1;
