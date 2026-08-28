// c8 per-target header availability — the FAIL-LOUD witness.
//
// <sys/time.h> is a POSIX header: its shipped descriptor (src/dss-config/
// shippedLibs/sys/time.json) declares `availableObjectFormats:["elf","macho"]`.
// Compiling an `#include` of it for a windows-pe target therefore must FAIL
// LOUD with F_ShippedHeaderUnavailableForTarget — never silently resolve the
// header as if it were present on every platform (the pre-c8 behavior, a
// wrong-platform silent miscompile).
//
// The header is NOT used (no `struct timeval`) so the ONLY diagnostic is the
// availability gate itself — a clean single-diagnostic error manifest.
//
// RED-ON-DISABLE: remove the availability gate (or `sys/time.json`'s
// availableObjectFormats) and the include resolves silently → zero diagnostics
// → this manifest fails (expected F_ShippedHeaderUnavailableForTarget, got none).
#include <sys/time.h>

int main(void) {
    return 42;
}
