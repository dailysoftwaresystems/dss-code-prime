/* The static library's one member: an ORDINARY read of the program's ordinary `shared`. */
extern int shared;

int read_shared(void) { return shared; }
