/* D-LK-MERGED-FOREIGN-FUNCTIONS-CARRY-NO-UNWIND-INFO-IN-THE-IMAGE.
 *
 * The DSS half of a link whose other half is a PREBUILT gcc archive
 * (`tests/link/data/libforeign_unwind_{x86_64,aarch64}.a`, built by
 * `gcc -c` + `ar rcs` with NO flags at all). Those members carry a real
 * `.eh_frame`, and until this cycle DSS read them, merged their code into
 * the image, and DROPPED their unwind description in silence.
 *
 * The call chain deliberately crosses the boundary TWICE:
 *
 *   main            (DSS)      -> dss_foreign_unwind_top   (FOREIGN, gcc)
 *   dss_foreign_unwind_top     -> dss_foreign_unwind_mid   (FOREIGN, gcc)
 *   dss_foreign_unwind_mid     -> dss_foreign_unwind_leaf  (DSS, below)
 *
 * so both foreign functions are NON-LEAF and gcc describes a real frame
 * for each. 21 + 20 + 1 = 42.
 *
 * `dss_foreign_unwind_leaf` is defined HERE on purpose: the merged member
 * references it as an undefined extern, so the archive pull, the merge and
 * the relocation all have to work before the unwind carry is even reachable.
 *
 * ⚠ WHAT THIS EXAMPLE WITNESSES, AND WHAT IT DOES NOT — stated because a
 * pin that overclaims is worse than one that is absent. It witnesses that a
 * foreign archive whose members CARRY unwind information (the manifest's
 * `containerWitness` is the literal `.eh_frame`, so a fixture regenerated
 * with `-fno-asynchronous-unwind-tables` fails the manifest instead of
 * passing vacuously) still LINKS and RUNS after the carry landed — i.e. that
 * decoding a foreign encoding into the neutral `CfiFunction` vocabulary and
 * re-emitting it does not corrupt the image. It does NOT witness the WALK:
 * observing an unwind from inside the program needs `backtrace` (glibc) or
 * `_Unwind_Backtrace` (libgcc), and ✔MEASURED 2026-08-24 on both ELF legs,
 * NEITHER binds — no shipped library descriptor exposes either symbol and
 * there is no platform-default binding (`K_SymbolUndefined` on x86_64 and on
 * aarch64, for a DSS-side extern AND for a merged member's own undefined
 * reference). The walk is witnessed instead by the row's own end-to-end
 * measurement, which uses `--resolve-library <libc.so.6>` to bind
 * `backtrace` and gets exit 42 through two merged foreign frames on both
 * legs — against exit 2 with the `unwind` section row removed.
 */

extern int dss_foreign_unwind_top(void);

/* Referenced by the FOREIGN member; defined here. */
int dss_foreign_unwind_leaf(int x) { return x + 1; }

int main(void) { return dss_foreign_unwind_top(); }
