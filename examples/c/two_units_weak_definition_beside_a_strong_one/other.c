/* The other unit: the strong definition of one name and a weak definition of the other (see main.c). */
int weak_here = 20;
__attribute__((weak)) int weak_there = 5;

int read_weak_here(void) { return weak_here; }
int read_weak_there(void) { return weak_there; }
