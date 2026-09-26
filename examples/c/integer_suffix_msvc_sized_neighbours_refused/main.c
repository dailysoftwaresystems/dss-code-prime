/* SPELLINGS BESIDE MSVC'S SIZED SUFFIXES THAT NO REFERENCE ACCEPTS. The C
 * language document takes MSVC's 24 sized suffixes (`i8` … `ui64`, the `i` and the
 * `u` each in either case) because MSVC accepts them; a spelling next to them is
 * not thereby admitted. ✔MEASURED 2026-09-25 through `dssharness run
 * probe-reference-cc`: MSVC 19.51.36260 (`/std:c17`) refuses every spelling below
 * ('bad suffix on number', C2059), and so do gcc 13.3.0 and clang 18.1.3
 * (`-std=c11 -pedantic-errors` and the default mode) and mingw-w64 gcc 13.2.0
 * ('invalid suffix'): a width MSVC has no type for (`i128`, `i7`), a width with a
 * leading zero (`i08`), C23's character-prefix spelling (`u8`), the `u` on the wrong
 * side or on both (`iu64`, `i64u`, `uu64`), a second suffix after a sized one
 * (`ui64l`), one before it (`LLi64`), and a doubled `i` (`Ii64`). DSS reports each as
 * its own P_MalformedNumber at its own position. This pin was written in P68 round 13
 * together with the change that admitted the 24, to hold the boundary that change
 * must not cross. */

long long a = 5i128;
long long b = 5i7;
long long c = 5i08;
long long d = 5u8;
long long e = 5iu64;
long long f = 5i64u;
long long g = 5ui64l;
long long h = 5LLi64;
long long k = 5Ii64;
long long m = 5uu64;

int main(void) { return (int)(a + b + c + d + e + f + g + h + k + m); }
