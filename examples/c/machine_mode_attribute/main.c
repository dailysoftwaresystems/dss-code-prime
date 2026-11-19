/* GNU `mode(M)` -- the declared type replaced by the type of a machine mode
 * (cycle P69 round 1, fold f26c, lane `cs`).
 *
 * WHAT WAS BROKEN. Until this fold the attribute was refused by name; before that
 * refusal it was warned as unknown and IGNORED, so `int x __attribute__((mode(DI)));`
 * was a 4-byte object where gcc 13.3.0, clang 18.1.3, mingw-w64 gcc 13.2.0, Apple
 * clang and aarch64 gcc 13.3.0 all make it 8 (lane `cs`'s probes md and md2). A
 * header that spells a fixed-width integer this way (`__mode__(__DI__)`) got a type
 * of the wrong width, and so a wrong layout and a wrong call.
 *
 * WHY IT IS RUNNABLE. The unit pins assert the analyzer's sizes and types. This
 * proves the bytes: arithmetic wraps at the MODE's width, a parameter is truncated
 * to it at the call, a member sits where the mode puts it, an enumeration is the
 * mode's size -- on every executable target, through the whole pipeline.
 *
 * Only what every target has: the integer modes, SF and DF. XF and TF name the x87
 * and binary128 formats, which only some targets' C has; the unit pins cover them.
 *
 * Exit 42 = every check held; any other code names the first that did not.
 */

typedef int i8m __attribute__((mode(QI)));
typedef unsigned u8m __attribute__((__mode__(__QI__)));
typedef int i16m __attribute__((mode(HI)));
typedef unsigned u32m __attribute__((mode(SI)));
typedef int i64m __attribute__((mode(DI)));
typedef unsigned u64m __attribute__((mode(DI)));
typedef int wordm __attribute__((mode(word)));
typedef float f32m __attribute__((mode(SF)));
typedef double f64m __attribute__((mode(DF)));

struct rec {
    char tag;
    int small __attribute__((mode(HI)));
    int wide __attribute__((mode(DI)));
};

enum flag { FLAG_A = 1, FLAG_B = 2 } __attribute__((mode(QI)));

static int takes_half(int h __attribute__((mode(HI)))) { return h; }

int main(void) {
    int failed = 0;
    if (sizeof(i8m) != 1 || sizeof(u8m) != 1 || sizeof(i16m) != 2 || sizeof(u32m) != 4
        || sizeof(i64m) != 8 || sizeof(u64m) != 8 || sizeof(wordm) != sizeof(void *)
        || sizeof(f32m) != 4 || sizeof(f64m) != 8)
        failed = failed ? failed : 1;

    u8m wrap = 250;
    wrap = (u8m)(wrap + 10);                 /* the QI width: 260 wraps to 4 */
    if (wrap != 4) failed = failed ? failed : 2;

    i8m neg = -1;                             /* the declared signedness is kept */
    if (neg >= 0) failed = failed ? failed : 3;

    u64m big = 1;
    big <<= 40;                               /* the DI width: no bit is lost */
    if ((big >> 40) != 1) failed = failed ? failed : 4;

    i64m product = -5;
    product *= 1000000000;
    if (product != -5000000000LL) failed = failed ? failed : 5;

    if (takes_half(70000) != 4464) failed = failed ? failed : 6;   /* 70000 - 65536 */

    struct rec r = {1, 2, 3};
    if (sizeof r != 16 || r.small + r.wide != 5) failed = failed ? failed : 7;

    enum flag f = FLAG_B;
    if (sizeof f != 1 || f != 2) failed = failed ? failed : 8;

    f64m d = 1.5;
    f32m s = 2.5f;
    if (d + s != 4.0) failed = failed ? failed : 9;

    int v __attribute__((mode(DI))) = 0x7fffffff;
    v += 1;                                   /* past INT_MAX, inside the DI width */
    if (v != 0x80000000LL) failed = failed ? failed : 10;

    return failed ? failed : 42;
}
