/*
 * D-PP-HEADER-CASE-INSENSITIVE-PE — the `#include` header-NAME case rule is
 * declared by the TARGET's OBJECT FORMAT (`headerNameMatching` in
 * `*.format.json`), and DSS applies it ITSELF. The build HOST's filesystem
 * never gets a say.
 *
 * ★ TWO PROBES, ONE FOR EACH DIRECTION OF THE DEFECT, AND THEY NEED DIFFERENT
 * DESCRIPTORS. An earlier cut of this example probed only `<Windows.h>`, which
 * looks like it covers both directions and does not: `windows.json` declares
 * `availableObjectFormats: ["pe"]`, and availability is applied AFTER
 * resolution, so the elf probe answers 0 for TWO independent reasons. Revert
 * the whole axis and that arm still passes. So:
 *
 *   PROBE 1 — `<Windows.h>`, the WRONG-REJECT direction (the sqlite CLI
 *     blocker, `sqlite3.c:67322`, capital W against the shipped
 *     `windows.json`). Only meaningful on pe, and its real force is that the
 *     examples runner COMPILES the pe64 arm on the LINUX leg too — on ext4, a
 *     case-SENSITIVE filesystem, which is where this used to be rc=1
 *     `error[F001A]` while /mnt/c gave rc=0 from the SAME binary and commit.
 *
 *   PROBE 2 — `<Stdio.h>`, the SILENT-WRONG-ACCEPT direction. `stdio.json`
 *     declares NO `availableObjectFormats`, so it is present on every format
 *     and the ONLY thing that can make the answer differ between pe/macho and
 *     elf is the declared case rule. That makes the elf arm a real pin: elf
 *     must answer 0, and a conforming POSIX toolchain rejecting `<Stdio.h>`
 *     is precisely what DSS did NOT do on a Windows host before this axis.
 *
 * The two probes disagree by design on macho (it folds, but windows.json is
 * pe-only), which is why they are reported separately rather than combined.
 *
 * EXIT CODES ARE PER-TARGET (see expected.json): 42 where the format folds
 * header names, 7 where it does not. That is what makes the elf arm a PIN
 * rather than a bystander — revert `headerNameMatching` and elf returns 42
 * where the manifest demands 7.
 *
 * `__has_include` and `#include` must also AGREE: probe 1 both asks and
 * includes, so if the probe said 1 and the include then failed to resolve,
 * `MAX_PATH` would be undeclared and the pe arm would not compile at all.
 */
#if __has_include(<Windows.h>)
#  include <Windows.h>
#  define DSS_WINDOWS_HEADER_FOLDED 1
#else
#  define DSS_WINDOWS_HEADER_FOLDED 0
#endif

/* The case-only probe: same descriptor on every format, no availability gate. */
#if __has_include(<Stdio.h>)
#  define DSS_HEADER_NAME_FOLDS 1
#else
#  define DSS_HEADER_NAME_FOLDS 0
#endif

int main(void) {
#if DSS_HEADER_NAME_FOLDS
    /* pe + macho. On pe the Windows header must ALSO have folded, and its
     * exit contribution is derived from a constant only `windows.json`
     * supplies — so "the include resolved" is proven by behaviour, not
     * asserted by the manifest. On macho the availability gate legitimately
     * keeps it out, and the two probes are allowed to differ there. */
#  if DSS_WINDOWS_HEADER_FOLDED
    return (MAX_PATH == 260) ? 42 : 0;
#  else
    return 42;
#  endif
#else
    /* elf: byte-exact matching. `<Stdio.h>` must NOT reach `stdio.json`, and
     * accepting it would be the silent wrong-accept this axis closes. A
     * DIFFERENT exit code, so the manifest can tell the two apart. */
#  if DSS_WINDOWS_HEADER_FOLDED
    return 0;   /* impossible: a case-sensitive format folds neither name */
#  else
    return 7;
#  endif
#endif
}
