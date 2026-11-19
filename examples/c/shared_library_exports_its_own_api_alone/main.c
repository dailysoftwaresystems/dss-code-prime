/* The PROGRAM half: read the shared library the runner built beside this program (the arm's `dependsOn`) from its
 * FILE — the program never loads it — and print what it exports. A shared library's export table is its API: here
 * `lib_entry` alone, whatever shipped-runtime bodies (memalignment, strfromd, snprintf) it links to implement it.
 *
 * The format is read off the file's own magic bytes, so one source serves every arm:
 *   ELF64   exports = the .dynsym entries that are DEFINED, GLOBAL or WEAK, and DEFAULT or PROTECTED (`nm -D
 *           --defined-only`); the runtime is INSIDE iff .symtab defines memalignment and .dynsym names it not at all.
 *   Mach-O  exports = the symbol-table entries that are N_SECT and N_EXT and not N_PEXT (`nm -gU`); the runtime is
 *           INSIDE iff the table defines _memalignment as a non-exported N_SECT symbol.
 *   PE32+   exports = the export directory's names (`objdump -p`); the runtime is INSIDE iff no import descriptor
 *           names memalignment (the image linked, so the body that answers the call is its own).
 * The INSIDE half is what keeps the count from passing vacuously: a library that had NOT linked the runtime would
 * export `lib_entry` alone too.
 *
 * Prints "exports=<n> <names...> runtime=<inside|absent>" and exits 42 iff the one export is lib_entry and the
 * runtime is inside. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *image;
static size_t imageSize;

static int fits(size_t off, size_t len) { return off <= imageSize && len <= imageSize - off; }
static unsigned rd8(size_t o) { return fits(o, 1) ? image[o] : 0u; }
static unsigned rd16(size_t o) { return rd8(o) | rd8(o + 1) << 8; }
static unsigned long rd32(size_t o) { return (unsigned long)rd16(o) | (unsigned long)rd16(o + 2) << 16; }
static unsigned long long rd64(size_t o) { return (unsigned long long)rd32(o) | (unsigned long long)rd32(o + 4) << 32; }

/* A NUL-terminated name at `o`, or "" when it would run off the image. */
static char const *name(size_t o) {
    if (!fits(o, 1) || memchr(image + o, 0, imageSize - o) == NULL) return "";
    return (char const *)image + o;
}

#define MAX_EXPORTS 64
static char const *exported[MAX_EXPORTS];
static int exportCount;
static int runtimeInside;

static void addExport(char const *n) {
    if (exportCount < MAX_EXPORTS) exported[exportCount] = n;
    ++exportCount;
}

static void readElf(void) {
    size_t const shoff = (size_t)rd64(0x28);
    unsigned const shentsize = rd16(0x3A), shnum = rd16(0x3C);
    int dynsymNamesIt = 0, symtabDefinesIt = 0;
    for (unsigned i = 0; i < shnum; ++i) {
        size_t const sh = shoff + (size_t)i * shentsize;
        unsigned long const type = rd32(sh + 4);
        if (type != 11 && type != 2) continue; /* SHT_DYNSYM, SHT_SYMTAB */
        size_t const symOff = (size_t)rd64(sh + 0x18), symSize = (size_t)rd64(sh + 0x20);
        size_t const strSh = shoff + (size_t)rd32(sh + 0x28) * shentsize;
        size_t const strOff = (size_t)rd64(strSh + 0x18);
        for (size_t k = 0; k < symSize / 24; ++k) {
            size_t const s = symOff + 24 * k;
            char const *const n = name(strOff + rd32(s));
            unsigned const bind = rd8(s + 4) >> 4, vis = rd8(s + 5) & 3u, shndx = rd16(s + 6);
            if (type == 11) {
                if (strcmp(n, "memalignment") == 0) dynsymNamesIt = 1;
                if (shndx != 0 && (bind == 1 || bind == 2) && (vis == 0 || vis == 3)) addExport(n);
            } else if (shndx != 0 && strcmp(n, "memalignment") == 0) {
                symtabDefinesIt = 1;
            }
        }
    }
    runtimeInside = symtabDefinesIt && !dynsymNamesIt;
}

static void readMachO(void) {
    unsigned long const ncmds = rd32(16);
    size_t p = 32;
    for (unsigned long c = 0; c < ncmds; ++c) {
        unsigned long const cmd = rd32(p), size = rd32(p + 4);
        if (cmd == 0x2) { /* LC_SYMTAB */
            size_t const symOff = rd32(p + 8), stroff = rd32(p + 16);
            unsigned long const nsyms = rd32(p + 12);
            for (unsigned long i = 0; i < nsyms; ++i) {
                size_t const s = symOff + 16 * (size_t)i;
                unsigned const ntype = rd8(s + 4);
                char const *const n = name(stroff + rd32(s));
                if ((ntype & 0xE0u) != 0 || (ntype & 0x0Eu) != 0x0Eu) continue; /* a stab, or not N_SECT */
                int const exportedHere = (ntype & 0x01u) != 0 && (ntype & 0x10u) == 0;
                if (exportedHere) addExport(n[0] == '_' ? n + 1 : n); /* the C name, without Mach-O's '_' */
                else if (strcmp(n, "_memalignment") == 0) runtimeInside = 1;
            }
        }
        if (size == 0) break;
        p += size;
    }
}

/* PE32+: an RVA as a file offset, through the section table. */
static size_t peSections, peSectionCount;
static size_t rvaToOffset(unsigned long rva) {
    for (size_t i = 0; i < peSectionCount; ++i) {
        size_t const s = peSections + 40 * i;
        unsigned long const vsz = rd32(s + 8), va = rd32(s + 12), rsz = rd32(s + 16), raw = rd32(s + 20);
        unsigned long const span = vsz > rsz ? vsz : rsz;
        if (rva >= va && rva < va + span) return raw + (rva - va);
    }
    return (size_t)-1;
}

static void readPe(void) {
    size_t const pe = rd32(0x3C);
    peSectionCount = rd16(pe + 6);
    size_t const opt = pe + 24;
    peSections = opt + rd16(pe + 20);
    unsigned long const exportRva = rd32(opt + 112), importRva = rd32(opt + 120);
    if (exportRva != 0) {
        size_t const dir = rvaToOffset(exportRva);
        unsigned long const count = rd32(dir + 24);
        size_t const names = rvaToOffset(rd32(dir + 32));
        for (unsigned long i = 0; i < count; ++i) addExport(name(rvaToOffset(rd32(names + 4 * i))));
    }
    int importsIt = 0;
    if (importRva != 0) {
        for (size_t d = rvaToOffset(importRva); fits(d, 20) && rd32(d + 12) != 0; d += 20) {
            unsigned long thunkRva = rd32(d) != 0 ? rd32(d) : rd32(d + 16);
            for (size_t t = rvaToOffset(thunkRva); fits(t, 8) && rd64(t) != 0; t += 8) {
                unsigned long long const entry = rd64(t);
                if ((entry >> 63) != 0) continue; /* by ordinal */
                if (strcmp(name(rvaToOffset((unsigned long)entry) + 2), "memalignment") == 0) importsIt = 1;
            }
        }
    }
    runtimeInside = !importsIt;
}

static int load(char const *dir, size_t dirLen, char const *file) {
    char path[4096];
    if (dirLen + strlen(file) + 1 > sizeof path) return 0;
    memcpy(path, dir, dirLen);
    strcpy(path + dirLen, file);
    FILE *const f = fopen(path, "rb");
    if (f == NULL) return 0;
    fseek(f, 0, SEEK_END);
    long const size = ftell(f);
    fseek(f, 0, SEEK_SET);
    image = size > 0 ? malloc((size_t)size) : NULL;
    imageSize = image != NULL ? fread(image, 1, (size_t)size, f) : 0;
    fclose(f);
    return imageSize == (size_t)size && size > 0;
}

int main(int argc, char **argv) {
    char const *const self = argc > 0 ? argv[0] : "";
    size_t dirLen = 0;
    for (size_t i = 0; self[i] != '\0'; ++i)
        if (self[i] == '/' || self[i] == '\\') dirLen = i + 1;
    if (!load(self, dirLen, "dsslib.so") && !load(self, dirLen, "dsslib.dylib")
        && !load(self, dirLen, "dsslib.dll")) {
        puts("no library beside the program");
        return 1;
    }
    if (rd32(0) == 0x464C457FUL) readElf();
    else if (rd32(0) == 0xFEEDFACFUL) readMachO();
    else if (rd16(0) == 0x5A4Du) readPe();
    else {
        puts("unknown format");
        return 1;
    }
    printf("exports=%d", exportCount);
    for (int i = 0; i < exportCount && i < MAX_EXPORTS; ++i) printf(" %s", exported[i]);
    printf(" runtime=%s\n", runtimeInside ? "inside" : "absent");
    return exportCount == 1 && strcmp(exported[0], "lib_entry") == 0 && runtimeInside ? 42 : 1;
}
