/* The other unit: the definition of one name and a tentative definition of the other (see main.c). */
int seeded_there = 20;
int seeded_here;

int read_there(void) { return seeded_there; }
int read_here(void) { return seeded_here; }
