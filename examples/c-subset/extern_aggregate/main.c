/* D-CSUBSET-EXTERN-AGGREGATE-TYPE (TF arc C6, SQLite testfixture 2026-07-17):
   the RUN witness for a file-scope `extern` declaration whose type-specifier is a
   struct / union aggregate. Before C6, `extern struct Foo g;` P0009'd ("expected
   [simple-type-specifiers / Identifier / const / typeof] — got 'struct'"): the
   extern grammar path (externDecl -> typeRef -> typeBase) omitted the
   struct/union/enum specifiers, while `static struct`/plain `struct Foo g;`/
   `typedef struct` all parsed fine. This was the DOMINANT sqlite-testfixture
   compile blocker — 19 of the 44 src/test*.c TUs hit it FIRST via sqliteInt.h's
   `extern SQLITE_WSD struct Sqlite3Config sqlite3Config;` (SQLITE_WSD -> empty in
   the default non-WSD build; the amalgamation strips that line, which is why
   sqlite3.c compiled clean but the test TUs -- which #include "sqliteInt.h"
   directly -- did not).

   Each aggregate global is extern-DECLARED (the previously-failing form) then
   DEFINED, and read at runtime; the four contributions sum to 42:
     extern struct tag      : gCfg.lo   = 15
     extern union tag       : gBox.i    = 12
     extern const struct    : gRo.lo    = 10
     extern pointer-to-struct: gPtr->lo =  5
   The extern ENUM form parses too (covered by the parse-pin unit test
   ExternAggregateSpecifiersParse) but an enum GLOBAL hits a SEPARATE, pre-existing
   codegen gap (D-CSUBSET-ENUM-GLOBAL-CODEGEN) unrelated to this front-end parse
   fix, so the enum form is witnessed at the parse tier, not at runtime here.

   RED-ON-DISABLE: revert typeBase's alt list in c-subset.lang.json to
   {typeSpecifierSeq, Identifier, typeofSpecifier} and every `extern struct/union`
   line above P0009s -> the example fails to compile. */

struct Cfg { int lo; int hi; };
union  Box { int i; long l; };

extern struct Cfg gCfg;          /* extern + struct tag  (was P0009 "got struct") */
extern union  Box gBox;          /* extern + union tag   (was P0009 "got union")  */
extern const struct Cfg gRo;     /* extern + const struct                          */
extern struct Cfg *gPtr;         /* extern + pointer-to-struct                     */

struct Cfg gCfg = { 15, 0 };     /* the definitions */
union  Box gBox = { 12 };
const struct Cfg gRo = { 10, 0 };
struct Cfg gPtrTarget = { 5, 0 };
struct Cfg *gPtr = &gPtrTarget;

int main(void) {
    return gCfg.lo + gBox.i + gRo.lo + gPtr->lo;   /* 15 + 12 + 10 + 5 = 42 */
}
