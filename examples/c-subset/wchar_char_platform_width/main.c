// C11/C23 6.4.4.4 + D-FFI-STDDEF-WCHAR-PE-WIDTH: L'x' is a wchar_t constant, and
// wchar_t width is FORMAT-keyed — 2 bytes on Windows pe64 (the UTF-16 code unit),
// 4 bytes on elf/mach-o (the POSIX width). sizeof(L'x') is that per-target width
// WITHOUT needing <stddef.h>, so the exit code IS the wchar_t width: pe64 → 2,
// elf(x86_64/arm64) → 4, mach-o → 4. The ONE source witnesses the single config
// resolveElementCore resolution across every target; the release arm matches.

int main(void) {
    return (int)sizeof(L'x');
}
