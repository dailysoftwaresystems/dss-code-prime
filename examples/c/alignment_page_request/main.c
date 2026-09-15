// P63 (D-CSUBSET-ALIGNMENT-CEILING-REFUSES-WHAT-TWO-REFERENCES-RUN): a
// PAGE-ALIGNED request, in both spellings, on a TYPE and on a statically
// allocated OBJECT — the shape a program asking for a page actually writes.
//
// Before this cycle every line below was `error[S_AlignasExceedsMax]`: the
// requestable ceiling was a hardcoded 256 in the semantic ladder, hand-copied
// into `Alignment::fromBytes` and again into the interner's
// `representableCompositeAlign`. ✔MEASURED 2026-09-07, each reference probed
// SEPARATELY, BUILD **and** RUN with the exit asserted and an N=16 CONTROL arm
// green: gcc 13.3.0 and clang 18.1.3 both RUN `struct __attribute__((aligned(N)))`
// and its `_Alignas(N)` twin at N = 512, 1024, 4096, 8192, 65536 and 2^28.
// 4096 is a page — the first alignment a program asking for one would write.
//
// ★ WHY THIS EXAMPLE RUNS RATHER THAN MERELY COMPILING. The old refusal was LOUD,
// so nothing was ever miscompiled and no gate could see the gap; and the failure
// mode of the FIX is the opposite one — an alignment that is accepted and then
// not applied is silent. So the exit code is derived from the actual ADDRESS the
// linker chose and from the actual field offsets, never from a constant.
//
// ⚠ THE ALIGNMENT MUST REACH ITS SINK, which is why every decorated member sits
// AFTER a `char`: both the member's offset and the aggregate's size move when the
// request lands, and both are asserted. A parse-and-drop leaves them at 1 and 2.
//
// ⓘ THE SECOND, NARROWER CEILING IS DELIBERATELY NOT EXERCISED HERE. A statically
// allocated object also cannot exceed what the object format can PLACE it at —
// on PE that is the image's declared `SectionAlignment`, and a request above it is
// refused by name (`K_StaticObjectOveralignedForFormat`). 4096 is exactly that
// value on the shipped `pe64-*` documents, so this example sits AT the boundary on
// every shipped format and above none of them.

#define PAGE 4096

// (a) the whole-composite GNU spelling
struct __attribute__((aligned(PAGE))) gnu_page { char c; };

// (b) the C11/C23 member spelling — the SAME shared ladder, and the row's own
//     scope note says the two surfaces must move together or one construct means
//     two things by spelling.
struct std_page { _Alignas(PAGE) char c; };

// (c) the request applied to a MEMBER that follows a `char`, so the offset AND
//     the size both move. This is the sink assertion.
struct member_page { char pad; _Alignas(PAGE) char v; };

static struct gnu_page    g_gnu;
static struct std_page    g_std;
static struct member_page g_member;

// A runtime value the optimizer cannot fold the address checks away against.
static unsigned long long addr_of(const void *p) {
    return (unsigned long long)p;
}

int main(int argc, char **argv) {
    (void)argv;
    int score = 0;

    /* The TYPE facts: alignment and size both raised to the page. */
    if (_Alignof(struct gnu_page) != (unsigned)PAGE) return 1;
    if (sizeof(struct gnu_page)   != (unsigned)PAGE) return 2;
    if (_Alignof(struct std_page) != (unsigned)PAGE) return 3;
    if (sizeof(struct std_page)   != (unsigned)PAGE) return 4;

    /* The SINK: a decorated member after a `char` moves both numbers. An
       accepted-then-dropped request leaves v at 1 and sizeof at 2. */
    if (__builtin_offsetof(struct member_page, pad) != 0u)             return 5;
    if (__builtin_offsetof(struct member_page, v)   != (unsigned)PAGE) return 6;
    if (sizeof(struct member_page) != 2u * (unsigned)PAGE)             return 7;
    if (_Alignof(struct member_page) != (unsigned)PAGE)                return 8;

    /* The OBJECTS really land page-aligned. These are addresses the linker chose;
       nothing here is foldable from the type metadata the checks above read. */
    if (addr_of(&g_gnu)    % (unsigned long long)PAGE) return 9;
    if (addr_of(&g_std)    % (unsigned long long)PAGE) return 10;
    if (addr_of(&g_member) % (unsigned long long)PAGE) return 11;
    if (addr_of(&g_member.v) % (unsigned long long)PAGE) return 12;

    /* And they are writable at that alignment — the storage is real. */
    g_gnu.c    = (char)argc;      /* argc is a runtime value */
    g_std.c    = (char)(argc + 1);
    g_member.v = (char)(argc + 2);
    score = (int)g_gnu.c + (int)g_std.c + (int)g_member.v;  /* 1 + 2 + 3 == 6 */
    if (score != 6) return 13;

    /* 6 * 8 - 6 == 42, with 6 derived from argc at run time. */
    return score * 8 - 6;
}
