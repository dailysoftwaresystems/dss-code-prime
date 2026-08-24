/*
 * D-FF1-AR-BSD-CORPUS-EXAMPLE-NEEDS-A-PREBUILT-ARCHIVE-KEY-IN-BOTH-RUNNERS
 *
 * THE ONE THING THIS PROGRAM ASSERTS: DSS linked two functions out of a static
 * archive IT DID NOT BUILD, in a container ITS OWN `ar` WRITER CANNOT PRODUCE.
 *
 * Neither function is defined anywhere in this directory. They live inside the
 * BSD-flavour archives under `tests/ffi/data/`, produced on other machines by
 * other archivers (Apple `ar` on macOS 26.5.2, and `llvm-ar-19 --format=bsd` on
 * Ubuntu) from two trivial C sources recorded in `README-bsd-archives.md`:
 *
 *     dss_bsd_short() -> 20      the SHORT-named member ("s.o")
 *     dss_bsd_long()  -> 22      the member whose file name is longer than the
 *                                16-byte ar_hdr name field, which is what forces
 *                                the BSD `#1/N` INLINE-NAME shape
 *
 * ★ SO THE EXIT CODE IS THE WITNESS, AND IT CANNOT PASS BY ACCIDENT. Drop the
 * archive from the link and both names are unresolved externs — the build
 * fails. Resolve it with a reader that mis-parses the `#1/N` inline name and the
 * long-named member's payload starts N bytes off, so what gets merged is not a
 * function. There is no arrangement in which this program returns 42 without the
 * BSD parse arms having done their job.
 *
 * `folded_zero()` contributes exactly 0 and exists for the OTHER half of the
 * bar: the `release` arm declares `mustDifferFromBaseline`, so this file must
 * hold something the optimizer can actually transform. It must contribute ZERO
 * in both arms, because the example declares no `optimizationObservable` — the
 * two arms are required to agree on the exit code, and only on the bytes to
 * differ.
 */

extern int dss_bsd_short(void); /* 20, from the archive's short-named member */
extern int dss_bsd_long(void);  /* 22, from its `#1/N` inline-named member */

static int folded_zero(void) {
    int const a     = 6;
    int const b     = 7;
    int       total = 0;
    for (int i = 0; i < 3; ++i) {
        total += a * b;
    }
    return total - 126;
}

int main(void) {
    return dss_bsd_short() + dss_bsd_long() + folded_zero();
}
