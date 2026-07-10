// FC17.5 (D-CSUBSET-AUTO-TYPE-INFERENCE): C23 6.7.9 `auto` type inference,
// end-to-end. Every inference class the feature ships is exercised with a
// RUNTIME consequence (never just "it compiles"):
//   • width matters       — sizeof(short-inferred) == 2, sizeof(LL-inferred) == 8
//   • string literal      — decays to char*: sizeof == pointer size AND the
//                           pointer dereferences at runtime (str[1])
//   • array variable      — decays to int*: p[1] indexes the backing array
//   • function name       — decays to a function pointer; called through it
//   • braced single       — `auto b = {4};` (C23 6.7.10p12 single-expression)
//   • constexpr auto      — inferred int usable as an array dimension (ICE)
//   • static auto         — static storage duration: bump() accumulates
//   • for-init auto       — `for (auto i = 0; …)` (C23 6.8.5p3)
//   • Pass-1.5 visibility — `int arr[sizeof(x)]` dimensions from the
//                           inferred type AT THE SAME PASS (the backfill-only
//                           failure mode would S_NonConstantArrayLength here)
// argc-seeded so the release optimizer cannot fold the whole program away.
// exit = 1 + 2 + 8 + 1 + 2 + 4 + 6 + 4 + 3 + 9 + 4 - 2 = 42 (argc == 1).
static int twice(int v) { return v + v; }

static int bump(void) {
    static auto acc = 3;
    acc = acc + 1;
    return acc;
}

int main(int argc, char **argv) {
    auto n = argc;              // identifier initializer (runtime value): 1
    auto s = (short)1;          // sizeof(s) == 2
    auto ll = 1LL;              // sizeof(ll) == 8
    auto str = "abc";           // char*: sizeof(str) == 8, str[1] == 'b'
    int a[3];
    a[0] = 2; a[1] = 4; a[2] = 9;
    auto p = a;                 // int*: p[1] == 4
    auto f = twice;             // function pointer: f(3) == 6
    auto b = {4};               // braced single: 4
    constexpr auto K = 3;       // inferred int, compile-time constant
    int dim[K];
    dim[0] = bump();            // 4  (static auto persisted: 3 -> 4)
    dim[1] = bump();            // 5  (4 -> 5)
    dim[2] = 0;
    auto sum = 0;
    for (auto i = 0; i < K; i = i + 1) sum = sum + dim[i];   // 9
    auto x = 42;
    int arr[sizeof(x)];         // Pass-1.5 visibility: 4 ints
    arr[0] = (int)(sizeof(arr) / sizeof(int));               // 4
    return n + (int)sizeof(s) + (int)sizeof(ll)
         + (str[1] == 'b' ? 1 : 0) + (int)(sizeof(str) / 4)
         + p[1] + f(3) + b + K + sum + arr[0] - x / 21;
}
