/* c16 — shipped <dlfcn.h> + <sys/ioctl.h> on linux: the dynamic-loader surface
 * SQLite uses for extension loading, on a real libc. dlopen(NULL, ...) returns the
 * main-program handle; dlsym(h, "malloc") resolves a libc symbol (proves dlopen +
 * dlsym actually run, linked from libc.so.6 — glibc 2.34+ merged libdl). ioctl(-1)
 * links from libc + fails EBADF harmlessly (proves sys/ioctl's variadic extern).
 * RTLD_NOW=2 agrees cross-platform; RTLD_GLOBAL=256 is the ELF value.
 *
 * RED-ON-DISABLE: a broken per-format selector gives RTLD_GLOBAL the macho value 8
 * != 256 → exit 1; dropping the dl* symbols → undefined → compile/link fails. */
#include <dlfcn.h>
#include <sys/ioctl.h>

int main(void) {
    void *h   = dlopen((void*)0, RTLD_NOW | RTLD_GLOBAL);   /* main-program handle */
    void *sym = dlsym(h, "malloc");                          /* resolve a libc symbol */
    int   r   = ioctl(-1, 0);                                /* sys/ioctl links; EBADF -> <0 */
    return (RTLD_NOW == 2 && RTLD_GLOBAL == 256
            && h != (void*)0 && sym != (void*)0
            && r < 0) ? 42 : 1;
}
