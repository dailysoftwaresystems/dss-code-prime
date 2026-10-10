/* This program reads `shared` as a THREAD-LOCAL object, and the only definition the link finds is the static
   library's ORDINARY `int shared = 7;`: the link is refused by name, naming `shared`, both units and `main`
   (see expected.json). */
extern _Thread_local int shared;

int main(void) { return shared; }
