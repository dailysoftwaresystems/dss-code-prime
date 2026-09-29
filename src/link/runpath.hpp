#pragma once

// ── D-LK-IMAGE-CANNOT-DECLARE-A-RUNPATH: WHERE AN IMAGE SAYS ITS LIBRARIES ARE ──
//
// A RUNPATH is a list of directories an image records for its OWN loader to
// search when it resolves the libraries that image needs. The program names it
// (`--rpath <dir>`, or the project manifest's `runpaths`); the FORMAT decides
// whether an image of that format can carry one and in which structure; the
// WRITER records it from that declaration. This header is the one owner of the
// vocabulary all three share.
//
// WHAT THE REFERENCES DO, measured before anything here was written (lane `vn`,
// P68 round 8; the probes and logs are under the lane's `.temp/ref/`):
//   * ELF — gcc/GNU ld 2.42 and clang/ld.lld 18 both record `-Wl,-rpath,<dir>`
//     as ONE `.dynamic` entry, DT_RUNPATH (29) by default, whose value is the
//     `.dynstr` offset of every requested directory joined by `:` in command-line
//     order; `$ORIGIN` is stored verbatim for ld.so to expand. Executable, PIE
//     and shared object all take it. A static image (no dynamic section) takes
//     nothing, silently.
//   * Mach-O — ld64.lld 18 records one LC_RPATH load command per `-rpath`
//     (`rpath_command`: cmd, cmdsize, lc_str at offset 12, NUL-terminated,
//     cmdsize a multiple of 8). A `:` stays ONE path: dyld does not split it.
//   * PE — neither mingw GNU ld (silently) nor MSVC link 14.51 (LNK4044,
//     "unrecognized option … ignored") records anything, and both images RUN
//     with the DLL beside them: the Windows loader searches the application's
//     directory itself. A format that declares no carrier therefore ACCEPTS a
//     request, records nothing, and says so in a warning.
//
// ★ THE PORTABLE TOKEN. The only part of a runpath whose SPELLING differs by
// format is the image's own directory: ELF's `$ORIGIN`, Mach-O's
// `@loader_path` — both measured (ELF) and read at source (dyld) to mean the
// directory of the image that CARRIES the path, not the executable's. A
// manifest that builds for many targets must not carry either spelling, so it
// writes `${ORIGIN}` and each format DOCUMENT declares its own spelling
// (`runpath.origin`). ld.so already accepts the braced form, so a gcc habit
// reads right. `@executable_path` has no ELF equivalent and gets no token.
//
// ★ THE TOKEN IS RECOGNISED ONLY AS THE LEADING COMPONENT — the whole entry, or
// followed by `/`. That is the only position in which both loaders expand their
// spelling (dyld's `expandAtLoaderPath` requires `/` or NUL after the prefix;
// glibc expands an unbraced `$ORIGIN` only when no identifier character follows
// — measured: `${ORIGIN}x` finds `binx`, `$ORIGINx` exits 127). Anywhere else the
// entry passes through VERBATIM, which is exactly what gcc and ld64 do with it.

#include "core/export.hpp"
#include "core/types/enum_name_table.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dss {

// ── the CARRIER: which structure of an image records the list ──────────────
//
// A closed verb, like `weakDefinition.dialect`: a document names the structure
// and the loader refuses a verb that no walker writes, or that the resolved
// backend's walker does not write (`ObjectFormatBackend::runpathCarriers()`).
enum class RunpathCarrier : std::uint8_t {
    // Deliberately NOT in the table: a hand-built declaration left at its
    // default names no carrier, and `runpathDeclarationProblems` says so rather
    // than letting it read as an ELF declaration.
    Unspecified,
    // ONE `.dynamic` entry, tag `dynamicTag`, whose value is the `.dynstr`
    // offset of every recorded path joined by `separator`.
    ElfDynamicEntry,
    // ONE load command PER recorded path, command number `loadCommand`, in the
    // `rpath_command` shape (cmd, cmdsize, lc_str offset 12, the NUL-terminated
    // path, cmdsize padded to a multiple of 8).
    MachoLoadCommand,
};

inline constexpr EnumNameTable<RunpathCarrier, 2> kRunpathCarrierTable{{{
    { RunpathCarrier::ElfDynamicEntry,  "elf-dynamic-entry"  },
    { RunpathCarrier::MachoLoadCommand, "macho-load-command" },
}}};
DSS_CHECK_ENUM_NAME_TABLE(kRunpathCarrierTable);

[[nodiscard]] constexpr std::string_view
runpathCarrierName(RunpathCarrier c) noexcept {
    // The `-Werror=switch` backstop; it owns no spelling. `Unspecified` is left
    // out of the table, so it renders EMPTY (`nameOrEmpty`, never `name`).
    switch (c) {
        case RunpathCarrier::Unspecified:
        case RunpathCarrier::ElfDynamicEntry:
        case RunpathCarrier::MachoLoadCommand:
            break;
    }
    return kRunpathCarrierTable.nameOrEmpty(c);
}
[[nodiscard]] constexpr std::optional<RunpathCarrier>
runpathCarrierFromName(std::string_view s) noexcept {
    return kRunpathCarrierTable.fromName(s);
}

// The numbers a carrier's parameters may take — each the ONLY structure of its
// format whose body is a library search path. Named here, beside the verbs they
// qualify, so the loader and the writers' belts check one set.
inline constexpr std::uint64_t kElfDtRpath   = 15;           // gABI DT_RPATH
inline constexpr std::uint64_t kElfDtRunpath = 29;           // gABI DT_RUNPATH
inline constexpr std::uint32_t kMachoLcRpath = 0x8000001Cu;  // LC_RPATH = 0x1C | LC_REQ_DYLD

// A format's runpath declaration (`"runpath"` in its `.format.json`). Held in an
// `std::optional` on the schema: PRESENCE is the capability, absence means the
// format records nothing and a request against it is accepted with a warning.
struct DSS_EXPORT RunpathDeclaration {
    RunpathCarrier carrier = RunpathCarrier::Unspecified;
    // `elf-dynamic-entry` only: the `.dynamic` tag (DT_RUNPATH or DT_RPATH).
    std::uint64_t  dynamicTag = 0;
    // `elf-dynamic-entry` only: what the loader splits the ONE string on.
    std::string    separator;
    // `macho-load-command` only: the command number (LC_RPATH).
    std::uint32_t  loadCommand = 0;
    // Both: how THIS format spells the directory of the image that carries
    // the path — what a leading `${ORIGIN}` is written as.
    std::string    origin;
};

// ── WHY an image format records NO runpath: the remedy axis ────────────────
//
// `"runpathUnsupportedReason"` in a `.format.json` — the `stackReserve…Reason`
// shape: a closed verb an IMAGE format with no carrier declares, so the
// warning a request against it earns can say WHERE that format's loader looks
// for a library instead. Mutually exclusive with `runpath` (declaring both is
// contradictory config). Never declared by a relocatable object or an archive:
// that answer needs no declaration, because no loader ever loads one — the
// warning derives it from `isImageFlavor()`, a capability question.
// OPTIONAL: an image format that declares neither still ACCEPTS the request,
// just with a less specific warning.
enum class RunpathUnsupportedReason : std::uint8_t {
    Unspecified,                  // not in the table — renders empty
    // The loader searches the directory the APPLICATION was loaded from (then
    // the system directories and the search path), so a library placed beside
    // the executable is found with nothing recorded.
    ApplicationDirectorySearch,
};

inline constexpr EnumNameTable<RunpathUnsupportedReason, 1>
kRunpathUnsupportedReasonTable{{{
    { RunpathUnsupportedReason::ApplicationDirectorySearch,
      "application-directory-search" },
}}};
DSS_CHECK_ENUM_NAME_TABLE(kRunpathUnsupportedReasonTable);

[[nodiscard]] constexpr std::string_view
runpathUnsupportedReasonName(RunpathUnsupportedReason r) noexcept {
    switch (r) {   // the `-Werror=switch` backstop; it owns no spelling
        case RunpathUnsupportedReason::Unspecified:
        case RunpathUnsupportedReason::ApplicationDirectorySearch:
            break;
    }
    return kRunpathUnsupportedReasonTable.nameOrEmpty(r);
}
[[nodiscard]] constexpr std::optional<RunpathUnsupportedReason>
runpathUnsupportedReasonFromName(std::string_view s) noexcept {
    return kRunpathUnsupportedReasonTable.fromName(s);
}

// The sentence a warning carries for a declared reason: where the loader of a
// format that records no runpath finds a library instead.
[[nodiscard]] DSS_EXPORT std::string_view
runpathUnsupportedExplanation(RunpathUnsupportedReason r) noexcept;

// One problem with a declaration: the declaration KEY it concerns (so a
// document validator can point at `/runpath/<key>`) and why.
struct DSS_EXPORT RunpathDeclarationProblem {
    std::string_view key;
    std::string      message;
};

// ★ THE ONE RULE SET for a declaration's VALUES. The document loader's
// `validate()` runs it, and each writer runs it again before it records a
// single byte — a schema built in memory through the validation-free
// `ObjectFormatSchema{ObjectFormatData}` constructor never passes the loader,
// and two copies of these rules would drift. Empty ⇒ coherent. (Which keys a
// document may WRITE for each carrier is the loader's JSON-level question and
// not repeated here.)
[[nodiscard]] DSS_EXPORT std::vector<RunpathDeclarationProblem>
runpathDeclarationProblems(RunpathDeclaration const& decl);

// The portable token a runpath entry writes for "the directory of the image
// that carries this path". Recognised only as the leading component.
inline constexpr std::string_view kRunpathOriginToken = "${ORIGIN}";

// ── THE GATE RULE ──────────────────────────────────────────────────────────
//
// Why `entry` cannot be recorded by ANY carrier, or nullopt. This is the whole
// of what the linker gate refuses (`K_InvalidRunpathRequest`), and it is
// deliberately small, because the CLI takes gcc's literal semantics:
//   * an EMPTY entry — no reference makes one WORK: GNU ld stores `RUNPATH []`
//     and the program exits 127 even run from the library's own directory, and
//     clang's `-Wl,` splitter drops the empty argument so ld.lld takes the NEXT
//     argument as the path (measured, `RUNPATH [-lgcc]`);
//   * an entry holding a NUL byte — unwritable: both carriers store
//     NUL-terminated strings, so everything after it would be cut off.
// An unknown `${…}` is NOT refused: ld.so has its own tokens (`$LIB`,
// `$PLATFORM`), which gcc passes through and glibc expands.
[[nodiscard]] DSS_EXPORT std::optional<std::string>
runpathEntryRefusal(std::string_view entry);

// ── THE PORTABLE RULE (the project manifest's) ─────────────────────────────
//
// Why `entry` would NOT mean the same thing on every carrier, or nullopt. A
// manifest describes a project built for MANY targets, so it may say only what
// every loader reads alike:
//   * absolute (`/…`), or rooted at `${ORIGIN}` (the whole entry, or `${ORIGIN}/…`);
//     a RELATIVE entry is resolved against the PROCESS's working directory, not
//     the image's (measured: RUNPATH `lib` finds the library from the image's
//     directory and exits 127 from its parent), so it says nothing about the
//     image — and refusing it keeps `@loader_path` and `$ORIGIN` out as well;
//   * no `$` other than that leading token (musl IGNORES a whole RUNPATH holding
//     any other `$` — `fixup_rpath`, read at source — while glibc expands
//     `$LIB`/`$PLATFORM`, per ld.so(8));
//   * no `:` (ELF's loader splits the list on it, dyld keeps it as one path);
//   * and whatever the gate rule refuses.
// The refusal text names the CLI flag as the place for a loader-specific
// spelling, where gcc's literal semantics apply.
[[nodiscard]] DSS_EXPORT std::optional<std::string>
portableRunpathEntryRefusal(std::string_view entry);

// ── THE TRANSLATION ─────────────────────────────────────────────────────────
//
// The entries an image of this format records, in order: a leading
// `${ORIGIN}` written in the declaration's `origin` spelling, every other entry
// verbatim, and an entry EQUAL to one already recorded dropped (the first kept).
// Dropping is not cosmetic: dyld refuses an image carrying a duplicate LC_RPATH
// under its current enforcement policy (`"duplicate LC_RPATH '%s'"`, read at
// source), Apple's linker and GNU ld both drop duplicates, and on ELF a repeated
// directory is only searched twice. Entries are assumed to have passed
// `runpathEntryRefusal`.
[[nodiscard]] DSS_EXPORT std::vector<std::string>
recordedRunpaths(std::span<std::string const> requested,
                 RunpathDeclaration const&    decl);

} // namespace dss
