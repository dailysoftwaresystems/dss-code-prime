/* c15c — shipped <unistd.h> + <sys/mman.h> on macOS: the libSystem LINKAGE +
 * availability witness for the two headers whose constants/signatures AGREE
 * Linux↔macOS (so they carry no value variant — only the per-format availability
 * + library). getpagesize() (unistd) and munmap() (sys/mman) are resolved from
 * /usr/lib/libSystem.B.dylib and RUN on real macOS: getpagesize returns the page
 * size (4096 or 16384 on Apple Silicon, always > 0); munmap(NULL,0) fails EINVAL
 * harmlessly (no unmap). The SEEK_/access/PROT_/MAP_SHARED constants agree with
 * Linux (verified) and are checked for presence.
 *
 * RED-ON-DISABLE: drop the macho availability (availableObjectFormats) → the
 * #includes fail loud on the macho target; the runtime exit proves the symbols
 * actually resolve from libSystem on Apple Silicon. */
#include <unistd.h>
#include <sys/mman.h>

int main(void) {
    int pg = getpagesize();        /* unistd symbol, libSystem */
    munmap((void*)0, 0);           /* sys/mman symbol, libSystem (EINVAL, harmless) */
    return (pg > 0
            && SEEK_SET == 0 && R_OK == 4 && W_OK == 2
            && PROT_READ == 1 && PROT_WRITE == 2 && MAP_SHARED == 1) ? 42 : 1;
}
