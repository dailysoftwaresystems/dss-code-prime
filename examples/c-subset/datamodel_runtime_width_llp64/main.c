// FC3 c2 — THE dataModel RUNTIME divergence witness
// (D-CSUBSET-32BIT-ALU-FORMS x D-LANG-PLATFORM-DEPENDENT-PRIMITIVE-WIDTH).
//
// This SOURCE is byte-identical in datamodel_runtime_width_llp64/ and
// datamodel_runtime_width_lp64/ — ONE C vocabulary, TWO defined runtime
// behaviors, selected by the target format's dataModel:
//
//   LLP64 (pe64 / windows):  unsigned long = U32
//     x = 0xFFFFFFFF;  y = x + 1  WRAPS to 0   ->  return 42
//   LP64 (elf / macho):      unsigned long = U64
//     x = 0xFFFFFFFF;  y = x + 1 = 0x100000000 ->  return 23
//
// (exitCode is a manifest-level field, so each verdict carries its own
// manifest — the c1 datamodel_long_width / _llp64 split discipline.)
//
// The `x + one` sum mixes `int` into `unsigned long`, so the SAME line
// also EXERCISES the divergent UAC conversion (LLP64 same-width
// Bitcast; LP64 I32 -> I64 SExt = x86 movsxd / arm64 SXTW — the
// c2-new arm64 opcode's end-to-end consumer). HONEST REACH
// (audit-residue sweep c2, D-AUDIT-WITNESS-STRENGTHENING): the
// instruction EXECUTES on every LP64 run, but the converted operand
// is 1, where SExt == ZExt — the conversion DIRECTION is runtime-
// invisible here; it is MIR/asm-pinned (the c2 threading pins). What
// this example RUNTIME-witnesses is the WIDTH divergence above. A
// sign-extension runtime witness (negative converted operand) belongs
// to a future conversions example.
//
// **Width red-on-disable (the runtime lever)**: force the width axis
// to 64 (or strip the 32-bit add variants) and the LLP64 arm computes
// 0x100000000 != 0 -> returns 23 -> its manifest (exitCode 42) goes RED.
//
// **Literal care**: 0xFFFFFFFF NEVER appears as a literal — built at
// runtime from imm16-safe seeds (65535ul, 256ul) through FUNCTION ARGS
// (fold-resistant); every intermediate fits `unsigned long` under BOTH
// models (a*(b*b) = 65535*65536 = 0xFFFF0000; +a = 0xFFFFFFFF — no
// wrap before the witness).

unsigned long widthTag(unsigned long a, unsigned long b, int one) {
    unsigned long x;
    x = a * (b * b) + a;    // 0xFFFFFFFF under both models
    unsigned long y;
    y = x + one;            // UAC: int->unsigned long (LLP64 Bitcast /
                            // LP64 SExt), then the width-decisive add
    if (y == 0ul) {
        return 42ul;        // LLP64: wrapped — unsigned long is U32
    }
    return 23ul;            // LP64: no wrap — unsigned long is U64
}

int main() {
    unsigned long r;
    r = widthTag(65535ul, 256ul, 1);
    if (r == 42ul) {
        return 42;
    }
    if (r == 23ul) {
        return 23;
    }
    return 7;
}
