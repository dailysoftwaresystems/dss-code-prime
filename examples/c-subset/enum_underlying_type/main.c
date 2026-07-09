// D-CSUBSET-ENUM-UNDERLYING-TYPE (C23 6.7.2.2): an enum with an EXPLICIT
// underlying integer type — `enum E : unsigned char { … }`. The underlying type
// fixes the enum's size/representation (scalars[0] on the interned Enum type), so
// `sizeof(enum Byte) == 1` and it lays out at align 1 — NOT the default int's 4/4.
//
// Riding the D-PARSE-SPECULATIVE-OPTIONAL parser capability: the `enum <tag> :`
// prefix collides with the pre-existing anonymous enum-typed struct bit-field
// `enum Color : 4;`, so the underlying-type clause is a SPECULATIVE optional —
// tried after the `:` and rolled back to the bit-field reading when a type does
// not follow. (This example uses the underlying-type form; the bit-field form is
// pinned by the semantic-analyzer tests.)
//
// This RUNS on every target and — critically — performs a WRAP-AT-256 store+load
// at RUNTIME: `B_BASE(200) + 59 + argc` is stored into the unsigned-char-backed
// enum, which truncates to 8 bits (260 & 0xFF == 4 for argc == 1). The wrap is
// the empirical proof that the explicit underlying flows all the way into
// codegen: an enum value is stored/loaded at its underlying's 8-bit width, and a
// widen back to int uses the U8 conversion form (mir_to_lir routes enum → its
// underlying via `reprKind` at every width site, so no per-underlying fork).
//
// `argc` is a RUNTIME argument (the OS sets it — 1 when run with no args), so the
// wrap-at-256 store+load cannot be constant-folded away even in the RELEASE
// pipeline; the 8-bit truncation really executes.
//
// All values are data-model-INDEPENDENT (unsigned char / offsets are identical
// under LP64 and LLP64), so the one exit code holds across all four targets AND
// the shipped release pipeline.
//
// exit = wrapped(4) + sizeof(enum Byte)(1) + sizeof(struct Pair)(2)
//        + offsetof(Pair,b)(1) + 34 = 42.
//   (a default-int enum would give sizeof 4, a 2-byte-aligned struct, and no
//    wrap — a different exit code, so this is RED on a reverted feature.)

enum Byte : unsigned char { B_BASE = 200 };

// tag@0 (char), b@1 (the 1-byte enum) → sizeof 2, offsetof(b) == 1.
// With the default int underlying, b would be at offset 4 and sizeof would be 8.
struct Pair { char tag; enum Byte b; };

int run(int seed) {
    // 200 + 59 + argc. For argc == 1 the sum is 260, which WRAPS to 4 when stored
    // into the unsigned-char-backed enum (260 & 0xFF). Reading it back into an int
    // widens the 8-bit value (U8 ZExt), yielding 4.
    enum Byte v = B_BASE + 59 + seed;
    return (int)v;
}

int main(int argc, char **argv) {
    (void)argv;
    int wrapped = run(argc);                          // 4  (u8 wrap at 256)
    int se = (int)sizeof(enum Byte);                  // 1  (RED default-int: 4)
    int sp = (int)sizeof(struct Pair);                // 2  (RED default-int: 8)
    int ob = (int)(long long)&((struct Pair *)0)->b;  // 1  (RED default-int: 4)
    return wrapped + se + sp + ob + 34;               // 4 + 1 + 2 + 1 + 34 = 42
}
