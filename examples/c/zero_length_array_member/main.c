/* TF-C96 D-CSUBSET-ZERO-LENGTH-ARRAY-MEMBER witness: the GNU zero-length trailing array member — `struct S { … ; unsigned long long tail[0]; };` — contributes ZERO bytes.
** The pre-C99 flexible-array idiom, still shipped verbatim by the macOS SDK.
** Darwin's `sys/mount.h:366` is the site that blocks the goal leg:
**     struct netfs_status {
**         u_int32_t ns_status;  char ns_mountopts[512];
**         uint32_t  ns_waittime;  uint32_t ns_threadcount;
**         uint64_t  ns_threadids[0];   // Thread IDs of those blocked threads
**     };
** ISO C forbids `[0]` (6.7.6.2p1 requires a length > 0), so DSS's rejection was
** honest fail-loud — `error[S000C] got [0]` — but native clang accepts it
** silently and every `#include <sys/mount.h>` in sqlite's os_unix.c goes through
** that member. Admitting it is a LANGUAGE decision (type lattice + layout), not
** a descriptor row.
**
** WHAT IS ACTUALLY OBSERVABLE is the SIZE and the LAYOUT, so those are what this
** measures — three with-`[0]` structs, each against a TWIN that is byte-for-byte
** the same declaration with the `[0]` member DELETED:
**   NsWide  {u64 n;  u64 tail[0];}   vs NsWideT  {u64 n;}          -> 8 == 8
**   NsSplit {u32 a; u32 b; u64 tail[0];} vs NsSplitT {u32 a; u32 b;} -> 8 == 8
**   NsByte  {u32 n;  char tail[0];}  vs NsByteT  {u32 n;}          -> 4 == 4
** NsSplit is the deliberate one: `u64 tail[0]` raises the struct's ALIGNMENT to
** 8 while contributing no bytes, and the two leading u32s already fill 8 — so a
** correct implementation still lands on 8, and an implementation that charges
** the member 8 bytes lands on 16. NsByte pins the no-alignment-bump case.
** All six sizes and all three tail offsets were MEASURED natively (Apple clang,
** arm64 Darwin); the elf/pe legs agree because the rule is not per-target.
**
** THE EXIT IS BUILT FROM THOSE MEASURED FACTS, never from a `? 42 : 0` verdict:
**   sizeof NsWide(8) + NsSplit(8) + NsByte(4)                      = 20
** + tail offset in each, from POINTER ARITHMETIC on a REAL OBJECT:
**   (char*)w.tail-(char*)&w = 8 ; s: 8 ; b: 4                      = 20
** + the value read back out of a sized member written at runtime    = 2
**   ------------------------------------------------------------------
**                                                                    42
** Any non-zero contribution moves BOTH halves: charge `tail` 8 bytes and the
** three sizeofs become 16/16/8 (exit 62 if the guards did not fire first), while
** a mislaid tail offset moves the second 20. The `sizeof` terms fold, but they
** fold THROUGH the layout engine — the fold IS the feature's output, and there is
** no path to 42 that does not go through a zero-sized member.
**
** RED-ON-DISABLE: revert the admission (the zero-length arm of the array-length
** rule) -> `unsigned long long tail[0];` is rejected `error[S000C]` array length
** out of range at the DECLARATION -> the example no longer COMPILES (a compile
** failure, not a wrong exit). Charge the member its element size instead of 0 and
** the twin-equality guards return 1/2/3 and the absolute-size guards 4/5/6.
**
** Front-end feature (type lattice + layout): source-, target- and format-agnostic,
** so all four shipped targets run it; the release arm re-witnesses the layout
** under the optimizer. gcc/clang -std=c17 cross-checked: same source exits 42.
*/

/* The `[0]` member is TRAILING in each — the only position the extension admits.
** Each pair is the same declaration with and without that member. */
struct NsWide   { unsigned long long n; unsigned long long tail[0]; };
struct NsWideT  { unsigned long long n; };

struct NsSplit  { unsigned int a; unsigned int b; unsigned long long tail[0]; };
struct NsSplitT { unsigned int a; unsigned int b; };

struct NsByte   { unsigned int n; char tail[0]; };
struct NsByteT  { unsigned int n; };

/* A mutable global = a runtime-opaque operand for the read-back term. */
int g_val = 2;

int main(void) {
    struct NsWide  w;
    struct NsSplit s;
    struct NsByte  b;
    int offWide;
    int offSplit;
    int offByte;
    int total;

    /* (1) the TWIN pins — the `[0]` member must cost exactly what deleting it costs */
    if (sizeof(struct NsWide)  != sizeof(struct NsWideT))  return 1;
    if (sizeof(struct NsSplit) != sizeof(struct NsSplitT)) return 2;
    if (sizeof(struct NsByte)  != sizeof(struct NsByteT))  return 3;

    /* (2) the ABSOLUTE sizes, so a twin that is wrong the SAME way cannot pass */
    if (sizeof(struct NsWide)  != 8) return 4;
    if (sizeof(struct NsSplit) != 8) return 5;
    if (sizeof(struct NsByte)  != 4) return 6;

    /* (3) the tail's OFFSET, measured on a real object: it must begin exactly
    ** where the sized part ends, i.e. at the struct's full size — which is the
    ** other face of "contributes 0 bytes". */
    offWide  = (int)((char *)w.tail - (char *)&w);
    offSplit = (int)((char *)s.tail - (char *)&s);
    offByte  = (int)((char *)b.tail - (char *)&b);
    if (offWide  != 8) return 7;
    if (offSplit != 8) return 8;
    if (offByte  != 4) return 9;

    /* (4) the sized members are genuinely written and read back, so the objects
    ** are real storage and not just sizeof operands. */
    w.n = 0;
    s.a = 0;
    s.b = 0;
    b.n = (unsigned int)g_val;
    if (w.n != 0 || s.a != 0 || s.b != 0) return 10;

    total = (int)sizeof(struct NsWide)
          + (int)sizeof(struct NsSplit)
          + (int)sizeof(struct NsByte);        /*  8 +  8 + 4 = 20 */
    total = total + offWide + offSplit + offByte;   /* +8 + 8 + 4 = 40 */
    total = total + (int)b.n;                       /* +g_val (2)  = 42 */
    return total;
}
