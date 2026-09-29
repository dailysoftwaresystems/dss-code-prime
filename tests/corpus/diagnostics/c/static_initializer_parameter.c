static int f(int p) { static int x = p; return x; }
int main(void) { return f(42); }
