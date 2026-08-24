/* Plan 25 — per-target struct `variants` END-TO-END witness (arm64-linux leg)
 * AND the RED-ON-DISABLE pin for the variant selector.
 *
 * The glibc arm64-linux `struct stat` is 128 bytes — a DIFFERENT layout from
 * x86-64-linux's 144 (st_mode at offset 16 not 24; st_nlink is a 32-bit field;
 * st_blksize is 32-bit with a __pad2). The variant selector picks the field list
 * whose `when` matches (arch=arm64, format=elf), so `sizeof(struct stat)` folds
 * to 128 → st_mode(40)+st_blocks(2)=42.
 *
 * RED-ON-DISABLE: neuter the per-target selector (always take variant[0], the
 * x86-64 144-byte layout) and arm64 compiles the WRONG layout → `sizeof` folds
 * to 144 ≠ 128 → exit 7. This is the whole point of the mechanism: one layout
 * silently mis-fits the other arch. */
#include <sys/stat.h>

int main(void) {
    struct stat s;
    s.st_mode   = 40;
    s.st_blocks = 2;
    return (sizeof(struct stat) == 128) ? (int)(s.st_mode + s.st_blocks) : 7;
}
