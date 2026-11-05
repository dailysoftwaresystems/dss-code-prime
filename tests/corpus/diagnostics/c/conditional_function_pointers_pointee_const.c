static int f_plain(char *s) { return s[0]; }
static int f_const(const char *s) { return s[0]; }
int main(int argc, char **argv) { (void)argv; void *v = argc > 0 ? &f_plain : &f_const; return v != 0; }
