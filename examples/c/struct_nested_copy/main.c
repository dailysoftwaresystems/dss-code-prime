/* FC7 (D-FC7-AGGREGATE-COPY-MEMCPY): copy a STRUCT with an AGGREGATE field
 * (`struct Inner in;` inside Outer). A field-wise copy can't realize an
 * aggregate-width field Load, so lowerAggregateCopy copies the whole object
 * BYTE-WISE (Outer = 12 bytes = one I64 chunk @0 covering `in`, one I32 @8 for
 * `z`). The FAR field `z` (offset 8) must survive the copy — a truncating
 * 8-byte-only copy would drop it. Values (13,17,12) arrive as runtime args
 * across the non-inlined fill(). 13 + 17 + 12 = 42. RED-ON-DISABLE: revert the
 * aggregate-field byte-wise arm → the copy fails loud (field-wise can't Load
 * the struct-typed field `in`). */
struct Inner { int x; int y; };
struct Outer { struct Inner in; int z; };
void fill(struct Outer* o, int a, int b, int c) { o->in.x = a; o->in.y = b; o->z = c; }
int main(void) {
    struct Outer a;
    fill(&a, 13, 17, 12);
    struct Outer b = a;                  /* struct-with-aggregate-field copy → byte-wise */
    return b.in.x + b.in.y + b.z;        /* 13 + 17 + 12 = 42 (far field z survives) */
}
