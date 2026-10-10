/* The caller. It names two functions, each defined TWICE in this program: once in
 * `defaults.c`, where a prototype declares the function weak and the definition of
 * the same unit writes nothing, and once in `overrides.c`, strongly. A weak
 * definition yields to the strong one, so the image returns 120 + 80. */
int lead(void);
int trail(void);

int main(void) { return lead() + trail(); }
