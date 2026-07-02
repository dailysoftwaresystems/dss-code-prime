/* D-CSUBSET-PARAM-ARRAY-ADJUSTMENT witness (C 6.7.6.3p7): a parameter declared
** 'array of T' adjusts to 'pointer to T' — sized (`int a[4]`), unsized
** (`int a[]`, the sqlite shell.c `sqlite3_value *av[]` / SHA1 `data[]` shape),
** and multi-dimensional (`int m[][3]` -> int (*)[3], only the OUTERMOST
** dimension adjusts). The adjustment must hold at BOTH ends:
**   * the CALL SITE — an argument that decays to `int*` must satisfy an
**     `int a[4]`-declared parameter (pre-fix: S0003 type mismatch), and a
**     bare `[]` parameter must resolve at all (pre-fix: S000B);
**   * the BODY — sizeof(param) is sizeof(pointer) (gcc warns -Wsizeof-array-
**     argument for exactly this reason), and indexing reads through the
**     caller's storage (writes through the param are caller-visible).
**
** Value-divergent arithmetic:
**   sumSized(a4,4)=1+2+3+4=10 ; sumUnsized(&a4[1],2)=2+3=5 ; m[1][2]=20 via
**   the adjusted `int (*)[3]` row pointer ; bump() writes 7 into a4[0]
**   through the adjusted param (caller-visible => +7) ; and on every shipped
**   target sizeof(int*)==8 => szOk contributes 0 when right, wrecks the sum
**   when wrong. 10+5+20+7 = 42.
**
** RED-ON-DISABLE: drop `arrayToPointer` from the c-subset `param` row ->
**   `int a[]` errors S000B (S_NonConstantArrayLength) and the `p+1`/`&a4[1]`
**   pointer args error S0003 against the array-typed params — the example no
**   longer COMPILES. gcc -std=c17 cross-checked: same source exits 42.
**
** Front-end feature (semantic adjustment), target- and format-agnostic.
*/

static int sumSized(int a[4], int n) {
  int s = 0;
  int i;
  /* sizeof(a) is sizeof(int*) == 8 on every shipped 64-bit target (the
  ** ADJUSTED type) — an un-adjusted Array<int,4> would make this 16. */
  if (sizeof(a) != sizeof(int*)) return 999;
  for (i = 0; i < n; i = i + 1) s = s + a[i];
  return s;
}

static int sumUnsized(int a[], int n) {
  int s = 0;
  int i;
  for (i = 0; i < n; i = i + 1) s = s + a[i];
  return s;
}

/* Multi-dim: only the outermost dimension adjusts — `m` is int (*)[3];
** m[1][2] must stride the FULL inner row (3 ints). */
static int pick(int m[][3]) { return m[1][2]; }

/* A write through the adjusted param must land in the CALLER's array. */
static void bump(int a[2]) { a[0] = 7; }

int main(void) {
  int a4[4];
  int grid[2][3];
  int s;
  a4[0] = 1; a4[1] = 2; a4[2] = 3; a4[3] = 4;
  grid[0][0] = 0; grid[0][1] = 0; grid[0][2] = 0;
  grid[1][0] = 0; grid[1][1] = 0; grid[1][2] = 20;

  s = sumSized(a4, 4);            /* array decays -> adjusted param: 10 */
  s = s + sumUnsized(&a4[1], 2);  /* pointer arg -> `[]` param: 2+3 = 5 */
  s = s + pick(grid);             /* int (*)[3] row stride: 20 */
  bump(a4);                       /* writes 7 through the param */
  s = s + a4[0] - 1;              /* 7 - 1 = +6 ... 10+5+20+6 = 41 */
  return s + 1;                   /* 42 */
}
