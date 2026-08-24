/* TF-C85 — THE PROFILE-CENSUS FIXTURE.
 *
 * Preprocessed by `Preprocessor.TfC85NoUnclaimedPragmaUnderAnyPredefineClass`
 * under EVERY predefine class (pe / macho / elf / no-active-format), asserting
 * that no pragma it reaches goes unclaimed by `preprocess.pragmaEffects`.
 *
 * ★ WHY IT EXISTS. TF-C82 built the pragma registry from a REACHED census taken
 * with `clang -E` on macOS, and wrote the result up as universal. It was not:
 * `clang -E` on macOS never defines `_MSC_VER`, so the census was structurally
 * blind to every MSVC-gated pragma — and DSS's `pe` profile DOES define
 * `_MSC_VER`. The pe64 sqlite leg was broken by 2135 `error[P0020]` across 113
 * of 189 TUs for exactly that reason. A single-profile census cannot see this;
 * three passes over one fixture can.
 *
 * ★ THE VOCABULARY BELOW IS MEASURED, NOT INVENTED. Each pragma here is one this
 * project has actually observed reached in the sqlite corpus, under the profile
 * whose guard it sits behind. Do not add speculative pragmas: a fixture that
 * enumerates what someone imagined is a census of imagination.
 *
 * ★ KEEP THE MSVC ARM BEHIND A pe-ONLY PREDEFINE. The point is that the pe leg
 * REACHES it and the others ELIDE it (C 6.10p1). The non-vacuity twin test
 * asserts both halves, so flattening the guard here would silently convert this
 * from a profile census into a single-profile one — the very defect it exists
 * to prevent.
 *
 * ★★ THE GUARD MOVED FROM `_MSC_VER` TO `_WIN32`, AND THE ASSERTION DID NOT
 * WEAKEN (D-LANG-PE64-DEFINES-BOTH-MSC-VER-AND-GNUC). DSS used to predefine
 * `_MSC_VER` for `pe` alongside the un-gated `__GNUC__`/`__clang__` — an
 * identity ✔MEASURED to exist in no reference compiler (clang suppresses
 * `__GNUC__` in MS-compatibility mode; mingw never defines `_MSC_VER`), and the
 * reason pe64 alone died on sqlite's `src/hwtime.h`. `_MSC_VER` is gone, so a
 * guard spelled on it is now dead on EVERY leg and this census would have
 * quietly become the single-profile one it exists to prevent.
 * What this arm needs is A PREDEFINE THAT IS LIVE ON pe AND DEAD ELSEWHERE, and
 * `_WIN32` is exactly that — and is the HONEST one, because it names the OS
 * rather than a toolchain DSS is not. Same two legs, same two assertions, same
 * stamping witness; only the spelling of the pe-ness changed. The pragmas below
 * are still the MSVC vocabulary and are still reached only on the Windows leg,
 * which is the property the census actually measures. (In sqlite these lines sit
 * behind `_MSC_VER` and are therefore no longer reached by the real corpus at
 * all — that is a CONSEQUENCE of the identity fix, not a gap here: this fixture
 * exists to prove the REGISTRY answers under every predefine class, and a row
 * whose pragma the corpus stops reaching must still answer if it is ever met.)
 */

/* ── Universal: reached on every leg. ─────────────────────────────────────── */
#pragma pack(push, 4)
struct census_packed { char c; int i; };
#pragma pack(pop)

#pragma pack(4)
struct census_packed_flat { char c; int i; };
#pragma pack()

/* ── clang/gcc vocabulary. Guard-free on purpose: these are not profile-gated
 *    in the SDK either, and they must stay claimed on every leg. ──────────── */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic pop
#pragma GCC diagnostic push
#pragma GCC diagnostic pop
#pragma mark - census

/* The `_Pragma` spelling of the same registry — reached through a macro
 * REPLACEMENT LIST, which is how 24 of the corpus's macho-leg pragmas arrive
 * (`sys/queue.h`'s `__NULLABILITY_COMPLETENESS_PUSH`). */
#define CENSUS_NN_PUSH _Pragma("clang assume_nonnull begin")
#define CENSUS_NN_POP  _Pragma("clang assume_nonnull end")
CENSUS_NN_PUSH
int census_nullability_scoped(void);
CENSUS_NN_POP

/* ── MSVC-gated: LIVE on the `pe` leg, an elided dead branch everywhere else.
 *    These are the three prefixes TF-C82's macOS-only census could not see. ─ */
#if defined(_WIN32)

/* sqlite `src/msvc.h` — MEASURED 15 lines x 112 TUs = 1680 hits. */
#pragma warning(disable : 4054)
#pragma warning(disable : 4055)
#pragma warning(disable : 4100)
#pragma warning(disable : 4127)
#pragma warning(disable : 4706)

/* sqlite `src/mutex_w32.c` — the push/disable/pop trio. */
#pragma warning(push)
#pragma warning(disable : 4324)
#pragma warning(pop)

/* sqlite `ext/misc/totype.c` — the `default : N` shape. */
#pragma warning(default : 4748)

/* sqlite `src/sqliteInt.h` — MEASURED 4 lines x 112 TUs = 448 hits. */
#pragma intrinsic(_byteswap_ushort)
#pragma intrinsic(_byteswap_ulong)
#pragma intrinsic(_byteswap_uint64)
#pragma intrinsic(_ReadWriteBarrier)

/* sqlite `ext/misc/totype.c` — the `#pragma optimize` region. `msvc_no_optimize_marker` is
 * what the non-vacuity twin looks up: it must be STAMPED on the pe leg, proving
 * this arm was reached rather than skipped. */
#pragma optimize("", off)
int msvc_no_optimize_marker(void);
#pragma optimize("", on)

#endif /* _WIN32 */

int census_anchor = 1;
