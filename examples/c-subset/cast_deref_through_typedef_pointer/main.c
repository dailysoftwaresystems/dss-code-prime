// c99 regression pin (D-CSUBSET-TYPEDEF-CAST-SCOPE): a cast-DEREF through a
// typedef-named pointer type — `*(myu64*)&a` — must parse as a POINTER CAST
// followed by a unary deref, NOT as a multiply (`myu64 * <operand>`). This is the
// exact shape of sqlite shell.c's `span64` (`return (*(sqlite3_uint64*)&a) - …`).
//
// The parser's cast-vs-multiply disambiguation (FC2 binder-sketch triage) resolves
// the ambiguity by recognizing the base identifier as a TYPE in scope: the
// BinderSketch records `myu64`/`myi64` as Type bindings when it parses their
// `typedef`s, and at the cast site `sketch.lookup("myu64") == Type` COMMITS the
// cast (never a multiply). A c94 audit once saw 98× P9006 (P_BacktrackFailed) on a
// transient sqlite-master shell.c where a use-before-visible-typedef ordering hid
// the typedef from the sketch at the cast site; the current master (and this pin)
// declare the typedef BEFORE use, so the cast parses. This pin guards against a
// future regression of the in-scope-typedef cast-deref.
//
// It is also a run-green witness: a type-pun round-trip. `a = -1` is all-ones bits;
// reinterpreting them as unsigned gives 0xFFFF_FFFF_FFFF_FFFF, whose top byte is
// 0xFF == 255. If the cast were mis-parsed (or the deref/reinterpret miscompiled),
// the value would differ. `x` arrives as a function arg so the program is not
// const-folded to a literal.
//
//   exit = (int)(bitcast_u(-1) >> 56) + (x - 213) = 255 + (0 - 213) = 42.

typedef long long          myi64;
typedef unsigned long long myu64;

static myu64 bitcast_u(myi64 a) { return *(myu64*)&a; }

int run(int x) {
  myi64 a = -1;
  myu64 u = bitcast_u(a);
  return (int)(u >> 56) + (x - 213);
}

int main(void) { return run(0); }
