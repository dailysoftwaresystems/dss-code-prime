// c99 (D-CSUBSET-FAM-IN-UNION-MEMBER): a DIRECT flexible-array-member-bearing
// struct member of a UNION is LEGAL (C99 §6.7.2.1p18 forbids only a STRUCT member
// / an ARRAY element; gcc/clang accept the union form — sqlite's
// `union { SrcList sSrc; u8 srcSpace[N]; }`). So `union { struct Inner inner; }`
// is NOT an error and this corpus no longer pins it.
//
// What STAYS forbidden — and what this malformed-source corpus now pins — is an
// ARRAY of a FAM-struct as a union member: p18's "element of an array" bans it
// even inside a union. This is enforced UPSTREAM at array construction (the
// array-of-FAM element is rejected before the union carve-out runs), so it must
// still emit S_FlexibleArrayInAggregate. The diagnostic span points at the
// array-declarator `[3]` suffix (the "element of an array" violation site), not
// the field name `arr`.
struct Inner {
    int n;
    int d[];
};

union U {
    struct Inner arr[3];
    int x;
};
