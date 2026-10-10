/* <stdio.h>'s C 7.23 functions a program could not name before P69, run on every pair: a tmpfile round-tripped
 * through fgetpos/fsetpos, text formatted through vsnprintf/vsprintf/vprintf and read back through
 * sscanf/vsscanf/fscanf/vfscanf, setbuf on a BUFSIZ buffer, tmpnam into an L_tmpnam array. On pe and Mach-O the
 * printf and scanf families are DSS's C23 entry points (runtime/platform/src/stdio.c — over the UCRT's
 * __stdio_common_v* cores on pe, over libSystem's own functions on Mach-O); on ELF glibc's own. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int format_bounded(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int const r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

static int format_unbounded(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int const r = vsprintf(buf, fmt, ap);
    va_end(ap);
    return r;
}

static int read_string(const char *in, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int const r = vsscanf(in, fmt, ap);
    va_end(ap);
    return r;
}

static int read_stream(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int const r = vfscanf(f, fmt, ap);
    va_end(ap);
    return r;
}

static int print_line(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int const r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

static char stream_buffer[BUFSIZ];

int main(void) {
    char bounded[64];
    int const n = format_bounded(bounded, sizeof bounded, "%d-%s-%x", 42, "abc", 255);
    char small[4];
    int const want = format_bounded(small, sizeof small, "%d", 123456);
    char unbounded[64];
    int const m = format_unbounded(unbounded, "[%5.2f]", 3.14159);

    int a = 0;
    unsigned x = 0;
    char word[16];
    int const got = read_string("7 seven 1f", "%d %15s %x", &a, word, &x);
    int b = 0;
    int const got2 = sscanf("99 bottles", "%d", &b);

    FILE *f = tmpfile();
    if (f == NULL) {
        puts("tmpfile failed");
        return 1;
    }
    setbuf(f, stream_buffer);
    fprintf(f, "first ");
    fpos_t pos;
    if (fgetpos(f, &pos) != 0) return 2;
    fprintf(f, "second 17\n");
    if (fsetpos(f, &pos) != 0) return 3;
    char second[16];
    int v = 0;
    int const got3 = fscanf(f, "%15s %d", second, &v);
    rewind(f);
    char first[16];
    int const got4 = read_stream(f, "%15s", first);
    fclose(f);

    char name[L_tmpnam];
    char const *const tn = tmpnam(name);

    print_line("vsnprintf=%d:%s truncated=%d:%s vsprintf=%d:%s\n", n, bounded, want, small, m, unbounded);
    print_line("vsscanf=%d:%d,%s,%u sscanf=%d:%d\n", got, a, word, x, got2, b);
    print_line("tmpfile fsetpos+fscanf=%d:%s,%d vfscanf=%d:%s\n", got3, second, v, got4, first);
    print_line("minimums BUFSIZ=%d FOPEN_MAX=%d TMP_MAX=%d L_tmpnam=%d tmpnam=%d\n", BUFSIZ >= 256,
               FOPEN_MAX >= 8, TMP_MAX >= 25, L_tmpnam > 0,
               tn == name && strlen(name) > 0 && strlen(name) < L_tmpnam);
    return (n == 9 && want == 6 && m == 7 && got == 3 && got2 == 1 && got3 == 2 && got4 == 1) ? 42 : 1;
}
