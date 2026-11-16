#pragma once

#include "core/export.hpp"
#include "core/substrate/host_path.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

// PATH IDENTITY — the one value that every spelling of one file reduces to.
//
// ★★★ WHY THIS IS A TYPE AND NOT A FUNCTION. A canonicalizing FUNCTION returning
// `fs::path` or `std::string` produces a value INDISTINGUISHABLE from a raw one,
// so every map keyed on it is one careless `insert(path.string())` away from
// silently holding two entries for one file. The only thing standing between
// correct and broken is then a lint, and a lint knows only the spellings it was
// taught: `fs::canonical`, `fs::absolute`, `GetFullPathNameW`, `_fullpath`, or a
// hand-rolled string compare all sail past a grep for `weakly_canonical`.
// ⇒ `PathIdentity` is constructible ONLY through `of()`. Converting OUT to a
// path for I/O or for a diagnostic stays free; construction IN is the one gate.
// A raw `fs::path` does not compile as a key of these maps — anywhere, forever,
// with no enumeration to maintain.
//
// ★★★ THE DEFECT THAT MADE THIS NECESSARY, MEASURED 2026-08-18. Every path
// identity in the compiler keyed on `fs::weakly_canonical`, which under
// libstdc++ (the toolchain this repo's Windows build actually uses) returns an
// 8.3 SHORT NAME UNCHANGED and WITH NO ERROR:
//
//     input            : C:\Users\<user>\AppData\Local\Temp\DSS-SC~1
//     exists           : yes
//     weakly_canonical : C:\Users\<user>\AppData\Local\Temp\DSS-SC~1   ec: <none>
//     canonical        : C:\Users\<user>\AppData\Local\Temp\DSS-SC~1   ec: <none>
//
// libstdc++ resolves `.`/`..` and symlinks and has no concept of an 8.3 alias,
// so two spellings of ONE directory survived as TWO keys. The MSVC STL happens
// to normalize them (`GetFinalPathNameByHandleW` under the hood) — which is why
// the CI failure this closes appeared fixed: the property was never HELD, only
// accidentally satisfied by one toolchain. The consequences were live and each
// is a known-bad class: a header reached through both spellings would be
// included TWICE past its `#pragma once` (include-stack re-entry keys on this),
// and one source listed both ways would produce a duplicate CU and a
// duplicate-symbol link error naming no manifest.
//
// ★★ AGNOSTICISM. The `#ifdef _WIN32` below is HOST filesystem behaviour, not
// source-language / target-CPU / object-format identity — the axis the bar
// governs. It sits beside `process_spawn.cpp` and `large_stack_call.cpp` for
// exactly that reason: these are the host-OS shims, and nothing here reads what
// is being COMPILED.

namespace dss::core {

// The GENERIC (forward-slash) spelling of `p` -- one separator convention for
// every path this compiler compares or prints as a key.
//
// ★★★ THIS EXISTS BECAUSE `path::generic_string()` IS LOSSY AND THE LOSS
// SILENTLY RENAMES THE FILE ([[D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED]]).
// ✔MEASURED with the toolchain that builds DSS (libstdc++ 13.2, MinGW), all
// three printed from the SAME path object:
//     .string()          '\\wsl.localhost\Ubuntu\home\<user>\p\uncprobe.h'
//     .generic_string()  '/wsl.localhost/Ubuntu/home/<user>/p/uncprobe.h'
//     exists()           true
// The leading separator RUN is collapsed to one, so the authority is demoted to
// an ordinary component and the result names a path on the local drive root
// instead. A per-character substitution of the model's OWN
// `preferred_separator` is the same normalisation without the loss.
//
// ★ NO PLATFORM MACRO AND NO UNC SPELLING. The `if constexpr` reads the path
// model's declared separator, so on a host whose separator already is `/` the
// substitution compiles away entirely -- and it must be that character and not
// a literal backslash, because a POSIX filename may legitimately CONTAIN one.
[[nodiscard]] DSS_EXPORT std::string
genericSpelling(std::filesystem::path const& p);

// The same answer for a path that is being carried as a STRING — a stored
// label, a config value, a document origin — rather than as an `fs::path`.
//
// ★ IT EXISTS SO A TIER WITH NO OTHER PATH BUSINESS DOES NOT HAVE TO REACH FOR
// `<filesystem>` JUST TO PRINT ONE. `GrammarSchema`/`TargetSchema` keep the
// label their document was loaded under; the assembly walker names it in a
// refusal. Without this overload that walker would have to construct a
// temporary `fs::path`, which means including `<filesystem>` in a file whose
// only relationship to the filesystem is that it renders a string — exactly the
// "path code arriving by habit" that `check-path-identity` exists to stop, and
// an allowlist entry that claimed an I/O need it does not have.
//
// ⚠ RENDERING ONLY. It is NOT a resolution, NOT a canonicalisation and NOT a
// way to make a string into an identity — `PathIdentity::of()` remains the only
// gate for that, and this returning a `std::string` (never a `PathIdentity`) is
// what keeps the two from being confused.
[[nodiscard]] DSS_EXPORT std::string genericSpelling(std::string_view spelling);

// The same spelling as UTF-8, for the one caller that cannot use the narrow
// form: a diagnostic that must NAME a file whose characters the active code
// page cannot represent.
//
// ★★★ WHY THIS IS OWED SEPARATELY RATHER THAN LEFT AS `generic_u8string()`.
// The two losses are INDEPENDENT and each has already been measured, so a
// caller can only avoid both by having a transform that avoids both.
// `generic_string()` throws on a name the code page cannot encode
// ([[D-PP-HEADER-CASE-NON-ASCII-NAME-NARROWING-THROW]]), and `u8` was the
// repair; but `generic_u8string()` performs the SAME generic-format conversion
// and so eats the leading separator run exactly as its narrow sibling does.
// ✔MEASURED 2026-08-28 on a REACHABLE UNC directory (`exists()` true), one path
// object, both renderings printed side by side:
//     .string()          '//localhost/C$/Source/DailySoftware'   run 2
//     .generic_string()  '/localhost/C$/Source/DailySoftware'    run 1
//     .generic_u8string()'/localhost/C$/Source/DailySoftware'    run 1
// A header-collision report that renames the machine is the same lie as one
// that cannot spell the filename; escaping one and not the other would have
// left the report wrong in the case it was rewritten for.
//
// ⚠ THROWS EXACTLY WHERE `u8string()` DOES, WHICH IS THE POINT. A native name
// can be text no encoding accepts (NTFS permits lone surrogates) and the caller
// owns a code-unit-by-code-unit fallback for it; swallowing the throw here
// would take that fallback away and hand back a name that is not the file's.
[[nodiscard]] DSS_EXPORT std::u8string
genericSpellingU8(std::filesystem::path const& p);

// THE ROOT OF A PATH'S SPELLING -- ONE parse, this one, for every path the compiler splits, normalizes, compares or
// walks up (P69 H6). The library's own root model cannot be asked: the two Windows libraries DISAGREE on every
// namespace prefix. ✔MEASURED 2026-11-04 (probe-reference-cc run 20261104-204243-d5c02d5f, both Windows legs, MinGW
// g++ 13.2.0 and cl 19.51.36260, one file spelled every way on the scratch's own volume): MS STL's `root_name()` is
// `\\?` for `\\?\C:\x` and `\\?\Volume{G}\x`, `\\.` for `\\.\C:\x`, `\\localhost` for `\\localhost\C$\x` (the share
// left in the relative path), `//?` for `//?/C:/x`; libstdc++'s is EMPTY for every one of them, `is_absolute()` false,
// and its `lexically_normal()` destroys the prefix (`\\?\` -> `\?\`, `\\.\` -> `\`, `\\localhost` -> `\localhost`).
//
// ★★ WHAT WIN32 ITSELF DOES WITH EACH PREFIX, which is what the parse states (✔MEASURED, the run above and run
// 20261104-205708-1394b8a2, cl 19.51, a handle opened on each spelling and GetFullPathNameW of it):
//   - `\\?\` (exactly that, four backslashes and `?`) is VERBATIM: the opened handle takes the rest LITERALLY -- `..`
//     opens nothing (ERROR_INVALID_NAME), nor `.` or `/` (ERROR_PATH_NOT_FOUND), nor a trailing dot or space
//     (ERROR_FILE_NOT_FOUND), nor a doubled separator (ERROR_INVALID_NAME) -- although GetFullPathNameW collapses every
//     one of them. The handle is what is reached, so nothing under `\\?\` is normalized here either.
//   - `\\.\`, and the forward-slash `//./` and `//?/`, are DEVICE paths the OS NORMALIZES -- and its `..` stops only at
//     the four-character prefix: `\\.\C:\..\x` is `\\.\x`, `\\.\GLOBALROOT\Device\V\..\x` is `\\.\GLOBALROOT\Device\x`,
//     `\\.\UNC\h\s\..\x` is `\\.\UNC\h\x`. A `..` under `//?/` opens (`//?/` is NOT verbatim).
//   - `\\server\share\` holds its `..` (`\\localhost\C$\..\x` opens `\\localhost\C$\x`), as `C:\` does.
//   - `UNC`, `Volume{G}` and `GLOBALROOT` are matched in any case (each lowercase spelling opens).
// So a spelling has TWO extents: the PREFIX a normalization keeps whole, and the VOLUME ROOT -- the directory every
// path below it lies in, where a walk up stops, with its separator: `C:\`, `\\server\share\`, `\\.\C:\`, `\\?\C:\`,
// `\\?\Volume{G}\`, `\\?\UNC\server\share\`, `\\?\GLOBALROOT\Device\HarddiskVolumeN\` (an object directory and the
// device in it). On POSIX the root is the separator run: `/`, and a run of exactly two kept apart (POSIX makes `//`
// implementation-defined), a run of three or more being `/` (POSIX: "shall be treated as a single slash").
// ⚠ BOUNDARY, NAMED: a GLOBALROOT spelling whose device is deeper than one name below its object directory (the
// network redirector's `\Device\Mup\server\share`) has a volume root wider than the parse's; no route of the
// compiler produces one (`reachedPath` asks the NT name only of a volume with no DOS name), and a walk on one asks
// the OS for the heads it passes, which refuses what it cannot open.
enum class PathGrammar : std::uint8_t {
    Win32,   // `\` and `/` separate; drive letters, shares and the `\\?\` / `\\.\` namespaces
    Posix,   // `/` separates; nothing else is a root
};
#ifdef _WIN32
inline constexpr PathGrammar kHostPathGrammar = PathGrammar::Win32;
#else
inline constexpr PathGrammar kHostPathGrammar = PathGrammar::Posix;
#endif

struct RootSpelling {
    enum class Kind : std::uint8_t {
        Relative,        // `x`, `..\x`: no root
        DriveRelative,   // `C:x`: a drive and THAT drive's working directory
        Rooted,          // `\x`: the current drive's root (Win32); `/x`, `///x` (POSIX)
        Drive,           // `C:\x`
        Share,           // `\\server\share\x` (Win32)
        Authority,       // `//x` (POSIX: implementation-defined, kept apart from `/x`)
        Device,          // `\\.\`, `//./`, `//?/`: a device path Win32 normalizes
        Verbatim,        // `\\?\`, and its NT twin `\??\`: a device path Win32 passes unparsed (P69 H6b, N4)
    };
    Kind        kind      = Kind::Relative;
    std::size_t prefix    = 0;      // code units a normalization keeps whole: what a `..` never climbs into
    std::size_t volume    = 0;      // code units of the volume root, its separator included when spelled
    bool        authority = false;  // names another machine: a share (also `UNC\` under a device prefix), POSIX `//`
};

// The root of `native` under `grammar` (both grammars are pure text, so either can be asked on any host).
[[nodiscard]] DSS_EXPORT RootSpelling rootSpelling(std::wstring_view native, PathGrammar grammar);
[[nodiscard]] DSS_EXPORT RootSpelling rootSpelling(std::string_view native, PathGrammar grammar);
// The root of `p` as this host's path model spells it.
[[nodiscard]] DSS_EXPORT RootSpelling rootSpelling(std::filesystem::path const& p);

// THE ONE STEP OF A WALK UP: the directory `p` lies in, spelled as `p` spells it, its volume root kept whole with its
// separator -- or EMPTY when `p` is its volume root (there is nothing above it to step to) or a relative spelling's
// last name. The library's `parent_path()` steps from `\\?\C:\x` onto `\\?\C:`, the volume's DEVICE, and from a
// share onto its bare server; `reachedPath`'s and the short-name expansion's walks take this same step.
[[nodiscard]] DSS_EXPORT std::filesystem::path parentWithinRoot(std::filesystem::path const& p);

// THE ONE NORMALIZATION of a spelling, as text in either grammar -- `normalizeKeepingRoot` is this under the host's.
// The root `rootSpelling` parses keeps its prefix whole (its separators the grammar's own; a device prefix its own
// `\\.\` or `\\?\`, whichever separators wrote it -- `//?/C:/x` is `\\?\C:\x` to the OS; a POSIX run of three or more
// one `/`); the rest is normalized in components -- empty ones and `.` dropped, a `..` taking the name before it and
// never climbing into the prefix (kept below a relative or drive-relative one, as `lexically_normal()` keeps it), and,
// in the Win32 grammar, each name trimmed as Win32 trims it: ONE trailing period from a name a separator follows,
// every trailing period and space from the last (✔MEASURED 2026-11-04, probe-reference-cc run
// 20261104-210843-61c9329b: `C:\d.\x` is `C:\d\x`, `C:\d .\x` is `C:\d \x`, `d..`, `d ` and `...` stay; `x.txt. .` is
// `x.txt`; a last `...` vanishes, its separator staying). A separator ends the result where the grammar's resolver
// leaves one: POSIX's `lexically_normal()` after a last `.` or a `..` that took a name too; Win32's only where the
// spelling ended in one or in a name the trim removed (`C:\d\.` is `C:\d`, `\\server\share\..` is `\\server\share`,
// `C:\a\b\..\` is `C:\a\`, `C:\d\..` is `C:\`). In the Win32 grammar a LAST name `NUL` (any case, every trailing
// period, space and `:` dropped) is the device `\\.\NUL` below every root but a share's and a device path's.
// ★ UNDER WIN32 THIS IS GetFullPathNameW's ANSWER, PINNED EQUAL to it on both Windows legs over F3's universe
// (✔MEASURED, runs 20261104-222037-6ef9b98e and 20261104-223135-5f04ee9f; the OS's side is path_identity.cpp's) --
// the model a host with no Win32 answers by; on a Windows host `normalizeKeepingRoot` asks the OS itself. A verbatim
// spelling comes back as written; an empty one empty; one with nothing left `.`.
[[nodiscard]] DSS_EXPORT std::wstring normalizedSpelling(std::wstring_view native, PathGrammar grammar);
[[nodiscard]] DSS_EXPORT std::string normalizedSpelling(std::string_view native, PathGrammar grammar);

// ONE NAME OF A TYPED RELATIVE, AS THE OS'S PLAIN RULE MAKES IT BY ITS POSITION BELOW `dir` (P69 H6c, B1) -- for a
// consumer that matches a typed name against a LISTING name by name (the header search), and so cannot compose the
// whole value through `HostPath::operator/`, which takes this same rule (`normalizedSpelling`'s trim) over the whole
// right side. Below a `dir` whose text is DOS, in the Win32 grammar: a MIDDLE name (one a separator follows) loses ONE
// trailing period, the LAST every trailing period and space. ✔MEASURED, H6c u4 (cl 19.52.37029, MinGW gcc 13.2.0, both
// Windows legs): both open `dir\config.h` for `#include "config.h."` (`"sub./x.h"` and `"x.h "` alike) with that twin
// there, and NOTHING with the literal alone; `"sub /x.h"` (a middle space, which the rule keeps) opens the literal
// `sub `. Below a VERBATIM `dir`, and in the POSIX grammar, the name as typed: no reference reads a typed name below a
// verbatim directory (cl and gcc open nothing below a verbatim `-I`, u4) and DSS takes the whole verbatim spelling's.
// A name the rule makes no name of -- trimmed to nothing (a last `...`), a `.` or `..`, or a DEVICE (a last `NUL`) --
// and a name holding a separator come back as typed (an include of a device: UNMEASURED).
enum class NamePosition : std::uint8_t { Middle, Last };
[[nodiscard]] DSS_EXPORT std::filesystem::path typedNameBelow(std::filesystem::path const& dir,
                                                              std::filesystem::path const& name,
                                                              NamePosition                 position);

// Whether the DOS spelling of a tail below a Win32 volume root names the file its verbatim (`\\?\`) spelling names:
// true unless the DOS normalization would rewrite it -- an empty component (a doubled separator), a `.` or `..`, a
// `/`, a component ending in a dot or a space, or a last component that is a reserved device name (`NUL`, `CON`, ...).
// ✔MEASURED for each kind (run 20261104-205708-1394b8a2): `C:\d\x.txt.`, `C:\d\x.txt ` and `C:\d\\x.txt` open
// `x.txt`, and their `\\?\` spellings open nothing; `C:\d\NUL` is `\\.\NUL` and `\\?\C:\d\NUL` is a file name
// (`nul.txt` is a file on this Windows 11). The reserved set beyond NUL is DOCUMENTED (Microsoft, "Naming Files,
// Paths, and Namespaces"), unmeasured.
[[nodiscard]] DSS_EXPORT bool dosSpellingNamesTheSameFile(std::wstring_view tail);

// ════ THE SPELLING A FILE CALL TAKES, AT ANY LENGTH (P69 H6, H6b; H6c: the TYPED home) ════
//
// INVARIANT: every host call on a path opens what the value NAMES, at any length, and no code outside
// src/core/substrate/ makes one (the filesystem-queries guard, tests/core/test_filesystem_queries_guard.cpp). Each
// operation below takes a `HostPath` (host_path.hpp) and fixes the USE it asks it in; the one question both uses can
// ask -- a kind, an identity, a reached path, a stored name, a write time, a sameness -- takes the `Use` as a
// parameter with no default.
//
// THE REFERENCE RULE (path_identity.cpp's `referenceSpelling`, its one home): a verbatim spelling as written; `\??\` as
// its `\\?\` twin (N4: ✔MEASURED 2026-11-05, every file call reads `\??\` verbatim, GetFullPathNameW reads it rooted,
// CreateDirectoryW takes only `\\?\` past MAX_PATH, CreateProcessW refuses it, 267); otherwise the OS's full spelling
// (GetFullPathNameW) -- of `text + \` for a CONTAINER -- prefixed `\\?\`, `\\?\UNC\` for a share, `\\?\` for a device
// path's `\\.\` (one NT namespace: ✔MEASURED, run 20261104-215748-2946f6a6). A plain spelling of 260 characters or
// more opens nothing on a Windows that has not opted into long paths, in every Win32 call and every std::filesystem
// operation of both libraries, where `\\?\` opens at any length (✔MEASURED 2026-11-04, runs 20261104-215014-7ba4f08f,
// 20261104-215144-4893b98f, 20261104-215351-24246ad8) -- so a reference is handed to Win32 calls, streams and the C
// runtime, and NEVER to a std::filesystem operation: libstdc++'s `directory_iterator` over a long `\\?\` directory
// returns a wrong entry count with no error, and its `status`, `exists`, `file_size`, `create_directories` and
// `remove` fail on `\\?\UNC\` at any length.
// ⚠ A LEAF'S RESOLUTION IS NOT A WALK'S: GetFullPathNameW trims ONE trailing period from every segment that ends in a
// single one, but every trailing period and space, and a device name, from the LAST alone (✔MEASURED over the 256
// walk-up cells, runs 20261105-062441-7bd17744 / 062514-83af17c8 / 062546-4ccf610b / 062603-16841970, and the 270
// listed cells of H6c, runs 20261113-212927-50669bb8 / 212941-deb1d8ec). So a walk never re-resolves a level: it cuts
// the whole's reference LITERALLY, and a value made from a listing keeps its references fixed.

// THE DIRECTORY `file` IS IN -- the ONE cut (H6c, A9): the level above the last name of `file`'s LEAF reference (a
// GIVEN value's taken now by the rule, a made value's fixed one), cut literally -- only `\` separates under `\\?\`,
// and a volume root keeps its separator. Never the library's `parent_path()` of the text: re-resolved as a whole that
// is another directory when its last name ends in a space or is a device name, and at `\\?\D:\a.exe` it is `\\?\D:`,
// the volume's DEVICE (✔MEASURED, H6c u2 part 5: attributes ERR 87, open ERR 5). Its text is lossless (the DOS form
// exactly where that names the same directory). EMPTY at a volume root (there is no level above). POSIX: the text's
// level within its root (`parentWithinRoot`).
[[nodiscard]] DSS_EXPORT HostPath containingDirectory(HostPath const& file);

// The file-system OPERATIONS a site would otherwise ask of std::filesystem or of the OS, at any length: Win32 on
// Windows through the value's reference, std::filesystem or POSIX elsewhere. Each answers as the std::filesystem call
// of the same name does, `ec` included; the EMPTY spelling as a leaf is refused, `std::errc::invalid_argument` (H6c
// B2: GetFullPathNameW refuses "", ERR 123, and so does a spawn in "").
//   * `createDirectories`: every missing directory of `p`; true when it made one; a level that exists as a file is
//     an error (ERROR_ALREADY_EXISTS on Windows, as the library's EEXIST). `p` IS A CONTAINER (P69 H6b): it makes the
//     directory every `p \ name` is written into -- the library's create_directories resolves `p` as a leaf and makes
//     ANOTHER directory for a last name ending in a space, the device for `NUL` (✔MEASURED: 50 of 224 cells made
//     another directory, 52 none). And a level reported made is CONFIRMED a directory, opened as one: CreateDirectoryW
//     of a level below a device answers MADE and makes nothing (✔MEASURED, H6c u2 part 5: `\\?\NUL\x`, `\\.\NUL\x`,
//     `\\?\GLOBALROOT\Device\Null\x`), which is refused here, never passed as made.
//   * `createDirectoriesFor`: every missing directory `file` is to be written into -- `createDirectories` of
//     `containingDirectory(file)`; false, `ec` clear, when that is no level (a volume root's file).
//   * `removeEntry`: the file, link or EMPTY directory at `p` (a link itself, never what it reaches); false, with `ec`
//     clear, when nothing is there; a read-only file is removed as MS STL's `remove` removes one.
//   * `copyFile`: the file `from` reaches, copied to a NEW file `to` -- refused when `to` exists, as
//     `fs::copy_file` under `copy_options::none`.
//   * `openFileStream`: a C stream on `p`, opened as `std::fopen(p, mode)` opens it (`_wfopen` of the reference on
//     Windows, which takes it under both libraries); null when the open is refused, `errno` as it left it.
//   * `openInputStream` / `openOutputStream`: a C++ stream on `p` -- the reference, which `std::ifstream` and
//     `std::ofstream` take at any length under both libraries (✔MEASURED, the runs above); check `is_open()`.
[[nodiscard]] DSS_EXPORT std::FILE* openFileStream(HostPath const& p, char const* mode);
[[nodiscard]] DSS_EXPORT std::ifstream openInputStream(HostPath const& p, std::ios::openmode mode = std::ios::in);
[[nodiscard]] DSS_EXPORT std::ofstream openOutputStream(HostPath const& p, std::ios::openmode mode = std::ios::out);
[[nodiscard]] DSS_EXPORT bool createDirectories(HostPath const& p, std::error_code& ec);
[[nodiscard]] DSS_EXPORT bool createDirectoriesFor(HostPath const& file, std::error_code& ec);
[[nodiscard]] DSS_EXPORT bool removeEntry(HostPath const& p, std::error_code& ec);
DSS_EXPORT void copyFile(HostPath const& from, HostPath const& to, std::error_code& ec);

#ifdef _WIN32
// THE HANDLE CALLS a site makes on a path (H6c, A4) -- the writer's exclusive create, its holder query and its staged
// file's open, the dependency cache's lock: CreateFileW of `p`'s LEAF reference with the caller's own access, share,
// disposition and flags (the HANDLE as `void*`, INVALID_HANDLE_VALUE and GetLastError when refused; the caller owns
// and closes it); GetFileAttributesW of it (INVALID_FILE_ATTRIBUTES and GetLastError when refused); and the COMMIT BY
// HANDLE: SetFileInformationByHandle FileRenameInfoEx (POSIX semantics) or FileRenameInfo of the file behind `staged`
// -- a handle with DELETE access -- to `target`'s LEAF reference, replacing what is there (0, or the Win32 error;
// never 0 for a refusal). The rename's name is resolved as MoveFileExW resolves one -- forward slashes, a bare
// relative name and a `..` land against the current directory (✔MEASURED, the writer's measurement) -- so it is
// handed the reference, which names `target` at any length.
[[nodiscard]] DSS_EXPORT void* openHandle(HostPath const& p, unsigned long access, unsigned long share,
                                          unsigned long disposition, unsigned long flagsAndAttributes);
[[nodiscard]] DSS_EXPORT unsigned long fileAttributes(HostPath const& p);
[[nodiscard]] DSS_EXPORT unsigned long commitByHandle(void* staged, HostPath const& target, bool posixSemantics);
#else
// THE DESCRIPTOR CALL a site makes on a path (H6c, A4): `open(2)` of `p` with the caller's flags and mode -- the
// writer's exclusive create, the dependency cache's lock -- the descriptor, or -1 with `errno` as `open` left it.
[[nodiscard]] DSS_EXPORT int openDescriptor(HostPath const& p, int flags, unsigned mode);
// `renameEntry`: `from` renamed to `to`, replacing a file there (`rename(2)`). POSIX alone: on Windows the one rename
// DSS commits is the writer's, BY HANDLE (`commitByHandle`).
DSS_EXPORT void renameEntry(HostPath const& from, HostPath const& to, std::error_code& ec);
#endif

// THE OS'S ANSWERS a site would otherwise ask of std::filesystem or Win32 (H6c), each an ANSWERED value: its reference
// fixed to the answer, never resolved again, its text lossless.
//   * `currentDirectory`: the process's working directory (GetCurrentDirectoryW; `fs::current_path` elsewhere);
//   * `temporaryDirectory`: the directory for temporary files (GetTempPath2W where the OS has it, else GetTempPathW;
//     `fs::temp_directory_path` elsewhere);
//   * `runningExecutable`: the image this process runs. ★ On Windows QueryFullProcessImageNameW, NEVER
//     GetModuleFileNameW, which names the spelling the CREATOR handed, normalized only under `\\?\`, not the image the
//     OS opened (✔MEASURED, H6c u3 part 5, both Windows libraries: started verbatim from a listed `e.` directory the
//     literal image runs and the module name is the TWIN's, `\\?\...\e\u3.exe`; started plain, the twin's image runs
//     and the module name is `e.`). Its answer is a DOS text even for a literal `e.` image, so it is taken PREFIXED
//     (`\\?\`, `\\?\UNC\` for a share) and never resolved -- resolved by the rule, it would be the twin again.
//     ⚠ UNMEASURED, named: a child's DLL search from its application directory, INFERRED to take the same
//     normalized name. `/proc/self/exe` on Linux, `_NSGetExecutablePath` on macOS.
[[nodiscard]] DSS_EXPORT HostPath currentDirectory(std::error_code& ec);
[[nodiscard]] DSS_EXPORT HostPath temporaryDirectory(std::error_code& ec);
[[nodiscard]] DSS_EXPORT HostPath runningExecutable(std::error_code& ec);

// `fs::absolute` WITHOUT letting it invent a drive for a path that already names
// an AUTHORITY or a NAMESPACE (`rootSpelling`: a share, POSIX `//`, a device or verbatim prefix). Same invariant as
// `genericSpelling` above and as `normalizeKeepingRoot`: never change the root.
//
// ★★★ THE DEFECT THIS EXISTS FOR, ✔MEASURED 2026-08-28 on the toolchain that
// builds DSS (libstdc++ 13.2, MinGW-w64 UCRT), printed from one path object:
//     input        : //wsl.localhost/Ubuntu/home/<user>/p44_unc_inc
//     is_absolute  : false        has_root_name : false   root_name : <empty>
//     absolute()   : C:\wsl.localhost\Ubuntu\home\<user>\p44_unc_inc
//     exists(input): true         exists(absolute()) : false
// It does NOT merely prepend the cwd -- it RE-ROOTS a path naming another
// machine onto the local drive, silently and with no error. This path model
// gives a UNC authority no `root_name()` of its own, so `is_absolute()` is false
// and `absolute()` "repairs" it by supplying a drive that was never asked for.
//
// ★★ THE DISCRIMINATOR IS THE ROOT'S KIND (`rootSpelling`), NOT `is_absolute()` AND NOT
// `isRootedPath`. A root of ONE separator (`/foo`) genuinely IS a location on the current
// drive and MUST keep going through `fs::absolute`, which pins the drive that
// was current at resolution time -- skipping it there would be a regression, not
// a fix; so must a drive-relative `C:x`, which names that drive's working directory. A share, POSIX `//`, and a
// device or verbatim prefix (`\\.\…`, `\\?\…`) have no meaning to re-root.
//
// ⚠ NOT A WINDOWS-ONLY GUARD. On a model where a UNC path is already absolute,
// `fs::absolute` returns it unchanged, so skipping the call is a no-op there and
// the two hosts agree by construction rather than by a platform branch.
//
// `ec` follows `fs::absolute`: cleared on the preserved path (nothing can fail),
// otherwise whatever the call reports. Callers keep their existing
// on-failure-use-the-raw-path idiom unchanged.
// ★ H6c (A6): a VALUE resolved by `use`, answered as a value -- no exported function resolves plain text.
[[nodiscard]] DSS_EXPORT HostPath absoluteKeepingRoot(HostPath const& p, Use use, std::error_code& ec);

// `lexically_normal()` WITHOUT letting it eat the leading separator RUN — the
// third member of this file's trio, and the reason all three are exported
// together: a path survives a UNC round trip only if EVERY transform applied to
// it preserves the run, and there are exactly three transforms that do not.
//
// ★★★ ✔MEASURED 2026-08-28, and it is why exporting this was owed. The
// artifact-written report ran ONE path through all three
// (`absolute` -> `lexically_normal` -> `generic_string`) and each removed one
// separator. Fixing only `absolute` moved the reported path from
// `C:\host\share\…` to `/host\share\…`: still wrong, still naming the local
// drive, and now wrong for a DIFFERENT reason. A partial fix here reads exactly
// like a complete one, because both spellings are equally absent from disk.
//
// ★★ BY THE ROOT'S KIND (`rootSpelling`, whose header states each measurement): a VERBATIM spelling comes back
// UNCHANGED -- Win32 opens its rest literally, so a `..`, a `.` or a `/` there is a name, never a step; a SHARE keeps
// `\\server\share\` whole and a DEVICE path keeps its four-character prefix whole (its own `.` or `?`, its separators
// the OS's), and the rest is normalized in components, its `..` never climbing into what is kept -- what the OS does
// with each (`\\.\C:\..\x` is `\\.\x`). Every kind of root takes `normalizedSpelling` under the host's grammar -- the
// library's `lexically_normal()` keeps what Win32 trims and climbs where the OS does not -- and ON A WINDOWS HOST an
// absolute plain spelling (a drive, a share, a device path) is the OS's own answer, GetFullPathNameW
// (asked in path_identity.cpp), which that normalization is pinned equal to wherever every Windows release agrees;
// a relative, rooted or drive-relative one stays the text's, never resolved against a directory here.
// ★ H6c (A6): a VALUE resolved by `use`, answered as a value -- no exported function resolves plain text.
[[nodiscard]] DSS_EXPORT HostPath normalizeKeepingRoot(HostPath const& p, Use use);

// Reduce `p` to its identity. Never throws and never returns empty: a path that
// cannot be resolved degrades to its lexically-normal form, because a degraded
// key is strictly better than an exception out of a resolver and its only cost
// is that two exotic spellings might read as two nodes (a duplicate build, never
// a wrong one).
//
// ⚠ DELIBERATELY `weakly_canonical` AND NOT `canonical`: identity must be
// computable for a path that DOES NOT EXIST YET. Output artifacts, a `path`
// dependency resolved before anything confirmed the directory, and the cycle key
// needed for the reject's own message are all keyed before the file is there.
// ★ H6c (A2): the identity of `p` as `use` names it -- a directory used as a container is asked as one.
[[nodiscard]] DSS_EXPORT std::string canonicalIdentityKey(HostPath const& p, Use use);

// Same, but REPORTING the underlying resolution failure instead of
// swallowing it. `ec` is set only by the filesystem refusing to answer (an
// unreadable parent) -- NEVER by the path merely not existing yet, and never
// by the short-name expansion, whose failure is the ordinary
// does-not-exist-yet case. The returned key is usable either way.
[[nodiscard]] DSS_EXPORT std::string canonicalIdentityKey(HostPath const& p, Use use, std::error_code& ec);

class PathIdentity {
public:
    // THE ONLY CONSTRUCTOR. See the header note: this is the gate.
    [[nodiscard]] static PathIdentity of(HostPath const& p, Use use) {
        return PathIdentity{canonicalIdentityKey(p, use)};
    }
    // For callers that must FAIL LOUD on a filesystem refusal rather than
    // degrade. See `canonicalIdentityKey(p, ec)`.
    [[nodiscard]] static PathIdentity of(HostPath const& p, Use use, std::error_code& ec) {
        return PathIdentity{canonicalIdentityKey(p, use, ec)};
    }

    // Converting OUT is free — for I/O, and for a diagnostic an operator reads.
    //
    // ★★★ PREFERRED SEPARATORS, DELIBERATELY, AND THE KEY KEEPS GENERIC ONES.
    // The two answer different questions: `string()` is the IDENTITY and must be
    // uniform, while `path()` exists to be handed BACK to the filesystem, to a
    // spawned process, or to an operator. ✔MEASURED 2026-08-18: returning the
    // generic form here broke three native-probe tests with
    // `'""C:' is not recognized as an internal command` — the probe builds a
    // `.bat` path from a canonicalized base plus a natively-joined suffix, and
    // `cmd.exe` cannot run a forward-slashed path. ⚠ NOTHING ELSE NOTICED,
    // because the Win32 APIs accept `/` perfectly well; only the shell does not.
    [[nodiscard]] std::filesystem::path path() const {
        return std::filesystem::path{key_}.make_preferred();
    }
    [[nodiscard]] std::string const& string() const noexcept { return key_; }
    [[nodiscard]] std::string_view    view() const noexcept { return key_; }

    friend bool operator==(PathIdentity const& a,
                           PathIdentity const& b) noexcept {
        return a.key_ == b.key_;
    }
    friend std::strong_ordering operator<=>(PathIdentity const& a,
                                            PathIdentity const& b) noexcept {
        return a.key_ <=> b.key_;
    }

private:
    explicit PathIdentity(std::string key) : key_(std::move(key)) {}
    std::string key_;
};

// ════ THE KIND OF A DIRECTORY ENTRY ITSELF, AND THE WALK AND THE REMOVAL THAT ASK IT ════
//
// ★★★ ON WINDOWS THE OS IS ASKED, BECAUSE ONE OF THE TWO LIBRARIES THAT BUILD DSS THERE CANNOT
// TELL A LINK FROM WHAT IT REACHES. ✔MEASURED 2026-10-20 (one probe, both libraries, on one
// Windows host; every link made by the OS's own call): what `symlink_status` answers --
//     entry                     the OS (attributes, reparse tag)      MS STL      libstdc++ 13.2 (MinGW-w64 UCRT)
//     directory symbolic link   reparse + directory, SYMLINK          symlink     directory
//     dangling file link        reparse, SYMLINK                      symlink     regular
//     junction                  reparse + directory, MOUNT_POINT      junction    directory
// libstdc++ on Windows has no `lstat`: `is_symlink` is never true there, `create_symlink` and
// `create_directory_symlink` fail "Function not implemented", and `recursive_directory_iterator`
// DESCENDS a directory link and a junction that MS STL's never descends. So one source built two
// ways gave two answers: a walk built with MinGW read another tree (or this one again) through a
// link, and a removal emptied a link's TARGET. The OS's own reading -- the entry's attributes and,
// for a reparse point, its tag -- is one answer for both builds, and it is the one MS STL gives: a
// SYMLINK or a MOUNT_POINT (junction) tag is a LINK; any other reparse point (an App Execution
// Alias, a cloud placeholder) is what its attributes say. Off Windows, `symlink_status` already is
// that reading.
//
// ⚠ A HOST-OS BRANCH, like the rest of this file: it reads the host's filesystem, never what is
// being compiled.
enum class EntryKind : std::uint8_t {
    Absent,     // nothing is there
    Directory,  // a real directory: the only kind a walk descends and a removal empties
    Link,       // a symbolic link or a junction: never followed, removed as itself
    File,       // a regular file (on Windows also any reparse point that is neither of the above)
    Other,      // a device, a FIFO, a socket: an entry that is no file to read (POSIX)
};

// The kind of the entry AT `p` -- never of what a link there reaches. Absence is `Absent` with
// `ec` clear; `ec` is set only when the entry cannot be examined, and the answer is then `Other`,
// the kind nothing descends, empties or reads.
[[nodiscard]] DSS_EXPORT EntryKind entryKind(HostPath const& p, Use use, std::error_code& ec);

// The kind of what `p` REACHES, every link on the way followed: never `Link`, and `Absent` for a
// dangling link. `ec` is set only when what it reaches cannot be examined (a link cycle, a refusal),
// and the answer is then `Absent`: nothing usable is reached.
[[nodiscard]] DSS_EXPORT EntryKind reachedKind(HostPath const& p, Use use, std::error_code& ec);

// Whether `p` REACHES a NAMED PIPE (a FIFO), every link on the way followed: the one `Other` a reader takes for an
// `#embed` resource, a device staying refused there (Apple clang 21.0.0 refuses one, "device files are not yet
// supported by '#embed' directive", and embeds a fed pipe -- ✔MEASURED 2026-10-28, macos-arm64-debug, run
// 20261029-002044-5101633a; gcc 13.3 and clang 18.1.3 have no #embed). `false`, `ec` clear, for anything else and
// on Windows, whose directories record no FIFO kind. `ec` is set only when what `p` reaches cannot be examined.
[[nodiscard]] DSS_EXPORT bool reachedPipe(HostPath const& p, std::error_code& ec);

// The size in bytes of the regular file `p` REACHES, every link on the way followed -- or nothing,
// when it reaches no regular file (nothing there, a dangling link, a directory, a special file). `ec`
// is set only when what it reaches cannot be examined, and the answer is then nothing.
// ✔MEASURED (2026-08-29, the include-once comment of preprocessor.cpp; 2026-10-20): libstdc++ on
// Windows sizes a file link by the LINK's own attributes -- `fs::file_size` of a symlinked file is 0 --
// so how many bytes a path names is asked here, never of the library.
[[nodiscard]] DSS_EXPORT std::optional<std::uintmax_t> reachedFileSize(HostPath const& p,
                                                                       std::error_code& ec);

// The path `p` REACHES, every link on the way resolved, absolute and normal: the longest leading part
// that exists is resolved, and the rest is appended lexically -- `weakly_canonical`'s answer, asked
// of the OS on Windows (the final path of a handle opened without `FILE_FLAG_OPEN_REPARSE_POINT`).
// ✔MEASURED 2026-08-29 (the include-once comment of preprocessor.cpp): libstdc++ on Windows resolves
// no link -- its `weakly_canonical` and `canonical` hand a link back unchanged -- so where a link
// leads is asked here, never of the library. `ec` is set when the resolution fails, and the answer
// is then empty.
// ★ A VOLUME WITH NO DOS NAME (no drive letter, no folder it is mounted in) is reached through its NT
// name, `\?\GLOBALROOT\Device\...` -- MS STL's `canonical` answer there. ✔MEASURED 2026-10-28 (one
// probe, both Windows libraries, a host with two such volumes): the final path's DOS name is refused
// (ERROR_PATH_NOT_FOUND) and its NT name given; MS STL's `canonical` answers the GLOBALROOT spelling,
// while its own `weakly_canonical` REFUSES a tail that does not exist there (error 1), and MinGW's
// library re-roots such a path onto the C: drive. So this answers where MS STL's `weakly_canonical`
// does not: the existing part as `canonical` spells it, the rest appended as written.
// ★ THE WALK UP STOPS AT THE VOLUME ROOT `rootSpelling` parses, with its separator -- never one step past it onto the
// volume's DEVICE (`\\?\C:` without its separator opens the device: no file information, no final path); the walk
// splits a verbatim spelling at `\` alone, `/` being a character there. The answer is spelled in the DOS form
// (`C:\...`, `\\server\share\...`) exactly when that form names the same file (`dosSpellingNamesTheSameFile`), and
// in the OS's verbatim form otherwise.
[[nodiscard]] DSS_EXPORT HostPath reachedPath(HostPath const& p, Use use, std::error_code& ec);

// The path `p` REACHES, spelled relative to the path `base` REACHES -- every link on either side resolved
// (`reachedPath`), then compared lexically: `std::filesystem::relative`'s answer, which the standard defines
// as `weakly_canonical(p).lexically_relative(weakly_canonical(base))`. Empty when no relative spelling
// exists (another root -- the two reached paths' volume roots differ by `rootSpelling`, never by the library's
// `root_name()`, under which MinGW reads `\\a\s\x` and `\\b\s\y` as one root); `ec` is set when either resolution
// fails, and the answer is then empty.
// ✔MEASURED 2026-10-21 (one probe, both Windows libraries, links made by the OS): libstdc++ on Windows
// answers `relative` WITHOUT resolving a link -- `base/lnk/x.txt` from `base`, `lnk` naming a directory
// outside it, is `lnk/x.txt` there and `../outside/x.txt` under MS STL -- so where one path lies from
// another is asked here, never of the library.
// ★ H6c (A2): `p` as `use` names it, against `base` as a CONTAINER (fixed: a base is the directory the answer is
// spelled below). The answer is TEXT -- a relative spelling, for a label or a key.
[[nodiscard]] DSS_EXPORT std::filesystem::path reachedRelative(HostPath const& p, Use use, HostPath const& base,
                                                               std::error_code& ec);

// The time the file `p` REACHES was last written, every link on the way followed -- or nothing, when
// nothing is reached (nothing there, a dangling link); `ec` is set only when what it reaches cannot be
// examined, and the answer is then nothing.
// ✔MEASURED 2026-10-21 (one probe, both Windows libraries, every link made by the OS): libstdc++ on
// Windows answers `last_write_time` of a file link with the LINK's own time (0 days old, beside a
// target 1825 days old) where MS STL and libc++ answer the target's -- so when a file was written is
// asked here, never of the library.
[[nodiscard]] DSS_EXPORT std::optional<std::filesystem::file_time_type>
reachedWriteTime(HostPath const& p, Use use, std::error_code& ec);

// Whether `a` and `b` REACH one file -- every link on the way followed, a hard link to it included: the
// question `std::filesystem::equivalent` answers. Something absent on either side is no file: `false`,
// `ec` clear. `ec` is set only when what either reaches cannot be examined, and the answer is then
// `false`.
// ✔MEASURED 2026-10-21: of a DANGLING link libstdc++ on Windows answers `equivalent` with no error,
// MS STL and libc++ with one -- so this one answer is given on every build.
[[nodiscard]] DSS_EXPORT bool reachedSameFile(HostPath const& a, HostPath const& b, Use use,
                                              std::error_code& ec);

// Add `add` to the permissions of the file `p` REACHES, every link on the way followed (on Windows, as
// MS STL does: only the write bits mean anything there -- the read-only attribute; the others leave the
// file as it is). `ec` is set when nothing is reached or the change is refused.
// ✔MEASURED 2026-10-21: `permissions` through a link changes the TARGET under MS STL and libc++ and the
// LINK's own attribute under libstdc++ on Windows.
DSS_EXPORT void addReachedPermissions(HostPath const& p, std::filesystem::perms add,
                                      std::error_code& ec);

// ════ AN ENTRY'S STORED NAME AND A DIRECTORY'S CASE RULE, ASKED WITHOUT THE DIRECTORY'S LISTING ════
//
// A directory may grant a lookup by name and refuse its LISTING (POSIX search permission without read, mode 0300; a
// Windows ACL denying FILE_LIST_DIRECTORY). gcc and clang take a header there by name, so a search that verifies the
// case of a name asks the ENTRIES instead of the listing. ✔MEASURED 2026-10-28 (probe-reference-cc, the listing
// refused, `x.h`, a directory `Sub` and a file link `L.h` on disk, each looked up under the other case):
//     host                       the entry's stored name                       the directory's own case rule
//     Windows: cl 19.51 and      GetFinalPathNameByHandleW of a handle to the  FileCaseSensitiveInfo: 0 (folds); 1
//       MinGW-w64 g++ 13.2       entry itself: x.h, Sub, L.h -- DOS and NT     once a user made the directory
//                                volume names alike                            case-sensitive (A.H then not found)
//     macOS: AppleClang 21       getattrlist(ATTR_CMN_NAME, FSOPT_NOFOLLOW):   pathconf(_PC_CASE_SENSITIVE): 0
//                                x.h, Sub, L.h -- and z.h and Shut for a
//                                mode-0 file and a mode-0300 directory, whose
//                                OPEN is refused (F_GETPATH, which needs a
//                                descriptor, fails there)
//     Linux: gcc 13.3            no call tells it: /proc/self/fd and realpath  no call short of reading the
//                                answer the name asked for                     directory, which it refuses
// (runs 20261029-001355-a0213885, 20261029-003341-e5d4743a, 20261029-003600-81043948, 20261029-003902-59348b54,
// 20261029-003836-60f9e5a7, 20261029-001147-d875516c, 20261029-003230-414c4aaf, 20261029-001240-d119f0ff,
// 20261029-003249-5ebd90b8.) On Linux every other-case spelling measured was not found: its directories keep case.
// ⚠ A HOST-OS BRANCH, like the rest of this file: it reads the host's filesystem, never what is being compiled.

// Whether `a` and `b` name ONE directory entry -- the entry itself, a link there NOT followed: two spellings of one
// name in a directory that folds case. Something absent on either side is no entry: `false`, `ec` clear. `ec` is set
// only when either cannot be examined.
[[nodiscard]] DSS_EXPORT bool sameEntry(HostPath const& a, HostPath const& b, Use use,
                                        std::error_code& ec);

// The name the entry AT `p` -- its last component, a link there NOT followed -- is STORED under in its directory, asked
// of the entry and never of a listing (the table above). Nothing, `ec` clear, when nothing is there and on a host that
// has no such question (Linux). `ec` is set when the entry cannot be examined.
[[nodiscard]] DSS_EXPORT std::optional<std::filesystem::path::string_type>
storedEntryName(HostPath const& p, Use use, std::error_code& ec);

// Whether the directory `dir` reaches folds the case of the names it is asked for -- its own rule, asked of the
// directory and never of an entry in it (the table above). Nothing, `ec` clear, on a host that has no such question
// (Linux). `ec` is set when the directory cannot be examined.
[[nodiscard]] DSS_EXPORT std::optional<bool> directoryFoldsCase(HostPath const& dir, std::error_code& ec);

// One entry a walk yields.
// ★ H6c (R1, A1): the entry is a LISTED VALUE -- its references fixed to the listing, literally, never re-resolved --
// and its names below the root a MATCHING KEY that composes only through `HostPath::operator/`. A walk's entry
// handed out as plain text (`root / name`) named a listed `e.` as `e` the moment any call re-resolved it (the review
// of H6b, R1); its `storeRoutePath` is that text, kept for the STORE ROUTE alone (the block at the end of this
// header).
struct TreeEntry {
    HostPath       at;          // the entry, as listed
    ListedRelative relative;    // its names below the walked root, as listed
    EntryKind      kind;        // the entry itself (`Absent`: it went away while it was listed)
    EntryKind      reaches;     // `kind`, except a Link: what it reaches (`Absent` if dangling)
    std::size_t    depth;       // 0 for an entry directly under the root
    // A Link whose reach could not be examined (a loop, a refusal): why. `reaches` is then `Absent`.
    // Such an entry reaches a visitor only when the options hand it out; by default it ENDS the walk.
    std::error_code reachError{};
    // THE STORE ROUTE'S TEXT FORM (the block at the end of this header): the walked root's text, then each listed
    // name -- the entry's text exactly as before H6c. Fenced to its pinned sites; H6d deletes it.
    std::filesystem::path storeRoutePath{};
};

// What a walk does after the visitor has seen an entry: go on (descending it if it is a Directory),
// go on WITHOUT descending it, or end the walk.
enum class WalkStep : std::uint8_t { Continue, Prune, Stop };

// ★ THERE IS NO OPTION TO PASS OVER A DIRECTORY THE WALK CANNOT LIST: a walk that skipped one in silence
// answered for a tree it did not read (the knob `skip_permission_denied` was, and no caller keeps it).
struct TreeWalkOptions {
    // A link whose reach cannot be examined is handed to the visitor with its `reachError` set, instead
    // of ending the walk -- for a visitor that answers it itself. Off by default: a walk that cannot
    // tell what a link reaches did not read the tree it was asked about, and says so.
    bool handOutUnreadableLinks = false;
};

// What the walk found at its ROOT (H6c, A2 and N10): the walk reports it, so no caller asks the kind of what it walks
// first -- and in a host-neutral word, never an OS error code (ENOENT against ERROR_PATH_NOT_FOUND / ERROR_DIRECTORY).
enum class WalkRoot : std::uint8_t {
    Listed,          // a directory, listed
    Absent,          // nothing is there (`error` set: no_such_file_or_directory)
    NotADirectory,   // something that is no directory (`error` set: not_a_directory)
    Unreadable,      // a directory the walk could not list, or a root it could not examine (`error`: the OS's)
};

struct TreeWalk {
    std::error_code error;    // set: the walk ended there and did NOT read the whole tree
    HostPath        where;    // the directory it could not list (the root itself when that failed),
                              // or the link whose reach it could not examine
    bool            stopped = false;   // the visitor ended it
    WalkRoot        root    = WalkRoot::Listed;
    // THE STORE ROUTE'S TEXT FORM of `where` (the block at the end of this header): its text exactly as before H6c.
    // Fenced to its pinned sites; H6d deletes it.
    std::filesystem::path storeRouteWhere{};
};

// EVERY entry under `root` (the root itself is not yielded), each directory's entries before the
// next sibling of that directory, descending only a `Directory` the visitor did not prune -- NEVER
// a link. Iterative: the work is an explicit stack of listed directories on the heap, so a deep
// tree costs memory, never call stack. The first directory it cannot list ends the walk (`error`,
// `where`), and so does a link whose reach it cannot examine unless the options hand it out; the
// order within a directory is the filesystem's.
// ★ ITS LISTINGS NAME THE DIRECTORIES THE ENTRIES ARE IN (P69 H6b, F3; H6c): `root` is a CONTAINER, listed by its
// container reference, and a listed directory by its parent's reference extended literally with its name; a link's
// reach is asked by that reference too -- never a text re-resolved, which names ANOTHER entry for a name ending in a
// space or a period, or a device name (✔MEASURED, the walk-up universe: the step extended then listed reached another
// directory in 38 cells and was refused in 36). Each entry is a LISTED value (`TreeEntry::at`).
// ★ IT REPORTS ITS ROOT (H6c, A2): an absent root, or one that is no directory, ends the walk with `error`, `where`
// and `root` set -- no caller asks the kind of what it walks before it walks it.
[[nodiscard]] DSS_EXPORT TreeWalk walkTree(HostPath const&                               root,
                                           std::function<WalkStep(TreeEntry const&)> const& visit,
                                           TreeWalkOptions                               options = {});

// What a removal could not remove.
struct TreeRemoval {
    std::size_t     failures = 0;   // entries STILL THERE after their one attempt
    HostPath        firstPath;      // the first of them -- a LISTED value below the target (H6c), or the target
    std::error_code firstError;     // and why
};

// Remove `target` -- one entry, or a whole tree -- and NEVER anything a link reaches: a link (to a
// directory or a file, a junction too) is removed as itself, and only a real directory is emptied.
// Single-object operations only: one listing of each directory, at most one removal per entry, the
// directory itself after its entries; nothing is retried, so it ENDS whatever another deleter does,
// and an entry that failed and is now gone was beaten to the outcome wanted -- only an entry still
// there counts as a failure. An absent target is nothing to do.
//
// ★★★ RELATIVE TO A HANDLE, NEVER BY PATH, SO A LINK SWAPPED IN MID-REMOVAL IS NEVER FOLLOWED. Each
// directory is opened WITHOUT following a link, listed through that handle, and each of its entries is
// opened RELATIVE to it, again without following a link, and classified by what was OPENED -- not by
// the listing, which an entry can outlive: a directory replaced by a link between its listing and its
// descent is a link when it is opened, and is removed as one. POSIX: `openat(O_DIRECTORY | O_NOFOLLOW)`,
// `fdopendir`, `unlinkat` -- the design libc++ ships (its comment: the path-based fallback "is vulnerable
// to some race conditions"), and ✔MEASURED 2026-10-21 libstdc++'s on Linux (the swapped-in link was not
// followed). Windows: handles opened with `FILE_OPEN_REPARSE_POINT`, each entry opened relative to its
// directory's handle (`NtOpenFile` with a root directory), a directory by ONE opening that both lists and
// deletes it (✔MEASURED 2026-10-22, both Windows builds: a listing handle `ReOpenFile`d from one opened for
// removal alone is refused, ACCESS_DENIED), every entry deleted through its own handle. MS STL's
// `remove_all` (READ, its <filesystem>) recurses BY PATH, and MinGW's libstdc++ cannot see a link at
// all, so neither library is asked.
//
// ★ A DIRECTORY ANOTHER HOLDER KEEPS OPEN WITHOUT DELETE SHARING (Windows; a process's current directory
// is one) is LEFT WHOLE -- nothing under it is removed -- and is the failure named. That is MS STL's
// answer: ✔MEASURED 2026-10-28 (MSVC 14.51's STL; a directory held by a handle sharing read|write only, and
// one that is a child process's current directory) `remove_all` removes nothing under it and fails with
// ERROR_SHARING_VIOLATION. The removal opens a directory once to list and delete it, so the same refusal
// ends the same subtree.
//
// `options.lastEntry` (one component; empty, the default, names nothing): an entry of the TARGET
// directory removed only after every other entry of it was, and only when every one of those was -- else
// the removal ends with it, and so the target, in place. A marker that keeps a half-removed tree
// recognisable (a kept root's `run-info.txt`) is that entry. A target that is a link or no directory has
// no entries: it is removed as itself, whatever the options say.
struct RemovalOptions {
    std::filesystem::path lastEntry;
};
[[nodiscard]] DSS_EXPORT TreeRemoval removeTree(HostPath const& target, RemovalOptions const& options = {});

// ════ THE STORE ROUTE'S TEXT FORMS (P69 H6c, ledger 579; fenced by ledger 580): H6d DELETES EVERY ONE ════
//
// H6c gave every operation a value and a use. The STORES a made value flows into -- the config root, a glob's
// matches into a project's sources, the input resolver's list, the include search's header lists, the descriptor
// lists, and a build's output and staging directories (`Program::setOutputDir`, the default `<cwd>/target` and the
// runtime staging area under the temporary directory) -- still keep it as TEXT, and moving them is H6d's ("a made
// value never crosses to text"). Until then a
// store site reads its text through ONE of these forms -- each the text the store route read before H6c, so H6c
// changes nothing the store route does -- and no other site does: the filesystem-queries guard pins the exact set
// of sites that use one (the two members `TreeEntry::storeRoutePath` and `TreeWalk::storeRouteWhere` too), by file,
// form and count, and a use outside that set, or a count other than its pinned one, is red. They are an ESCAPE from
// the value, named so one search finds every use; none of them is a home operation, and a non-store site states its
// value and its use.
//
// `normalizeKeepingRoot`'s text form before H6c: on a Windows host an absolute plain spelling (a drive, a share, a
// device) is the OS's own answer (GetFullPathNameW); every other spelling, and every POSIX one, the text's own
// normalization under the host's grammar.
[[nodiscard]] DSS_EXPORT std::filesystem::path storeRouteNormalized(std::filesystem::path const& p);
// `absoluteKeepingRoot`'s text form before H6c: a share, an authority, a device or a verbatim spelling as it is (it
// names no location on the current drive); any other made absolute by the library.
[[nodiscard]] DSS_EXPORT std::filesystem::path storeRouteAbsolute(std::filesystem::path const& p, std::error_code& ec);
// The text of `reachedPath(p, Use::Leaf, ec)`, `p` GIVEN.
[[nodiscard]] DSS_EXPORT std::filesystem::path storeRouteReachedPath(std::filesystem::path const& p,
                                                                     std::error_code&             ec);
// The text of `currentDirectory(ec)`.
[[nodiscard]] DSS_EXPORT std::filesystem::path storeRouteCurrentDirectory(std::error_code& ec);
// The text of `runningExecutable(ec)`.
[[nodiscard]] DSS_EXPORT std::filesystem::path storeRouteRunningExecutable(std::error_code& ec);
// The text of `temporaryDirectory(ec)`.
[[nodiscard]] DSS_EXPORT std::filesystem::path storeRouteTemporaryDirectory(std::error_code& ec);

// THE TEST SEAM of `removeTree`: called with each entry's TEXT (its listed value's, H6c) after its directory was
// listed and before the entry is opened -- the window a swap would use. Empty (the default) in every build; a test
// sets it to make the race deterministic, and clears it.
namespace substrate_testing {
DSS_EXPORT void setRemoveTreeEntryHook(std::function<void(std::filesystem::path const&)> hook);
// THE REFERENCE RULE'S ANSWER for `value` asked by `use` (P69 H6c): what every host call on it is handed -- for the
// rule's own table pin (tests/core/test_path_identity.cpp). A seam for the substrate's TESTS alone: invariant (3)
// of host_path.hpp holds for every product caller, because the filesystem-queries guard refuses any use of
// `substrate_testing` in src/ outside src/core/substrate/ (family (f), with its control).
[[nodiscard]] DSS_EXPORT std::filesystem::path::string_type referenceForTest(HostPath const& value, Use use);
}  // namespace substrate_testing

}  // namespace dss::core

template <>
struct std::hash<dss::core::PathIdentity> {
    [[nodiscard]] std::size_t
    operator()(dss::core::PathIdentity const& p) const noexcept {
        return std::hash<std::string>{}(p.string());
    }
};
