/* c14d — shipped <sys/stat.h> constants/symbols/macros witness (the per-target
 * struct stat itself is witnessed by shipped_struct_stat_x86/arm64, c13). Here:
 * the S_ mode constants, the S_IS* function-like macros, and fstat (dynamically
 * linked; takes the struct stat* as a void* — standard C pointer coercion). A
 * struct stat is declared + field-accessed (the per-target layout) and passed to
 * fstat on stdin. The witness is the constant VALUES + the macros → exit 42 on a
 * real Linux runtime. RED-ON-DISABLE: drop a surface and S_IFREG/fstat/S_ISDIR
 * is undefined → compile fails. */
#include <sys/stat.h>

int main(void) {
    struct stat st;
    st.st_size = 5;          /* struct stat field write (per-target layout) */
    fstat(0, &st);           /* links fstat; struct stat* coerces to its void* param */
    return (S_IFREG == 32768 && S_IFDIR == 16384 && S_IRUSR == 256
            && S_ISDIR(S_IFDIR) && !S_ISREG(S_IFDIR)
            && st.st_size >= 0) ? 42 : 1;   /* st_size read (>=0 whether fstat filled it or not) */
}
