// FC3 c2 — the UNSIGNED-INT (U32) sibling of `unsigned_division/`
// (D-CSUBSET-UDIV-RUNTIME-HIGH-BIT-PIN's original 0x80001003u literal
// shape; the U64 example carried the pin while U32 arithmetic was
// D-CSUBSET-32BIT-ALU-FORMS-gated — c2's 32-bit div family lifts it).
//
// A HIGH-BIT `unsigned int` dividend exercises the 32-bit UNSIGNED
// divide (x86: XOR EDX,EDX + 32-bit F7 /6 DIV; arm64: UDIV W-form):
//
//   n = a*(b*b) + c  =  32768*65536 + 4099  =  0x80001003 (bit 31 SET)
//   unsigned:  n / 3 = 715829249  (0x2AAAB001, sign bit CLEAR) -> exit 42
//   signed(!): n as int = -2147479549; /3 = -715826516
//              (sign bit SET after the same-width reinterpret)  -> exit 7
//
// The discriminator reinterprets the quotient as `int` (same-width
// Bitcast) and tests its sign — exit-DIVERGENT if the division were
// mis-routed through the signed pair (CDQ + 32-bit IDIV).
//
// **Encoding discipline**: every literal fits the narrowest shipped
// immediate slot (arm64 movz imm16 <= 65535); the high-bit dividend is
// BUILT at runtime (32768*65536 = 0x80000000 fits unsigned int
// exactly — no wrap before the witness divide).
//
// **Fold resistance**: seeds + divisor reach quotientTagU32 as
// FUNCTION ARGS — the baseline arm keeps a live runtime 32-bit UDiv.

unsigned int quotientTagU32(unsigned int a, unsigned int b,
                            unsigned int d) {
    unsigned int n;
    n = a * (b * b);    // 32768u * 65536u = 0x80000000 — bit 31 SET
    n = n + 4099u;      // 0x80001003 — the anchor's literal shape
    unsigned int q;
    q = n / d;          // MIR UDiv at U32 — THE witness
    int s;
    s = (int)q;         // same-width reinterpret (Bitcast)
    if (s < 0) {
        return 7u;      // a signed-routed quotient has bit 31 set
    }
    return 42u;
}

int main() {
    if (quotientTagU32(32768u, 256u, 3u) == 42u) {
        return 42;
    }
    return 7;
}
