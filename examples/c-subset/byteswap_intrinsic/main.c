// D-CSUBSET-INTRINSIC-BSWAP: end-to-end runtime witness for the 6 byte-swap
// builtins — the MSVC family `_byteswap_{ushort,ulong,uint64}` (sqlite's pe path:
// sqliteInt.h's `#pragma intrinsic` + btreeInt.h:743 / util.c:1856,1869) and the
// GCC family `__builtin_bswap{16,32,64}`. All six are config-declared always-on
// builtins (c-subset.lang.json, `"lowering": "bswap"`) over ONE pure-unary
// MirOpcode::Bswap — NEVER a call/import: msvcrt.dll exports no `_byteswap_*`
// (MEASURED, objdump -p), so a mis-bound verb would produce a binary that links
// and then fails to LOAD (pe 0xC0000139).
//
// PER-TARGET, THIS ONE FILE WITNESSES BOTH REALIZATIONS OF `lowerBswap`:
//   x86-64 (pe64 + elf-x86_64): BSWAP r32 / BSWAP r64 native (0F C8+rd), and —
//     because x86 has NO width-16 BSWAP (architecturally undefined; GAS refuses
//     `bswap %ax`) — the universal-ALU byte-reversal EXPANSION for the two u16
//     cases. So the x86 arms are the runtime closure for the FALLBACK path.
//   arm64 (elf-aarch64 under qemu + macho-darwin): REV16 Wd,Wn / REV Wd,Wn /
//     REV Xd,Xn native at all three widths. So the arm64 arms are the runtime
//     closure for REV16 — the per-halfword instruction that no x86 arm can reach.
// One example, both halves of the capability split, per the audit's completeness
// rule.
//
// ★ ANTI-FOLD — AND THE RISK HERE IS THE OPPOSITE OF THE FENCE EXAMPLE'S.
// `__sync_synchronize`'s AtomicFence is side-effecting, so its risk was being
// DROPPED. Bswap is PURE (no side effect, no memory clobber — that purity is
// pinned in test_mir_opcode), so ITS risk is being FOLDED AWAY into a constant,
// which would leave the whole lowering unexercised while the exit code stayed 42.
// Two independent defences, both required:
//   (1) every operand is read from a MUTABLE STATIC GLOBAL assigned at runtime
//       (the sync_synchronize_fence idiom) — the baseline pipeline never sees a
//       literal at the call site;
//   (2) every builtin sits behind a WRAPPER FN taking the value as an ARG, so
//       when the `release` arm INLINES the wrappers into main the operand is
//       still a runtime value.
// ✔ VERIFIED that this is sufficient rather than merely hopeful: MirOpcode::Bswap
// is NOT in ConstFold's fold set — src/opt/passes/const_fold.cpp maps only
// Add/Sub/Mul/SDiv/SMod/And/Or/Xor/Shl/AShr/ICmp*/Neg/Not (+ the unsigned
// UDiv/UMod/LShr/ICmpU* group) to a HirOpKind, and an opcode with no mapping is
// never folded. So even a fully constant-propagated operand could not delete the
// op; defences (1) and (2) keep the OPERAND runtime as well, which is what makes
// the emitted instruction actually execute on the real CPU.
//
// ★ CASE 1 IS THE DIRTY-UPPER-BITS CASE, AND IT IS THE POINT OF THIS EXAMPLE.
// DSS narrows lazily: a u16 value may sit in a register with STALE bits 31:16,
// because narrowing realizes at the width-exact CONSUMER, not at the producer.
// `(u16)gDirty` with gDirty = 0xDEAD1234 leaves 0xDEAD1234 live in the input
// register while the VALUE is 0x1234. The x86 expansion must therefore mask each
// byte BEFORE placing it; the tempting two-instruction form `(x<<8)|(x>>8)` plus a
// trailing `& 0xFFFF` is WRONG and this case FAILS LOUDLY on it:
//     (0xDEAD1234<<8)|(0xDEAD1234>>8) = 0xADDE9912 → low 16 = 0x9912 ≠ 0x3412,
// so check 1 stops contributing and the exit code becomes 41, not 42.
// A fresh 16-bit load would NOT catch this (x86's movzx zero-extends, so the
// upper bits would happen to be clean) — hence the deliberately dirty source.
//
// WIDTH DISCRIMINATION (so a wrong-width lowering changes the exit code):
//   u16 : 0x1234              -> 0x3412
//   u32 : 0x11223344          -> 0x44332211      / 0xAABBCCDD -> 0xDDCCBBAA
//   u64 : 0x1122334400000000  -> 0x0000000044332211
//         0xFEDCBA9800000000  -> 0x0000000098BADCFE
// Both u64 inputs carry significant bits ONLY ABOVE bit 32, so a 64-bit swap
// lowered at 32 bits returns 0 in the low half and the check fails. Conversely a
// u16 swap lowered as a 32-bit REV (the arm64 trap REV16 exists to avoid) moves
// the input's bits 31:24 into the result's low byte — caught by case 1.
//
// The six checks carry DISTINCT weights summing to exactly 42, so the exit code
// names which one failed (every weight is positive, so any proper subset is < 42
// and 42 is reachable only when all six hold).
//
// RED-on-disable: drop any of the 6 rows from c-subset.lang.json's
// builtinFunctions -> S0001 undeclared at the call site (the EXACT pre-feature
// pe64 sqlite failure); strip x86_64's `bswap` opcode row -> every width takes
// the expansion and the exit stays 42 (the fallback is correct by construction —
// which is why NATIVE SELECTION is pinned at the LIR tier in test_mir_to_lir,
// not here).

typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

/* ANTI-FOLD (1): mutable static globals, assigned at runtime in main. */
static u32 gDirty;      /* 0xDEAD1234 — narrowed to u16 AT THE CALL SITE, so the
                           input register carries live garbage above bit 15 */
static u16 gClean16;
static u32 gMs32;
static u64 gMs64;
static u32 gGc32;
static u64 gGc64;

/* ANTI-FOLD (2): wrappers taking the value as an ARG — the operand survives the
   release pipeline's Inlining as a runtime value. */
static u16 ms16(u16 x) { return _byteswap_ushort(x); }
static u32 ms32(u32 x) { return _byteswap_ulong(x); }
static u64 ms64(u64 x) { return _byteswap_uint64(x); }
static u16 gc16(u16 x) { return __builtin_bswap16(x); }
static u32 gc32(u32 x) { return __builtin_bswap32(x); }
static u64 gc64(u64 x) { return __builtin_bswap64(x); }

int main(void) {
    int total = 0;

    gDirty   = 0xDEAD1234u;
    gClean16 = 0x1234;
    gMs32    = 0x11223344u;
    gMs64    = 0x1122334400000000ull;
    gGc32    = 0xAABBCCDDu;
    gGc64    = 0xFEDCBA9800000000ull;

    /* 1 — u16, MSVC spelling, DIRTY upper bits (x86 expansion / arm64 REV16). */
    if (ms16((u16)gDirty) == 0x3412) total += 1;
    /* 2 — u16, GCC spelling, clean input (same two realizations). */
    if (gc16(gClean16) == 0x3412) total += 2;
    /* 4 — u32, MSVC spelling (x86 BSWAP r32 / arm64 REV Wd,Wn). */
    if (ms32(gMs32) == 0x44332211u) total += 4;
    /* 8 — u32, GCC spelling. */
    if (gc32(gGc32) == 0xDDCCBBAAu) total += 8;
    /* 16 — u64, MSVC spelling (x86 REX.W BSWAP / arm64 REV Xd,Xn); the input's
       significant bytes are all ABOVE bit 32, so a 32-bit lowering yields 0. */
    if (ms64(gMs64) == 0x0000000044332211ull) total += 16;
    /* 11 — u64, GCC spelling, same above-bit-32 discrimination. */
    if (gc64(gGc64) == 0x0000000098BADCFEull) total += 11;

    return total;   /* 1 + 2 + 4 + 8 + 16 + 11 = 42 */
}
