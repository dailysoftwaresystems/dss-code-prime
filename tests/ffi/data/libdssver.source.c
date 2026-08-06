/* SOURCE of the committed fixture tests/ffi/data/libdssver.so.1.
 *
 * Built by a REAL GNU ld from a REAL --version-script (see libdssver.map);
 * the committed artifact is that linker's own output, never bytes this
 * repository hand-assembled. Rebuild with:
 *
 *   gcc -shared -fPIC -Os -o libdssver.so.1 libdssver.source.c \
 *       -Wl,--version-script=libdssver.map -Wl,-soname,libdssver.so.1 \
 *       -Wl,--build-id=none && strip --strip-all libdssver.so.1
 *
 * EXPORT shapes, one image, all four the reader must tell apart:
 *   dss_ver_default -> DEFAULT-versioned        (sym@@DSSVER_2.0)
 *   dss_ver_compat  -> TWO definitions of ONE name: the OLD compat
 *                      (sym@DSSVER_1.0, VERSYM_HIDDEN set) plus the DEFAULT
 *                      (sym@@DSSVER_2.0) -- the glibc `realpath` shape
 *   dss_ver_plain   -> UNVERSIONED (base/global, versym == VER_NDX_GLOBAL)
 *   dss_ver_data    -> a versioned DATA object (STT_OBJECT), not a function
 *
 * IMPORT shape, and it is load-bearing: `dss_ver_uses_realpath` calls a
 * VERSIONED glibc symbol, so ld emits a `.gnu.version_r` (verneed) whose
 * vernaux indices continue the SAME numbering the verdef entries started.
 * A reader that resolved every versym slot through the verdef table without
 * first filtering SHN_UNDEF rows would hit an index verdef never defines --
 * which is exactly the confusion this fixture exists to make visible.
 */

#include <stdlib.h>

int dss_ver_default(void) { return 20; }

int dss_ver_compat_v1(void) { return 1; }
int dss_ver_compat_v2(void) { return 2; }
__asm__(".symver dss_ver_compat_v1, dss_ver_compat@DSSVER_1.0");
__asm__(".symver dss_ver_compat_v2, dss_ver_compat@@DSSVER_2.0");

int dss_ver_plain(void) { return 7; }

int dss_ver_data = 42;

/* Forces a VERSIONED undefined import (realpath@GLIBC_2.3 on x86_64 glibc). */
char *dss_ver_uses_realpath(char const *p) { return realpath(p, NULL); }
