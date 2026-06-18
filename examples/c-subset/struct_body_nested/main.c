typedef struct {
    struct Inner { int x; int y; } in;
    int z;
} Outer;

int main(void) {
    Outer o;
    o.in.x = 30;
    o.in.y = 0;
    o.z = 12;
    struct Pt { int a; } p;
    p.a = 0;
    return o.in.x + o.in.y + o.z + p.a;
}
