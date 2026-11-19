/* P69 fold 3f, lane p69/xa: the FOREIGN units of tests/link/test_manifest_dependency.cpp -- objects that state a
 * side-by-side assembly the image depends on with a linker directive (`#pragma comment(linker,
 * "/manifestdependency:...")`, which cl writes into the object's `.drectve`) and nothing else of the program: the
 * program's entry is the test's own C unit.
 *
 * One unit per macro:
 *   MANIFEST_CC6   the common-controls 6 dependency every GUI program states
 *   MANIFEST_GDIP  the GdiPlus 1.0 dependency
 *   MANIFEST_BOTH  both, the common-controls one first, in ONE unit
 *   MANIFEST_MISSING  an assembly no Windows installs: the loader refuses to start the image (the dependency is
 *                     ACTIVATED, not merely present)
 * (MEASURED 2026-11-17 by probe-reference-cc runs 20261117-140648-927055da, 20261117-140952-a037b4db, 20261117-142310-025e5c2e and 20261117-142455-10b66d4b, cl 19.52 + link.exe 14.52 and clang 18.1.3 +
 * lld-link: the test's header holds every cell.)
 *
 * HOW EACH OBJECT IS BUILT (probe-reference-cc run 20261117-143614-c0ec388c, windows-x86_64-release): cl for x64,
 *   cl /nologo /c /O2 /MD /GS- /Brepro /experimental:deterministic /pathmap:<its dir>=X:\dss-fixture /D<MACRO> probe.c
 * from this file's text, each twice in two directories and compared byte for byte, recording no host path (the
 * object names itself X:\dss-fixture\probe.obj), then committed as
 * manifest_dependency_<cc6|gdip|both|missing>_x86_64_pe_cl_MD_O2.obj. Their sizes and md5s: the test's header.
 */
#define DSS_CC6 "type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
                "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'"
#define DSS_GDIP "type='win32' name='Microsoft.Windows.GdiPlus' version='1.0.0.0' " \
                 "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'"
#define DSS_MISSING "type='win32' name='Dss.Probe.MissingAssembly' version='1.0.0.0' " \
                    "processorArchitecture='*' publicKeyToken='0000000000000000' language='*'"

#if defined(MANIFEST_CC6)
#pragma comment(linker, "/manifestdependency:\"" DSS_CC6 "\"")
int dss_manifest_cc6_unit(void) { return 0; }
#elif defined(MANIFEST_GDIP)
#pragma comment(linker, "/manifestdependency:\"" DSS_GDIP "\"")
int dss_manifest_gdip_unit(void) { return 0; }
#elif defined(MANIFEST_BOTH)
#pragma comment(linker, "/manifestdependency:\"" DSS_CC6 "\"")
#pragma comment(linker, "/manifestdependency:\"" DSS_GDIP "\"")
int dss_manifest_both_unit(void) { return 0; }
#elif defined(MANIFEST_MISSING)
#pragma comment(linker, "/manifestdependency:\"" DSS_MISSING "\"")
int dss_manifest_missing_unit(void) { return 0; }
#else
#error "name the unit: -DMANIFEST_CC6, -DMANIFEST_GDIP, -DMANIFEST_BOTH or -DMANIFEST_MISSING"
#endif
