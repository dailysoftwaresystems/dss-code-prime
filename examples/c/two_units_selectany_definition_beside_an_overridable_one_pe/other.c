/* The other unit: the select-any definition of one name and an overridable weak definition of the other. */
__declspec(selectany) int picked_there = 10;
__attribute__((weak)) int picked_here = 5;

int read_picked_there(void) { return picked_there; }
int read_picked_here(void) { return picked_here; }
