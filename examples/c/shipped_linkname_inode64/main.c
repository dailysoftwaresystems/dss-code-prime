/* TF-C121 (D-FFI-SHIPPED-SYMBOL-PER-TARGET-LINK-NAME) — the RUNTIME witness for
 * the per-target link BASE name, and specifically for the ONE assertion whose
 * absence let a silent misbinding corrupt real data.
 *
 * THE DEFECT THIS PINS. Darwin reaches its modern 64-bit-inode ABI through
 * `$INODE64` asm-label aliases on x86_64 and through the PLAIN names on arm64
 * (sys/cdefs.h: __DARWIN_ONLY_64_BIT_INO_T is 0 on x86_64, 1 on arm64). DSS
 * declared the plain names everywhere, so an x86_64-Darwin build bound the
 * LEGACY 32-bit-inode implementations while compiling the MODERN 144-byte
 * `struct stat`. The legacy callee writes 120 bytes; `st_size` is READ at
 * offset 96 and WRITTEN at 72. Result: `fstat` reports st_size == 0, sqlite
 * concludes every database file is empty, and the CLI answers "database disk
 * image is malformed". Nothing failed loud — a descriptor SHADOWS the SDK
 * header entirely, so the platform's own asm label never participates, and
 * libSystem exports BOTH spellings so the plain import resolves.
 *
 * ★ WHY THIS FILE EXISTS ALONGSIDE examples/c/shipped_sys_stat. That
 *   example already calls fstat — and it PASSED throughout the bug, because its
 *   assertion is `st.st_size >= 0`, which a zero satisfies. THE VALUE WAS NEVER
 *   CHECKED. So this one writes a known number of bytes and demands EXACTLY
 *   that number back, three times (fstat by fd, stat by path, lstat by path).
 *   Under the misbinding every one of them reads 0 and this example reds.
 *
 * ★ opendir/readdir are the LATENT half of the same defect, so they are here
 *   too: DSS compiles the modern 1048-byte `struct dirent` (d_name at 21) and
 *   the legacy binding returns the old one (d_name at 8), so the name compare
 *   below reads out of the middle of d_ino/d_seekoff and never matches.
 *
 * ★ arm64 IS THE MATCHED CONTROL, not an afterthought. On arm64-Darwin the
 *   modern ABI is the only ABI, no `linkName` variant matches, the plain names
 *   stay — and this example must keep passing there byte-for-byte. The elf arms
 *   are the second control: no Linux symbol carries a link name at all, so they
 *   exercise the DEFAULT path through the same single decoration rule.
 *
 * HERMETIC: the only file created is /tmp/dss_c121_inode64_<pid>.bin, unlinked
 * before exit; <pid> makes it collision-free against a concurrent run, and /tmp
 * is the same directory the readdir scan walks, so the entry the scan must find
 * is one this process just created rather than a guessed environment fixture.
 * No size, path-layout, or filesystem assumption of any kind.
 *
 * EXIT ARITHMETIC: 4141 / 101 = 41, plus the readdir hit = 42. The 4141 is not
 * a literal in the sum — it is `st_size` as REPORTED BY THE CALL, so a binding
 * that answers 0 cannot reach 42 by any folding. Guard codes 11..22 name the
 * exact call that failed rather than leaving a bare "not 42".
 *
 * RED-ON-DISABLE: remove the `linkName` key from sys/stat.json's fstat row and
 * the macho64-x86_64 arm returns 14 (fstat reported st_size 0). Remove it from
 * dirent.json's readdir row and the same arm returns 22 (the entry is never
 * found — the name is read at the legacy offset). Both arms stay green on
 * arm64-macho and on elf, which is what makes the failure attributable. */
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#define PAYLOAD_BYTES 4141

static char payload[PAYLOAD_BYTES];

/* Decimal-render `v` into `out`; returns the digit count. Spelled out because
 * this example must not depend on the printf family to build a filename. */
static int u32_to_dec(unsigned v, char *out) {
    char tmp[12];
    int  n = 0;
    int  i;
    if (v == 0u) { out[0] = '0'; return 1; }
    while (v > 0u) { tmp[n] = (char)('0' + (int)(v % 10u)); n = n + 1; v = v / 10u; }
    for (i = 0; i < n; i = i + 1) out[i] = tmp[n - 1 - i];
    return n;
}

int main(void) {
    char base[48];   /* "dss_c121_inode64_<pid>.bin" — what readdir must find */
    char path[64];   /* "/tmp/" + base                                        */
    int  n;
    int  fd;
    int  found = 0;
    struct stat   sf;
    struct stat   sp;
    struct stat   sl;
    DIR          *d;
    struct dirent *e;

    n = 0;
    strcpy(base, "dss_c121_inode64_");
    n = (int)strlen(base);
    n = n + u32_to_dec((unsigned)getpid(), base + n);
    base[n] = '.'; base[n + 1] = 'b'; base[n + 2] = 'i'; base[n + 3] = 'n';
    base[n + 4] = 0;

    strcpy(path, "/tmp/");
    strcpy(path + 5, base);

    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return 11;
    if (write(fd, payload, (unsigned long)PAYLOAD_BYTES) != PAYLOAD_BYTES) {
        close(fd); unlink(path); return 12;
    }

    /* (1) fstat BY DESCRIPTOR — the exact call sqlite's os_unix.c makes, and
     *     the one that reported 0 under the legacy binding. */
    if (fstat(fd, &sf) != 0)              { close(fd); unlink(path); return 13; }
    if (sf.st_size != PAYLOAD_BYTES)      { close(fd); unlink(path); return 14; }
    if (!S_ISREG(sf.st_mode))             { close(fd); unlink(path); return 15; }

    /* (2) stat BY PATH — a different symbol with the same divergence. */
    if (stat(path, &sp) != 0)             { close(fd); unlink(path); return 16; }
    if (sp.st_size != PAYLOAD_BYTES)      { close(fd); unlink(path); return 17; }

    /* (3) lstat BY PATH — the third. The file is not a symlink, so lstat and
     *     stat must agree; a divergence here is a wrong callee, not a race. */
    if (lstat(path, &sl) != 0)            { close(fd); unlink(path); return 18; }
    if (sl.st_size != PAYLOAD_BYTES)      { close(fd); unlink(path); return 19; }
    if (sl.st_size != sp.st_size)         { close(fd); unlink(path); return 20; }

    /* (4) opendir/readdir — the LATENT half. A legacy `struct dirent` puts
     *     d_name at offset 8, not 21, so the compare below reads garbage and
     *     the file this process just created is never found. */
    d = opendir("/tmp");
    if (d == 0)                           { close(fd); unlink(path); return 21; }
    while ((e = readdir(d)) != 0) {
        if (strcmp(e->d_name, base) == 0) found = 1;
    }
    closedir(d);
    close(fd);
    unlink(path);
    if (found == 0) return 22;

    /* st_size as REPORTED, never as a literal: 4141/101 = 41, + the readdir
     * hit = 42. A binding that answers 0 yields 1, not 42. */
    return (int)(sf.st_size / 101) + found;
}
