// P53 (D-CSUBSET-ABSTRACT-ARRAY-TYPE-NAME, ISO C 6.7.7).
//
// A direct-abstract-declarator may BE a bare `[ ... ]` suffix, so `char[4]`,
// `int[2][3]` and `long[2]` are type-names wherever a type-name is admitted.
// DSS refused every one of them at this cycle's base — the abstract declarator
// carried a pointer/function base but no ARRAY one — so the compile-time size
// assertion idiom `sizeof(char[(e) ? -1 : 1])` was unavailable and a shipped
// config had to hang its guard array off a struct member instead.
// ✔MEASURED 2026-09-02, each reference invoked SEPARATELY: gcc 13.3.0
// `-std=c2x`, clang 18.1.3 `-std=c23` and MSVC 19.51 `/std:clatest` accept
// every construct below.
//
// ★ EVERY TYPE-NAME POSITION IS EXERCISED, because they all route through the
// one `castTypeRef` shape and a fix that reached only `sizeof` would read as a
// complete one: sizeof, _Alignof, _Alignas, a cast, a compound literal,
// __builtin_types_compatible_p and _Generic.
//
// ⚠ THE ELEMENT-COUNT IDIOM IS THE CONTROL, AND IT IS THE REASON THE ARRAY BASE
// WAS EXCLUDED IN THE FIRST PLACE. `sizeof(b)/sizeof(b[0])` must stay the VALUE
// reading: `b` is a declared object, so the speculative type-vs-value triage
// must roll the type-name reading back on the binder sketch rather than commit
// to a type named `b[0]`. Both readings live in this one program.
//
// ★ RED-ON-DISABLE (REMOVE direction), ✔EXERCISED P53: remove `arrayDeclSuffix`
// from `abstractDirectDeclarator`'s base alt in
// `src/dss-config/sources/c.lang.json` and this example FAILS IN BOTH RUNNERS
// while `sizeof_value_array_dim` and `abstract_fnptr_cast` stay green — the
// control that says the mutant is targeted and did not merely break parsing.

// The strictly-ISO compile-time assertion this construct unlocks: a false
// predicate makes the array length negative, which is a constraint violation
// rather than a silent 0-sized extension.
#define SIZE_ASSERT(e) ((void)sizeof(char[(e) ? 1 : -1]))

static int b[7];

int main(void) {
    // 1. sizeof over a bare array type-name, one and two dimensions.
    if (sizeof(char[4]) != 4) return 1;
    if (sizeof(char[6][7]) != 42) return 2;
    if (sizeof(int[2][3]) != 6 * sizeof(int)) return 3;
    if (sizeof(char[2 + 2]) != 4) return 4;

    // 2. _Alignof over one.
    if (_Alignof(int[4]) != _Alignof(int)) return 5;

    // 3. _Alignas taking an array type-name as its argument.
    {
        _Alignas(long[2]) char buf[16];
        buf[0] = 42;
        if (buf[0] != 42) return 6;
    }

    // 4. A compound literal whose type-name is a bare array.
    if (sizeof((char[42]){0}) != 42) return 7;
    {
        int *q = (int *)(char[16]){0};
        if (q == 0) return 8;
    }

    // 5. __builtin_types_compatible_p and _Generic over array type-names.
    if (!__builtin_types_compatible_p(int[3], int[3])) return 9;
    if (__builtin_types_compatible_p(int[3], int[4])) return 10;
    if (_Generic((int(*)[3])0, int(*)[3] : 1, default : 0) != 1) return 11;

    // 6. The compile-time assertion idiom itself, in both directions: a true
    //    predicate compiles, and the macro is the shape a shipped config wants.
    SIZE_ASSERT(sizeof(int) >= 2);
    SIZE_ASSERT(sizeof(char[4]) == 4);

    // 7. THE CONTROL — `sizeof(b)/sizeof(b[0])` is a VALUE expression, not a
    //    type-name. `b` is a declared object; a commit to the type reading
    //    would size the element wrongly and this would not be 7.
    if ((int)(sizeof(b) / sizeof(b[0])) != 7) return 12;
    if ((b[0]) != 0) return 13;

    // 8. The paren-form abstract declarators that already worked, re-asserted
    //    beside the new base so a regression there is visible here too.
    if (sizeof(char (*)[4]) != sizeof(void *)) return 14;
    if (sizeof(int (*)(void)) != sizeof(void *)) return 15;

    return 42;
}
