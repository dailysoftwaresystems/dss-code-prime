struct Inner {
    int n;
    int d[];
};

struct Outer {
    struct Inner inner;
    int x;
};
