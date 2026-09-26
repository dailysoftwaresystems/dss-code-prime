// P68 round 13 (fold F7) — an address whose BASE is null, converted to an integer narrower
// than a pointer, in a static initializer: no relocation is involved, the value is a plain
// integer constant, and every reference builds each line (the offsetof spelling with an `int`
// cast among them). Only a SYMBOL's address in a narrower integer is refused (the diagnostics
// corpus holds that refusal). `bad` counts the checks that fail.

struct T { int a; int m; };

int off = (int)&((struct T *)0)->m;                 // the offsetof spelling, narrowed
char coff = (char)&((struct T *)0)->m;              // ... to a char
int zero = (int)(void *)0;                          // a null pointer
short five = (short)(char *)5;                      // an integer constant cast to a pointer
int idx = (int)&((int *)0)[3];                      // an element of a null-based array

static int check(void) {
    int bad = 0;
    bad += off != (int)sizeof(int) || coff != (char)sizeof(int);
    bad += zero != 0 || five != 5 || idx != 3 * (int)sizeof(int);
    return bad;
}

int main(void) { return check() == 0 ? 42 : 1; }
