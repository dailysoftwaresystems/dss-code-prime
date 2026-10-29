/* A linked object's `/STACK:` sets the program's stack (see expected.json). The REFERENCE-built
   member states `/STACK:0x10000000,0x20000` -- 256 MiB reserved, 128 KiB committed. This program
   reads both header fields back from its own image and then recurses about 12 MiB deep, which the
   default 1 MiB reserve cannot hold: an image that dropped the directive faults with a stack
   overflow. Each frame stays under a page, so no frame needs a stack probe, and each one reads its
   own frame after the call returns, so no optimizer can turn the recursion into a loop. */
#include <windows.h>

extern int dss_stack_member_frames(void);

static int descend(int n) {
    volatile char frame[3072];
    frame[0] = 1;
    frame[sizeof frame - 1] = 1;
    if (n == 0) return 0;
    int const below = descend(n - 1);
    return below + frame[0] * frame[sizeof frame - 1];
}

static unsigned long long field(unsigned char const *p, int width) {
    unsigned long long v = 0;
    for (int i = width - 1; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

int main(void) {
    unsigned char const *image = (unsigned char const *)LoadLibraryW(L"main.exe");
    if (image == 0) return 1;
    unsigned char const *opt = image + field(image + 0x3C, 4) + 24;   /* the PE32+ optional header */
    if (field(opt + 72, 8) != 0x10000000ull) return 2;                 /* SizeOfStackReserve */
    if (field(opt + 80, 8) != 0x20000ull) return 3;                    /* SizeOfStackCommit */
    int const frames = dss_stack_member_frames();
    if (descend(frames) != frames) return 4;
    return 42;
}
