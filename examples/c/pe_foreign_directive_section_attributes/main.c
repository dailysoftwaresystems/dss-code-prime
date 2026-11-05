/* A linked object's `/SECTION:` sets a section's attributes (see expected.json). The
   REFERENCE-built member states `/SECTION:.rdata,RW` and writes its own const table, which lives in
   `.rdata`: the write faults in an image whose `.rdata` is read-only. This program first reads the
   section's Characteristics back from its own image. */
#include <windows.h>

extern int dss_rdata_poke(void);

static unsigned long long field(unsigned char const *p, int width) {
    unsigned long long v = 0;
    for (int i = width - 1; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

static int isRdata(unsigned char const *name) {
    static char const want[8] = {'.', 'r', 'd', 'a', 't', 'a', 0, 0};
    for (int i = 0; i < 8; ++i) {
        if (name[i] != (unsigned char)want[i]) return 0;
    }
    return 1;
}

int main(void) {
    unsigned char const *image = (unsigned char const *)LoadLibraryW(L"main.exe");
    if (image == 0) return 1;
    unsigned char const *pe = image + field(image + 0x3C, 4);
    unsigned const sections = (unsigned)field(pe + 6, 2);
    unsigned char const *header = pe + 24 + field(pe + 20, 2);
    int found = 0;
    for (unsigned i = 0; i < sections; ++i, header += 40) {
        if (!isRdata(header)) continue;
        found = 1;
        unsigned long long const chars = field(header + 36, 4);
        if ((chars & 0x80000000ull) == 0) return 2;   /* IMAGE_SCN_MEM_WRITE */
        if ((chars & 0x40000000ull) == 0) return 3;   /* IMAGE_SCN_MEM_READ */
    }
    if (!found) return 4;
    return dss_rdata_poke() + 2;
}
