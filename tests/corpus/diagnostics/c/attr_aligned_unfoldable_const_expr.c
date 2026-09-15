int n;
void f(void) {
    int vla[n];
    long long v __attribute__((__aligned__(sizeof(vla))));
    (void)v;
}
