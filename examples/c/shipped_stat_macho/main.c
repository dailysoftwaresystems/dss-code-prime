/* c15c — shipped <sys/stat.h> on macOS: the struct stat LAYOUT witness + the
 * RED-ON-DISABLE pin for the macho variant selector. THE same-size-swap case
 * the c13 lesson flagged.
 *
 * macOS's struct stat is 144 bytes — the SAME SIZE as x86-64-linux (variant[0])
 * but a COMPLETELY different layout. So sizeof alone CANNOT discriminate macho
 * from x86-64-elf; the pin is a FIELD OFFSET. On macOS (Darwin field order:
 * st_dev/mode/nlink/ino/uid/gid/rdev, a 4-byte pad, then FOUR 16-byte timespecs
 * incl. st_birthtimespec) st_size sits at byte 96; on x86-64-elf it is at byte
 * 48. st_birthtim_sec is a macho-ONLY field (x86-64-elf has no birthtime).
 * fstat() links from libSystem (plain `fstat` on arm64-macho — the 64-bit-inode
 * ABI, no $INODE64 suffix).
 *
 * RED-ON-DISABLE: neuter the variant selector (always variant[0]=x86-64-elf) and
 * the macho build either (a) cannot resolve st_birthtim_sec (compile fails) or,
 * with that line removed, (b) computes st_size@48 != 96 → exit 1. A size-only
 * check (sizeof==144) would NOT catch the swap — that is the whole point. */
#include <sys/stat.h>

int main(void) {
    struct stat s;
    fstat(1, &s);                                /* fstat links from libSystem; fills s */
    long off_size = (long)&s.st_size - (long)&s; /* macho:96  x86-64-elf:48 */
    s.st_birthtim_sec = 9;                       /* macho-ONLY field → proves the macho variant */
    return (sizeof(struct stat) == 144 && off_size == 96
            && (long)s.st_birthtim_sec == 9) ? 42 : 1;
}
