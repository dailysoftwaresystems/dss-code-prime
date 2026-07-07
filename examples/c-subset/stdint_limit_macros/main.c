/* c82 D-FFI-STDINT-LIMIT-MACROS (first slice) witness: INT64_MIN / INT64_MAX
** ship on stdint.json's `constants` surface with EXACT int64 values — the
** sqlite shell.c:15586 shape `N = N==INT64_MIN ? INT64_MAX : -N;` (integer
** absolute-value with the INT64_MIN overflow guard) is reproduced verbatim.
**
** Exactness pins (any decode drift — e.g. a double transit losing low bits —
** wrecks these):
**   INT64_MAX must equal 0x7fffffffffffffff  (low 32 bits 0xffffffff)
**   INT64_MIN must equal -INT64_MAX - 1
**   the sqlite guard: N==INT64_MIN keeps N positive via INT64_MAX
**
** exit 42 by value-divergent accumulation. RED-ON-DISABLE: remove the two
** stdint.json constants -> S0001 undeclared INT64_MIN/INT64_MAX (compile
** failure). gcc -std=c17 cross-checked: same source exits 42.
*/

#include <stdint.h>

static long long absClamped(long long N) {
  if (N < 0) {
    N = N == INT64_MIN ? INT64_MAX : -N;   /* shell.c:15586 verbatim shape */
  }
  return N;
}

int main(void) {
  int rc = 0;
  long long maxv = INT64_MAX;
  long long minv = INT64_MIN;

  /* Bit-exactness: low 32 bits of INT64_MAX are all-ones. */
  if ((maxv & 0xffffffff) != 0xffffffff) rc = rc + 100;
  /* Two's-complement identity. */
  if (minv != -maxv - 1) rc = rc + 100;
  /* The overflow guard takes the INT64_MAX arm... */
  if (absClamped(minv) != maxv) rc = rc + 100;
  /* ...and an ordinary negative negates. */
  if (absClamped(-42) != 42) rc = rc + 100;

  return rc + (int)absClamped(-42);   /* 42 */
}
