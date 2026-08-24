// FC3 c1 — the dataModel WIDTH differential, now a RUNTIME truncation witness.
//
// Under LLP64 (the pe64 windows format's declared dataModel), `long` is I32.
// 2147483648l: the l-suffixed decimal ladder climbs to `long long` (I64) since
// 2^31 does not fit I32. Passing that I64 literal to the I32 `long` parameter is
// an implicit same-signedness NARROWING (C 6.3.1.3, now admitted —
// D-CSUBSET-INT-SAME-SIGN-NARROW) that TRUNCATES to the low 32 bits: 0x80000000,
// which as a signed I32 `long` is NEGATIVE. So `r > 0l` is false → exit 7. The
// sibling datamodel_long_width/ (LP64, `long` = I64 holds the value) returns 42.
//
// THE dataModel runtime red-on-disable lever: stop threading the pe64 format's
// declared LLP64 into the pipeline (or mis-declare it LP64) and `long` widens to
// I64 → the literal fits → r > 0 → exit 42, and the manifest's exitCode 7 fails.

long pick(long v) {
    return v;
}

int main() {
    long r;
    r = pick(2147483648l);
    if (r > 0l) {
        return 42;
    }
    return 7;
}
