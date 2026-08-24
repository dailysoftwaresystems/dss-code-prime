// C11/C23 6.4.5 + D-FFI-STDDEF-WCHAR-PE-WIDTH: wchar_t (the `L"…"` element) width
// is FORMAT-keyed — 2 bytes on Windows pe64 (the UTF-16 code unit), 4 bytes on
// elf/mach-o (the POSIX width). `sizeof(L"x"[0])` sizes ONE wchar_t element
// without needing <stddef.h>, so the exit code IS the per-target wchar_t width:
// pe64 → 2, elf(x86_64/arm64) → 4, mach-o → 4 (per-target exitCode in the
// manifest). The release arm returns the same constant.

int main(void) {
    return (int)sizeof(L"x"[0]);
}
