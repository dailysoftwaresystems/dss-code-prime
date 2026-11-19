/* A `_Bool` BIT-FIELD, READ AND WRITTEN (C 6.7.2.1p5: `_Bool` is a bit-field type).
 *
 * A `_Bool` field shares its byte with its neighbours; a read is the field's bit, a write
 * converts the value to `_Bool` (C 6.3.1.2: 0 when it compares equal to 0, else 1) and
 * keeps every other bit of the byte. Each check returns its own exit code; the program
 * exits 42 when every one holds — as it does on gcc 13, clang 18, AppleClang 21 and
 * mingw-w64 gcc 13.2.
 */

struct F {
    unsigned char lo : 3;
    _Bool f : 1;
    _Bool g : 1;
    unsigned char hi : 3;
};

static volatile struct F v = { 5, 1, 0, 6 };
static volatile int seven = 7, zero = 0;

static int read_f(volatile struct F *p) { return p->f; }

int main(void) {
    if (v.f != 1 || v.g != 0) return 1;              /* a static object's fields */
    if (v.lo != 5 || v.hi != 6) return 2;
    v.g = seven;                                     /* 7 converts to 1 */
    if (v.g != 1) return 3;
    v.f = zero;
    if (v.f != 0 || v.g != 1 || v.lo != 5 || v.hi != 6) return 4;   /* neighbours kept */
    if (v.g + v.g != 2) return 5;                    /* the value is an int 1 */
    if (-v.g != -1) return 6;
    v.f = 0;
    v.f++;
    v.f++;                                           /* a _Bool incremented stays 1 */
    if (v.f != 1) return 7;
    v.f = 0;
    v.f += 2;                                        /* 0 + 2 converts to 1 */
    if (v.f != 1) return 8;
    {
        struct F a = { 1, 1, 0, 2 };                 /* an automatic object */
        a.g = !a.f;
        if (a.g != 0) return 9;
        a.f = !a.f;
        if (a.f != 0 || a.lo != 1 || a.hi != 2) return 10;
        _Bool t = a.f;
        if (t) return 11;
        a.g = (_Bool)seven;
        if (!a.g) return 12;
    }
    if (read_f(&v) != 1) return 13;                  /* through a pointer */
    return 42;
}
