/* The defaults. `weak` is written on the PROTOTYPE only — once before the
 * declaration specifiers, once after the declarator — and each definition below
 * carries nothing: the attribute of an earlier declaration is the entity's. */
__attribute__((weak)) int lead(void);
int trail(void) __attribute__((weak));

int lead(void) { return 1; }
int trail(void) { return 2; }
