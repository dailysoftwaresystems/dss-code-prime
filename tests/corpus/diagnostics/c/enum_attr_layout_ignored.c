// D-CSUBSET-ATTRIBUTE-TYPE-POSITION, the LOUDLY-IGNORED half. gcc 13.3.0 and
// clang 19.1.1 both ACCEPT `packed` / `aligned(N)` on an enum (MEASURED
// 2026-08-25), so DSS may not refuse them — but DSS's enum has exactly one
// layout channel, the C23 `enum E : T` underlying type, which neither attribute
// feeds. They are therefore accepted and reported IGNORED through the shared
// decl-kind gate rather than silently dropped, which is the failure the
// `compositeAttrLead` design note was written about. Before P34 both lines were
// error[P0001] — nothing here regresses.
enum __attribute__((packed)) P1 { B1 = 1 };
enum __attribute__((aligned(16))) P2 { B2 = 2 };
int main(void) {
    enum P1 p1 = B1;
    enum P2 p2 = B2;
    return (int)p1 + (int)p2;
}
