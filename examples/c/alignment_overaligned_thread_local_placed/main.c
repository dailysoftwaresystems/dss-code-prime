/* P64 (D-CSUBSET-THREAD-LOCAL-PE-OVERALIGN): the POSITIVE runtime witness that
 * an over-aligned `thread_local` is PLACED on pe64 — in the MAIN thread's block
 * and in a freshly created thread's block alike.
 *
 * ⚠⚠ WHAT THIS REPLACES, AND WHY THE OLD REFUSAL WAS WRONG. The pe64 writer used
 * to refuse every thread-local stricter than 16 bytes, on the premise that the
 * Windows loader guarantees only MEMORY_ALLOCATION_ALIGNMENT and that
 * IMAGE_TLS_DIRECTORY64 "has no field to request more". ✔BOTH HALVES REFUTED
 * (P64): the field is declared in the Windows SDK's own `winnt.h` — a four-bit
 * IMAGE_SCN_ALIGN_* nibble at bits 20..23 of `Characteristics` — and the loader
 * honours it. THE DISCRIMINATOR was not a correlation: one MSVC-linked image
 * carrying seven 4096-aligned `__declspec(thread)` objects ran 42 with every
 * address `% 4096 == 0` on both threads, and the SAME FILE with only that nibble
 * rewritten 4096 → 16 ran 50 (`t1 mod 4096 = 720`). Same template, same block
 * size, same code bytes, one field.
 *
 * ★ THE SECOND THREAD IS THE MEASUREMENT, not decoration. The main thread's TLS
 * block is set up very early in process init and can be aligned by accident; a
 * `CreateThread` worker gets a block the loader allocates fresh, so an image
 * whose directory does NOT ask for the alignment fails there. Both threads are
 * checked, and each object is written and read back through its far end so an
 * overlap would be caught too.
 *
 * ★ EIGHT OBJECTS, ODD FILLERS, BOTH TIERS. The `tdata` (initialized) and `tbss`
 * (zero-init) halves land at different offsets inside one per-thread block, and
 * an object at block offset 0 is right by luck — so nothing here sits alone.
 *
 * ✔THE REFERENCE, each probed SEPARATELY, BUILD **and** RUN, addresses asserted
 * on two threads: MSVC 19.51 (which uses NATIVE Windows TLS, so it speaks
 * directly about the loader) returns 42 at 8, 16 (control), 32, 64 and 4096, and
 * at 8192 with `/link /ALIGN:8192`. mingw-w64 gcc 13.2.0 returns 42 across the
 * same range via emutls, and refuses 16384 by name. 8192 is the largest value
 * the four-bit field encodes, and it is where both stop.
 *
 * pe64-ONLY, deliberately: `CreateThread` is the native Windows primitive (as in
 * `examples/c/thread_local_win_threads`, this example's single-alignment
 * sibling), and the Mach-O leg still refuses over-aligned thread-locals under
 * its own anchor, in a file this lane may not touch.
 *
 * Exit 42. RED-on-disable: restore `Characteristics = 0` in the pe.cpp TLS
 * directory writer and this still BUILDS CLEAN, then returns 50 at run time.
 */
#include <windows.h>

#define BIG   4096
#define PAGE  4096
#define MID     64

/* tbss (zero-init) arm, interleaved with odd-sized fillers */
__attribute__((aligned(BIG))) thread_local char t1[3];
thread_local char tp1[7];
__attribute__((aligned(BIG))) thread_local char t2[5];
thread_local char tp2[13];
__attribute__((aligned(MID))) thread_local char t3[11];
thread_local char tp3[9];
__attribute__((aligned(BIG))) thread_local char t4[17];

/* tdata (initialized) arm, same shape */
__attribute__((aligned(BIG))) thread_local char u1[3]  = {1};
thread_local char up1[7]  = {1};
__attribute__((aligned(BIG))) thread_local char u2[5]  = {1};
thread_local char up2[13] = {1};
__attribute__((aligned(MID))) thread_local char u3[11] = {1};
thread_local char up3[9]  = {1};
__attribute__((aligned(BIG))) thread_local char u4[17] = {1};

/* checkAll returns 0 when this thread's block is laid out correctly, or a
 * non-zero code naming the first object that is not. It reads only addresses
 * the loader produced — no alignment constant reaches the verdict except as the
 * modulus being tested. */
static int checkAll(int tag) {
    char *objs[8];
    unsigned long long wants[8];
    unsigned long long sizes[8];
    int i;

    objs[0] = t1; wants[0] = BIG;  sizes[0] = 3;
    objs[1] = t2; wants[1] = BIG;  sizes[1] = 5;
    objs[2] = t3; wants[2] = MID;  sizes[2] = 11;
    objs[3] = t4; wants[3] = BIG;  sizes[3] = 17;
    objs[4] = u1; wants[4] = BIG;  sizes[4] = 3;
    objs[5] = u2; wants[5] = BIG;  sizes[5] = 5;
    objs[6] = u3; wants[6] = MID;  sizes[6] = 11;
    objs[7] = u4; wants[7] = BIG;  sizes[7] = 17;

    tp1[0] = 1; tp2[0] = 1; tp3[0] = 1;
    up1[0] = 1; up2[0] = 1; up3[0] = 1;

    for (i = 0; i < 8; ++i) {
        unsigned long long const a = (unsigned long long)(void *)objs[i];
        if (a % wants[i] != 0ull) return tag + i;
    }
    /* the tdata half must arrive INITIALIZED in every thread, and the tbss half
     * must arrive ZEROED — an over-aligned block that shifted the template copy
     * would still satisfy the modulus above */
    if (u1[0] != 1 || u2[0] != 1 || u3[0] != 1 || u4[0] != 1) return tag + 8;
    if (t1[0] != 0 || t2[0] != 0 || t3[0] != 0 || t4[0] != 0) return tag + 9;

    for (i = 0; i < 8; ++i) {
        objs[i][0] = (char)(i + 1);
        objs[i][sizes[i] - 1] = (char)(100 + i);
    }
    for (i = 0; i < 8; ++i) {
        if (objs[i][0] != (char)(i + 1)) return tag + 10;
        if (objs[i][sizes[i] - 1] != (char)(100 + i)) return tag + 11;
    }
    return 0;
}

static int g_worker = -1;

static unsigned long worker(void *arg) {
    (void)arg;
    g_worker = checkAll(70);   /* fresh loader-allocated block */
    return 0;
}

int main(void) {
    int const mainRc = checkAll(50);
    if (mainRc != 0) return mainRc;

    HANDLE h = CreateThread(0, 0, (void *)worker, 0, 0, 0);
    if (h == 0) return 90;
    if (WaitForSingleObject(h, INFINITE) != 0) return 91;
    CloseHandle(h);
    if (g_worker != 0) return g_worker;

    return 42;
}
