/* P69 fold 3f (lane xa; D-LK-PE-IMAGE-CANNOT-HONOUR-EVERY-LINKER-DIRECTIVE item (6)): the RUN witness for a program
 * whose entry is `wWinMain`, declared as Visual Studio's own template spells it (`APIENTRY`).
 *
 * The C runtime's wide GUI startup (wWinMainCRTStartup) hands wWinMain the four values WinMain gets, the command line
 * WIDE (✔MEASURED 2026-11-17, cl 19.52 + link.exe 14.52, and MinGW-w64 gcc 13.2 with -municode): the image's own
 * module instance, a null previous instance, the command line past the program's name, and the show value the
 * startup information states (STARTF_USESHOWWINDOW), else SW_SHOWDEFAULT. DSS's startup computes them the same way,
 * the command line through the UCRT's own `_get_wide_winmain_command_line`.
 *
 * The runner passes no argument, so the command line past the name is empty. Exit 42 when every value holds; each
 * failed one exits a distinct code:
 *   90 the instance is not this image's base (its DOS and PE headers, holding this image's own datum)
 *   91 the previous instance is not null
 *   92 the command line past the program's name is not empty
 *   93 the show value is not the one the startup information states, else SW_SHOWDEFAULT
 */
#include <windows.h>

static int anchor;

static int isThisImage(HINSTANCE instance) {
    unsigned char const *base = (unsigned char const *)instance;
    if (base == 0 || base[0] != 'M' || base[1] != 'Z') return 0;
    unsigned char const *pe = base + *(unsigned int const *)(base + 60);
    if (pe[0] != 'P' || pe[1] != 'E' || pe[2] != 0 || pe[3] != 0) return 0;
    unsigned int const sizeOfImage = *(unsigned int const *)(pe + 24 + 56);
    unsigned char const *self = (unsigned char const *)&anchor;
    return self > base && self < base + sizeOfImage;
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR commandLine, int show) {
    STARTUPINFOW si;
    if (!isThisImage(instance)) return 90;
    if (previous != 0) return 91;
    if (commandLine == 0 || commandLine[0] != 0) return 92;
    GetStartupInfoW(&si);
    if (show != ((si.dwFlags & STARTF_USESHOWWINDOW) != 0 ? (int)si.wShowWindow : SW_SHOWDEFAULT)) return 93;
    return 42;
}
