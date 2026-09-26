static int g(void) { return 42; }
static int x = g();
int main(void) { return x; }
