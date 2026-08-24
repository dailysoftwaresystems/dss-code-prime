// FC13 cycle 1 runtime witness (header leg). A quote-`#include`d header whose
// object-like `#define`s must reach the includer: if the preprocessor's
// recursive quote-include text splice OR the macro table build regresses,
// these names are undefined in main.c and the program FAILS TO COMPILE (the
// example harness's zero-diagnostic assert flips RED before any run).
#define BASE   30
#define MARGIN 12
