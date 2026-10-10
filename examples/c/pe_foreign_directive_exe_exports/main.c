/* A linked object's `/EXPORT:` exports from an EXE (see expected.json). The REFERENCE-built
   archive's first member asks for a dllexport function and datum, a rename, a PRIVATE export, this
   unit's `dss_main_fn` (exports are link-wide) and `dss_pulled_fn`, which only the archive's
   SECOND member defines and nothing but the export names -- so the export alone pulls that member.
   This program looks each one up in its own image. */
#include <windows.h>

extern int dss_export_member_anchor(void);

int dss_main_fn(void) { return 11; }

typedef int (*dss_fn)(void);

int main(void) {
    HMODULE const self = LoadLibraryW(L"main.exe");
    if (self == 0) return 1;
    dss_fn const exported = (dss_fn)GetProcAddress(self, "dss_exported_fn");
    if (exported == 0 || exported() != 40) return 2;
    int const *const datum = (int const *)GetProcAddress(self, "dss_exported_datum");
    if (datum == 0 || *datum != 5) return 3;
    dss_fn const renamed = (dss_fn)GetProcAddress(self, "dss_renamed_fn");
    if (renamed == 0 || renamed() != 2) return 4;
    if (GetProcAddress(self, "dss_internal_fn") != 0) return 5;   /* a rename exports the new name alone */
    dss_fn const priv = (dss_fn)GetProcAddress(self, "dss_private_fn");
    if (priv == 0 || priv() != 3) return 6;
    dss_fn const mine = (dss_fn)GetProcAddress(self, "dss_main_fn");
    if (mine == 0 || mine() != 11) return 7;
    dss_fn const pulled = (dss_fn)GetProcAddress(self, "dss_pulled_fn");
    if (pulled == 0 || pulled() != 9) return 8;
    if (GetProcAddress(self, "main") != 0) return 9;              /* an EXE exports what it was asked to */
    return 42 + dss_export_member_anchor();
}
