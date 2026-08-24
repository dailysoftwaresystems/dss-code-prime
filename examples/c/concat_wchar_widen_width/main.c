// Cycle D (C11/C23 6.4.5p5) + D-FFI-STDDEF-WCHAR-PE-WIDTH: the headline
// narrow-widen `L"ab" "cd"` — the narrow trailing "cd" widens to wchar_t so the
// whole run is Array<wchar_t, 5> ('a','b','c','d', wide NUL). `sizeof` of it is
// therefore 5 * sizeof(wchar_t), and wchar_t is FORMAT-keyed: 2 bytes on Windows
// pe64 (the UTF-16 unit) → 10, and 4 bytes on elf/mach-o (the POSIX width) → 20.
//
// The single sizeof fold witnesses BOTH facts at once: the COUNT 5 proves the
// narrow "cd" was included in the run (a dropped piece → 3 elements → 6/12; a
// per-token mis-split → wrong count), and the WIDTH multiplier proves the effective
// prefix is wchar_t (a narrow-mistype → element size 1 → 5). Per-target exitCode in
// the manifest. The release arm returns the same constant per target.

int main(void) {
    return (int)sizeof(L"ab" "cd");
}
