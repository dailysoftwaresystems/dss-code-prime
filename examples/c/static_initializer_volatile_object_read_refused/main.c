// P68 round 13 (fold F7) — a VOLATILE object's value in a static initializer. An object is
// volatile when its type, looked through its ARRAY spine, is volatile-qualified: C 6.7.3p10 puts
// an array's qualifier on its ELEMENT type, where the static-initializer check once missed it and
// DSS built the first marked line silently, running 42. Every reference refuses each marked
// initializer, and so does DSS, at the construct.
static const volatile int cva[2] = { 1, 42 };
int x = cva[1];                                  // a const volatile array's element
static const volatile int m[2][2] = { { 1, 2 }, { 3, 42 } };
int y = m[1][1];                                 // ... a 2-D one's
int z = ((const volatile int[]){ 1, 42 })[1];    // ... a const volatile compound literal's

int main(void) { return x + y + z; }
