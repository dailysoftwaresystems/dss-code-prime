// FC2 (V2-4.X, 2026-06-10): the binder-sketch runtime witness. The SAME
// ambiguous surface form `(myint)-X` parses BOTH ways in one program,
// decided purely by the sketch's scope-aware type-vs-value lookup:
//
//   * in helper: the parameter `myint` SHADOWS the file-scope typedef
//     (funcDefTail opens one scope over params + body), so `(myint)-2`
//     is the VALUE reading — binary subtraction: myint - 2.
//   * in main: the file-scope typedef is live, so `(myint)-helper(10)`
//     is the TYPE reading — a cast of the negated call result.
//
//   helper(10) = 10 - 2 = 8
//   exit       = (myint)(-8) + 50 = 42
//
// The readings are VALUE-DIVERGENT, not just error-divergent: a sketch
// that ignored the shadow would parse helper's body as the cast
// `(myint)(-2)` = -2 -> exit -(-2) + 50 = 52 != 42. A sketch that lost
// the file-scope typedef would parse main's form as subtraction with
// `myint` as an operand -> S_UndeclaredIdentifier, a loud compile
// failure under this harness's zero-diagnostic assert. Either
// regression flips this example RED.
//
// The typedef-to-int cast is the identity I32 cast — deliberately: a
// width cast's result cannot flow back into an int context today
// ((char) in return position fires S_ReturnTypeMismatch; implicit
// char->int widening is not in c-subset's assignability), so identity
// keeps the witness focused on the PARSE decision. Pure int ops ->
// all four target arms (the modulo arm set), including arm64.
typedef int myint;

int helper(int myint) { return (myint)-2; }

int main() { return (myint)-helper(10) + 50; }
