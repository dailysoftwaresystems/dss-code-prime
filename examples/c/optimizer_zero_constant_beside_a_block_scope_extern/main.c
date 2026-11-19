/* A constant the OPTIMIZER makes must not take the id of something the unit already names.
   `x` is assigned on the path this program takes (c is argc, never 0), so no uninitialized object is read and C
   defines the result. A release build still makes a zero constant for the path NOT taken, and has to number it.
   This unit is arranged so that a number counted from the symbols the module holds lands on a declaration: its
   highest-numbered module symbol is the static local `tick`, and the very next declaration is the block-scope
   extern `ext_in`, which the other unit defines. No float or string literal here, whose own objects would lift
   such a count clear. Run with no argument: x = 41, ext_in(41) = 42. */
double pick(int, double);
int main(int argc, char **argv) { (void)argv; return (int)pick(argc, argc + 40); }
double pick(int c, double v) {
    static int tick;
    extern double ext_in(double);
    double x;
    if (c) x = v;
    ++tick;
    return ext_in(x);
}
