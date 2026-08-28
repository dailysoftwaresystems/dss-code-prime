// c99 (D-CSUBSET-FAM-IN-UNION-MEMBER): a flexible-array-member-bearing struct
// is a legal DIRECT member of a UNION. C99 §6.7.2.1p18 forbids a FAM-struct as a
// member of a STRUCTURE or an element of an ARRAY, but says nothing about a
// UNION — and gcc/clang both accept it. SQLite relies on this
// (`union { SrcList sSrc; u8 srcSpace[SZ_SRCLIST_1]; }` — a stack-allocated slab
// that backs a "fake" one-entry SrcList); before this cycle DSS rejected it with
// error[S001D] (S_FlexibleArrayInAggregate).
//
//   struct Slab { int n; int a[]; };            // a FAM-bearing struct
//   union U { struct Slab s; char space[16]; }; // s is a DIRECT union member
//
// LAYOUT (the real risk — verified sensitive here):
//   * struct Slab: n @ 0 (4 bytes); a[] is the FAM at offset 4 (right after the
//     single leading int, element align 4) but contributes ZERO size (the unsized
//     tail), so sizeof(struct Slab) == 4 (just `n`), align 4. Verified vs gcc.
//   * union U: every member at offset 0; size = max(sizeof(Slab-prefix)=4,
//     sizeof(space)=16) = 16, align = max(4,1) = 4.  If the FAM tail were wrongly
//     counted, or the union took the FAM-struct's size instead of the max, the
//     size would differ.
//
// The exit is a RUNTIME value (x arrives as a function arg, so ConstFold cannot
// collapse the whole program), built from three independent, layout-sensitive
// facts so a miscompile in any one is visible:
//   (1) sizeof(union U) must be 16               → contributes 16
//   (2) write .s.n through the struct member, read byte 0 back through the
//       .space[] member (both at offset 0) — a type-pun round-trip: store
//       n = 26 (0x1A), little-endian byte 0 == 26 → contributes space[0]
//   (3) the FAM element write/read: s.a[0] lands at offset 4; write 0 there and
//       confirm it does not disturb n (offset 0) — contributes 0
//   exit = (sizeof==16 ? 16 : 0) + space[0](=26) + s.a[0](=0) = 16 + 26 = 42.

struct Slab { int n; int a[]; };

union U { struct Slab s; char space[16]; };

int run(int x) {
  union U u;
  int i;
  /* zero the slab bytes so the read-back is deterministic */
  for (i = 0; i < 16; i = i + 1) u.space[i] = 0;

  /* (2) write through the FAM-struct member, read through the byte member */
  u.s.n = x;                 /* x == 26 == 0x0000001A */
  /* (3) the FAM element sits AFTER n; writing it must not clobber n */
  u.s.a[0] = 999;            /* offset 4 — independent of n at offset 0 */
  u.s.a[0] = 0;

  int szTerm = ((int)sizeof(union U) == 16) ? 16 : 0;   /* (1) */
  int punByte = (int)(unsigned char)u.space[0];         /* (2): low byte of n */
  int famVal  = u.s.a[0];                               /* (3): 0 */
  return szTerm + punByte + famVal;
}

int main(void) { return run(26); }
