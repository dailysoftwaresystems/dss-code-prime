// [[D-TOK-STRING-STYLE-MULTILINE-IS-NEVER-READ]] — the program the defect was.
// The ' of `V's missing` opened a character constant that ran to the ' of
// `V's wrong`, and every directive between them was body bytes: DSS compiled
// and RAN this, while gcc, clang and MSVC refuse it on the second #error.
#define V 4
#ifndef V
#error V's missing
#endif
#if V != 5
#error V's wrong
#endif
int main(void) { return V; }
