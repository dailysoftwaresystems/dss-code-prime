/* LANE-O (D-LK-IMAGE-DATA-SLOT-EXTERN-ADDR, the PE witness of roadmap C2): the ADDRESS of an extern CRT DATA export
   PLUS AN OFFSET, in a file-scope (static) initializer, on the PE image arm.

   ★★ THIS EXAMPLE WAS THE ARM'S FAIL-LOUD WITNESS, AND SINCE P69 IT RUNS. Until P69 the shape below was REJECTED
   (K_RelocationKindMismatch, no artifact): PE has no symbol-based image relocation, so the only value a writer could
   bake was the IAT slot's own address plus the offset — one indirection off. P69 bound it in two steps
   (D-LK-LIBRARY-FUNCTION-ADDRESS-IS-THE-IMAGE-STUB, design c2, then D-LK-PE-IMPORT-ADDRESS-SLOT-RESIDUE):
   `&_mbcasemap` alone became the FirstThunk of an import descriptor of its own, which the Windows LOADER fills; and
   `&_mbcasemap + 1`, which the loader cannot write (it writes an import's address and nothing else), is written by
   the image's RESIDUE RUNNER — one synthesized function, the image's first TLS callback, which runs on
   DLL_PROCESS_ATTACH after the loader has bound the IAT and before `main`, reads `_mbcasemap`'s bound address from
   its IAT entry and stores it plus 8 into `p`. It was `extern_data_addr_reject_pe` until P69 round 3, when it was
   renamed for what it witnesses (it refuses nothing).

   THE REFERENCES (✔MEASURED 2026-10-01, this source's shape, work/xa/drafts/mbprobe): MinGW gcc 13.2.0 + GNU ld 2.42
   link it and run it to 42 at -O0 and -O2 through their runtime pseudo-relocations, `p - 1` being the address
   GetProcAddress gives `_mbcasemap` — one working reference makes it REQUIRED. cl 19.51 and clang-cl (/O2 /MD) link
   a plain `extern` of it too, but bind the name to the import library's THUNK (`&_mbcasemap` != GetProcAddress, exit
   106 under the check below), and cl refuses a `__declspec(dllimport)` declaration of it (C2099).

   The check, against the loader: `p` (written at load) must equal `&_mbcasemap + 1` as `main` computes it at run
   time (through the IAT), and `p - 1` and `&_mbcasemap` must both be the address GetProcAddress gives `_mbcasemap`.
   Exit 42, or 100 + a bitset: 1 the static differs from the code's, 2 `p - 1` is not the loader's datum, 4 the code's
   `&_mbcasemap` is not, 8 the loader lookup failed.

   ★ WHY `_mbcasemap`, NOT `_fmode`. `_fmode` is not a ucrtbase export at all (✔MEASURED over all 2,484 ucrtbase.dll
   exports on two instruments; UCRT publishes it only through `__p__fmode`), so a bare `extern int _fmode;` is unbound
   and fails K_SymbolUndefined. `_mbcasemap` IS a real ucrtbase DATA export (ordinal 527, in READ-WRITE `.data`) and
   ships from ctype.json as a pe-gated `kind: object` row. It is typed `unsigned char *` because the UCRT export holds a
   POINTER to the case map, not the map (examples/c/extern_data_import_pe has the probe).

   RED-ON-DISABLE: make the runner skip DLL_PROCESS_ATTACH (the reason value its guard compares, in pe.cpp) -> `p`
   keeps the file's zero -> exit 103. */
#include <windows.h>

extern unsigned char *_mbcasemap;
unsigned char **p = &_mbcasemap + 1;

int main(void) {
    HMODULE crt = LoadLibraryA("ucrtbase.dll");
    void *bound = crt ? (void *)GetProcAddress(crt, "_mbcasemap") : 0;
    unsigned char **q = &_mbcasemap;
    int bad = 0;
    if (bound == 0) bad |= 8;
    if (p != q + 1) bad |= 1;
    if ((void *)(p - 1) != bound) bad |= 2;
    if ((void *)q != bound) bad |= 4;
    return bad == 0 ? 42 : 100 + bad;
}
