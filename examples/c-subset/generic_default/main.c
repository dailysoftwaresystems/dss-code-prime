// FC16 (D-CSUBSET-GENERIC-SELECTION): the `default` fallback of C11/C23 6.5.1.1.
// The controlling expression's type (`char *`) matches NEITHER `int` NOR
// `double`, so the `default` association is selected and its value is the result.
//   pick(p): p is `char *` -> no int/double match -> default -> 7.
// Uses a pointer controlling type to hit ONLY the default (a distinct code path
// from the typed-match `generic_selection` example).
#define pick(x) _Generic((x), int: 1, double: 3, default: 7)

int main(void) {
    char  c = 0;
    char *p = &c;
    return pick(p);
}
