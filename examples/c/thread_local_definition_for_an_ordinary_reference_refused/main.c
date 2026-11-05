/* This program DEFINES `shared` with thread storage duration, and the static library's member that defines
   `read_shared` reads it as an ORDINARY object (`extern int shared;`): the link is refused by name, naming
   `shared`, both units and `read_shared` (see expected.json). */
_Thread_local int shared = 7;

int read_shared(void);

int main(void) { return read_shared(); }
