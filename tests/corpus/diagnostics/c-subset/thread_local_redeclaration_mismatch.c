extern int g;
thread_local int g = 5;
extern thread_local int h;
int h = 7;
int main(void) { return g + h; }
