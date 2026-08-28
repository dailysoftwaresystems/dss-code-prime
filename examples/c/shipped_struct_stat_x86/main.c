/* Plan 25 — per-target struct `variants` END-TO-END witness (x86-64-linux leg).
 *
 * `#include <sys/stat.h>` resolves to shippedLibs/sys/stat.json, whose `struct
 * stat` carries PER-TARGET `variants`. The glibc x86-64-linux layout is 144
 * bytes (st_nlink is a 64-bit field; st_mode at offset 24, after a 4-byte
 * __pad0 that 8-aligns st_rdev). The variant selector picks the field list whose
 * `when` matches (arch=x86_64, format=elf), so `sizeof(struct stat)` folds to
 * 144 and the real field names resolve. exit 42.
 *
 * The DIVERGENCE (and the red-on-disable) is on the arm64 leg
 * (shipped_struct_stat_arm64): there `struct stat` is 128 bytes, so neutering
 * the selector — which would hand arm64 the x86-64 144-byte variant — flips that
 * leg's exit. This leg pins that x86-64 still gets its own 144-byte layout. */
#include <sys/stat.h>

int main(void) {
    struct stat s;
    s.st_mode   = 40;
    s.st_blocks = 2;
    return (sizeof(struct stat) == 144) ? (int)(s.st_mode + s.st_blocks) : 7;
}
