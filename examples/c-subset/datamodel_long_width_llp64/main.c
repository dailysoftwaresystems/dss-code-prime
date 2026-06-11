// FC3 c1 — the dataModel DIAGNOSTIC arm (LLP64: the windows format).
//
// 2147483648l under LLP64: `long` is I32 (the pe64 format's declared
// dataModel) and 2^31 does not fit — the l-suffixed decimal ladder
// climbs to `long long` (I64). Passing that I64 literal to the I32
// `long` parameter is a narrowing mismatch → S_TypeMismatch, and the
// compile REJECTS. The SAME `long` vocabulary compiles AND runs under
// the LP64 formats (the sibling `datamodel_long_width/` example).
//
// THE dataModel red-on-disable lever: stop threading the pe64 format's
// declared LLP64 into analyze() (or mis-declare it LP64) and `long`
// silently widens to I64 — this compile then SUCCEEDS and the
// expectDiagnostics assertion below fails the harness.

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
