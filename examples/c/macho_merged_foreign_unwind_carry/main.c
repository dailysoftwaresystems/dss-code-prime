/* D-LK-MERGED-FOREIGN-FUNCTIONS-CARRY-NO-UNWIND-INFO-IN-THE-IMAGE — the
 * MACH-O half. Sibling of `examples/c/merged_foreign_unwind_carry`, which is
 * the ELF one; the difference is the ENCODING, and that is the whole point.
 *
 * The DSS half of a link whose other half is a PREBUILT **Apple clang**
 * archive (`tests/link/data/libmacho_foreign_unwind_x86_64.a`, built by
 * `/usr/bin/cc -arch x86_64 -c` + `/usr/bin/ar rcs` on real Apple Silicon,
 * with NO flags at all). ✔MEASURED 2026-08-25: an x86_64 object from that
 * invocation carries `__LD,__compact_unwind` AND `__TEXT,__eh_frame`, while
 * its arm64 sibling from the SAME compiler and the SAME source carries
 * compact only. Until this cycle DSS read the compact section, merged the
 * code, and dropped every function's unwind description — and said so, which
 * is why the arm64 half of this row is still open.
 *
 * The call chain crosses the boundary TWICE:
 *
 *   main                     (DSS)    -> dss_macho_foreign_outer (FOREIGN)
 *   dss_macho_foreign_outer  (FOREIGN)-> dss_macho_foreign_inner (FOREIGN)
 *   dss_macho_foreign_inner  (FOREIGN)-> dss_macho_unwind_leaf   (DSS, below)
 *
 * so BOTH foreign functions are NON-LEAF and clang describes a real frame for
 * each — ✔MEASURED with Apple's own `dwarfdump --eh-frame` on the member:
 * exactly TWO FDEs, `pc=0..0x6d` and `pc=0x70..0x97`, matching the two
 * symbols. `dss_macho_unwind_leaf` is UNDEFINED in the member and defined
 * HERE, so the archive pull, the merge and the relocation all have to work
 * before the unwind carry is even reachable.
 *
 * inner(5,5) = (5+6+7+8+9) + leaf(5) = 35 + 6 = 41; outer = 42.
 *
 * ⚠ WHAT THIS EXAMPLE WITNESSES, AND WHAT IT DOES NOT — stated because a pin
 * that overclaims is worse than one that is absent. It witnesses that an
 * archive whose members CARRY DWARF call-frame information (the manifest's
 * `containerWitness` is the literal `__eh_frame`, so a member rebuilt with
 * `-fno-asynchronous-unwind-tables` fails the manifest instead of passing
 * vacuously) still LINKS and RUNS after the carry landed. It does NOT witness
 * the WALK from inside the program: that needs `backtrace` (libSystem) or
 * `_Unwind_Backtrace`, and no shipped library descriptor exposes either — the
 * same measured limit its ELF sibling records.
 *
 * The WALK is witnessed OUTSIDE this example, end to end on real hardware:
 * the linked image carries THREE FDEs whose pc-ranges match `_main` and BOTH
 * merged foreign symbols exactly, Apple's own `dwarfdump --eh-frame` renders
 * the merged function's per-PC state as the rules clang originally stated,
 * and the image RUNS natively on macOS with exit 42 — against ONE FDE (`_main`
 * alone) when the `dwarf-cfi` unwind row is deleted from the two x86_64
 * relocatable format documents, a CONFIG-level mutant with the compiler
 * binary byte-identical either side.
 */

extern long dss_macho_foreign_outer(long a, long b);

/* Referenced by the FOREIGN member; defined here. */
int dss_macho_unwind_leaf(int x) { return x + 1; }

int main(void) { return (int)dss_macho_foreign_outer(5, 5); }
