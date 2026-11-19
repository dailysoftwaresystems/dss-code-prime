/* `__declspec(selectany)` says "several units may define this — keep one copy". A STRONG definition of the name in
   another unit (b.c) is not one of the copies: on PE the pair is a duplicate definition, in either order. */
__declspec(selectany) int v = 1;

int main(void) { return v; }
