/* P69 fold 3f, lane p69/xa: the FOREIGN units of tests/link/test_gui_startups.cpp's stated-subsystem cells -- an
 * object that states the image's SUBSYSTEM with a linker directive (`#pragma comment(linker, "/SUBSYSTEM:<name>")`,
 * which cl writes into the object's `.drectve`) and nothing else of the program: the program's entries are the
 * test's own C units.
 *
 * One unit per macro:
 *   SUBSYSTEM_WINDOWS  /SUBSYSTEM:WINDOWS -- beside `main` and WinMain, link.exe starts WinMain (44)
 *   SUBSYSTEM_CONSOLE  /SUBSYSTEM:CONSOLE -- beside `main` and WinMain, link.exe starts `main` (11)
 * (MEASURED 2026-11-17 by probe-reference-cc runs 20261117-092912-9c3db0d9 and 20261117-100011-e9df126e, cl 19.52 + link.exe 14.52 /MD: the test's header holds
 * every cell.)
 *
 * HOW EACH OBJECT IS BUILT (probe-reference-cc run 20261117-100011-e9df126e, windows-x86_64-release): cl for x64,
 *   cl /nologo /c /O2 /MD /GS- /Brepro /experimental:deterministic /pathmap:<its dir>=X:\dss-fixture /D<MACRO> probe.c
 * from this file's text, each twice in two directories and compared byte for byte, recording no host path (the
 * object names itself X:\dss-fixture\probe.obj), then committed as
 * object_subsystem_<windows|console>_x86_64_pe_cl_MD_O2.obj. Their sizes and md5s: the test's header.
 */
#if defined(SUBSYSTEM_WINDOWS)
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
int dss_subsystem_windows_unit(void) { return 0; }
#elif defined(SUBSYSTEM_CONSOLE)
#pragma comment(linker, "/SUBSYSTEM:CONSOLE")
int dss_subsystem_console_unit(void) { return 0; }
#else
#error "name the unit: -DSUBSYSTEM_WINDOWS or -DSUBSYSTEM_CONSOLE"
#endif
