#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "link/linker.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <span>
#include <string>

// Linker image file emission — plan 14 LK10 cycle 1 substrate.
//
// Bridges `link()`'s in-memory `LinkedImage::bytes` to a real
// on-disk artifact. The hermetic-acceptance gate (LK10 full) needs
// this so the build pipeline can produce a runnable `.exe` /
// `.o` / `.dylib` / `.wasm` / `.spv` file without shelling out to
// a system linker; cycle 1 lands the substrate, cycle 2 wires it
// into the driver's `compileFiles` / `compileProject` entry
// points + CLI argument routing (anchored at plan 14 §3 LK10 row).
//
// Namespace `dss::linker` — same namespace as `dss::linker::link()`
// (the linker entry-point function in `linker.hpp`). The D-LK9-2
// rename landed at LK10 cycle 2; both substrate functions now
// share this single namespace.
//
// The function deliberately leaves three concerns to the caller:
//   * Format/target selection — already encoded in the `Linked
//     Image::format` discriminator + the JSON the caller loaded.
//   * File-extension policy — `.exe` vs `.dll` vs `.o` etc. lives
//     in the artifact profile (plan 6) / driver layer, not here.
//     The caller hands us a fully-qualified path.
//   * Parent-directory creation — `std::filesystem::create_
//     directories` is the caller's responsibility. We fail loud
//     rather than silently creating arbitrary paths (a substrate
//     that silently mkdir's would mask config errors that ship
//     binaries to the wrong target dir).

namespace dss::linker {

// Write `image.bytes` to `path`, REPLACING any existing file with a new
// file identity (see `writeBytes` below — the artifact is staged in a
// sibling temp and renamed over the target; it is never truncated in
// place). Returns `true` iff the bytes landed on disk. On failure, emits
// one of seven remediation-distinct K_* codes into `reporter` and
// returns `false`:
//   * `K_ArtifactWithheldAfterError` — the compilation had already
//                                    recorded an error (the artifact
//                                    invariant; enforced in `writeBytes`
//                                    below, which every image write
//                                    passes through)
//   * `K_ImageNotOk`               — `image.ok() == false`
//   * `K_ImageEmpty`               — `ok() == true` but bytes empty
//   * `K_ImageWriteParentMissing`  — parent dir absent
//   * `K_ImageWriteOpenFailed`     — no filename component, temp-create
//                                    failbit, or the commit rename failed
//                                    (after any HOLDER of the target had
//                                    `detail::kCommitHolderWait` to let go)
//   * `K_ImageWriteShort`          — write() mid-stream failbit
//   * `K_ImageWriteCloseFailed`    — close() flush failbit
//
// Three preconditions enforced:
//   * `image.ok()` — parallel-index gate. Writing a half-built
//     image would silently produce a corrupt artifact whose
//     `expectedFuncCount != resolvedFuncCount`; we reject at the
//     write surface.
//   * `!image.bytes.empty()` — every shipping format produces at
//     least a header (8 bytes for WASM, 20 for SPIR-V, 64+ for
//     ELF/PE/MachO). Empty bytes are a substrate failure.
//   * `path.parent_path()` exists (or `path` is in the current
//     working directory).
//
// The format discriminator on `image.format` is NOT consulted to
// pick a file extension — the caller fully owns the path. This
// keeps the substrate format-blind in the same shape as the
// rest of plan 14's substrate.
//
// `executable` (D-OUTPUT-EXEC-BIT): when true, the written file is
// marked executable (POSIX — adds owner/group/others exec via
// std::filesystem::permissions) so a produced binary runs directly
// (`./out`) without a manual `chmod +x`. The CALLER decides this from
// CONFIG — `ObjectFormatSchema::isImageFlavor()` (an exec/image-flavor
// output), never an arch/format identity branch — so `writeImage` stays
// format-blind (it switches on a bool, not the format). A no-op on
// Windows (PE ignores Unix modes). Default false: a plain artifact
// (.o / non-image output) is written at the host umask, unchanged. If
// the exec-bit add fails (rare — e.g. a read-only FS) a WARNING
// `K_ImageExecBitFailed` surfaces but the call still returns true: the
// bytes are valid, the exec bit a best-effort convenience (distinct
// from the byte-integrity `K_ImageWrite*` errors).
[[nodiscard]] DSS_EXPORT bool
writeImage(LinkedImage const&             image,
           std::filesystem::path const&   path,
           DiagnosticReporter&            reporter,
           bool                           executable = false);

// Commit a raw byte buffer to `path`. The byte-integrity core `writeImage`
// delegates to AFTER its LinkedImage-shape preconditions -- factored out so
// other artifact producers that are NOT a `LinkedImage` (the c163 `ar`
// static-archive writer -- an archive has no `ok()`/function-count contract)
// reuse the SAME fail-loud discipline.
//
// ★★★ THE ARTIFACT INVARIANT LIVES HERE: A COMPILATION THAT RECORDED AN ERROR
// COMMITS NO ARTIFACT. Because this is the ONE place any artifact byte in this
// compiler reaches disk, the FIRST thing it asks is whether `reporter` already
// holds an error; if so it emits `K_ArtifactWithheldAfterError` at INFO
// severity (a consequence, never a second fault -- see the code's own note)
// and returns false WITHOUT touching `path`. The reporter a real build hands
// down is `runCusToTargets`'s fresh per-target scratch, and that same
// `hasErrors()` decides the target's exit code, so the predicate that says the
// build failed and the predicate that withholds the file are ONE predicate.
// The gate can only ever fire on a run whose exit code is already 1, so it
// cannot turn a passing build red -- it changes what a FAILING build leaves on
// disk. A caller that needs to commit bytes irrespective of compile outcome
// must use a reporter that is not the compilation's, and should say why.
//
// Returns true iff every byte landed on
// disk; on failure emits one of the same four remediation-distinct K_* codes
// into `reporter` (plus `K_ArtifactWithheldAfterError` above):
//   * `K_ImageWriteParentMissing`  -- parent dir absent
//   * `K_ImageWriteOpenFailed`     -- path has no filename component, the
//                                     staging temp could not be created, or
//                                     the commit rename failed (see
//                                     `detail::commitReplacing` for what a
//                                     holder of the target gets first)
//   * `K_ImageWriteShort`          -- write() mid-stream failbit
//   * `K_ImageWriteCloseFailed`    -- close() flush failbit
//
// NON-DESTRUCTIVE ON EVERY REFUSAL. Whichever precondition fails -- the
// artifact invariant above, an empty filename, a missing parent, or a mid-write
// error -- a file already at `path` is left BYTE-INTACT. This compiler never
// destroys an artifact it did not write; it only ever declines to write a new
// one. ⓘ That is a MEASURED divergence from gcc 13.3.0 / clang 18.1.3 / MSVC
// 19.51 for a failed LINK specifically (all three unlink the previous output
// there; all three LEAVE it byte-identical when the FRONT END fails), and it is
// deliberate: a compiler that deletes a working binary on a build it aborted
// before opening the file destroys something it cannot give back.
//
// ATOMIC REPLACE, NEW IDENTITY (D-LK-WRITER-TRUNCATES-INSTEAD-OF-RENAMING).
// The bytes are staged in a uniquely-named SIBLING temp (`<path>.dsstmp-*`,
// claimed with an exclusive create -- `detail::createExclusiveBinary` below;
// NOT `std::ios::noreplace`, which is rejected outright for a
// `std::filesystem::path` on MinGW/libstdc++ --
// D-LINK-WRITER-NOREPLACE-WIDE-PATH-UNSUPPORTED) and then renamed over `path`. The
// artifact is therefore never truncated in place, and every write produces a
// NEW file identity -- required because macOS attaches an exec DENY to a
// REUSED inode, killing a rebuilt binary at that inode (exit 137) while a
// byte-identical copy runs. Consequences callers should know: `path` appears
// with complete bytes or not at all (a failed write leaves the PREVIOUS
// artifact intact and untouched); the new file carries fresh permission bits
// at the host umask rather than inheriting the old file's; and if `path` is a
// symlink or hard link the rename replaces THAT NAME rather than writing
// through to the shared file. Reasoning and the cross-platform determination
// are in writer.cpp at the fix site. The rename itself is
// `detail::commitReplacing` below: on POSIX exactly `rename(2)`; on Windows a
// POSIX-semantics replace, so another process holding the previous artifact
// open (✔ sharing delete; 🧠 a real-time scan) no longer refuses the commit
// ([[D-LINK-WRITER-RENAME-OVER-FAILS-WHILE-ANOTHER-PROCESS-HOLDS-THE-FILE]]).
// Does NOT gate on `bytes.empty()` (that is a LinkedImage-specific contract --
// an empty write is a legitimate raw-bytes request) and never sets the POSIX
// execute bit (an archive / relocatable output is not executable; `writeImage`
// owns the exec-bit step for image-flavor outputs). Parent-directory creation
// stays the caller's responsibility (the substrate never auto-mkdir's).
[[nodiscard]] DSS_EXPORT bool
writeBytes(std::span<std::uint8_t const> bytes,
           std::filesystem::path const&  path,
           DiagnosticReporter&           reporter);

// ── Internal seam ──────────────────────────────────────────────────────────
//
// NOT part of the substrate's contract; declared here only so the claim
// behaviour `writeBytes` cannot expose through its own surface can be pinned
// directly — the exclusive-claim primitive itself, and the attempt cap that
// makes its exhaustion arm reachable. Same shape as the tree's other
// independently-testable sub-builders
// (`dss::macho::detail::buildAdHocCodeSignature`,
// `dss::link::format::detail::writeU32LEAt`): the primitive is defined in the
// .cpp and called by nothing outside it; both are exercised by
// `tests/link/test_link_writer_exclusive_claim.cpp`.
namespace detail {

// How many staging-temp candidates `writeBytes` will try before it gives up
// and fails loud. THE SINGLE SOURCE OF TRUTH for that cap: `writeBytes` reads
// it, and a test that drives the exhaustion arm must READ IT TOO rather than
// hand-copy the literal — which is why an implementation number is published
// through this seam at all. The exhaustion arm is only reachable by occupying
// exactly this many consecutive slots, so a copied literal goes silently
// VACUOUS the moment the cap is LOWERED here: the test would occupy more slots
// than the writer ever tries, still see the exhaustion failure it expects, and
// then advance its derived slot counter further than the writer advanced its
// own — leaving every later decoy off the writer's candidate list, where it
// survives for the wrong reason and pins nothing.
inline constexpr std::uint32_t kMaxClaimAttempts = 1000;

// Create `p` and open it for binary write EXCLUSIVELY: the call succeeds ONLY
// if it was the one that created the file, and returns NULL for every other
// outcome INCLUDING "the path already exists" -- i.e. O_CREAT|O_EXCL, exactly
// what C++23's `std::ios::noreplace` is specified to mean, taken from the layer
// that actually implements it (D-LINK-WRITER-NOREPLACE-WIDE-PATH-UNSUPPORTED).
// The two arms reach that ONE semantic by DIFFERENT primitives and the wording
// must not imply otherwise: Windows uses `_wfopen(…, L"wbxN")` (the C11 "x"
// mode), POSIX uses `open(O_CREAT|O_EXCL|O_CLOEXEC)` + `fdopen` and never calls
// `fopen` at all. See writer.cpp for why that asymmetry is deliberate.
//
// The trailing `N` / `O_CLOEXEC` are a SECOND, independent guard and not part
// of the exclusivity: they keep the claimed handle from being inherited by a
// process the compiler spawns (`core/substrate/process_spawn.cpp`), which on
// Windows would otherwise make the commit rename fail with
// ERROR_SHARING_VIOLATION and on POSIX would hand a build hook a writable
// descriptor onto the staged artifact. Both measurements are at the definition
// site; the pin is `AClaimedStagingTempNeverCrossesIntoASpawnedChild`.
//
// That exclusivity is load-bearing twice over, and neither consequence is
// observable through `writeBytes`: it makes the staging-temp claim race-free
// against a concurrent process, and it makes a stale `.dsstmp-*` left by a
// killed run get STEPPED OVER rather than silently shared and truncated (the
// `tests/test_support/scratch_dir.hpp` lesson). An existing file is left
// BYTE-INTACT by a refused claim -- never truncated. Reasoning, the measured
// three-way path-type differential, and the host split are at the definition
// site in writer.cpp.
//
// Caller owns the returned stream and must `std::fclose` it.
[[nodiscard]] DSS_EXPORT std::FILE*
createExclusiveBinary(std::filesystem::path const& p);

// ── THE COMMIT: rename a staged file over its target ───────────────────────
//
// [[D-LINK-WRITER-RENAME-OVER-FAILS-WHILE-ANOTHER-PROCESS-HOLDS-THE-FILE]]
//
// ONE OWNER for every writer in this tree that stages a file and renames it
// over a target — `writeBytes` above, the runtime object cache's store and the
// dependency lockfile — so the Windows half is decided once. Measurements and
// reasoning are at the definition in writer.cpp; the contract is:
//
//   * POSIX: exactly `rename(2)`. It never consults another open descriptor,
//     so it has no holder to wait for and `patience` is never asked.
//   * Windows: the replace is made with POSIX SEMANTICS, so a holder that
//     shares delete access — ✔ as the one that failed the round-7 gate does
//     (🧠 a real-time scan) — cannot refuse it: the name moves to the staged file
//     and the holder keeps reading the one it opened. A volume that cannot do
//     that (measured: a WSL 9P share answers 87) gets the classic replace. A
//     holder that STILL refuses — one withholding FILE_SHARE_DELETE, a running
//     program's image, any holder under the classic replace — is offered to
//     `patience` after every refusal; the commit tries again while it answers
//     true and refuses, naming the holders, when it answers false. A refusal
//     waiting cannot change (a directory, a read-only target, anything else)
//     is returned at once, without asking.
//
// `staged` is never deleted here: on a refusal it is still on disk and its
// cleanup belongs to the caller, who owns its name.

// How long the production commit keeps asking a HOLDER to let go before it
// refuses — the patience of the overload that takes none. THE SINGLE SOURCE OF
// TRUTH: a test that exercises the cap reads it, never copies it.
//
// A CAP, NOT A RESERVATION: the commit returns the moment a replace lands, so a
// healthy build never spends it; only a holder that never lets go does, and
// then the refusal names it. SIZED FROM THE MEASUREMENT, per the sizing rule the
// test tier's `test_wait_budget.hpp` states for every budget in this repository:
//   * ✔MEASURED 2026-09-18, the holder that failed the gate, with nothing else
//     touching the file and the CPU at 99–100% on 32 threads (four other lanes
//     building): 68 holds in 20000 classic replaces, 2.9 ms min, 10.8 ms
//     median, 21.5 ms max. 2000 ms is ~93x the longest — room for a host far
//     slower than the one that measured it, which is the host a cap is for.
//   * 📄 and it is the one allowance a reference makes for this kind of
//     holder: LLVM's `sys::fs::rename` retries opening its SOURCE 200 x
//     `Sleep(10)` "to defeat badly behaved file system scanners"
//     (`llvm/lib/Support/Windows/Path.inc`, 19.1.5).
//     No reference's OUTPUT commit waits at all (✔MEASURED: MSVC link.exe,
//     lld-link and GNU ld each return within 342 ms against a 3 s holder), so
//     waiting is where DSS goes past the bar, never below it.
// ⚠ Under POSIX semantics that measured holder never reaches this wait; it
// governs the holders that do — see the list above.
inline constexpr std::chrono::milliseconds kCommitHolderWait{2000};

// What one commit did.
struct CommitOutcome {
    // The staged file now IS the target.
    bool committed = false;
    // How many attempts a HOLDER refused before the outcome — every one of them
    // was offered to the patience. Always 0 on POSIX.
    std::uint32_t holderRefusals = 0;
    // Empty when committed. Otherwise the whole cause, ready to follow a
    // writer's own "could not rename … over '<target>': " — the host error BY
    // NUMBER first (the text is localized; the number is what a report can be
    // searched on), then how often and for how long a holder refused, which
    // replace semantics were in force, and who holds the file.
    std::string refusal;
};

// Asked after each refusal the commit attributes to a HOLDER, with the running
// count (1-based): `true` = try again (having waited as long as the caller
// wants), `false` = give up and refuse.
using CommitPatience = std::function<bool(std::uint32_t holderRefusals)>;

// The production commit: waits up to `kCommitHolderWait` for a holder.
[[nodiscard]] DSS_EXPORT CommitOutcome
commitReplacing(std::filesystem::path const& staged,
                std::filesystem::path const& target);

// The same commit with the caller's patience — the seam a test uses to release
// its holder at the moment the commit has been refused, by handshake rather
// than by a timer.
[[nodiscard]] DSS_EXPORT CommitOutcome
commitReplacing(std::filesystem::path const& staged,
                std::filesystem::path const& target,
                CommitPatience const&        patience);

} // namespace detail

} // namespace dss::linker
