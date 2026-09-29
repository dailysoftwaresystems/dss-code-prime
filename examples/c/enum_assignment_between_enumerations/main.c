// P68 round 12 (lane `cs`) — C 6.5.16.1p1: simple assignment admits arithmetic <-
// arithmetic, and an enumerated type is an integer type (6.2.5p17), so a value of one
// enumeration assigns to an object of ANOTHER — in an initializer, an assignment, an
// argument and a return, as the conversion to the target type. `bad` counts the checks
// that fail.

enum E { E0 = 40, E42 = 42 };
enum F { F2 = 2, F42 = 42 };

static enum E take(enum E e) { return e; }
static enum E give(enum F f) { return f; }

static int check(void) {
    int bad = 0;
    enum F f = F42;
    enum E a = F42;          // a constant of another enumeration
    enum E b = f;            // an object of another enumeration
    enum E c = E0;
    c = f;                   // assignment
    bad += a != E42 || b != E42 || c != E42;
    bad += take(f) != E42;   // argument
    bad += give(f) != E42;   // return
    bad += (f == E42) != 1;  // comparison across the two (always valid)
    return bad;
}

int main(void) { return check() == 0 ? 42 : 1; }
