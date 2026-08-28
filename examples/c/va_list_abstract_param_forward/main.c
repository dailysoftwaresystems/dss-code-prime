/* c82 D-CSUBSET-PARAM-ARRAY-ADJUSTMENT — the ABSTRACT-parameter corner
** (the sqlite3_vmprintf shape that broke mid-c82, pinned here): a PROTOTYPE
** with an UNNAMED va_list parameter,
**
**     static int vsum(int n, va_list);      <- abstract: type-only param
**     static int vsum(int n, va_list ap) {  <- definition: named param
**
** must land the SAME FnSig as the definition — C 6.7.6.3p7 treats a
** parameter's declared type identically with or without a declarator name,
** and BOTH resolution paths run the ONE shared adjustment helper, so a
** prototype/definition pair can never drift (pre-fix the named side and the
** abstract side disagreed and the forward call errored S0003 `got ap`,
** witnessed on shell.c:34079).
**
** va_list itself is deliberately EXCLUDED from the array->pointer
** adjustment (on SysV va_list IS an array type): C 7.16 makes a va_list
** observable only through the va_* macros + forwarding, and the per-CC
** va_* machinery owns its parameter passing end-to-end — adjusting it
** silently added an indirection level under va_arg (a SysV-only SIGSEGV,
** witnessed on THIS example mid-c82; arm64/pe were immune since their
** va_list is not an array). The exclusion applies on BOTH paths, keeping
** the symmetry. This example therefore pins BOTH properties: the
** prototype==definition FnSig equality AND the working va_arg reads
** through a double forward on every ABI.
**
** vsum(3, 20, 12, 9) = 41 ; +1 = 42. gcc -std=c17 cross-checked.
*/

#include <stdarg.h>

static int vsum(int n, va_list);   /* ABSTRACT va_list param (prototype) */

static int vsum(int n, va_list ap) {   /* NAMED va_list param (definition) */
  int s = 0;
  int i;
  for (i = 0; i < n; i = i + 1) s = s + va_arg(ap, int);
  return s;
}

/* Forward a RECEIVED va_list through the prototype — the shell.c shape. */
static int vforward(int n, va_list ap) { return vsum(n, ap); }

static int sum(int n, ...) {
  va_list ap;
  int s;
  va_start(ap, n);
  s = vforward(n, ap);
  va_end(ap);
  return s;
}

int main(void) {
  return sum(3, 20, 12, 9) + 1;   /* 41 + 1 = 42 */
}
