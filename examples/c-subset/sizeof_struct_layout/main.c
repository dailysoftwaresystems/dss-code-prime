// FC6 (D-FF3-1): the aggregate-layout substrate witnessed end-to-end via
// `sizeof` — the classic alignment-padding + array-stride cases (the FAM case
// lives in sizeof_flexible_array).
//
//   struct Mix { char c; int n; char tail; };
//     c    @ 0  (1 byte)
//     n    @ 4  (4 bytes — 3 bytes of padding after `c` to satisfy int's
//                 4-byte alignment)
//     tail @ 8  (1 byte)
//     size  = 12 (rounded up to the struct's 4-byte alignment — 3 bytes of
//                 tail padding). sizeof(struct Mix) == 12.
//
//   struct Arr { int xs[5]; };
//     xs   @ 0  (5 * 4-byte stride) → size 20. sizeof(struct Arr) == 20.
//
// Both fold through the type_layout engine; `+ x` keeps the exit RUNTIME
// (`x` is a function arg, so ConstFold cannot collapse the program). A
// mis-padded Mix (e.g. a packed 6) or a mis-strided Arr shifts the exit off
// 42 — the witness is sensitive to offsets, padding AND stride.
//
//   exit = 10 + (int)sizeof(struct Mix) + (int)sizeof(struct Arr)
//        = 10 + 12 + 20 = 42.

struct Mix { char c; int n; char tail; };
struct Arr { int xs[5]; };

int run(int x) {
  return x + (int)sizeof(struct Mix) + (int)sizeof(struct Arr);
}

int main(void) { return run(10); }
