// D-LK-ELF-EMITS-NO-BUILD-ID-NOTE — the RUNTIME witness.
//
// A build id is not observable from inside a program the way an arithmetic
// result is, so this example makes it observable the only honest way: the
// binary OPENS ITSELF (`/proc/self/exe`), walks its own section header table,
// finds `.note.gnu.build-id`, and validates the note record byte for byte.
// Nothing here is a compile-time constant — every number is read back out of
// the file the linker wrote, so the example fails the moment the writer stops
// emitting the section, mis-sizes it, or mis-places it.
//
// Each check contributes a DISTINCT non-42 exit code so a regression names its
// failing layer rather than just "not 42":
//    1 fopen(/proc/self/exe)      2 ELF header short read
//    3 not an ELF64 LSB file      4 no section header table
//    5 shstrtab header read       6 shstrtab body allocation/read
//    7 section header read        8 no `.note.gnu.build-id` section
//    9 note body read            10 n_namesz != 4
//   11 n_type != NT_GNU_BUILD_ID 12 owner name is not "GNU\0"
//   13 n_descsz == 0             14 descriptor is all zero
//   15 sh_type != SHT_NOTE       16 sh_flags lacks SHF_ALLOC
//
// ⚠ ELF ONLY, and the `targets` list says so rather than the code: `/proc/self/exe`
// is a Linux fact. The equivalent Mach-O identity is LC_UUID, which has its own
// witness.

#include <stdio.h>
#include <string.h>

static unsigned long rd(unsigned char const* p, int n) {
    unsigned long v = 0;
    int i;
    for (i = n - 1; i >= 0; --i) {
        v = (v << 8) | (unsigned long)p[i];
    }
    return v;
}

int main(void) {
    FILE* f;
    unsigned char ehdr[64];
    unsigned char shdr[64];
    unsigned char note[64];
    unsigned char names[4096];
    unsigned long shoff, shentsize, shnum, shstrndx;
    unsigned long strOff, strSize;
    unsigned long noteOff = 0, noteSize = 0, noteType = 0, noteFlags = 0;
    unsigned long namesz, descsz, ntype;
    unsigned long i;
    int found = 0;
    int nonZero = 0;

    f = fopen("/proc/self/exe", "rb");
    if (f == NULL) return 1;
    if (fread(ehdr, 1, 64, f) != 64) { fclose(f); return 2; }
    if (ehdr[0] != 0x7F || ehdr[1] != 'E' || ehdr[2] != 'L' || ehdr[3] != 'F'
        || ehdr[4] != 2 || ehdr[5] != 1) { fclose(f); return 3; }

    shoff     = rd(ehdr + 40, 8);
    shentsize = rd(ehdr + 58, 2);
    shnum     = rd(ehdr + 60, 2);
    shstrndx  = rd(ehdr + 62, 2);
    if (shoff == 0 || shnum == 0 || shentsize != 64) { fclose(f); return 4; }

    /* The section-name string table, so the search is by NAME. */
    if (fseek(f, (long)(shoff + shstrndx * shentsize), SEEK_SET) != 0
        || fread(shdr, 1, 64, f) != 64) { fclose(f); return 5; }
    strOff  = rd(shdr + 24, 8);
    strSize = rd(shdr + 32, 8);
    if (strSize == 0 || strSize > sizeof(names)) { fclose(f); return 6; }
    if (fseek(f, (long)strOff, SEEK_SET) != 0
        || fread(names, 1, (size_t)strSize, f) != (size_t)strSize) {
        fclose(f);
        return 6;
    }

    for (i = 0; i < shnum; ++i) {
        unsigned long nameOff;
        if (fseek(f, (long)(shoff + i * shentsize), SEEK_SET) != 0
            || fread(shdr, 1, 64, f) != 64) { fclose(f); return 7; }
        nameOff = rd(shdr + 0, 4);
        if (nameOff >= strSize) continue;
        if (strcmp((char const*)(names + nameOff), ".note.gnu.build-id") != 0) {
            continue;
        }
        noteType  = rd(shdr + 4, 4);
        noteFlags = rd(shdr + 8, 8);
        noteOff   = rd(shdr + 24, 8);
        noteSize  = rd(shdr + 32, 8);
        found = 1;
        break;
    }
    if (!found) { fclose(f); return 8; }
    if (noteType != 7) { fclose(f); return 15; }          /* SHT_NOTE */
    if ((noteFlags & 2UL) == 0) { fclose(f); return 16; } /* SHF_ALLOC */
    if (noteSize < 16 || noteSize > sizeof(note)) { fclose(f); return 9; }
    if (fseek(f, (long)noteOff, SEEK_SET) != 0
        || fread(note, 1, (size_t)noteSize, f) != (size_t)noteSize) {
        fclose(f);
        return 9;
    }
    fclose(f);

    namesz = rd(note + 0, 4);
    descsz = rd(note + 4, 4);
    ntype  = rd(note + 8, 4);
    if (namesz != 4) return 10;
    if (ntype != 3) return 11;                            /* NT_GNU_BUILD_ID */
    if (note[12] != 'G' || note[13] != 'N' || note[14] != 'U'
        || note[15] != 0) {
        return 12;
    }
    if (descsz == 0 || 16 + descsz > noteSize) return 13;
    for (i = 0; i < descsz; ++i) {
        if (note[16 + i] != 0) nonZero = 1;
    }
    if (!nonZero) return 14;   /* a never-stamped descriptor is all zero */
    return 42;
}
