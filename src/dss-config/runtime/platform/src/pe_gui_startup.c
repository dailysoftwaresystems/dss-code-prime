/* THE TWO VALUES A C RUNTIME'S OWN STATIC GUI STARTUP COMPUTES (P69 fold 3f, lane xa) -- a unit of the pe formats'
 * `compilerSupport` row, whose routines the program-entry init calls when the program's entry is `WinMain` or
 * `wWinMain` (the exec document's `processArgs.winMainStartupRoutines`). The other two values those entries take come
 * from elsewhere: the command line past the program's name is the C library's own answer
 * (`_get_narrow_winmain_command_line` / `_get_wide_winmain_command_line`), and the previous instance is null.
 *
 * ✔MEASURED 2026-11-17 (lane xa, probe-reference-cc, cl 19.52 + link.exe 14.52 /O2 /MD and MinGW-w64 gcc 13.2 + GNU
 * ld 2.42, each image run with no argument): inside WinMain and wWinMain the instance is the image's own module handle
 * (== GetModuleHandleW(NULL)), the previous instance is null, the command line is empty, and the show value is the
 * one STARTUPINFOW states when its flags carry STARTF_USESHOWWINDOW, else SW_SHOWDEFAULT (10); STARTUPINFOW is 104
 * bytes, `dwFlags` at 60 and `wShowWindow` at 64 (the reference's own header, printed by the probe). MSVC's startup
 * passes `&__ImageBase` and `__scrt_get_show_window_mode()` (DOCUMENTED, the toolset's exe_winmain / exe_common.inl);
 * these are the same two computations. A program whose entry is neither never names these routines, so the archive
 * search never pulls this unit for it. */
#include <windows.h>

/* The image's DOS header, which every reference linker of the format defines in each image it links (the pe
 * documents' `imageDefinedSymbols`): its address IS the module handle of the image that holds this code. */
extern unsigned short const __ImageBase;

void *__dss_pe_module_instance(void) {
    return (void *)&__ImageBase;
}

int __dss_pe_show_window_mode(void) {
    STARTUPINFOW si;
    GetStartupInfoW(&si);
    return (si.dwFlags & STARTF_USESHOWWINDOW) != 0 ? (int)si.wShowWindow : SW_SHOWDEFAULT;
}
