// FC3 c1 — the dataModel POSITIVE arm (LP64: linux/darwin formats).
//
// Witnesses at RUNTIME that `long` is 64-bit under the LP64 formats:
// squaring 2^15 gives 2^30, and doubling THAT yields 2^31 —
// representable only in a 64-bit long (a 32-bit long would wrap
// negative and take the exit-7 path).
//
//   LP64 long (I64):  h = 2^31 > v = 2^30  → exit 42
//   32-bit long:      h wraps negative < v → exit 7
//
// Every literal fits the narrowest shipped immediate slot (arm64 movz
// imm16 ≤ 65535; the x86 mov has no imm64 form): 2^30 and 2^31 exist
// only in REGISTERS, built by runtime mul/add over function-arg inputs
// (the fold-resistant `division/` fixture discipline keeps the baseline
// arm honest; the optimized arm may fold — same exit).
//
// The DIAGNOSTIC half of the dataModel differential — the SAME `long`
// vocabulary REJECTING under the LLP64 (windows) format — is the
// sibling example `datamodel_long_width_llp64/` (one example per
// verdict: `expectDiagnostics` is a manifest-level field, so a
// per-target diagnostic arm needs its own manifest — honest split).
// The red-on-disable lever lives THERE: stop reading the pe64 format's
// dataModel and the windows compile silently succeeds, failing that
// manifest's expectDiagnostics assertion.

long doubleIt(long v) {
    long h;
    h = v + v;          // 2^30 + 2^30 = 2^31 — needs the 64-bit long
    if (h > v) {
        return 42l;     // I64: 2147483648 > 1073741824
    }
    return 7l;          // a 32-bit long would have wrapped negative
}

long square(long b) {
    return b * b;       // 32768^2 = 2^30 — built at runtime
}

int main() {
    long v;
    v = square(32768l);
    long r;
    r = doubleIt(v);
    if (r == 42l) {
        return 42;
    }
    return 7;
}
