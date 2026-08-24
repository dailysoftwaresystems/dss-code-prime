/* FC7 (D-FC7-NESTED-STRUCT-FIELD): a struct whose FIELD is itself a struct
 * type (`Inner in;` inside `Outer`) — now parses, and nested member access
 * `o->in.x` read+write composes the byte offsets (in @ 0 within Outer, x @ 0
 * / y @ 4 within Inner, z @ 8 within Outer). The values (10, 20, 12) arrive
 * as runtime args across the non-inlined fill()/sum() calls, so the fields
 * genuinely round-trip through Outer's frame slot — no pass folds them away.
 * 10 + 20 + 12 = 42. RED-ON-DISABLE: revert the grammar `Identifier` alt and
 * this no longer parses; a wrong nested offset (e.g. in.y resolved to 0)
 * drops it off 42. */
typedef struct { int x; int y; } Inner;
typedef struct { Inner in; int z; } Outer;
void fill(Outer* o, int a, int b, int c) { o->in.x = a; o->in.y = b; o->z = c; }
int sum(Outer* o) { return o->in.x + o->in.y + o->z; }
int main(void) {
    Outer o;
    fill(&o, 10, 20, 12);
    return sum(&o);
}
