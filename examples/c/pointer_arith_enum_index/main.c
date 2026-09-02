// D-CSUBSET-POINTER-ARITH-ENUM-INDEX (C23 6.5.7p2 + 6.2.5p22): an ENUM-typed
// operand is an INTEGER operand of `+`/`-` on a pointer, so `p + e`, `e + p`,
// `p - e`, `p += e` and `p -= e` must compile and give the same answers a plain
// integer index gives. Before the fix EVERY one of those refused at the MIR
// `p ± n` index-widen site: `mapCast` is TypeKind-only and its `isInt` excludes
// Enum, so the widen to a 64-bit byte offset had no opcode and the site failed
// loud. (The SUBSCRIPT spelling `p[e]` never did refuse — the HIR tier's
// `combineIndex` integer-promotes a subscript, which resolves the enum before
// MIR ever sees it. The additive spelling has no such promotion, because C
// applies no usual arithmetic conversion to a pointer/integer pair.)
//
// ★ THE EXIT CODE DISCRIMINATES THE *SIGN* OF THE WIDEN, not merely the fact
// that one happened. C 6.5.7p8 keeps the integer operand's OWN type, so a
// signed underlying must SIGN-extend and an unsigned one must ZERO-extend, and
// getting that backwards is a silent miscompile rather than a refusal. Every
// read below is placed so that BOTH sign choices land INSIDE `buf` — a wrong
// widen therefore returns a different exit code instead of crashing, which is
// the only way an exit-code comparison can see it at all:
//
//   read            correct index      wrong-sign index     buf value / wrong
//   *(mid + sb)     buf[254]           buf[510]             127  /  255
//   *(mid - sb)     buf[258]           buf[  2]             129  /    1
//   *(mid + uf)     buf[456]           buf[200]             228  /  100
//   *(mid - uf)     buf[ 56]           buf[312]              28  /  156
//
//   sb = -2 in a `signed char`-backed enum   (SExt required; ZExt gives 254)
//   uf = 200 in an `unsigned char`-backed enum (ZExt required; SExt gives -56)
//
// ★ EVERY ENUM VALUE IS LOADED FROM A TABLE INDEXED BY `argc`, so none of them
// is a compile-time constant and the widen really executes — including under the
// shipped RELEASE pipeline, where a constant would simply be folded away and the
// arm would prove nothing. `argc` is 1 when the runner spawns the artifact.
//
// Red-on-disable: revert the `resolveScalarIntKind` resolution at the
// `combineBinaryOp` `p ± n` widen site and this example no longer COMPILES
// (error[H_UnsupportedLoweringForKind] naming the index TypeKind).
//
// Data-model independent (unsigned char values and small offsets are identical
// under LP64 and LLP64), so one exit code holds on all four targets.

enum SNarrow : signed char   { S_BACK = -2, S_OTHER = 3 };
enum UNarrow : unsigned char { U_NEAR =  1, U_FAR   = 200 };
enum Step                    { ST_ONE =  1, ST_TWO  = 2 };
typedef enum { T_ZERO = 0, T_FOUR = 4 } StepT;
enum { A_THREE = 3 };            // anonymous enum: an enumerator with no tag

// argc-indexed tables: slot 1 is what a no-argument run reads, so every enum
// value below is produced by a RUNTIME load rather than a literal.
static enum SNarrow const sTab[2] = { S_OTHER, S_BACK };
static enum UNarrow const uTab[2] = { U_NEAR,  U_FAR  };
static enum Step    const pTab[2] = { ST_ONE,  ST_TWO };
static StepT        const tTab[2] = { T_ZERO,  T_FOUR };

static unsigned char buf[512];
static int const iv[8] = { 10, 11, 12, 13, 14, 15, 16, 17 };

int main(int argc, char **argv) {
    unsigned char *mid;
    unsigned char *q;
    enum SNarrow sb;
    enum UNarrow uf;
    enum Step    st;
    StepT        te;
    unsigned     sum = 0u;
    int          i;

    (void)argv;
    for (i = 0; i < 512; ++i) buf[i] = (unsigned char)(i >> 1);
    mid = buf + 256;

    sb = sTab[argc];                 // -2, signed char underlying  -> SExt
    uf = uTab[argc];                 // 200, unsigned char underlying -> ZExt
    st = pTab[argc];                 // 2, default (int) underlying
    te = tTab[argc];                 // 4, typedef'd anonymous enum

    sum += *(mid + sb);              // p + e   (signed, negative)   -> buf[254]
    sum += *(sb + mid);              // e + p   (commuted addition)  -> buf[254]
    sum += *(mid - sb);              // p - e                        -> buf[258]
    sum += *(mid + uf);              // p + e   (unsigned, 200)      -> buf[456]
    sum += *(mid - uf);              // p - e                        -> buf[ 56]

    q = mid; q += sb;                // p += e                       -> buf[254]
    sum += *q;
    q = mid; q -= uf;                // p -= e                       -> buf[ 56]
    sum += *q;

    sum += *(mid + S_BACK);          // an enumeration CONSTANT, no enum object
    sum += *(mid + A_THREE);         // an ANONYMOUS enum's enumerator -> buf[259]
    sum += *(mid + te);              // a TYPEDEF'd enum object        -> buf[260]

    sum += (unsigned)*(iv + st);     // a non-1 element STRIDE: iv[2] == 12

    return (int)(sum % 200u);
}
