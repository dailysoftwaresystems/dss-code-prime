int x = ((const volatile int[]){ 1, 42 })[1];
int main(void) { return x; }
