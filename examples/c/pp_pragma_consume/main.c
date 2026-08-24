// ★★ TF-C82 (D-PP-PRAGMA-REGISTRY) — `#pragma` (C 6.10.6) + `_Pragma` (C 6.10.9)
// runtime witness. REWRITTEN from the FC15c version, which witnessed the
// opposite claim: that EVERY `#pragma` line is consumed-and-DROPPED with no
// error, citing C 6.10.6p2. The citation was right and the conclusion was wrong.
// 6.10.6p2 licenses IGNORING a pragma; it does not license reporting nothing
// about one that silently relayouts memory. MEASURED on macOS/arm64 with the
// sqlite harness's own recipe, the corpus REACHES 40 `#pragma pack` lines across
// 5 TUs, and `sys/fcntl.h`'s `#pragma pack(4)` region makes `struct log2phys` 20
// bytes / align 4 where DSS computed 24 / 8 — a wrong-ABI struct handed to a live
// `fcntl(F_LOG2PHYS)` syscall.
//
// So what this example witnesses now is the DISTINCTION:
//   (a) a pragma a `pragmaEffects` row CLAIMS is inert is still consumed and
//       dropped, from every position — file scope, inside a function, and via
//       the `_Pragma` operator INCLUDING from a macro replacement list; and
//   (b) `#pragma pack` REALLY APPLIES, with the numbers gated on the exit code.
//
// ★ THE SIZES ARE CLANG GROUND TRUTH, not DSS's opinion: this file compiles
// clean under `/usr/bin/clang -fsyntax-only -Wall -Wextra -isysroot $(xcrun
// --show-sdk-path)` and the clang-built binary independently exits 42.
//
// ★★ THE `log2phys` BLOCK IS THE CORPUS WITNESS, and it is VERBATIM from
// `$(xcrun --show-sdk-path)/usr/include/sys/fcntl.h:558-570` apart from `off_t`
// being spelled as its Darwin definition (`long long`) — that substitution
// sidesteps an UNRELATED gap (`<sys/cdefs.h>`'s `__asm("_" ...)` does not parse
// yet, so the real header cannot be included end to end) and changes nothing
// about the layout being measured. Under `pack(4)` it is 20/4; uncapped, 24/8.
// That single struct is why this cycle exists.
//
// RED-ON-DISABLE, per check, each flipping 42 -> its own exit number: drop the
// pack cap and 3/4/5 fire (both sizes and the offset); drop the `pack()` reset
// and 6 fires (the cap would leak past the region and shrink `Unpacked`); drop
// push/pop and 7/8 fire; break `_Pragma` routing and 9 fires; break the
// MACRO-BORNE `_Pragma` specifically — the halfway state where file-scope works
// and macro-borne silently does not — and 10 fires while 9 stays green. Make a
// registered-inert pragma loud instead of ignored and the compile fails
// outright, which is the (a) half.

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma mark ---- TF-C82 pragma registry witness ----

// (b) THE CORPUS WITNESS — sys/fcntl.h:558-570.
typedef long long dss_off_t;
#pragma pack(4)

struct log2phys {
	unsigned int    l2p_flags;
	dss_off_t       l2p_contigbytes;
	dss_off_t       l2p_devoffset;
};

#pragma pack()

// The RESET arm: identical members, now OUTSIDE the region. If `pack()` did not
// restore the default the two would agree and check 6 could not tell them apart.
struct Unpacked {
	unsigned int    l2p_flags;
	dss_off_t       l2p_contigbytes;
	dss_off_t       l2p_devoffset;
};

// The STACK arm — `pack(push, N)` / `pack(pop)`, MEASURED 6x in the corpus.
#pragma pack(push, 1)
struct PushedOne { char c; int i; };                 /* 5 / 1 */
#pragma pack(push, 4)
struct PushedFour { unsigned int a; long long b; };  /* 12 / 4 */
#pragma pack(pop)
struct StillOne { char c; int i; };                  /* still pack(1): 5 / 1 */
#pragma pack(pop)

// The `_Pragma` OPERATOR spelling — same registry, same state.
_Pragma("pack(4)")
struct ViaOperator { unsigned int a; long long b; };  /* 12 / 4 */
_Pragma("pack()")

// ★ THE HALFWAY-STATE DISCRIMINATOR: a `_Pragma` reached through a macro
// REPLACEMENT LIST. This is the `sys/queue.h` shape
// (`__NULLABILITY_COMPLETENESS_PUSH`, expanded at 40 use sites), and MEASURED it
// is how 24 of the corpus's reached pragmas arrive. Route `_Pragma` at the
// directive scan only and `ViaOperator` above stays correct while THIS one
// silently loses its cap.
#define DSS_PACK4()   _Pragma("pack(4)")
#define DSS_PACKEND() _Pragma("pack()")
DSS_PACK4()
struct ViaMacro { unsigned int a; long long b; };     /* 12 / 4 */
DSS_PACKEND()

// A global of the packed type, so the layout is exercised by real stores and
// loads rather than by `sizeof` alone. `g_breaker` keeps `g_probe` off a
// section-start address, where an alignment-derived check can hold whether or
// not the sink ran (the vacuity trap `gnu_attribute_packed_aligned` documents).
static char             g_breaker = 1;
static struct log2phys  g_probe;

int main(void) {
#pragma GCC diagnostic push
    // (a) INERT-BY-DECLARATION pragmas, from inside a function body.
#pragma clang diagnostic ignored "-Wunused-variable"
    if (g_breaker != 1) return 1;

    // The layout must survive a real round trip through memory.
    g_probe.l2p_flags       = 7;
    g_probe.l2p_contigbytes = 0x1122334455667788LL;
    g_probe.l2p_devoffset   = 0x7877665544332211LL;
    if (g_probe.l2p_flags != 7u) return 2;
    if (g_probe.l2p_contigbytes != 0x1122334455667788LL) return 2;
    if (g_probe.l2p_devoffset != 0x7877665544332211LL) return 2;

    // (b) THE NUMBERS. Each is clang ground truth for this exact source.
    if (sizeof(struct log2phys) != 20) return 3;
    if (_Alignof(struct log2phys) != 4) return 4;
    // The offset is the sharpest observable: uncapped, `l2p_contigbytes` sits at
    // byte 8; under `pack(4)` it sits at byte 4.
    if ((unsigned long)((char *)&g_probe.l2p_contigbytes - (char *)&g_probe) != 4u)
        return 5;
    // The `pack()` RESET arm: the same members, laid out with no cap.
    if (sizeof(struct Unpacked) != 24 || _Alignof(struct Unpacked) != 8) return 6;
    // The push/pop STACK arm.
    if (sizeof(struct PushedOne) != 5 || _Alignof(struct PushedOne) != 1) return 7;
    if (sizeof(struct PushedFour) != 12 || _Alignof(struct PushedFour) != 4) return 7;
    if (sizeof(struct StillOne) != 5 || _Alignof(struct StillOne) != 1) return 8;
    // The `_Pragma` spellings — file scope, then from a macro replacement list.
    if (sizeof(struct ViaOperator) != 12 || _Alignof(struct ViaOperator) != 4) return 9;
    if (sizeof(struct ViaMacro) != 12 || _Alignof(struct ViaMacro) != 4) return 10;

#pragma GCC diagnostic pop
    return 42;
}

#pragma GCC diagnostic pop
