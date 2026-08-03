// TF-C85 corpus witness: the THREE MSVC pragmas the pe64 sqlite leg reaches are
// CLAIMED by `preprocess.pragmaEffects` rows — and the one with a real sink,
// `#pragma optimize("", off)`, produces a CORRECT runnable program under the
// SHIPPED release optimizer on every target.
//
// ★ WHY THIS EXAMPLE EXISTS. TF-C82 made an unclaimed `#pragma` a hard error and
// built its row set from a census taken with `clang -E` ON macOS — an instrument
// that cannot see MSVC-gated code, because `_MSC_VER` is never defined under it.
// DSS's `pe` profile DOES define `_MSC_VER`. MEASURED: the pe64 sqlite build was
// broken by 2135 `error[P0020]` across 113 of 189 TUs — `warning` 1685,
// `intrinsic` 448, `optimize` 2 — for the entire life of that cycle.
//
// ★ THE PRAGMAS ARE SPELLED UNGUARDED HERE, DELIBERATELY. In sqlite they sit
// behind `#if defined(_MSC_VER)`; here they must be REACHED on all four targets,
// because the rows that claim them are LANGUAGE config, not target config. The
// profile-gating half — that the pe leg reaches them and the others elide them —
// is pinned separately by `Preprocessor.TfC85NoUnclaimedPragmaUnderAnyPredefine
// Class` and its non-vacuity twin over `tests/corpus/c-subset/
// pragma_profile_census.c`.

// ── 1. `warning` -> `diagnosticsOnly` ────────────────────────────────────────
// MSVC warning suppression has no translation semantics: it configures
// diagnostics of a compiler DSS is not, and DSS emits none of them. All four
// MEASURED-reached payload shapes appear, because one PREFIX row covers all four
// and the row makes no claim about the argument list.
#pragma warning(push)
#pragma warning(disable : 4127)
#pragma warning(disable : 4706)
#pragma warning(default : 4748)
#pragma warning(pop)

// ── 2. `intrinsic` -> `realizationRequestOnly` ───────────────────────────────
// ★★ THE POINT OF THE VERB, WITNESSED. `#pragma intrinsic` asks HOW a listed
// name is realized — inline expansion instead of a CRT call — and NEVER whether
// it exists. So this program NAMES four intrinsics and CALLS NONE of them, and
// that is not a gap in the witness, it IS the witness: DSS chooses realization
// itself, and the pragma changes nothing either way.
//
// Two of the four are honest about their asymmetry (MEASURED from
// `shippedLibs/intrin.json`): `_ReadWriteBarrier` IS realized here, as an
// always-on lowered builtin; the three `_byteswap_*` are NOT DECLARED AT ALL.
// The row is true of all four ANYWAY — because if this file DID reference
// `_byteswap_ulong` on a target that does not provide it, the REFERENCE would
// fail loud at the CALL SITE (S0001) whether or not the pragma was honored.
// MEASURED in the field: that is exactly what the pe64 sqlite leg now reports at
// `src/btree.c:23784` and `src/util.c:24880`, with `D-CSUBSET-INTRINSIC-BSWAP`
// as the open anchor. Ignoring the pragma cannot mask a missing symbol.
#pragma intrinsic(_byteswap_ushort)
#pragma intrinsic(_byteswap_ulong)
#pragma intrinsic(_byteswap_uint64)
#pragma intrinsic(_ReadWriteBarrier)

// ── 3. `optimize` -> `optimizerControl`, THE SINK ────────────────────────────
// ★ WHAT THIS ARM HONESTLY WITNESSES, AND WHAT IT DOES NOT. Every DSS optimizer
// pass is semantics-preserving, so NO exit code can distinguish "this function
// was optimized" from "it was not" — which is why `shielded` and `exposed` below
// have IDENTICAL bodies and the program asserts they AGREE. That agreement is
// not a tautology: `shielded` is rebuilt VERBATIM (the shared rebuilder swaps in
// an identity policy) while `exposed` goes through ConstFold / Mem2Reg /
// CopyProp / Cse / Licm / SimplifyCfg / Dce, so any defect in the neuter shows
// up here as a wrong exit code on four targets at once.
//
// ★★ AND THAT IS NOT HYPOTHETICAL — THE BODY IS SHAPED BY TWO REAL BUGS THIS
// CYCLE FOUND IN ITS OWN FIRST DRAFT, both MEASURED on the real pe64 corpus:
//   * the `for`/`if` diamond gives `shielded` PHI NODES. Neutering the policy
//     without telling `Mem2Reg` (which plans phis in `analyze`, emits them in a
//     hook, and wires them in a POST-rebuild step) aborted the whole build with
//     "phi marker has incomings but no emitted phi".
//   * the UNREACHABLE `return` gives it a dead BLOCK. Neutering
//     `PruneUnreachable` — which is MANDATORY NORMALIZATION, not optimization —
//     left that block alive and produced `I_UnreachableBlock` verifier errors.
// A `#pragma optimize` region switches optimization off. It has never meant
// "emit invalid IR", and this function is what holds that line.
//
// ★ IT IS NOT A FLOATING-POINT FIX, and this example will not imply otherwise.
// MSVC's own motivating use (sqlite's `ext/misc/totype.c`) targets x87 EXCESS
// PRECISION. MEASURED in this tree that hazard structurally cannot occur:
// const-fold's maps are integer-only, there is no reassociation pass, there is
// no FMA or fast-math anywhere, and `double` is SSE2 at exactly 64 bits. This
// sink is FAITHFULNESS to a real MSVC contract that also unblocks pe64.
#pragma optimize("", off)
static int shielded(int k) {
    int acc = 0;
    for (int i = 0; i < 4; ++i) {
        if ((i & 1) != 0) {
            acc += k * i;
        } else {
            acc -= i;
        }
    }
    if (acc > 1000000) {
        return -1;
    }
    return acc;
    return 0;   /* UNREACHABLE — a dead block for the normalize prune */
}
#pragma optimize("", on)

// The byte-identical body OUTSIDE the region: fully optimized. The two must
// agree, because optimization preserves semantics.
static int exposed(int k) {
    int acc = 0;
    for (int i = 0; i < 4; ++i) {
        if ((i & 1) != 0) {
            acc += k * i;
        } else {
            acc -= i;
        }
    }
    if (acc > 1000000) {
        return -1;
    }
    return acc;
    return 0;   /* UNREACHABLE */
}

// shielded(3): i=0 acc=0; i=1 acc=3; i=2 acc=1; i=3 acc=10.
#define EXPECTED 10

int main(void) {
    int const a = shielded(3);
    int const b = exposed(3);
    if (a != EXPECTED) return 1;
    if (b != EXPECTED) return 2;
    if (a != b)        return 3;
    return 42;
}
