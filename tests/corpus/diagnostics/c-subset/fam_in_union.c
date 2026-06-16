struct Inner {
    int n;
    int d[];
};

union U {
    struct Inner inner;
    int x;
};
