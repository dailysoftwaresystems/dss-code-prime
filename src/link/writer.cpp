#include "link/writer.hpp"

#include "core/types/parse_diagnostic.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>

// HOST facilities, not target ones: `getpid` seeds the unique temp filename
// (see `processSeed` below), and on POSIX `open`/`O_EXCL` performs the atomic
// exclusive claim (see `detail::createExclusiveBinary`). Same include dance as
// `tests/test_support/scratch_dir.hpp`, which established this precedent.
#ifdef _WIN32
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

// Linker image file emission — plan 14 LK10 cycle 1 substrate
// implementation. See writer.hpp for the contract.

namespace dss::linker {

namespace {

void emit(DiagnosticReporter& reporter,
          DiagnosticCode code,
          std::string msg) {
    dss::report(reporter, code,
                          DiagnosticSeverity::Error,
                          std::move(msg));
}

// Per-process seed for the temp filename. `getpid()` discriminates
// CONCURRENT PROCESSES (two `dss` invocations emitting into one output
// dir); the atomic counter in `writeBytes` discriminates concurrent
// writes WITHIN one process.
//
// Neither is a guarantee on its own, and this is the exact correction
// `tests/test_support/scratch_dir.hpp` records for
// D-TEST-EXAMPLES-RUNNER-PARALLEL-CONTENTION-FLAKE: pids RECYCLE, and a
// killed run can leave a stale artifact at any (pid, counter) pair — 55
// stale scratch dirs were being silently SHARED because the code claimed
// its slot with a call that reports success when the slot already exists.
// So the pid+counter pair here is only the SEED. The claim itself is made
// ATOMICALLY by `createExclusiveBinary` below (`fopen` mode "x", i.e.
// O_CREAT|O_EXCL), which succeeds only when THIS call created the file; a
// slot we did not create is stepped over, never reused.
[[nodiscard]] std::uint64_t processSeed() {
#ifdef _WIN32
    return static_cast<std::uint64_t>(_getpid());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

} // namespace

// `createExclusiveBinary` lives in `detail` rather than the anonymous
// namespace ABOVE for exactly one reason: the EXCLUSIVITY it provides is the
// whole point of the claim loop below, and nothing that goes through
// `writeBytes` can observe it directly — the claim's candidate name embeds a
// pid and a process-wide counter, so a test that merely pre-creates a name and
// watches it survive cannot tell "stepped over" from "never considered".
// (Only exhausting every one of the `kMaxClaimAttempts` slots can, which is
// what the integration arm of that test does — at a cost of a thousand files.
// The property itself deserves a direct pin.) Declared in `writer.hpp` under the
// same `detail` sub-namespace convention this tree already uses for
// independently-testable sub-builders (`dss::macho::detail::
// buildAdHocCodeSignature`, `dss::link::format::detail::writeU32LEAt`), and
// pinned directly by `tests/link/test_link_writer_exclusive_claim.cpp`:
// a fresh path opens, an EXISTING path is REFUSED (null) and left byte-intact.
// RED ON DISABLE, stated PER ARM because the two arms are genuinely different
// code and one measurement cannot cover both (the TF-C104 lesson recurring):
// on Windows, swap `L"wbxN"` for `L"wbN"`; on POSIX, drop `O_EXCL` from the
// `open` flags. Either one turns that second pin red.
namespace detail {

// Create `p` and open it for binary write EXCLUSIVELY — the call succeeds
// only if it was the one that created the file. Returns null on ANY failure,
// including the "someone already holds this name" case; the caller
// disambiguates the two (see the claim loop in `writeBytes`).
//
// D-LINK-WRITER-NOREPLACE-WIDE-PATH-UNSUPPORTED — this reaches straight for
// the CRT primitive instead of `std::ios::noreplace` (C++23 P2467R1), which
// is the identical request expressed one layer up, because the iostreams
// layer does not honour it for the argument type we pass. MEASURED on the
// build compiler (MinGW-W64 UCRT g++ 13.2.0, `-std=c++23`), same directory,
// same openmode `binary|out|noreplace`, varying ONLY what is handed to
// `std::ofstream`:
//     std::string             -> OPEN OK
//     char const*             -> OPEN OK
//     std::filesystem::path   -> FAILS, every time
// On Windows `path::value_type` is `wchar_t`, so the path overload dispatches
// into libstdc++'s WIDE `basic_filebuf` open while the other two arms take
// the NARROW one. On POSIX `value_type` is `char`, so all three arms ARE the
// narrow route — the Linux control (g++ 13.3.0) opens on all three. It is a
// narrow-vs-wide CODE-PATH gap, not a compiler-version gap.
//
// The wide route does not merely IGNORE `noreplace` — it REJECTS it.
// Controlled on the same wide (`fs::path`) overload, same directory, varying
// ONLY that one bit:
//     binary|out             -> OPEN OK
//     binary|out|trunc       -> OPEN OK
//     binary|out|noreplace   -> FAILS, and leaves NO file behind
// So the wide openmode -> mode-string mapping matches no case containing the
// bit and fails the open whole; it never degrades to a silent non-exclusive
// open. Worth knowing when reading the C104 fallout: the regression was LOUD
// (every write errored) and cannot have shipped a silently non-atomic claim
// — but "loud" there meant no artifact at all.
//
// The trap is that it compiles perfectly clean (`__cpp_lib_ios_noreplace` ==
// 202207 — the feature is "present"); the defect is runtime-only. That is how
// it shipped in TF-C104 and took out EVERY artifact write on Windows
// (`error[K_ImageWriteOpenFailed] ... failed to create the staging temp`,
// 546 of 770 ctest cases red) while the two Linux legs stayed green.
//
// Going to `fopen`'s "x" is not a weaker substitute for `noreplace`: "x" is
// what P2467R1 SPECIFIES `noreplace` to mean, so this is the same
// atomic-check-and-create guarantee taken from the layer that actually
// implements it. MEASURED here on the same toolchain: a fresh path opens, an
// EXISTING path is REFUSED (null) and is left byte-intact — not truncated.
//
// `path::c_str()` returns `value_type const*`, which is already exactly the
// type each host's CRT entry point wants, so neither arm narrows a path (the
// throw hazard `pathForDiag` documents). This is the same shape as the
// `getpid` split above: ONE `#ifdef _WIN32` for ONE CRT spelling, confined to
// this function. It is a HOST split, never a target/format/language one.
//
// ★ THE TWO ARMS DELIBERATELY USE DIFFERENT PRIMITIVES, and the asymmetry is
// the point rather than an oversight:
//
//   * Windows uses `_wfopen(…, L"wbxN")` because it is MEASURED working on the
//     leg we actually build here (fresh path opens; existing path refused,
//     left byte-intact), and it keeps the wide path wide.
//
//   * POSIX uses `open(O_CREAT|O_EXCL)` + `fdopen` rather than
//     `fopen(…, "wbx")`. Both were measured EQUIVALENT on glibc (fresh opens,
//     existing refused, occupant's bytes preserved) — so this is not chosen on
//     behaviour. It is chosen on GUARANTEE: `O_EXCL` is POSIX.1 MANDATORY and
//     predates C11 by decades, whereas `"x"` is a C11 addition. Our POSIX legs
//     are glibc; the DARWIN leg (libc++ / Apple libc) CANNOT BE TESTED FROM
//     HERE, and "macOS surely implements C11 fopen 'x'" is exactly the kind of
//     read-it-and-assume claim that produced the TF-C104 outage this function
//     exists to repair. `O_EXCL` needs no such assumption.
//
//   * And the deeper reason: the defect being repaired WAS a library layer
//     refusing a mode/openmode it did not recognise. `O_EXCL` expresses
//     exclusivity as a kernel open flag, so it never passes through a
//     mode-string parser at all — the failure mode is structurally absent
//     rather than merely absent on the libcs we happened to test.
//
// `fdopen` adopts the descriptor on success. On FAILURE the POSIX arm has to
// undo BOTH halves of what it has already done, because by the time `fdopen`
// can fail the `open` above has ALREADY CREATED the file:
//   * the descriptor is closed, so a failed claim can never leak an fd (the
//     claim loop may run up to `kMaxClaimAttempts` times); and
//   * the file that `open` just created is unlinked, so a failed claim leaves
//     no residue behind.
//
// The second half is not housekeeping. The claim loop in `writeBytes`
// disambiguates a null return by asking `std::filesystem::exists(candidate)`:
// a zero-byte file WE created would read back as "another writer holds that
// slot", so the loop would step over its OWN debris and try the next name. A
// persistent `fdopen` failure (ENOMEM) would therefore leave up to
// `kMaxClaimAttempts` empty `.dsstmp-*` files next to the artifact and then
// emit a diagnostic blaming "an earlier run was killed before it could clean
// up" — a FALSE attribution of a fault that is entirely inside THIS process.
//
// The Windows arm has no such window: `_wfopen(…, L"wbxN")` creates nothing
// when it fails. So both arms keep one promise — a claim that does not hand
// back a stream leaves the directory exactly as it found it.
//
// ★ AND NEITHER ARM MAY LET THIS HANDLE CROSS INTO A SPAWNED CHILD — which is
// what the `N` and the `O_CLOEXEC` below are for, and they are the ONLY reason
// either appears. `writeBytes` holds the returned stream open across its
// `fwrite`, and the compiler now creates processes
// (`core/substrate/process_spawn.cpp`, whose header names two consumers: a
// user build hook and `git` dependency acquisition). Inheritance is a property
// of the HANDLE, decided where the handle is made, so refusing it here is the
// whole fix; refusing it at the spawn instead would mean adopting
// `STARTF_USESTDHANDLES` plus a `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` to filter,
// and that is precisely the mechanism `process_spawn.cpp` declines to use (its
// `bInheritHandles=TRUE` with NO `STARTF_USESTDHANDLES` is deliberate and has
// its own pin, `TheChildWritesToTheParentsOwnStdoutAndStderr`).
//
// ⚠ MEASURED, because the CRT's mode-flag behaviour is documented but the call
// had never been probed. `GetHandleInformation` on `_get_osfhandle(_fileno(f))`
// immediately after the open:
//     L"wbx"  -> flags=0x00000001, HANDLE_FLAG_INHERIT SET
//     L"wbxN" -> flags=0x00000000, HANDLE_FLAG_INHERIT clear
// ON BOTH WINDOWS TOOLCHAINS — MSVC 14.51 and MinGW-W64 UCRT g++ 13.2 — with
// identical flags and, the half that actually matters here, a SUCCESSFUL open
// in every case. That second half is not a formality: this function exists
// because TF-C104 shipped a spelling one library layer REFUSED, which failed
// every artifact write on Windows. An unrecognised mode character would do the
// same thing, so "does `N` open at all" was measured before it was adopted.
// And end to end, with a child created (`bInheritHandles=TRUE`) while the temp
// was open, the parent's own copy then closed, and the commit below attempted:
//     L"wbx"  -> MoveFileExW FAILS, GetLastError=32 ERROR_SHARING_VIOLATION
//     L"wbxN" -> MoveFileExW succeeds
// 32 is the rename at the bottom of `writeBytes` refusing to replace a target
// that is open elsewhere — and the diagnostic it emits blames "a running .exe
// or a scanner" while the real holder is OUR OWN child, so the operator is
// sent after a process that has nothing to do with it. Non-deterministic by
// construction: it needs a spawn to overlap a staged write.
//
// The POSIX consequence is a DIFFERENT one and is not a rename failure —
// `rename(2)` is a directory operation and does not care that the file is
// open, so the commit succeeds either way. What leaks there is the descriptor
// itself: a hook or a `git` gets a WRITABLE fd onto the compiler's staged
// artifact, which is a capability the substrate's own security note ("every
// consumer interpolates user-supplied text") says it must not hand out.
// MEASURED on gcc 13.3 / glibc 2.39 / Linux 5.15, a real `fork` + `execv`:
// without `O_CLOEXEC` the child finds fd 3 in its own `/proc/self/fd` pointing
// at `…dsstmp-…` and OPEN FOR WRITING; with it, the child inherits nothing of
// ours. `fdopen` does not disturb the descriptor flag either way, so the one
// `O_CLOEXEC` on the `open` is the whole of it. The same run confirms the
// asymmetry above is real rather than assumed: `rename(2)` returned 0 in BOTH
// modes, so the leak here never shows up as the Windows failure.
// Both arms therefore hold ONE property — the staging temp never crosses an
// exec — even though the failure each was preventing differs.
//
// RED ON DISABLE, per arm: drop the `N` on Windows, or `O_CLOEXEC` on POSIX,
// and `AClaimedStagingTempNeverCrossesIntoASpawnedChild` goes red on that leg.
[[nodiscard]] std::FILE*
createExclusiveBinary(std::filesystem::path const& p) {
#ifdef _WIN32
    return ::_wfopen(p.c_str(), L"wbxN");
#else
    // 0666 & umask — the same permissions `fopen` would have created, so
    // `writeImage`'s load-bearing re-apply (D-OUTPUT-EXEC-BIT) is unaffected.
    int const fd =
        ::open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (fd < 0) { return nullptr; }
    std::FILE* const f = ::fdopen(fd, "wb");
    if (!f) {
        ::close(fd);
        ::unlink(p.c_str());
    }
    return f;
#endif
}

} // namespace detail

namespace {

// Owning handle for the staging temp, so it is closed on EVERY path out of
// `writeBytes` — including one where building a diagnostic string throws.
// The COMMIT path deliberately takes the handle back with `release()` and
// closes it itself, because there `fclose`'s return value is load-bearing
// (K_ImageWriteCloseFailed). This deleter therefore only ever runs on paths
// that have ALREADY failed loudly, where a deferred flush error on a file
// that is about to be deleted is not new information — the same
// one-failure == one-diagnostic rule `removeTempNote` follows.
struct StagedFileCloser {
    void operator()(std::FILE* f) const noexcept { std::fclose(f); }
};
using StagedFile = std::unique_ptr<std::FILE, StagedFileCloser>;

// Windows-safe path-to-string for diagnostic messages. Both
// `path::string()` AND `path::generic_string()` perform narrowing
// from `wchar_t` on MSVC and CAN THROW `std::system_error` when
// the path contains code units that can't be narrowed to the
// current locale's ANSI codepage. A throw inside the failure-
// reporting path would silently abort the writer mid-diagnostic.
//
// Safe approach: try the narrow form first; on throw, fall back
// to `u8string()` (returns `std::u8string`, guaranteed UTF-8, never
// throws per the C++20 spec) and reinterpret to `std::string`.
// (silent-failure-hunter HIGH fold #2, LK10 cycle 1 post-fold #2
// review — the post-fold #1 `generic_string()`-only fix did not
// fully close the Windows throw hazard.)
[[nodiscard]] std::string pathForDiag(std::filesystem::path const& p) {
    try {
        return p.generic_string();
    } catch (...) {
        auto const u8 = p.u8string();
        return std::string(reinterpret_cast<char const*>(u8.data()),
                           u8.size());
    }
}

// Delete the staging temp on a failure path and return a note to APPEND to
// the caller's primary diagnostic. Returns an empty string on success.
//
// Fail-loud discipline: a failure to clean up is itself a real fact (a
// leaked temp sitting next to the artifact), but it is SECONDARY — the
// caller already has a primary error to report and emitting a second
// diagnostic would split one event across two codes. Folding it into the
// primary message keeps one failure == one diagnostic while never hiding
// the leak. Never returns a "success" indication that would let the caller
// report the write as clean.
[[nodiscard]] std::string
removeTempNote(std::filesystem::path const& tempPath) {
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
    if (!ec) {
        return {};
    }
    return std::string{" [additionally: the staging temp '"}
         + pathForDiag(tempPath)
         + "' could NOT be removed (" + ec.message()
         + ") — delete it manually]";
}

} // namespace

bool writeImage(LinkedImage const&             image,
                std::filesystem::path const&   path,
                DiagnosticReporter&            reporter,
                bool                           executable) {
    // Precondition 1: parallel-index gate. Writing an image whose
    // `ok()` is false would silently ship bytes that don't match
    // the expected function count. Fail loud here so a
    // misconfigured build script can't bypass the gate by calling
    // writeImage unconditionally.
    if (!image.ok()) {
        emit(reporter, DiagnosticCode::K_ImageNotOk,
             std::string{"link::writeImage: refusing to write "
                         "image whose ok() is false ("}
                 + "expectedFuncCount="
                 + std::to_string(image.expectedFuncCount)
                 + ", resolvedFuncCount="
                 + std::to_string(image.resolvedFuncCount)
                 + ", bytes.size()="
                 + std::to_string(image.bytes.size())
                 + "). The upstream walker likely emitted a "
                   "diagnostic; check `reporter.errorCount()` "
                   "before calling writeImage.");
        return false;
    }
    // The `ok()` check requires resolvedFuncCount == expectedFuncCount
    // (0 == 0 for a valid EMPTY module); if ok() returned true but bytes
    // are empty, the walker is contract-broken. This is the load-bearing
    // guard for the empty-module case: even a declaration-only TU (0
    // functions) must still produce real object bytes — a valid header +
    // section table — never zero bytes (D-CSUBSET-TESTTU-SILENT-EXIT1).
    // Surface here.
    if (image.bytes.empty()) {
        emit(reporter, DiagnosticCode::K_ImageEmpty,
             std::string{"link::writeImage: LinkedImage.bytes is "
                         "empty despite ok() == true — the walker "
                         "returned success with no output. "
                         "Substrate contract violation; fix the "
                         "walker, not the caller. (Type-design "
                         "split: distinct from K_ImageNotOk which "
                         "signals upstream walker failure that "
                         "already raised a diagnostic.)"});
        return false;
    }
    // Byte-integrity commit -- shared with the raw-bytes producers via
    // `writeBytes` (parent check + sibling-temp claim + write + close +
    // rename-over-target, all fail-loud). The artifact appears at `path`
    // only on full success, and always as a NEW file identity
    // (D-LK-WRITER-TRUNCATES-INSTEAD-OF-RENAMING).
    if (!writeBytes(image.bytes, path, reporter)) {
        return false;
    }
    // D-OUTPUT-EXEC-BIT: an EXECUTABLE-flavor output must carry the POSIX
    // execute bit so the produced binary runs directly (`./out`) without a
    // manual `chmod +x` (qemu's prepare_binprm + the kernel's execve both
    // reject a file lacking `mode & 0111`). Add owner/group/others-exec on
    // top of whatever the umask left (`perm_options::add`); a no-op on
    // Windows, where PE ignores Unix modes. Best-effort by design: the bytes
    // are already safely flushed above, so a failure to set the bit is a
    // WARNING (the artifact is valid — it just needs a manual chmod), NOT a
    // write failure. `executable` is the CALLER's config-driven decision
    // (`ObjectFormatSchema::isImageFlavor()`); this code never inspects the
    // format itself, staying format-blind.
    if (executable) {
        std::error_code ec;
        std::filesystem::permissions(
            path,
            std::filesystem::perms::owner_exec
                | std::filesystem::perms::group_exec
                | std::filesystem::perms::others_exec,
            std::filesystem::perm_options::add, ec);
        if (ec) {
            dss::report(reporter, DiagnosticCode::K_ImageExecBitFailed,
                        DiagnosticSeverity::Warning,
                        std::string{"link::writeImage: wrote '"}
                            + pathForDiag(path)
                            + "' but could not set its POSIX execute bit ("
                            + ec.message()
                            + "); the binary is valid but needs `chmod +x` to "
                              "run directly.");
        }
    }
    return true;
}

bool writeBytes(std::span<std::uint8_t const> bytes,
                std::filesystem::path const&  path,
                DiagnosticReporter&           reporter) {
    // Precondition: the path names a FILE. `rename()` needs a real final
    // component, and an empty one (an unset config field passing "") could
    // never name an artifact. Reject up front so a bad path can never leave
    // a staging temp behind in the current working directory.
    if (path.filename().empty()) {
        emit(reporter, DiagnosticCode::K_ImageWriteOpenFailed,
             std::string{"link::writeBytes: refusing to write to '"}
                 + pathForDiag(path)
                 + "' — the path has no filename component (empty path, or "
                   "a path ending in a directory separator). The caller must "
                   "supply a fully-qualified output file path.");
        return false;
    }
    // Precondition: parent directory exists.
    auto const parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        bool const exists = std::filesystem::exists(parent, ec);
        if (ec) {
            emit(reporter, DiagnosticCode::K_ImageWriteParentMissing,
                 std::string{"link::writeBytes: failed to stat "
                             "parent directory '"}
                     + pathForDiag(parent) + "': " + ec.message());
            return false;
        }
        if (!exists) {
            emit(reporter, DiagnosticCode::K_ImageWriteParentMissing,
                 std::string{"link::writeBytes: parent directory "
                             "does not exist: '"}
                     + pathForDiag(parent)
                     + "'. The substrate does not auto-create "
                       "directories — the caller must call "
                       "std::filesystem::create_directories before "
                       "writing.");
            return false;
        }
    }
    // ─────────────────────────────────────────────────────────────────
    // D-LK-WRITER-TRUNCATES-INSTEAD-OF-RENAMING — write a SIBLING TEMP, then
    // RENAME it over the target. DO NOT "simplify" this back to opening
    // `path` with `std::ios::trunc`. Why, MEASURED on macOS 26.5.2:
    //
    // `trunc` REUSES the target's existing inode. A consumer that rebuilds
    // repeatedly to one stable output path therefore keeps handing the OS
    // the SAME inode, and that inode eventually acquires a per-inode exec
    // DENY (`AMFI ... bailing out`; `AppleSystemPolicy: Security policy
    // would not allow process`). The artifact then dies with exit 137
    // SIGKILL and no output — while a byte-identical copy at another path
    // runs fine. The controls that isolated this: a hard link (different
    // path, SAME inode) is equally dead, so the deny is INODE-bound, not
    // path-bound; an in-place `cp` over the soured path stays dead, but a
    // `mv` of a fresh temp over it runs. `codesign -f -s -` appearing to
    // "repair" it was always the RENAME — codesign writes a temp and
    // renames (inode 20615856 -> 20615857), never the signature.
    //
    // A NEW FILE IDENTITY per write is the cure, and it is the established
    // convention (codesign, every editor's atomic-save), not a workaround.
    //
    // This is HOST FILESYSTEM behaviour, not target-format behaviour: it is
    // deliberately identical for ELF / PE / Mach-O / wasm / SPIR-V and for
    // every arch. Nothing here may ever branch on `image.format`.
    //
    // SIBLING TEMP IS MANDATORY. The temp is built by appending an ASCII
    // suffix to `path` itself, so it is a sibling BY CONSTRUCTION — there is
    // no separate directory expression a later edit could point at
    // `/tmp`/`$TMPDIR`. That matters on BOTH hosts: a temp on a different
    // filesystem turns POSIX `rename(2)` into EXDEV, and on Windows MSVC's
    // `std::filesystem::rename` is MoveFileExW with MOVEFILE_COPY_ALLOWED,
    // which "simulates the move by using the CopyFile and DeleteFile
    // functions" — silently copying INTO the existing target and
    // reintroducing exactly this defect.
    //
    // CROSS-PLATFORM, determined by reading the sources rather than
    // assuming: [fs.op.rename] specifies rename "as if by POSIX rename",
    // and "if new_p is an existing non-directory file, new_p is removed" —
    // so replace-over-existing is the STANDARD's contract, not a POSIX
    // extra. MSVC's STL implements it as
    // `MoveFileExW(src, dst, MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING)`
    // (microsoft/STL `stl/src/filesystem.cpp`), and MOVEFILE_REPLACE_EXISTING
    // replaces an existing target. So plain `std::filesystem::rename` is
    // correct on every host we build for and needs no `_WIN32` arm. The one
    // residual Windows difference — it does not use POSIX-semantics rename,
    // so a target held OPEN by another process (a running .exe, a scanner)
    // refuses to be replaced — is NOT a regression: the `trunc` open this
    // replaces failed on the very same input one step earlier. It is
    // handled the same way every other failure here is: loudly, with the
    // temp cleaned up. The rename diagnostic names that cause explicitly.
    //
    // KNOWN, ACCEPTED CONSEQUENCES (inherent to getting a new identity):
    //   * Overwriting no longer inherits the old file's permission bits;
    //     the artifact is created at the host umask. `writeImage` re-applies
    //     the load-bearing one (D-OUTPUT-EXEC-BIT) after this returns.
    //   * If `path` is a symlink or a hard link, the rename replaces THAT
    //     NAME rather than writing through to the shared file. That is the
    //     point — writing through is what reuses the inode.
    // ─────────────────────────────────────────────────────────────────

    // Claim a staging temp ATOMICALLY via `createExclusiveBinary` — `fopen`
    // mode "x", O_CREAT|O_EXCL — so the open succeeds only when THIS call
    // created the file. That check-and-create is atomic in the OS, making it
    // race-free against concurrent processes AND making it step over a stale
    // temp left by a killed run instead of silently sharing it (the
    // `scratch_dir.hpp` lesson; see `processSeed`). Do NOT "modernise" this
    // to `std::ofstream` + `std::ios::noreplace`: on this host that spelling is
    // REJECTED OUTRIGHT — the open fails whole and leaves no file behind, so
    // NO artifact is emitted at all. It never degrades to a silent
    // non-exclusive open; it stops every write dead. Read the measurement on
    // `createExclusiveBinary`
    // (D-LINK-WRITER-NOREPLACE-WIDE-PATH-UNSUPPORTED).
    static std::atomic<std::uint64_t> tempCounter{0};
    auto const                        seed = processSeed();

    std::filesystem::path tempPath;
    StagedFile            out;
    for (std::uint32_t attempt = 0;; ++attempt) {
        if (attempt >= detail::kMaxClaimAttempts) {
            emit(reporter, DiagnosticCode::K_ImageWriteOpenFailed,
                 std::string{"link::writeBytes: could not claim a unique "
                             "staging temp next to '"}
                     + pathForDiag(path) + "' after "
                     + std::to_string(detail::kMaxClaimAttempts)
                     + " attempts. Stale '.dsstmp-*' files are accumulating "
                       "in that directory — an earlier run was killed before "
                       "it could clean up. Delete them and re-run.");
            return false;
        }
        // Sibling by construction: the target path plus an ASCII suffix.
        // `operator+=` concatenates in the path's NATIVE encoding, so this
        // never narrows a wide path (the MSVC throw hazard `pathForDiag`
        // documents) and never touches the directory portion.
        auto candidate = path;
        candidate += ".dsstmp-" + std::to_string(seed) + "-"
                   + std::to_string(tempCounter.fetch_add(1));

        if (std::FILE* claimed = detail::createExclusiveBinary(candidate)) {
            // Take ownership FIRST: the path assignment below allocates, and
            // nothing between the successful open and the guard may be able
            // to leave the handle unowned.
            out.reset(claimed);
            tempPath = std::move(candidate);
            break;
        }
        // The open failed, and the two causes need opposite responses.
        // If the candidate EXISTS, another writer (or a stale temp) holds
        // that slot — take the next one. Otherwise this is a real error
        // (permission denied, parent removed post-stat, path component is
        // not a directory, invalid filename, disk full) and spinning would
        // only turn it into a slow, misleading failure. Fail loud now.
        std::error_code eec;
        bool const      taken = std::filesystem::exists(candidate, eec);
        if (!eec && taken) {
            continue;
        }
        emit(reporter, DiagnosticCode::K_ImageWriteOpenFailed,
             std::string{"link::writeBytes: failed to create the staging "
                         "temp '"}
                 + pathForDiag(candidate)
                 + "' for binary write (permission denied, a path component "
                   "is not a directory, invalid filename, or parent removed "
                   "post-stat). The artifact '"
                 + pathForDiag(path) + "' was NOT written.");
        return false;
    }

    // `fwrite` is UB on a null pointer even with a zero count, and
    // `span::data()` is permitted to be null for an EMPTY span, so a
    // zero-byte artifact skips the call rather than betting on the libc being
    // lenient. It still commits: an empty staging temp is renamed into place,
    // which is the right answer for a producer whose output genuinely is zero
    // bytes. (`writeImage` rejects empty bytes one level up, but the
    // raw-bytes producers that call `writeBytes` directly do not.)
    std::size_t const written =
        bytes.empty()
            ? std::size_t{0}
            : std::fwrite(bytes.data(), 1, bytes.size(), out.get());
    if (written != bytes.size()) {
        // Close BEFORE attempting cleanup: on Windows an open handle blocks
        // `remove`, so leaving it open would turn every short write into a
        // leaked temp plus a misleading "could NOT be removed" note. The
        // close's own status is deliberately not reported here — the write
        // already failed loudly and these bytes are being discarded, so a
        // deferred flush error on a file we are about to delete is not new
        // information (one failure == one diagnostic, per `removeTempNote`).
        out.reset();
        emit(reporter, DiagnosticCode::K_ImageWriteShort,
             std::string{"link::writeBytes: short write to the staging temp "
                         "for '"}
                 + pathForDiag(path) + "' (disk full or I/O error). The "
                   "previous artifact at that path is unchanged."
                 + removeTempNote(tempPath));
        return false;
    }
    // `release()` hands the handle out of the guard so it is closed EXACTLY
    // once — here, where the result is actually checked — and the guard is
    // left empty for every path below.
    if (std::fclose(out.release()) != 0) {
        // fclose() can fail when buffered writes flush to disk.
        // Letting the guard close it silently would swallow this; we
        // surface it as a write failure. Because the bytes were staged in a
        // temp, the artifact at `path` is still the PREVIOUS good one — the
        // partial file is the temp, and it is removed. (`fclose` closes the
        // stream even when it reports failure, so there is nothing left to
        // release.)
        emit(reporter, DiagnosticCode::K_ImageWriteCloseFailed,
             std::string{"link::writeBytes: close() failed for the staging "
                         "temp for '"}
                 + pathForDiag(path)
                 + "' (deferred I/O error on flush — the staged bytes are "
                   "incomplete). The previous artifact at that path is "
                   "unchanged." + removeTempNote(tempPath));
        return false;
    }

    // COMMIT. Same-directory rename over the target: atomic on POSIX,
    // MOVEFILE_REPLACE_EXISTING on Windows. A failure here must NEVER be
    // reported as success — that would leave the stale previous artifact in
    // place while the caller believes it shipped fresh bytes, which is the
    // precise silent-failure class this substrate exists to prevent.
    std::error_code rec;
    std::filesystem::rename(tempPath, path, rec);
    if (rec) {
        emit(reporter, DiagnosticCode::K_ImageWriteOpenFailed,
             std::string{"link::writeBytes: staged the bytes but could not "
                         "rename the temp over '"}
                 + pathForDiag(path) + "': " + rec.message()
                 + ". The artifact was NOT updated — any file at that path "
                   "is the PREVIOUS build's output. Likely causes: the "
                   "target is a directory, the target or its directory is "
                   "not writable, the parent was removed mid-write, or (on "
                   "Windows) the target is currently open/running and so "
                   "cannot be replaced."
                 + removeTempNote(tempPath));
        return false;
    }
    return true;
}

} // namespace dss::linker
