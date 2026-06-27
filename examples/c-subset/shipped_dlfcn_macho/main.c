/* c16 — shipped <dlfcn.h> on macOS: the RED-ON-DISABLE pin for the RTLD_GLOBAL
 * per-format constant variant. RTLD_GLOBAL = 8 (0x8) on Darwin vs 256 (0x100) on
 * Linux — and there's a COLLISION TRAP (0x8 is RTLD_DEEPBIND on glibc, 0x100 is
 * RTLD_FIRST on Darwin), so the value MUST come from the macho variant, never a
 * flat constant. dlopen(NULL, ...) + dlsym(h, "malloc") run from libSystem.
 *
 * RED-ON-DISABLE: a broken selector hands macho the elf value 256 != 8 → exit 1.
 * Runs on the macos-latest CI leg (native Apple Silicon). */
#include <dlfcn.h>

int main(void) {
    void *h   = dlopen((void*)0, RTLD_NOW | RTLD_GLOBAL);   /* main-program handle (libSystem) */
    void *sym = dlsym(h, "malloc");                          /* resolve a libSystem symbol */
    return (RTLD_NOW == 2 && RTLD_GLOBAL == 8
            && h != (void*)0 && sym != (void*)0) ? 42 : 1;
}
