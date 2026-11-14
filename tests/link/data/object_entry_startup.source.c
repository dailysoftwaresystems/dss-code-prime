/* P69 fold 3e, lane p69/xa (class C, N5): the FOREIGN units of tests/link/test_object_entry_startup.cpp -- an object
 * that STATES the image's entry with a linker directive (`#pragma comment(linker, "/ENTRY:<name>")`, which cl writes
 * into the object's `.drectve`), linked alone or beside an object defining `main`.
 *
 * One unit per macro; every image is run with no arguments:
 *   STARTUP_WMAIN     /ENTRY:wmainCRTStartup, defines `wmain`   -> 22 under link.exe and lld-link, beside `main` too
 *   STARTUP_MAIN      /ENTRY:mainCRTStartup, defines `main`     -> 33
 *   STARTUP_RAW       /ENTRY:dss_raw_entry, defines it          -> 66 (no C runtime runs; beside `main` too)
 *   STARTUP_WINMAIN   /ENTRY:WinMainCRTStartup, defines WinMain -> 44 (beside `main` too)
 *   STARTUP_WWINMAIN  /ENTRY:wWinMainCRTStartup, defines wWinMain -> 55 (beside `main` too)
 *   STARTUP_WIDE_ONLY /ENTRY:wmainCRTStartup, defines no `wmain` -> refused beside `main` (LNK2019 `wmain`); 22 beside
 *                     an archive of the STARTUP_WMAIN object, alone, beside `main` and after it
 *   ENTRY_MAIN_ONLY   defines `main`, states nothing            -> 11
 * (MEASURED 2026-11-05 by lane xa's n5_universe.py and n5_archive.py: link.exe 14.44 and lld-link 19.1.5 agree in every
 * cell, the request as a directive or as the link's own `/ENTRY:`, each beside `main` in either order, /SUBSYSTEM:CONSOLE.)
 *
 * HOW EACH OBJECT WAS BUILT (lane xa's build_startup_fixtures.py, 2026-11-05): cl 19.44.35229 for x64,
 *   cl /nologo /c /O2 /MD /GS- /Brepro /experimental:deterministic /pathmap:<dir>=X:\dss-fixture /D<MACRO>
 *      object_entry_startup.c /Foobject_entry_startup_<shape>_x86_64_pe_cl_MD_O2.obj
 * from this text less this paragraph, each built twice in two directories, byte-identical, recording no host path:
 *   wmain     656 B  md5 32d2d1ccb2a264d3f3ba1dd0e33cf3da     main      651 B  md5 8daa83bb5efab9b827a0dc7895c7c0b6
 *   raw       664 B  md5 d73652ea13b6c24bcdfb9ee19d097c51     winmain   658 B  md5 0a454d1a08ac0167ac7abb85e1db8ab2
 *   wwinmain  659 B  md5 ee69dbd53f74e960164e8c415573e665     wideonly  673 B  md5 7c3b000f93355aa0937d853c573d7749
 *   mainonly  633 B  md5 94ec4de94f67972465cbfd386d3e4864
 */
#if defined(STARTUP_WMAIN)
#pragma comment(linker, "/ENTRY:wmainCRTStartup")
int wmain(int argc, unsigned short **argv) { (void)argc; (void)argv; return 22; }
#elif defined(STARTUP_MAIN)
#pragma comment(linker, "/ENTRY:mainCRTStartup")
int main(void) { return 33; }
#elif defined(STARTUP_RAW)
#pragma comment(linker, "/ENTRY:dss_raw_entry")
int dss_raw_entry(void) { return 66; }
#elif defined(STARTUP_WINMAIN)
#pragma comment(linker, "/ENTRY:WinMainCRTStartup")
int __stdcall WinMain(void *a, void *b, char *c, int d) { (void)a; (void)b; (void)c; (void)d; return 44; }
#elif defined(STARTUP_WWINMAIN)
#pragma comment(linker, "/ENTRY:wWinMainCRTStartup")
int __stdcall wWinMain(void *a, void *b, unsigned short *c, int d) { (void)a; (void)b; (void)c; (void)d; return 55; }
#elif defined(STARTUP_WIDE_ONLY)
#pragma comment(linker, "/ENTRY:wmainCRTStartup")
int startup_wide_helper(void) { return 0; }
#elif defined(ENTRY_MAIN_ONLY)
int main(void) { return 11; }
#else
#error "name the unit: -DSTARTUP_WMAIN, -DSTARTUP_MAIN, -DSTARTUP_RAW, -DSTARTUP_WINMAIN, -DSTARTUP_WWINMAIN, -DSTARTUP_WIDE_ONLY or -DENTRY_MAIN_ONLY"
#endif
