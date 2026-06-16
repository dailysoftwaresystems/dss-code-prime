/* FC7 (D-FC7-AGGREGATE-COPY-MEMCPY): a struct's AGGREGATE field initialized
 * from an aggregate VALUE (not a nested brace) — `struct Outer o = { i, ... }`
 * where `i` is an existing Inner. lowerAggregateInitIntoSlot copies the value
 * `i` into the field `in`'s sub-slot via lowerAggregateCopy (the path Cycle A
 * had deferred to this anchor; now closed). Values arrive via the non-inlined
 * mk() so nothing folds. 13 + 17 + 12 = 42. RED-ON-DISABLE: revert the
 * value-into-aggregate-field copy → this initializer fails loud. */
struct Inner { int x; int y; };
struct Outer { struct Inner in; int z; };
int mk(int v) { return v; }
int main(void) {
    struct Inner i;
    i.x = mk(13);
    i.y = mk(17);
    struct Outer o = { i, mk(12) };      /* field `in` <- value `i` (aggregate copy) */
    return o.in.x + o.in.y + o.z;        /* 13 + 17 + 12 = 42 */
}
