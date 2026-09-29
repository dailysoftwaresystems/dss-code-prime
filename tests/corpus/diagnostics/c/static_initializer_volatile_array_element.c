static const volatile int cva[2] = { 1, 42 };
int x = cva[1];
int main(void) { return x; }
