/* c82 D-CSUBSET-ARRAY-ARROW-DECAY witness (C 6.3.2.1p3 + 6.5.2.3): an ARRAY
** expression as the LHS of `->` decays to a pointer to its first element
** before the arrow applies — the sqlite shell.c shape
**
**     ShellState data;  ...  data.aAuxDb->zDbFilename
**
** (aAuxDb is an IN-STRUCT ARRAY member; the arrow reads/writes element
** [0]'s field). Covered on BOTH tiers with one rule: the semantic member
** resolver unwraps Array exactly like Ptr, and the HIR arrow lowering
** decays the array LHS through the ONE shared coerce decay arm before its
** synthetic Deref.
**
** The witness proves ELEMENT-0 IDENTITY, not just type-acceptance:
** writes through `data.aAuxDb->` must land in aAuxDb[0] (read back via the
** explicit [0] index), aAuxDb[1] must stay untouched, and a plain
** file-scope array-of-struct gets the same treatment.
**
** exit = 40 (arrow-written, [0]-read) + 2 ([1] explicit) + 0 (identity
** checks) = 42.
**
** RED-ON-DISABLE: revert either tier's Array arm -> S000E (semantic) or
** H0001 (lowering) — the example no longer COMPILES (witnessed pre-fix).
** gcc -std=c17 cross-checked: same source exits 42.
*/

typedef struct AuxDb {
  const char *zDbFilename;
  int n;
} AuxDb;

typedef struct ShellState {
  int pad;             /* keeps aAuxDb off offset 0 — the decay must not
                       ** collapse to the CONTAINER's address */
  AuxDb aAuxDb[3];
  AuxDb *pAuxDb;
} ShellState;

static AuxDb gPool[2];   /* file-scope array-of-struct: gPool->n == gPool[0].n */

int main(void) {
  ShellState data;
  const char *z = "x";
  int rc = 0;

  data.aAuxDb[0].n = 0;
  data.aAuxDb[1].n = 2;
  data.aAuxDb[2].n = 99;

  /* Writes through the arrow land in element [0]... */
  data.aAuxDb->zDbFilename = 0;
  data.aAuxDb->n = 40;
  /* ...and are read back through the arrow AND the explicit [0]. */
  if (data.aAuxDb->zDbFilename != 0) rc = rc + 100;
  if (data.aAuxDb[0].n != 40) rc = rc + 100;
  if (data.aAuxDb->n != 40) rc = rc + 100;
  /* Neighbors untouched. */
  if (data.aAuxDb[1].n != 2 || data.aAuxDb[2].n != 99) rc = rc + 100;

  /* The shell.c null-then-assign shape. */
  if (data.aAuxDb->zDbFilename == 0) { data.aAuxDb->zDbFilename = z; }
  if (data.aAuxDb->zDbFilename != z) rc = rc + 100;

  /* A file-scope array decays the same way. */
  gPool->n = 5;
  if (gPool[0].n != 5) rc = rc + 100;
  gPool[1].n = 6;
  if (gPool->n != 5) rc = rc + 100;

  return rc + data.aAuxDb->n + data.aAuxDb[1].n;   /* 40 + 2 = 42 */
}
