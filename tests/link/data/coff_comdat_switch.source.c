/* P69 fold 3d -- the source of coff_comdat_switch_clangcl_O2.obj (D-LK-COFF-COMDAT-UNSUPPORTED-SELECTION).
 *
 * A dense switch in a function defined once. clang-cl /O2 writes the function in a `.text` COMDAT of selection
 * NODUPLICATES and its jump table in an `.rdata` COMDAT ASSOCIATIVE to it: ten REL32 relocations against the
 * function's section symbol. The table goes wherever the function goes, and the function is never one of several
 * copies -- so the table is an ordinary section of its unit. Until P69 fold 3d DSS refused the object at read
 * (F_CorruptedBinary, "COMDAT Selection 5"); link.exe 14.44 links it beside a C `main` and the program exits 42.
 *
 * REBUILD (Windows, clang-cl 19.1.5 of the VS Build Tools, from tests/link/data):
 *   clang-cl /c /O2 /GS- coff_comdat_switch.source.c /Focoff_comdat_switch_clangcl_O2.obj
 *
 * The same function under cl 19.51 (P69 fold 3d, D-LK-PE-IMAGE-DEFINES-NO-IMAGE-BASE-SYMBOL), at /O2 and /Od: the
 * jump table in `.text$mn` as IMAGE_REL_AMD64_ADDR32NB entries (the cases' RVAs), loaded through an `lea` of
 * `__ImageBase` (IMAGE_REL_AMD64_REL32) -- the symbol every PE linker defines at the image's DOS header. Built
 * through probe-reference-cc's keep mode (object mode, each twice, the two reports' md5s equal), the probe's own
 * directory mapped away so the object records no host path:
 *   cl  /O2 /GS- /MD /Brepro /experimental:deterministic /pathmap:{probe}=X:\dss-fixture  -> coff_comdat_switch_cl_O2.obj
 *   cl  /Od /GS- (the rest alike)                                                         -> coff_comdat_switch_cl_Od.obj
 * (cl 19.51, windows-x86_64-release.)
 *
 * The same function under MinGW-w64 gcc 13.2.0 (x86_64-ucrt-posix-seh), at -O0 and -O1: no COMDAT, and the jump
 * table in a plain `.rdata` as ten REL32 entries, each its case's distance from the table -- a PC-RELATIVE field in a
 * DATA section. Until P69 fold 3d the PE image writer refused it ("is pc-relative") and the relocatable writer did
 * too; gcc's own program, link.exe 14.44's and lld-link 19.1.5's each exit 42 beside the C `main`.
 *   gcc -O0 -c coff_comdat_switch.source.c -o coff_comdat_switch_gcc_O0.obj
 *   gcc -O1 -c coff_comdat_switch.source.c -o coff_comdat_switch_gcc_O1.obj
 */
int pick(int k, int x) {
    switch (k) {
        case 0: return x + 3;
        case 1: return x * 5;
        case 2: return x - 7;
        case 3: return x ^ 11;
        case 4: return x << 2;
        case 5: return x / 3;
        case 6: return x % 13;
        case 7: return ~x;
        case 8: return x | 64;
        case 9: return x & 6;
        default: return -1;
    }
}
