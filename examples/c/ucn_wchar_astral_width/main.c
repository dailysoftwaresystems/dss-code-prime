// C11/C23 6.4.3 + D-FFI-STDDEF-WCHAR-PE-WIDTH: `L"\U0001F600"` (😀) is a wchar_t
// string, and wchar_t is FORMAT-keyed. On pe (16-bit UTF-16) the astral cp needs a
// SURROGATE PAIR → 2 units + wide NUL = 3 elements × 2 bytes = sizeof 6. On elf/
// mach-o (32-bit) it is ONE unit + NUL = 2 elements × 4 bytes = sizeof 8. The ONE
// source witnesses the surrogate-pair count on pe vs. the single unit on the wide
// default, decided purely by the config wchar core — the definitive cross-format
// surrogate witness. The release arm returns the same constant per target.

int main(void) {
    return (int)sizeof(L"\U0001F600");
}
