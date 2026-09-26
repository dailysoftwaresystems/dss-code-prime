/* INTEGER-SUFFIX SPELLINGS C 6.4.4.1 DOES NOT DEFINE AND NO REFERENCE ACCEPTS.
 * The C language document lists the ones a reference accepts: ISO C's 22, and
 * MSVC's ten mixed-case `ll` forms (`lL`, `Ll`, `ulL`, `uLl`, `UlL`, `ULl`, `lLu`,
 * `lLU`, `Llu`, `LlU`); taking a spelling outside both would be an invention.
 * ✔MEASURED 2026-09-24 over every string of `u`, `U`, `l`, `L` up to four letters:
 * gcc 13.3.0 and clang 18.1.3 (`-std=c11 -pedantic-errors`, and gcc's default
 * mode), mingw-w64 gcc and MSVC 19.51 (`/std:c17`) each reject each spelling
 * below — a doubled `u`, a tripled `l`, a `u` between the two `l`s, a case mix
 * inside a letter run MSVC does not take, and a `u` on both sides. DSS reports
 * each as its own P_MalformedNumber at its own position. This pin was written in
 * P68 round 11 together with the change that admitted the mixed-case spellings
 * the references do accept, to hold the boundary that change must not cross. */

unsigned long long a = 5uu;
unsigned long long b = 5lll;
unsigned long long c = 5lul;
unsigned long long d = 5llL;
unsigned long long e = 5uLLu;

int main(void) { return (int)(a + b + c + d + e); }
