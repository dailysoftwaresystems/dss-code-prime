/* SOURCE of the committed fixture `tests/link/data/pe_security_cookie_probe_x86_64_pe_cl_MD_O2_GSoff.obj` (P69 fold
 * 3d, lane p69/xa; D-LK-PE-FOREIGN-OBJECT-GS-COOKIE-ROUTINES-UNDEFINED, the /GS- cell of the cookie's seeding).
 *
 * WHAT IT IS. A /GS- object -- it names no routine of the /GS family -- whose one function's frame is past a page, so
 * cl's prologue calls the stack probe `__chkstk`. DSS's probe lives in the runtime's assembly member beside
 * `__security_check_cookie`, which reads `__security_cookie`, whose member defines `__security_init_cookie`: so the
 * program this object ends in DEFINES the initializer, and DSS's startup calls it, as every reference startup calls it
 * for every program. `main` returns `probe_touch(1)`, 42.
 *
 * REBUILD: built through probe-reference-cc's keep mode (object mode, each twice, the two reports' md5s equal), the
 * probe's own directory mapped away so the object records no host path:
 *   cl /O2 /GS- /MD /Brepro /experimental:deterministic /pathmap:{probe}=X:\dss-fixture -> ..._cl_MD_O2_GSoff.obj
 * (cl 19.51, windows-x86_64-release.) The object's md5, and the compiler that wrote it, are recorded where the fixture
 * is read; no guard enforces them.
 */
int probe_touch(int k) {
    char volatile b[8192];
    b[k] = (char)(k + 40);
    b[8191 - k] = (char)k;
    return b[k] + b[8191 - k];
}
