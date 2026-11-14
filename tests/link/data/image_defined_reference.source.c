/* The reference objects of tests/link/test_image_defined_reference_shapes.cpp: `check()` reads the header of the
 * image it is linked into through the name every reference linker of the format defines there -- `__ehdr_start` on
 * ELF (`ELF_HEADER_NAME`), `__ImageBase` on PE (`PE_HEADER_NAME`) -- and returns 42 when the first bytes are the
 * format's magic. The reference compiler chooses HOW the object reaches the name; that shape is the fixture's point.
 *
 * HOW EACH WAS BUILT (P69 fold 3e, lane `xa`, 2026-11-13). Each object compiled TWICE from this file (as
 * `image_defined_reference.c`) in two directories, the two byte-identical; its relocations against the name read by
 * readelf -rW / llvm-objdump -r -t; and the reference's own program of it -- the object beside
 * `int check(void); int main(void) { return check(); }`, linked by the same compiler driver -- RUN (aarch64 under
 * qemu-aarch64 with the cross sysroot), each exiting 42:
 *   image_defined_reference_ehdr_x86_64_elf_gcc_pie_O2.o    gcc-13 (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0,
 *       `-O2 -fPIE -DELF_HEADER_NAME -c`; 1480 bytes, md5 87e58d4f36ec9cbf9c6357d1f7508d25; R_X86_64_PC32 (DIRECT);
 *       its program linked `-pie`.
 *   image_defined_reference_ehdr_x86_64_elf_clang_pie_O2.o  Ubuntu clang version 19.1.1 (1ubuntu1~24.04.2),
 *       `-O2 -fPIE -DELF_HEADER_NAME -c`; 1144 bytes, md5 9946db6b010f512b61b7eb7dd5e30c17; R_X86_64_REX_GOTPCRELX
 *       (a GOT SLOT); its program linked `-pie`.
 *   image_defined_reference_ehdr_arm64_elf_gcc_pie_O2.o     aarch64-linux-gnu-gcc-13 (Ubuntu 13.3.0-6ubuntu2~24.04.1)
 *       13.3.0, `-O2 -fPIE -DELF_HEADER_NAME -c`; 1504 bytes, md5 0560c17eade8d6780462aa3a21a8d7d2;
 *       R_AARCH64_ADR_GOT_PAGE + R_AARCH64_LD64_GOT_LO12_NC (a GOT SLOT); its program linked `-pie`.
 *   image_defined_reference_imagebase_x86_64_pe_gcc_O2.o    gcc.exe (MinGW-W64 x86_64-ucrt-posix-seh, built by Brecht
 *       Sanders, r8) 13.2.0, `-O2 -DPE_HEADER_NAME -c`; 1008 bytes, md5 ca72addbb6656f64552dce800b255c65;
 *       IMAGE_REL_AMD64_REL32 to `.refptr.__ImageBase`, a COMDAT datum the object defines in
 *       `.rdata$.refptr.__ImageBase` whose IMAGE_REL_AMD64_ADDR64 names `__ImageBase` (a SLOT THE OBJECT DEFINES).
 * The ELF objects were built in WSL (Ubuntu 24.04), the PE object on Windows x86_64; no object records a host path. */
#if defined(ELF_HEADER_NAME)
extern const unsigned char __ehdr_start[];
int check(void) {
    const unsigned char *h = __ehdr_start;
    return h[0] == 0x7f && h[1] == 0x45 && h[2] == 0x4c && h[3] == 0x46 ? 42 : 1;
}
#elif defined(PE_HEADER_NAME)
extern const unsigned char __ImageBase[];
int check(void) {
    const unsigned char *h = __ImageBase;
    return h[0] == 0x4d && h[1] == 0x5a ? 42 : 1;
}
#else
#error "define ELF_HEADER_NAME or PE_HEADER_NAME"
#endif
