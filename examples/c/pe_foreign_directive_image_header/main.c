/* A linked object's image requests land in the header (see expected.json): the REFERENCE-built
   member states `/HEAP:0x200000,0x10000 /VERSION:3.5 /SUBSYSTEM:WINDOWS,6.02
   /ENTRY:mainCRTStartup /BASE:0x200000000 /ALIGN:0x2000`. This program reads each field back from
   its own image; the image is a GUI one, and runs with every section on an 8 KiB boundary. */
#include <windows.h>

extern int dss_header_member(void);

static unsigned long long field(unsigned char const *p, int width) {
    unsigned long long v = 0;
    for (int i = width - 1; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

int main(void) {
    unsigned char const *image = (unsigned char const *)LoadLibraryW(L"main.exe");
    if (image == 0) return 1;
    unsigned char const *pe = image + field(image + 0x3C, 4);
    unsigned char const *opt = pe + 24;                                         /* PE32+ optional header */
    if (field(opt + 88, 8) != 0x200000ull || field(opt + 96, 8) != 0x10000ull) return 2;   /* heap */
    if (field(opt + 44, 2) != 3 || field(opt + 46, 2) != 5) return 3;          /* image version 3.5 */
    if (field(opt + 68, 2) != 2) return 4;                                      /* IMAGE_SUBSYSTEM_WINDOWS_GUI */
    if (field(opt + 48, 2) != 6 || field(opt + 50, 2) != 2) return 5;          /* subsystem version 6.02 */
    if (field(opt + 40, 2) != 6 || field(opt + 42, 2) != 2) return 6;          /* and the OS version with it */
    if (field(opt + 32, 4) != 0x2000) return 7;                                 /* SectionAlignment */
    unsigned const sections = (unsigned)field(pe + 6, 2);
    unsigned char const *header = opt + field(pe + 20, 2);
    for (unsigned i = 0; i < sections; ++i, header += 40) {
        if (field(header + 12, 4) % 0x2000 != 0) return 8;                      /* VirtualAddress */
    }
    return 42 + dss_header_member();
}
