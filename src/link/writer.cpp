#include "link/writer.hpp"

#include "core/substrate/path_identity.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

// HOST facilities, not target ones: `getpid` seeds the unique temp filename
// (see `processSeed` below), and on POSIX `open`/`O_EXCL` performs the atomic
// exclusive claim (see `detail::createExclusiveBinary`). Same include dance as
// `tests/test_support/scratch_dir.hpp`, which established this precedent. On
// Windows the header also carries the commit's replace
// (`SetFileInformationByHandle`) and its holder query (see
// `detail::commitReplacing`).
#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>

    #include <fcntl.h>
    #include <io.h>
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
// `tests/test_support/scratch_dir.hpp` records for the examples runner's
// parallel-contention flake: pids RECYCLE, and a killed run can leave a stale
// artifact at any (pid, counter) pair — 55
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
// ★★★ AND THE WINDOWS ARM IS NOT `_wfopen(…, L"wbxN")`, BECAUSE `CREATE_NEW`
// IS NOT THE WINDOWS SPELLING OF `O_EXCL`.
// [[D-LINK-WRITER-WINDOWS-EXCLUSIVE-CLAIM-FOLLOWS-A-DANGLING-SYMLINK]]
//
// POSIX.1 REQUIRES `O_CREAT|O_EXCL` to fail with `EEXIST` when the pathname is a
// SYMBOLIC LINK — target existing or not. That mandate is the whole reason the
// staging claim is safe: a planted link at a candidate name is refused, and the
// writer steps to the next name. Windows `CREATE_NEW` carries no such rule: it
// FOLLOWS the reparse point and creates the TARGET, so the claim succeeds and
// the compiler's staged artifact is written wherever the link pointed.
//
// ✔MEASURED 2026-08-28 (cycle P43), first MSVC run of this suite:
// `LinkWriterExclusiveClaim.ADanglingSymlinkRefusesTheClaimButExistsSaysNo`
// reported `claim(p)` = TRUE where the case demands false, and then
// `fs::exists(p)` = TRUE — because the claim had just created the target
// through the link. Same defect in
// `RuntimeObjectCacheStore.TempClaimNeverEscapesThroughADanglingSymlink`, which
// reuses this primitive.
//
// ⚠⚠ IT WAS INVISIBLE FOR A REASON WORTH RECORDING, AND NOT BECAUSE NOBODY
// LOOKED: the cases exist, and their own header states — correctly, and
// measured — that MinGW's libstdc++ `create_symlink` returns ENOSYS, so the
// shipping Windows toolchain CANNOT CONSTRUCT the input. They therefore SKIP on
// the Windows leg and run only on POSIX, where the primitive was always right.
// MSVC implements `create_symlink`, so the first build that reached this suite
// also ran the guard. ★ A test that skips on the one platform whose primitive
// differs is not covering that platform — it is reporting that it did not.
//
// ⇒ `FILE_FLAG_OPEN_REPARSE_POINT` is what restores the POSIX guarantee: the
// create no longer traverses the link, so an existing reparse point at `p` makes
// `CREATE_NEW` fail with `ERROR_FILE_EXISTS`, exactly as `O_EXCL` fails with
// `EEXIST`. On a name that holds nothing the flag is inert and an ordinary file
// is created.
//
// ⓘ THE TWO PROPERTIES THE OLD SPELLING CARRIED ARE BOTH PRESERVED, and neither
// is incidental — see the `wbxN` measurement above:
//   * `N` (no inherit) -> `bInheritHandle` is FALSE, which is what a null
//     `SECURITY_ATTRIBUTES` means. Without it a spawned child inherits the
//     handle and the commit's `MoveFileExW` fails with ERROR_SHARING_VIOLATION
//     blaming a scanner for our own child.
//   * `wb` (binary, write) -> `_O_WRONLY | _O_BINARY` on the fd, then `"wb"` on
//     the `FILE*`. `_wfopen`'s default share mode is deny-none, so the three
//     `FILE_SHARE_*` bits keep a reader from being newly locked out.
// ⓘ `_close` owns the descriptor AND the underlying handle once
// `_open_osfhandle` succeeds, so the failure paths below must not also
// `CloseHandle` — that would be a double close.
[[nodiscard]] std::FILE*
createExclusiveBinary(std::filesystem::path const& p) {
#ifdef _WIN32
    HANDLE const h = ::CreateFileW(
        p.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,                                    // bInheritHandle = FALSE
        CREATE_NEW,                                 // the exclusive claim
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) { return nullptr; }
    int const fd =
        ::_open_osfhandle(reinterpret_cast<std::intptr_t>(h), _O_WRONLY | _O_BINARY);
    if (fd < 0) {
        ::CloseHandle(h);
        return nullptr;
    }
    std::FILE* const f = ::_fdopen(fd, "wb");
    if (!f) {
        ::_close(fd);   // closes the handle too
        return nullptr;
    }
    return f;
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
// Safe approach: try the narrow form first; on throw, fall back to the
// UTF-8 form and reinterpret to `std::string`.
// (silent-failure-hunter HIGH fold #2, LK10 cycle 1 post-fold #2
// review — the post-fold #1 `generic_string()`-only fix did not
// fully close the Windows throw hazard.)
//
// ★★★ AND THE TWO ARMS USED TO DISAGREE ABOUT WHICH FILE THEY NAME, which is
// strictly worse than the throw they were written for. `generic_string()`
// collapses a UNC path's leading separator RUN, so the try arm reported
// `//host/share/x` as `/host/share/x` — a path on the LOCAL drive root, a
// different file — while the catch arm's `u8string()` kept the run and also
// kept NATIVE separators. One path object, two spellings, and which one a user
// saw depended on whether narrowing happened to throw for an unrelated reason.
// Both arms now go through the run-preserving transforms
// ([[D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED]]), so they differ in ENCODING
// only — which is the one axis this fallback exists for.
//
// ⚠ `genericSpellingU8` throws exactly where `u8string()` does (a native name
// can be text no encoding accepts), which is unchanged from the code this
// replaced: the catch arm could always propagate. Swallowing it here would hand
// back a name that is not the file's, and this function's whole job is to name
// the file.
[[nodiscard]] std::string pathForDiag(std::filesystem::path const& p) {
    try {
        return core::genericSpelling(p);
    } catch (...) {
        auto const u8 = core::genericSpellingU8(p);
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

// ═════════════════════════════════════════════════════════════════════════════
// THE COMMIT — [[D-LINK-WRITER-RENAME-OVER-FAILS-WHILE-ANOTHER-PROCESS-HOLDS-THE-FILE]]
// ═════════════════════════════════════════════════════════════════════════════
//
// ★★★ WHAT FAILED, AND WHY IT WAS A WRONG ANSWER RATHER THAN A USER ERROR.
// Round 7 of cycle P68's gate failed a correct compile: `writeBytes` staged
// `repeat0.o` and the commit — then `std::filesystem::rename`, i.e.
// `MoveFileExW(MOVEFILE_REPLACE_EXISTING)` under both Windows standard
// libraries — was refused with `Permission denied`. ✔MEASURED out of tree on
// that workstation, repeating the failing test's own pattern (stage, close,
// commit over the target, read it back) with the CPU at 100%:
//
//   * WHICH HANDLE: the TARGET, the previous build's output. The classic
//     replace was refused with ERROR_ACCESS_DENIED (5) on 9/10000, 44/8000 and
//     68/20000 commits. A held STAGED file refuses with 32 instead, and
//     libstdc++ spells 5 `Permission denied` and 32 `Input/output error` (both
//     measured) — so the gate's text alone says which one was held.
//   * WHICH SHARE MODE: it shares DELETE. Each time the classic replace was
//     refused, the POSIX-semantics replace below, tried IMMEDIATELY on the same
//     staged file, succeeded (9/9); used for EVERY commit it was refused 0/18000.
//   * HOW LONG: 2.9–21.5 ms, median 10.8, measured with nothing else touching
//     the file. 🧠 A real-time scan of the freshly written file: the holder
//     PENDS other opens of it (a filter or an oplock break does; a bare handle
//     cannot), and `FileProcessIdsUsingFileInformation` names no process for it.
//
// ★★ CONTROLLED, one extra `GENERIC_READ` handle with the stated share mode,
// the error each replace returns (✔MEASURED, Windows 11 26200, NTFS):
//
//     held      holder shares     classic (MoveFileExW,    POSIX semantics
//                                 FileRenameInfo)
//     target    none, R, R+W      5                        32
//     target    R+W+DELETE        5                        0: it LANDS, and the
//                                                          holder keeps reading
//                                                          the old file
//     staged    none, R, R+W      32 (opening it for       32
//                                 DELETE)
//     staged    R+W+DELETE        0                        0
//     target is a running .exe    5                        5
//
// ⇒ THE REPLACE ASKS FOR POSIX SEMANTICS (`FileRenameInfoEx`,
// `FILE_RENAME_FLAG_POSIX_SEMANTICS`): a holder that shares delete keeps its
// handle on the file it opened while the name moves to the new one — exactly
// what `rename(2)` has always done on the other hosts. It is still ONE atomic
// rename: the name never stops existing, never serves a partial file, and ends
// on the staged file's NEW identity, as
// `D-LK-WRITER-TRUNCATES-INSTEAD-OF-RENAMING` requires. Nothing is ever copied.
//
// ★★ THE REFERENCES, ✔MEASURED under the same controlled holder (link
// generation A, hold the output, link generation B over it, run the result):
//   * MSVC link.exe 14.51 — LNK1104 against every holder; against a
//     delete-sharing one it had already DELETED the old output: nothing left.
//   * lld-link 19.1.5 — survives a delete-sharing holder (📄 LLVM's source:
//     it renames the busy target aside, delete-on-close, then its own file in);
//     refuses every other holder at once.
//   * GNU ld (MinGW-w64 13.2) — survives a delete-sharing holder with a NEW
//     file, and a read+write-sharing one with the SAME file: TRUNCATED IN PLACE
//     under the holder (✔ file indices; 🧠 unlink-then-create, source not read).
// Two references survive the holder that failed the gate ⇒ surviving it is
// REQUIRED. Both do it NON-atomically — the name is briefly absent, or is
// rewritten under an open handle; POSIX semantics does it atomically. GNU ld's
// in-place survival is the inode reuse
// `D-LK-WRITER-TRUNCATES-INSTEAD-OF-RENAMING` removed, so that one holder is
// refused here ON PURPOSE.
//
// ★ NO REFERENCE WAITS — every one returned within 342 ms against a 3 s holder.
// The wait below exists for the holders POSIX semantics cannot step past: one
// withholding FILE_SHARE_DELETE, a program running from the target, and ANY
// holder on a volume that cannot rename with POSIX semantics (✔MEASURED: a WSL
// 9P share answers 87 and moves nothing — there the classic replace is the only
// atomic one, and waiting is the only atomic way past a holder). Bounded by
// `detail::kCommitHolderWait`, sized at its declaration.
//
// ⓘ POSIX IS UNTOUCHED: `rename(2)` is a directory operation that never
// consults another open descriptor (the measurement the staging-temp note above
// records), so that arm is the one call it always was, and the patience is
// never asked.

namespace {

#ifdef _WIN32

// `FileRenameInfoEx` and its two flags are SPELLED HERE because both
// toolchains' headers declare them only when a build targets Windows 10 RS1 or
// later, and this tree names no target version: MinGW-w64's `_mingw.h`
// defaults `_WIN32_WINNT` to 0x0601, which hides the enumerator. ✔MEASURED
// which checks below are live: MSVC 14.51 (SDK 10.0.26100) declares all three,
// so all three asserts run; MinGW-w64 13.2 declares the two flag macros — only
// because their gate names `_WIN32_WINNT_WIN10_RS1`, which it never defines and
// the preprocessor reads as 0 — so the flag asserts run there too. The values
// are the documented ABI either way.
constexpr auto  kFileRenameInfoEx          = static_cast<FILE_INFO_BY_HANDLE_CLASS>(22);
constexpr DWORD kRenameFlagReplaceIfExists = 0x00000001;
constexpr DWORD kRenameFlagPosixSemantics  = 0x00000002;
#ifdef FILE_RENAME_FLAG_POSIX_SEMANTICS
static_assert(FILE_RENAME_FLAG_REPLACE_IF_EXISTS == kRenameFlagReplaceIfExists);
static_assert(FILE_RENAME_FLAG_POSIX_SEMANTICS == kRenameFlagPosixSemantics);
#endif
#if defined(NTDDI_VERSION) && defined(NTDDI_WIN10_RS1)
#if NTDDI_VERSION >= NTDDI_WIN10_RS1
static_assert(FileRenameInfoEx == kFileRenameInfoEx);
#endif
#endif

// `FILE_RENAME_INFO` as the RS1 headers declare it — the first member is the
// union of the classic class's `BOOLEAN ReplaceIfExists` and the Ex class's
// `DWORD Flags` — spelled here for the same reason, and held to the header's
// own layout.
struct RenameInfo {
    DWORD  Flags;
    HANDLE RootDirectory;
    DWORD  FileNameLength;  // in bytes, without the terminator
    WCHAR  FileName[1];
};
static_assert(offsetof(RenameInfo, RootDirectory)
              == offsetof(FILE_RENAME_INFO, RootDirectory));
static_assert(offsetof(RenameInfo, FileNameLength)
              == offsetof(FILE_RENAME_INFO, FileNameLength));
static_assert(offsetof(RenameInfo, FileName) == offsetof(FILE_RENAME_INFO, FileName));

// The Win32 error BY NUMBER first: the text is localized (this workstation's is
// Portuguese) and the number is what a report can be searched on — the same
// order `core/substrate/process_spawn.cpp` gives its host error text.
[[nodiscard]] std::string windowsError(DWORD error) {
    return "Windows error " + std::to_string(error) + " ("
         + std::system_category().message(static_cast<int>(error)) + ")";
}

// Rename the file behind `staged` — a handle with DELETE access — to `target`,
// replacing whatever is there. `posix` selects `FileRenameInfoEx` with POSIX
// semantics, else the classic `FileRenameInfo`, the replace `MoveFileExW`
// performs. ✔MEASURED: the name is resolved the way `MoveFileExW` resolves it —
// forward slashes, a bare relative name and a `..` component all land against
// the current directory — so `target` is handed over exactly as spelled.
[[nodiscard]] DWORD renameByHandle(HANDLE                       staged,
                                   std::filesystem::path const& target,
                                   bool                         posix) {
    std::wstring const& name  = target.native();
    std::size_t const   bytes =
        offsetof(RenameInfo, FileName) + (name.size() + 1) * sizeof(WCHAR);
    // `std::uint64_t` storage so the HANDLE member is aligned whatever the size.
    std::vector<std::uint64_t> storage(
        (bytes + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t), 0);
    auto* const info = reinterpret_cast<RenameInfo*>(storage.data());
    // The classic class reads byte 0 as `ReplaceIfExists`, so TRUE lands there.
    info->Flags = posix ? (kRenameFlagReplaceIfExists | kRenameFlagPosixSemantics)
                        : DWORD{TRUE};
    info->RootDirectory  = nullptr;
    info->FileNameLength = static_cast<DWORD>(name.size() * sizeof(WCHAR));
    std::memcpy(info->FileName, name.c_str(), (name.size() + 1) * sizeof(WCHAR));
    if (::SetFileInformationByHandle(staged,
                                     posix ? kFileRenameInfoEx : FileRenameInfo,
                                     info, static_cast<DWORD>(bytes))) {
        return ERROR_SUCCESS;
    }
    DWORD const error = ::GetLastError();
    // A failed call must never read as a success, whatever the slot holds.
    return error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error;
}

// The answers that mean the VOLUME (or an OS older than the class) cannot
// rename with POSIX semantics — so the classic replace is the one to make. The
// same three `microsoft/STL` treats as "use the classic form" when it asks for
// POSIX delete semantics in `__std_fs_remove`. ✔MEASURED: a WSL 9P share
// answers 87 and moves nothing, so falling through to the classic replace on
// the same handle is a clean second attempt, never a half-done first.
[[nodiscard]] bool posixRenameUnavailable(DWORD error) {
    return error == ERROR_INVALID_PARAMETER || error == ERROR_INVALID_FUNCTION
        || error == ERROR_NOT_SUPPORTED;
}

enum class RefusalKind : std::uint8_t {
    Holder,     // another open handle — the only kind waiting can change
    Directory,  // a file can never replace a directory
    ReadOnly,   // a read-only file is never replaced
    Other,      // anything else: reported at once
};

// Which refusals a HOLDER causes. 32 always is one. 5 is one too — the classic
// replace's answer to ANY open target, and the POSIX one's to a running image —
// except that 5 is also what a directory target and a read-only target answer
// (✔MEASURED, both replaces), and neither of those ever lets go, so waiting on
// them would only delay a refusal that is already certain. A target that cannot
// even be stat'ed is held when the answer is 5 — ✔MEASURED: a delete-PENDING
// file (classic disposition, a holder still open) answers 5 here and to both
// replaces, and is gone once its last handle closes — and is NOT held when
// nothing is at the name at all: then the 5 came from elsewhere (an ACL).
[[nodiscard]] RefusalKind classifyRefusal(DWORD                        error,
                                          bool                         onStaged,
                                          std::filesystem::path const& target) {
    if (error == ERROR_SHARING_VIOLATION) return RefusalKind::Holder;
    if (error != ERROR_ACCESS_DENIED || onStaged) return RefusalKind::Other;
    DWORD const attributes = ::GetFileAttributesW(target.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD const why = ::GetLastError();
        return (why == ERROR_FILE_NOT_FOUND || why == ERROR_PATH_NOT_FOUND)
                 ? RefusalKind::Other
                 : RefusalKind::Holder;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return RefusalKind::Directory;
    if ((attributes & FILE_ATTRIBUTE_READONLY) != 0) return RefusalKind::ReadOnly;
    return RefusalKind::Holder;
}

// ── Naming the holders ─────────────────────────────────────────────────────
// `NtQueryInformationFile` with `FileProcessIdsUsingFileInformation` (class 47,
// declared with its FILE_PROCESS_IDS_USING_FILE_INFORMATION result in the WDK's
// `ntifs.h`): every process with a handle open on the file. Reached
// through `GetProcAddress` so the library gains no link dependency, and asked
// ONCE, after the commit has already given up — a refusal that names its cause
// is the reason to ask at all. ⓘ ✔MEASURED: it names a running program and
// any ordinary handle; for the real-time scan it names nobody (and its own open
// is pended until the scan ends), so "nobody" is reported as exactly that.
struct IoStatusBlock {
    union {
        LONG  Status;
        void* Pointer;
    };
    ULONG_PTR Information;
};
struct ProcessIdsUsingFile {
    ULONG     NumberOfProcessIdsInList;
    ULONG_PTR ProcessIdList[1];
};
using NtQueryInformationFileFn = LONG(WINAPI*)(HANDLE, IoStatusBlock*, void*, ULONG, int);
constexpr int  kFileProcessIdsUsingFileInformation = 47;
constexpr LONG kStatusInfoLengthMismatch = static_cast<LONG>(0xC0000004UL);
constexpr LONG kStatusBufferOverflow     = static_cast<LONG>(0x80000005UL);

[[nodiscard]] std::string nameProcess(DWORD pid) {
    std::string const id = "(pid " + std::to_string(pid) + ")";
    if (pid == ::GetCurrentProcessId()) return "this process " + id;
    HANDLE const process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return "a process " + id + " whose image could not be read ("
             + windowsError(::GetLastError()) + ")";
    }
    // 32767 is the longest path the NT namespace can hold, so this never truncates.
    std::wstring image(32768, L'\0');
    DWORD        length = static_cast<DWORD>(image.size());
    BOOL const   ok     = ::QueryFullProcessImageNameW(process, 0, image.data(), &length);
    DWORD const  error  = ok ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(process);
    if (!ok) {
        return "a process " + id + " whose image could not be read (" + windowsError(error) + ")";
    }
    image.resize(length);
    return pathForDiag(std::filesystem::path{image}) + " " + id;
}

[[nodiscard]] std::string describeHolders(std::filesystem::path const& file) {
    auto const query = reinterpret_cast<NtQueryInformationFileFn>(
        reinterpret_cast<void (*)()>(::GetProcAddress(
            ::GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationFile")));
    if (query == nullptr) {
        return "the holders could not be listed (ntdll exports no "
               "NtQueryInformationFile)";
    }
    HANDLE const handle = ::CreateFileW(
        file.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return "the holders could not be listed (" + windowsError(::GetLastError()) + ")";
    }
    // Grown until the list fits; every growth is the system saying "too small".
    std::vector<std::uint64_t> buffer(1 + 16, 0);
    LONG                       status = 0;
    for (;;) {
        IoStatusBlock io{};
        status = query(handle, &io, buffer.data(),
                       static_cast<ULONG>(buffer.size() * sizeof(std::uint64_t)),
                       kFileProcessIdsUsingFileInformation);
        if (status != kStatusInfoLengthMismatch && status != kStatusBufferOverflow) break;
        buffer.assign(buffer.size() * 2, 0);
    }
    ::CloseHandle(handle);
    if (status < 0) {
        char hex[16];
        std::snprintf(hex, sizeof hex, "0x%08lX", static_cast<unsigned long>(status));
        return std::string{"the holders could not be listed (NTSTATUS "} + hex + ")";
    }
    auto const* const list = reinterpret_cast<ProcessIdsUsingFile const*>(buffer.data());
    std::string       names;
    for (ULONG i = 0; i < list->NumberOfProcessIdsInList; ++i) {
        if (!names.empty()) names += ", ";
        names += nameProcess(static_cast<DWORD>(list->ProcessIdList[i]));
    }
    if (names.empty()) {
        return "no process the system can name — the holder let go as it was "
               "asked, or it is not an ordinary handle (a real-time scan names "
               "none; measured)";
    }
    return names;
}

[[nodiscard]] std::string composeRefusal(DWORD                        error,
                                         RefusalKind                  kind,
                                         bool                         onStaged,
                                         bool                         posix,
                                         std::uint32_t                holderRefusals,
                                         std::chrono::milliseconds    waited,
                                         std::filesystem::path const& staged,
                                         std::filesystem::path const& target) {
    std::string text = windowsError(error);
    if (onStaged) {
        text += " opening the staged file '" + pathForDiag(staged) + "' for the rename";
    }
    switch (kind) {
    case RefusalKind::Directory:
        return text + " — the target is a DIRECTORY, which a file can never "
                      "replace; nothing was waited for";
    case RefusalKind::ReadOnly:
        return text + " — the target carries the READ-ONLY attribute, and a "
                      "read-only file is never replaced; nothing was waited for";
    case RefusalKind::Other:
        return text;
    case RefusalKind::Holder:
        break;
    }
    text += " — another open handle on the " + std::string{onStaged ? "staged file" : "target"}
          + " refused it " + std::to_string(holderRefusals) + " time(s) across "
          + std::to_string(waited.count()) + " ms, and a commit waits at most "
          + std::to_string(detail::kCommitHolderWait.count())
          + " ms (linker::detail::kCommitHolderWait) for a holder to let go. ";
    text += posix ? "The replace asked for POSIX semantics, which a holder that "
                    "shares delete access cannot refuse — so this one withholds "
                    "FILE_SHARE_DELETE, or the target is a running program's "
                    "image. "
                  : "This volume cannot rename with POSIX semantics, so the "
                    "classic replace was used, which ANY open handle on the "
                    "target refuses. ";
    text += "Held open by: " + describeHolders(onStaged ? staged : target);
    return text;
}

#endif  // _WIN32

}  // namespace

namespace detail {

CommitOutcome commitReplacing(std::filesystem::path const& staged,
                              std::filesystem::path const& target,
                              CommitPatience const&        patience) {
    CommitOutcome outcome;
#ifdef _WIN32
    bool   posix    = true;  // until the volume answers that it cannot
    bool   onStaged = false;
    DWORD  error    = ERROR_SUCCESS;
    HANDLE handle   = INVALID_HANDLE_VALUE;
    auto   firstRefusal = std::chrono::steady_clock::time_point{};
    for (;;) {
        // The rename is made on a handle to the staged file — DELETE access,
        // every share granted, not inheritable (null SECURITY_ATTRIBUTES, the
        // `createExclusiveBinary` rule) — so no second open of it is ever
        // needed and a cross-volume target cannot be copied into.
        // FILE_FLAG_OPEN_REPARSE_POINT renames the ENTRY, as `MoveFileExW`
        // does, never a link's target.
        if (handle == INVALID_HANDLE_VALUE) {
            handle = ::CreateFileW(staged.c_str(), DELETE | SYNCHRONIZE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT,
                                   nullptr);
        }
        if (handle == INVALID_HANDLE_VALUE) {
            error    = ::GetLastError();
            onStaged = true;
        } else {
            onStaged = false;
            error    = renameByHandle(handle, target, posix);
            if (posix && posixRenameUnavailable(error)) {
                posix = false;
                error = renameByHandle(handle, target, false);
            }
            if (error == ERROR_SUCCESS) {
                ::CloseHandle(handle);
                outcome.committed = true;
                return outcome;
            }
        }
        RefusalKind const kind = classifyRefusal(error, onStaged, target);
        if (kind != RefusalKind::Holder) {
            if (handle != INVALID_HANDLE_VALUE) ::CloseHandle(handle);
            outcome.refusal = composeRefusal(error, kind, onStaged, posix,
                                             outcome.holderRefusals,
                                             std::chrono::milliseconds{0}, staged,
                                             target);
            return outcome;
        }
        if (outcome.holderRefusals == 0) firstRefusal = std::chrono::steady_clock::now();
        ++outcome.holderRefusals;
        if (!patience(outcome.holderRefusals)) break;
    }
    if (handle != INVALID_HANDLE_VALUE) ::CloseHandle(handle);
    auto const waited = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - firstRefusal);
    outcome.refusal = composeRefusal(error, RefusalKind::Holder, onStaged, posix,
                                     outcome.holderRefusals, waited, staged, target);
    return outcome;
#else
    // `rename(2)`: never refused because another process holds the file, so
    // there is no holder to wait for and `patience` is never asked.
    (void)patience;
    std::error_code ec;
    std::filesystem::rename(staged, target, ec);
    if (!ec) {
        outcome.committed = true;
        return outcome;
    }
    outcome.refusal = ec.message()
                    + " (rename(2); it is never refused because another process "
                      "holds the file, so nothing was waited for — likely "
                      "causes: the target is a directory, the target's directory "
                      "is not writable, or the parent was removed mid-write)";
    return outcome;
#endif
}

CommitOutcome commitReplacing(std::filesystem::path const& staged,
                              std::filesystem::path const& target) {
    // The cap runs from the FIRST refusal, so it measures the holder and never
    // the attempt before it.
    std::optional<std::chrono::steady_clock::time_point> deadline;
    return commitReplacing(staged, target, [&deadline](std::uint32_t) {
        auto const now = std::chrono::steady_clock::now();
        if (!deadline) deadline = now + kCommitHolderWait;
        if (now >= *deadline) return false;
        // The shortest wait the host can express, and not a tuning: the
        // measured holds are 3–22 ms, so re-asking at the timer's own
        // granularity notices a release within one tick, while a zero wait
        // would spin a core against the holder for the whole cap.
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        return true;
    });
}

}  // namespace detail

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
                   "diagnostic; `writeBytes` below enforces "
                   "`reporter.errorCount() == 0` for every "
                   "artifact, so a compile that failed earlier "
                   "never reaches this arm's write.");
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
    // ── THE ARTIFACT INVARIANT: A COMPILATION THAT RECORDED AN ERROR COMMITS
    //    NO ARTIFACT ────────────────────────────────────────────────────────
    //
    // ★★★ FIRST, AND IN THIS FUNCTION RATHER THAN IN ANY CALLER, BECAUSE THIS
    // IS THE ONE PLACE ANY ARTIFACT BYTE IN THIS COMPILER REACHES DISK.
    // `writeImage` delegates its byte commit here after its LinkedImage-shape
    // preconditions, and the `ar` static-archive producer calls it directly;
    // ✔MEASURED — those are the only two call sites in `src/`. A caller that
    // adds a third inherits the invariant instead of having to remember it,
    // which is the whole reason the question is asked here and not twice in
    // `compile_pipeline.cpp`.
    //
    // ⚠ THE PRECONDITION ALREADY EXISTED — AS A SENTENCE ADDRESSED TO
    // CALLERS, AND NO CALLER OBEYED IT. `writeImage`'s K_ImageNotOk arm has
    // told the reader to *"check `reporter.errorCount()` before calling
    // writeImage"* since the LK10 substrate landed. A precondition every
    // caller must check and none does is a precondition in the wrong place.
    //
    // ★ WHY `errorCount()` ON THIS REPORTER IS EXACTLY "THIS COMPILATION",
    // with no snapshot to thread and no cross-target leak: `compileOneTarget`
    // is the single owner of an output path (✔MEASURED: one call site) and is
    // handed a FRESH per-target `DiagnosticReporter scratch` by
    // `runCusToTargets`, which then decides that target's exit code from the
    // very same `scratch.hasErrors()` one statement after the call. So the
    // predicate that says the build FAILED and the predicate that withholds
    // the file are now ONE predicate — which is the defect stated positively:
    // the tool could report rc=1 and hand back the binary that rc denied.
    //
    // ★★ IT CANNOT TURN A PASSING BUILD RED. A recorded error already makes
    // `runCusToTargets` set `exitCode = 1`, so every run this gate can reach
    // was failing before it existed. The change is in what such a run LEAVES
    // ON DISK, never in which runs succeed.
    //
    // ⓘ REACHING THIS GATE MEANS AN UPSTREAM TIER REPORTED AN ERROR AND LET
    // THE PIPELINE RUN ON. Every tier gate in `compile_pipeline.cpp` is a
    // `tierClean(reporter, entryCount)` SNAPSHOT — right, and deliberately so,
    // since a shared reporter accumulates and a tier must not fail on another
    // tier's errors — but that discipline has no WHOLE-COMPILATION level, and
    // this is it. ✔MEASURED on the shipped CLI at the base of this lane: the
    // one live producer is `mergeCuMirs`, which reports each cross-unit strong
    // redefinition and then returns a valid merged module, so a two-TU program
    // redefining a symbol printed `dsscp: artifact …`, exited 1, and left a
    // runnable `.exe` bound to CU#1's definition.
    //
    // ⓘ AND THE PREVIOUS ARTIFACT IS LEFT BYTE-INTACT. The refusal is ahead of
    // the staging-temp claim, so the commit rename never runs and a good
    // binary from an earlier build is neither replaced nor removed. That is a
    // DELIBERATE, MEASURED divergence from all three references on a failed
    // LINK (gcc 13.3.0, clang 18.1.3 and MSVC 19.51 each unlink the previous
    // output when `ld`/`link` fails; all three LEAVE it byte-identical when
    // the FRONT END fails, ✔measured separately per reference) and it is the
    // same non-destructive policy `writeBytes` already documents for a failed
    // write: this compiler never destroys an artifact it did not write, it
    // only ever declines to write a new one. Removing one instead would make
    // a compiler delete a working user binary on a build it aborted before
    // opening the file.
    if (reporter.errorCount() != 0) {
        dss::report(reporter, DiagnosticCode::K_ArtifactWithheldAfterError,
                    DiagnosticSeverity::Info,
                    std::string{"link::writeBytes: withholding the artifact '"}
                        + pathForDiag(path) + "' — this compilation recorded "
                        + std::to_string(reporter.errorCount())
                        + " error(s), and a build that reports failure must "
                          "not also produce the file that failure denies. "
                          "Nothing was written; any file already at that path "
                          "is untouched. The error(s) above are the fault — "
                          "this line is only its consequence.");
        return false;
    }
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
    // replaces an existing target. So rename-over-target is the right commit
    // on every host we build for.
    //
    // ⚠ SUPERSEDED FOR WINDOWS — the paragraph that stood here accepted "the
    // one residual Windows difference — it does not use POSIX-semantics rename,
    // so a target held OPEN by another process (a running .exe, a scanner)
    // refuses to be replaced" as NOT a regression, and failed loudly on it. The
    // round-7 gate of cycle P68 measured that holder on the PREVIOUS artifact:
    // sharing delete, 3–22 ms, 🧠 a real-time scan — not a user error, and a
    // compile that fails because of it is a wrong answer. The commit is now
    // `detail::commitReplacing`, which asks for exactly the POSIX semantics
    // this paragraph said it lacked
    // ([[D-LINK-WRITER-RENAME-OVER-FAILS-WHILE-ANOTHER-PROCESS-HOLDS-THE-FILE]]).
    // It also drops `MOVEFILE_COPY_ALLOWED`: the rename is made on a handle, so
    // a cross-volume target is REFUSED rather than silently copied into — the
    // hazard the sibling-temp note above describes can no longer be reached
    // even by a construction that stops being a sibling.
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
        // If the candidate NAME is occupied, another writer (or a stale temp)
        // holds that slot — take the next one. Otherwise this is a real error
        // (permission denied, parent removed post-stat, path component is
        // not a directory, invalid filename, disk full) and spinning would
        // only turn it into a slow, misleading failure. Fail loud now.
        //
        // ── THE PROBE MUST ASK THE QUESTION THE CLAIM ASKED ───────────────
        //    D-LINK-WRITER-DANGLING-SYMLINK-CLAIM-MISROUTE
        //
        // ★★ IT IS `symlink_status`, NOT `exists`, AND THE DIFFERENCE IS THE
        // WHOLE DEFECT. `std::filesystem::exists(p)` FOLLOWS the link and
        // answers about the TARGET; the exclusive create refuses a directory
        // ENTRY. On a DANGLING symlink the two disagree, the loop believes the
        // wrong one, and an occupied slot is reported as a real error.
        //
        // ✔MEASURED 2026-08-26 on glibc/ext4 (WSL, g++ 13.3.0) by driving the
        // two probes and the claim primitive over one name each — the claim
        // spelled exactly as `createExclusiveBinary` spells it:
        //
        //   candidate                claim    exists()   exists(symlink_status())
        //   fresh name               OPENED   —          —
        //   regular-file occupant    NULL     true       true
        //   DANGLING symlink         NULL     **false**  **true**
        //   symlink to a live file   NULL     true       true
        //
        // Only the dangling row diverges, and `symlink_status` is right on all
        // four. The live-symlink row is worth stating because it shows the
        // repair is not "symlinks are special": POSIX makes `O_CREAT|O_EXCL`
        // fail on ANY symlink regardless of its target, so that row was already
        // routed correctly and stays correct — it is the ENTRY-vs-TARGET
        // question that was being asked wrongly, not the symlink-ness.
        //
        // ⚠ WINDOWS CANNOT REACH THIS ROW AT ALL, and it is a LIBRARY limit
        // rather than a privilege one — which is not what the anchor predicted.
        // ✔MEASURED 2026-08-26 on this host with the shipping toolchain
        // (MinGW-W64 UCRT g++ 13.2.0): `std::filesystem::create_symlink`
        // returns `ENOSYS` ("Function not implemented", 40) even though
        // `mklink` succeeds in the same directory, so no test on that leg can
        // construct the input. The pin is therefore POSIX-leg only and SAYS SO;
        // see `tests/link/test_link_writer_exclusive_claim.cpp`.
        //
        // `status_known` is not used as the test: `symlink_status` reports a
        // NON-EXISTENT name as `file_type::not_found`, which IS a known status,
        // so `status_known` would answer true for every name and route a real
        // error into the retry loop. `exists(file_status)` is the predicate
        // that distinguishes them.
        std::error_code eec;
        auto const      entry = std::filesystem::symlink_status(candidate, eec);
        if (!eec && std::filesystem::exists(entry)) {
            continue;
        }
        emit(reporter, DiagnosticCode::K_ImageWriteOpenFailed,
             std::string{"link::writeBytes: failed to create the staging "
                         "temp '"}
                 + pathForDiag(candidate)
                 + "' for binary write (permission denied, a path component "
                   "is not a directory, invalid filename, or parent removed "
                   "post-stat). The name itself was probed as a directory "
                   "ENTRY, so an occupant of ANY kind — a regular file, a "
                   "directory, or a dangling symlink — would have been stepped "
                   "over rather than reported here. The artifact '"
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

    // COMMIT. A same-directory rename over the target, owned by
    // `detail::commitReplacing`: `rename(2)` on POSIX; on Windows a
    // POSIX-semantics replace that a holder sharing delete — as the round-7
    // gate's holder does (🧠 a real-time scan) — cannot refuse, and that gives any other
    // holder `detail::kCommitHolderWait` to let go before it refuses, naming it
    // ([[D-LINK-WRITER-RENAME-OVER-FAILS-WHILE-ANOTHER-PROCESS-HOLDS-THE-FILE]]).
    // A failure here must NEVER be reported as success — that would leave the
    // stale previous artifact in place while the caller believes it shipped
    // fresh bytes, which is the precise silent-failure class this substrate
    // exists to prevent.
    auto const commit = detail::commitReplacing(tempPath, path);
    if (!commit.committed) {
        emit(reporter, DiagnosticCode::K_ImageWriteOpenFailed,
             std::string{"link::writeBytes: staged the bytes but could not "
                         "rename the temp over '"}
                 + pathForDiag(path) + "': " + commit.refusal
                 + ". The artifact was NOT updated — any file at that path "
                   "is the PREVIOUS build's output."
                 + removeTempNote(tempPath));
        return false;
    }
    return true;
}

} // namespace dss::linker
