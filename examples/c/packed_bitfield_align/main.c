// D-CSUBSET-PACKED-BITFIELD-INTERACTION (TF-C97): `#pragma pack(N)` must apply to a struct that CONTAINS a bit-field, exactly as it does to one that does not.
//
// The defect this pins was pure silence: the pack cap was read only on the
// non-bit-field layout path, so adding one `unsigned b:1;` to a capped struct made
// the compiler forget the cap — no diagnostic, just a struct laid out to the wrong
// ABI. MEASURED against `/usr/bin/clang -arch arm64` (and cross-checked -arch
// x86_64, which agrees on every value below): `struct A` was 16/8 instead of 12/4,
// and `struct B` was 4/4 instead of 3/1.
//
// Alignment is pinned alongside size on purpose. All three shapes below happened to
// be wrong in BOTH before the fix, but the real-world instances are alignment-ONLY:
// the macOS SDK's `mach/message.h` `pack(4)` descriptors come out 16 bytes with the
// cap and 16 without — identical `sizeof`, `_Alignof` 4 vs 8. A size-only witness
// waves those through while the ABI is still wrong, and the damage only appears once
// the struct is put in an array or embedded in another.
//
// Every value here is data-model INDEPENDENT — fixed-width members only, no pointer
// or `long` member — so one exit code covers every target this runs on.
// exit = stride(12) + sizeof B(3) + _Alignof A(4) + offsetof C.a(4)
//      + sizeof C(12) + _Alignof B(1) + _Alignof C(4) + 2 = 42.
//
// GNU-ABI targets only (ELF x86_64 / ELF aarch64 / Mach-O arm64); the PE leg is
// deliberately absent. `#pragma pack` + bit-fields is exactly where the two shipped
// bit-field strategies DISAGREE, and `struct B` is a case that does: gnu_packed
// flows the 9-bit field into the byte after `c` (size 3), while msvc_straddle gives
// the field its own full 4-byte declared-type unit at offset 1 (size 5). Both are
// right for their ABI, so this cannot be one exit code. `struct A` and `struct C`
// happen to agree at 12/4 under both. The msvc_straddle cap is implemented from
// MSVC's documented `pack(n)` semantics but is NOT cl.exe-measured in this repo, so
// pinning a PE number here would be pinning a guess — the failure mode the house
// rule about hand-derived expectations exists to prevent.

#pragma pack(4)
struct A { unsigned long long a; unsigned b:1; };   // cap 4 beats the u64's natural 8
#pragma pack()

#pragma pack(1)
struct B { char c; unsigned b:9; };                 // the strongest cap: no padding at all
#pragma pack()

#pragma pack(4)
struct C { unsigned b:3; unsigned long long a; };   // bit-field FIRST, then a capped field
#pragma pack()

// Compile-time witnesses: size AND alignment, for all three shapes.
_Static_assert(sizeof(struct A)  == 12, "pack(4) must cap the u64 member: 12, not 16");
_Static_assert(_Alignof(struct A) == 4, "pack(4) must cap the struct alignment: 4, not 8");
_Static_assert(sizeof(struct B)  ==  3, "pack(1) must remove all padding: 3, not 4");
_Static_assert(_Alignof(struct B) == 1, "pack(1) must give alignment 1, not 4");
_Static_assert(sizeof(struct C)  == 12, "the cap must reach a field that FOLLOWS a bit-field: 12, not 16");
_Static_assert(_Alignof(struct C) == 4, "...and the alignment with it: 4, not 8");

static struct A arr[3];

// `volatile` is load-bearing: it stops the index from being folded away, so
// `&arr[idx]` must be computed at RUNTIME as `arr + idx * sizeof(struct A)`. That
// makes the stride below a real address computation carrying the laid-out size into
// generated CODE — not a `sizeof` the constant-folder answers on its own.
//
// Why that matters when the `_Static_assert`s above already pin the same numbers:
// they only pin what the FRONT END folded. The stride is the independent check that
// the same layout reached the back end's address arithmetic, so a layout that folds
// right but lowers wrong still fails here. It is also the half that survives if the
// asserts are ever weakened. Under the pre-fix layout `struct A` is 16 bytes, so the
// scaled index lands 16 bytes along and this term alone shifts the exit code by 4.
static volatile int idx = 1;

int main(void) {
    int stride = (int)((char *)&arr[idx] - (char *)&arr[0]);   // 12, computed at runtime

    // The stride is only trustworthy if neighbouring elements really are disjoint:
    // write through element 1 and confirm element 0 is untouched. An overlapping
    // (under-sized) stride would corrupt the neighbour and change the exit code.
    arr[0].a = 0;
    arr[0].b = 0;
    arr[idx].a = ~0ull;
    arr[idx].b = 1;
    if (arr[0].a != 0 || arr[0].b != 0) return 1;   // neighbour clobbered
    if (arr[idx].b != 1) return 2;                  // bit-field placement wrong

    int oc = (int)(long long)&((struct C *)0)->a;   // offsetof(C, a) == 4, capped

    return stride + (int)sizeof(struct B) + (int)_Alignof(struct A) + oc
         + (int)sizeof(struct C) + (int)_Alignof(struct B) + (int)_Alignof(struct C)
         + 2;                                       // 12+3+4+4+12+1+4+2 = 42
}
