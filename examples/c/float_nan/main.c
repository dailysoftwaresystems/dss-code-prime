// FC3.5 sweep-c2: the NaN witnesses for the
// D-COND-FLOAT-NAN-TRUTHINESS-FCMP adjudication (C 6.5.9 + IEEE 754).
// NaN is CONSTRUCTED at runtime — 0.0/0.0 through the new fdiv
// encodings (DIVSD / arm64 FDIV-D; this corpus is the encoding's
// first consumer) with the zero arriving as a function PARAMETER so
// no fold can pre-compute it (MIR ConstFold is int-only anyway).
//
// The discriminating matrix (each wrong mapping lands on a DISTINCT
// exit code, never 42):
//   nan != nan  → TRUE  (Une — the C 6.5.9 adjudication: `!=` is the
//                 negation of `==`, so NaN != x holds; the interim
//                 FCmpOne mapping would make this FALSE → t loses 1)
//   nan == nan  → false (Oeq; x86 sete ALONE would say TRUE — the
//                 missing setnp conjunct → +100)
//   nan <  1.0  → false (Olt swap-canonicalized to Fogt/seta; the
//                 naive setb would say TRUE on unordered → +100)
//   nan >= 1.0  → false (Foge/setae — CF=1 on unordered → false ✓;
//                 a wrong NZCV pick on arm64, e.g. integer-uge
//                 CS, would say TRUE → +100)
//   nan >  1.0  → false (Fogt; arm64 integer-ugt HI would say TRUE)
//   nan <= 1.0  → false (swapped Foge)
//   if (nan)    → TRUE  (truthiness = Ne(nan, 0.0) → Une — C: a NaN
//                 compares UNEQUAL to 0, so it is truthy; the One
//                 mapping would make `if (NaN)` silently false)
//   t = 1 + 41 = 42.
double makeNan(double z) { return z / z; }

int main() {
    double nan = makeNan(0.0);
    int t = 0;
    if (nan != nan) t = t + 1;
    if (nan == nan) t = t + 100;
    if (nan < 1.0)  t = t + 100;
    if (nan >= 1.0) t = t + 100;
    if (nan > 1.0)  t = t + 100;
    if (nan <= 1.0) t = t + 100;
    if (nan) t = t + 41;
    if (t == 42) return 42;
    return t + 200;
}
