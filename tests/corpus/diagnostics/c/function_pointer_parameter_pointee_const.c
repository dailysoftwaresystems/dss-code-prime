static int g(char *s) { return s[0]; }
int main(void) { int (*p)(const char *) = g; return p("*"); }
