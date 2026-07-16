// c155 D-LK10-CRT-INIT-INVOKE closure witness #2 (2026-07-17):
// the fopen/fwrite/fread/fclose stream roundtrip — the anchor's
// second cited "genuinely CRT-init-requiring" case ("fopen with
// locale-aware paths" / stdio stream + buffering state).
//
// The c155 diagnosis DISPROVED the premise: the full buffered-stream
// lifecycle works on every runnable leg TODAY with the trampoline
// calling main directly — each format's loader runs libc's own
// initialization before the DSS entry (pe: msvcrt DllMain at
// DLL_PROCESS_ATTACH; elf: ld.so runs libc.so.6's DT_INIT/init_array
// with argc/argv/envp; macho: dyld runs libSystem's initializers
// before LC_MAIN). See printf_float/main.c for the per-format
// contract; sibling stdio_file_typedef pins the vfprintf/fread shape
// — THIS example pins the anchor-cited fwrite path.
//
// Shape: fwrite two bytes into a fresh binary stream, fclose (the
// flush), re-fopen, fread them back, remove the file, verify every
// byte and every libc return value. Each check contributes a
// DISTINCT non-42 exit code so a regression names its failing layer
// (fopen=1, fwrite=2, close-flush=3, reopen=4, fread-close=5,
// remove=6, short-read=7, byte0=8, byte1=9).

#include <stdio.h>

int main(void) {
    FILE* f;
    char buf[8];
    size_t n;
    f = fopen("c155_fwrite_tmp.bin", "wb");
    if (f == NULL) return 1;
    if (fwrite("hi", 1, 2, f) != 2) return 2;
    if (fclose(f) != 0) return 3;
    f = fopen("c155_fwrite_tmp.bin", "rb");
    if (f == NULL) return 4;
    n = fread(buf, 1, sizeof(buf), f);
    if (fclose(f) != 0) return 5;
    if (remove("c155_fwrite_tmp.bin") != 0) return 6;
    if (n != 2) return 7;
    if (buf[0] != 'h') return 8;
    if (buf[1] != 'i') return 9;
    return 42;
}
