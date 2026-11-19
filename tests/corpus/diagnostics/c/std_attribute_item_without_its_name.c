// [[D-C-A-STANDARD-ATTRIBUTE-ITEM-THAT-DOES-NOT-PARSE-TERMINATES-THE-ANALYZER]]: a C23
// attribute item broken after its namespace leaves the parser's recovery node INSIDE the
// item, the shape that terminated `analyze()` (P69 fold f26e). Its value is not the codes it
// pins but that the run SURVIVES it: at the end of the specifiers, leading, on a member and
// on a parameter.
int [[gnu::]] x;
[[gnu::]] int y;
struct S { int [[gnu::]] m; };
void f(int [[gnu:: 3]] p);
