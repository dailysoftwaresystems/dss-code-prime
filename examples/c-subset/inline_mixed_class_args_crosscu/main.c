// 2-TU merge witness for D-OPT-RELEASE-SYSV-MIXED-CLASS-REG-ARG-DROP: main (this
// CU) calls a mixed int+FP-class function DEFINED in a sibling CU (helper.c). At
// link the two CUs MERGE into one module, and the shipped `release` pipeline
// then optimizes the MERGED module — Inlining splices isum into main across the
// former CU boundary. This exercises the merge-site Arg re-encode
// (mir_merge.cpp) that a single-TU example cannot, then the inline's
// position-based actual mapping. isum(2,10.0,3,20.0,43.0) = (int)73 + 2 + 3 = 78.
extern int isum(int a, double x, int b, double y, double z);

int main(void) {
    return isum(2, 10.0, 3, 20.0, 43.0);
}
