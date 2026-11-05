/* AN ATTRIBUTE WRITTEN WHERE A TYPE IS WRITTEN.
 *
 * Until cycle P69 the grammar took an attribute in a declaration's prefix, in
 * its attribute slots and after a declarator. Everywhere else one can be
 * written — among a type's specifiers, after a tag that is only referred to, in
 * a type name, in a pointer layer, at the start of a parenthesized declarator —
 * it was a parse error, or worse. `struct S __attribute__((aligned(16))) w;`
 * PARSED: the specifier's own word was read as the tag, a `struct aligned` was
 * looked up, and when the program had one, `w` silently became an object of
 * THAT structure, with its size. Arm 4 below is that program.
 *
 * Each arm scores 1 when it holds and 0 otherwise, so no two can cancel; the
 * program exits 29 + (the number of arms that hold), 42 when all thirteen do.
 *
 * WHAT THE REFERENCES SAY (each cell compiled and run through `dssharness run
 * probe-reference-cc`, 2026-10-08: gcc 13.3.0 and clang 18.1.3 on Linux x86_64,
 * Apple clang on macOS arm64, mingw-w64 gcc 13.2.0):
 *
 *   IN A DECLARATION — arms 1 to 4, and all four agree — a specifier among the
 *   type's own specifiers, or after a tag that is only referred to, is the
 *   DECLARATION's: it aligns the declared object (or member), never the type.
 *
 *   IN A TYPE NAME — arms 5 to 9 — gcc applies `aligned` to the type named and
 *   clang ignores it with a warning ("'aligned' attribute ignored when parsing
 *   type"). The attribute is GNU's and gcc's manual makes it a type attribute a
 *   specifier list takes, so gcc's reading is the one taken here; a program
 *   built with clang reads 4 where this one reads 16.
 *
 *   INSIDE A DECLARATOR — arms 10 to 13 — what a specifier decorates is decided
 *   by what follows it. Beside the declared name it is the declared entity's:
 *   `int * __attribute__((aligned(16))) p;` is a 16-aligned pointer on gcc and
 *   clang alike, and each of two members declared so sits where the request
 *   puts it. `packed` there is the one split: gcc ignores it for a pointer type
 *   and says so (the member stays at 8), clang packs it (the member at 1); the
 *   reading taken is gcc's, and the compiler says so too — the `expectWarnings`
 *   entry of the manifest.
 *
 * WHAT MAKES THIS RED. Let the specifier after a referred tag be read as the
 * tag again and arm 4 fails (`sizeof` 64). Drop the specifier-run fold and arms
 * 1 to 3 fail; the type-name fold, arms 5 to 9; the name-adjacent fold, arms 10
 * to 13 — and with it the warning of arm 12 disappears.
 */
#include <stddef.h>
#include <stdint.h>

#define ALIGNED16 __attribute__((aligned(16)))

/* ── in a declaration ───────────────────────────────────────────────────── */

unsigned ALIGNED16 int among_specifiers;
unsigned ALIGNED16 int *pointer_among_specifiers;
struct member_among_specifiers { char c; unsigned ALIGNED16 int m; };

struct aligned { char pad[64]; };       /* the tag the old parse looked up */
struct small { int a; };
struct small ALIGNED16 after_referred_tag;

/* ── inside a declarator ────────────────────────────────────────────────── */

int * ALIGNED16 beside_the_name;
struct two_members { char c; int * ALIGNED16 m, n; };
struct packed_after_star { char c; int * __attribute__((packed)) m; };
int (ALIGNED16 in_parentheses)[2];

static int is_aligned_to_16(void const *p) { return ((uintptr_t)p % 16) == 0; }

int identity(int v) { return v; }

int main(void) {
    int score = 0;

    /*  1 */ score += (sizeof among_specifiers == 4 && is_aligned_to_16(&among_specifiers));
    /*  2 */ score += (sizeof pointer_among_specifiers == sizeof(void *)
                       && is_aligned_to_16(&pointer_among_specifiers)
                       && _Alignof(__typeof__(*pointer_among_specifiers)) == 4);
    /*  3 */ score += (offsetof(struct member_among_specifiers, m) == 16);
    /*  4 */ score += (sizeof after_referred_tag == sizeof(struct small)
                       && sizeof(struct small) == 4 && _Alignof(struct small) == 4
                       && is_aligned_to_16(&after_referred_tag));

    /*  5 */ score += (_Alignof(ALIGNED16 int) == 16 && sizeof(ALIGNED16 int) == 4);
    /*  6 */ score += (_Alignof(ALIGNED16 int *) == 16 && sizeof(ALIGNED16 int *) == sizeof(void *));
    /*  7 */ score += (_Alignof(int ALIGNED16 *) == 16);
    /*  8 */ score += (_Alignof(ALIGNED16 int [3]) == 16 && sizeof(ALIGNED16 int [3]) == 12);
    /*  9 */ score += (_Alignof(__attribute__((aligned(1))) int) == 1
                       && sizeof(__attribute__((aligned(1))) int) == 4);

    /* 10 */ score += (sizeof beside_the_name == sizeof(void *) && is_aligned_to_16(&beside_the_name));
    /* 11 */ score += (offsetof(struct two_members, m) == 16 && offsetof(struct two_members, n) == 24);
    /* 12 */ score += (offsetof(struct packed_after_star, m) == sizeof(void *));
    /* 13 */ score += (sizeof in_parentheses == 8 && is_aligned_to_16(&in_parentheses));

    return identity(score) + 29;
}
