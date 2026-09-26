// P68 round 9 (lane `cs`) — an array of UNKNOWN size that an index designator would
// size past this implementation's limit is REFUSED, not sized wrong (negative /
// diagnostic).
//
// C 6.7.9p22 sizes `a` by its largest initialized index plus one: 4294967297
// elements, 16 GiB of `int`. DSS narrowed the index to 32 bits, so it sized the array
// ONE element long and built it SILENTLY (the run returned 1). gcc 13.3.0 builds it
// and the run faults on its 16 GiB stack array; clang 18.1.3 builds it with a
// frame-size warning and the run returns 42. DSS places an initializer through a
// 32-bit index and builds one slot per element
// (about 195 bytes each), so four billion elements cannot be built here: it says so,
// naming the index and the limit, instead of compiling a different program.

int main(void) {
    int a[] = { [4294967296] = 7 };
    return sizeof a / sizeof a[0] == 4294967297u ? 42 : 1;   // C's length: 42
}
