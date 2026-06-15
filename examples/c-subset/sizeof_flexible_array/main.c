// FC6 (D-FF3-1): the aggregate-layout substrate witnessed end-to-end via
// `sizeof`. The compiler computes struct field offsets + padding + total size
// from per-ABI params in the .target.json (read by the generic type_layout
// engine), then const-folds `sizeof` through it.
//
//   struct Buf { int len; int cap; int data[]; };
//
// Natural layout (SysV / Win64 / AAPCS64 / Apple all agree for `int`):
//   len  @ 0   (4 bytes)
//   cap  @ 4   (4 bytes)
//   data @ 8   — FLEXIBLE ARRAY MEMBER (C99 §6.7.2.1): contributes its
//               offset but ZERO size (the unsized tail), so the struct's
//               size is just the two leading ints: sizeof(struct Buf) == 8.
//
// `sizeof` const-folds to 8 at compile time; the `+ x` keeps the exit a
// RUNTIME value — `x` arrives as a function argument, so the ConstFold arm
// cannot collapse the whole program to a literal. If the FAM tail were
// wrongly counted toward the size, or the two ints mis-padded, sizeof != 8
// and the exit != 42 — the witness is sensitive to the exact layout.
//
//   exit = 34 + (int)sizeof(struct Buf) = 34 + 8 = 42.

struct Buf { int len; int cap; int data[]; };

int run(int x) { return x + (int)sizeof(struct Buf); }

int main(void) { return run(34); }
