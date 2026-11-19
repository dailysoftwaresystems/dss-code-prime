/* `__declspec(...)` IS READ, NOT ERASED.
 *
 * Until cycle P69 `__declspec(x)` was a predefined macro of the Windows pairs
 * that expanded to nothing, so every request written in it vanished before the
 * parser: an alignment was dropped, a thread-storage request left ONE object
 * shared by every thread, and the compiler said nothing either time. It is a
 * second spelling of the attribute specifier now, and this program is the
 * runtime witness that what it asks for reaches the emitted image.
 *
 * Every arm below is a cell cl 19.51 was MEASURED to give (compiled and run
 * through `dssharness run probe-reference-cc`, 2026-10-08); where mingw-w64 gcc
 * 13 differs it is said. Each arm scores 1 when it holds and 0 otherwise, so no
 * two arms can cancel; the program exits 26 + (the number of arms that hold),
 * and all sixteen holding is 42.
 *
 * `align` — cl's own rule, which mingw-w64 gcc does not have (it ignores the
 * word with a warning):
 *   - written before a DEFINITION of a structure, the request aligns the TYPE:
 *     every object of it, a later one included, and a declaration that declares
 *     no object at all still aligns the type it defines;
 *   - written before a declaration that only REFERS to the structure, or after
 *     the structure's body, it aligns the declared OBJECT and leaves the type;
 *   - on a member it moves the member and raises the structure;
 *   - on a type alias it can only RAISE (`align(1)` on an `int` alias stays 4);
 *   - beside `_Alignas` the larger of the two stands.
 *
 * `thread` — cl gives the object THREAD storage: what a second thread writes is
 * not what the first reads. (mingw-w64 gcc ignores the word with a warning and
 * the object is shared; cl's meaning is the one taken, because a program written
 * for it is wrong without it.) And the one case where BOTH compilers give a
 * shared object: `extern __declspec(thread) int v; int v = 1;` — the definition
 * does not repeat the request, both compile it, and on both a second thread's
 * write IS seen. Here the request yields and the compiler SAYS so (the
 * `expectWarnings` entry of the manifest).
 *
 * The rest are requests that mean what their plain names mean (`noinline`,
 * `noreturn`, `deprecated`, `selectany`), the ones that are accepted and inert
 * in an executable (`dllexport`, `restrict`, `noalias`), and the forms of the
 * specifier itself: an empty one, several modifiers separated by white space or
 * by commas (cl; mingw-w64 gcc takes neither), and cl's one-underscore
 * `_declspec`.
 *
 * WHAT MAKES THIS RED. Erase the specifier again and arms 1 to 10 fail, the
 * deprecation warning disappears and `H_NonVoidFunctionEndReachable` is
 * reported (the manifest forbids it: `noreturn` is what makes the end of
 * `ends_in_a_call_that_never_returns` unreachable). Read `align` under the
 * plain spelling's rules and arms 1, 3, 5 and 7 fail. Drop the thread-storage
 * request and arm 9 fails; refuse the disagreeing pair instead of yielding and
 * the program does not build.
 */
#include <stddef.h>
#include <stdint.h>
#include <threads.h>

/* ── align ──────────────────────────────────────────────────────────────── */

__declspec(align(32)) struct defined_here { int a; } defined_object;
struct defined_here defined_later;

__declspec(align(32)) struct defined_alone { int a; };

struct referred { int a; };
__declspec(align(32)) struct referred referred_object;

struct after_body { int a; } __declspec(align(32)) after_body_object;

struct with_member { char c; __declspec(align(32)) int m; };

typedef __declspec(align(32)) int raised_alias;
typedef __declspec(align(1)) int not_lowered_alias;

_Alignas(16) __declspec(align(32)) int beside_alignas;
__declspec(align(32)) int plain_object;

/* ── thread ─────────────────────────────────────────────────────────────── */

__declspec(thread) int per_thread = 1;
static __declspec(thread) int per_thread_internal = 2;

extern __declspec(thread) int shared_after_all;
int shared_after_all = 1;

static int writer(void *unused) {
    (void)unused;
    per_thread = 7;
    per_thread_internal = 7;
    shared_after_all = 7;
    return 0;
}

/* ── the names that keep their meaning, and the inert ones ──────────────── */

__declspec(noinline) static int one(void) { return 1; }
_declspec(noinline) static int uno(void) { return 1; }
__declspec() static int nothing_asked(void) { return 1; }

__declspec(noinline noreturn) static void never_returns(void) { for (;;) { } }
__declspec(noinline, noreturn) static void never_returns_either(void) { for (;;) { } }

static int ends_in_a_call_that_never_returns(int v) {
    if (v == 1) return 1;
    if (v == 2) never_returns_either();
    never_returns();
}

__declspec(dllexport) int exported_datum = 1;
__declspec(dllexport) int exported_function(void) { return 1; }
__declspec(restrict) static void *fresh_pointer(void) { return 0; }
__declspec(noalias) static int reads_nothing_global(int v) { return v; }

__declspec(selectany) int picked_from_any = 1;
__declspec(deprecated("use new_thing")) int old_thing = 1;

static int is_aligned_to_32(void const *p) { return ((uintptr_t)p % 32) == 0; }

int identity(int v) { return v; }

int main(void) {
    int score = 0;
    thrd_t t;
    int joined = -1;

    /*  1 */ score += (_Alignof(struct defined_here) == 32 && sizeof(struct defined_here) == 32);
    /*  2 */ score += (is_aligned_to_32(&defined_object) && is_aligned_to_32(&defined_later));
    /*  3 */ score += (_Alignof(struct defined_alone) == 32 && sizeof(struct defined_alone) == 32);
    /*  4 */ score += (_Alignof(struct referred) == 4 && sizeof(struct referred) == 4
                       && is_aligned_to_32(&referred_object));
    /*  5 */ score += (_Alignof(struct after_body) == 4 && is_aligned_to_32(&after_body_object));
    /*  6 */ score += (offsetof(struct with_member, m) == 32 && sizeof(struct with_member) == 64
                       && _Alignof(struct with_member) == 32);
    /*  7 */ score += (_Alignof(raised_alias) == 32 && _Alignof(not_lowered_alias) == 4
                       && sizeof(raised_alias) == 4);
    /*  8 */ score += (is_aligned_to_32(&beside_alignas) && is_aligned_to_32(&plain_object));

    thrd_create(&t, writer, 0);
    thrd_join(t, &joined);
    /*  9 */ score += (per_thread == 1 && per_thread_internal == 2);
    /* 10 */ score += (shared_after_all == 7 && joined == 0);

    /* 11 */ score += (one() + uno() + nothing_asked() == 3);
    /* 12 */ score += (ends_in_a_call_that_never_returns(1) == 1);
    /* 13 */ score += (exported_datum + exported_function() == 2);
    /* 14 */ score += (fresh_pointer() == 0 && reads_nothing_global(5) == 5);
    /* 15 */ score += (picked_from_any == 1);
    /* 16 */ score += (old_thing == 1);

    return identity(score) + 26;
}
