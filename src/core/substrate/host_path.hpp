#pragma once

#include "core/export.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

// ════ A PATH ON THIS HOST, AS A VALUE (P69 H6c, ledger 568 ruling (A)) ════
//
// ★★★ WHY A TYPE. The home of every host path call took and handed out PLAIN SPELLINGS, so "take the reference
// spelling, by use" was a rule each of its callers had to remember -- and the walk-up class reopened four times, each
// time at a caller that did not: a walk's entry named a listed `e.` as `e` (its text re-resolved), a container's kind
// asked as a leaf, a bare `C:` read as its root, a spawn handed `\??\` on as text. A `HostPath` holds the spelling it
// was GIVEN and yields its REFERENCE spelling by use -- only to the substrate (host_path_access.hpp, never exported).
//
// THE TWO USES (ledger 541; MEASURED over the walk-up universe and the listed names, H6b and H6c): a LEAF names the
// object the whole spelling names (an open, a size, a removal's target); a CONTAINER names the directory every
// `container \ name` lies in (a directory to make, to list, to start a process in). They differ on Windows exactly
// where the plain rule trims a LAST name -- a trailing period or space, a last `NUL` -- and POSIX resolves no name by
// its position, so there both uses are the text.
//
// THE FOUR ORIGINS: a GIVEN value (a person's, a document's, DSS's own spelling) is resolved by the reference rule at
// each use; a LISTED value (a walk's entry), a COMPOSED one (a child of a made value) and an ANSWERED one (the OS's
// answer: the current directory, the running image, a resolution) carry their references FIXED when they were made,
// literally, and are never re-resolved.
//
// INVARIANTS (the type's, not its callers'):
//   (1) a value's reference for a use is taken ONCE, by that use, in path_identity.cpp alone;
//   (2) its one text NAMES, in this process, under either use, the object(s) its references name -- "names", never
//       "reaches": past 259 characters no plain resolver reaches a DOS text (MEASURED, ERR 3);
//   (3) no member, export or friend outside the substrate hands a caller a reference spelling.
//
// ★★ A VALUE'S TEXT IS A RENDERING (`PathText`): it converts to no path. A rendering turned into a string re-enters
// only through a construction or an implicit conversion at a path parameter, and the filesystem-queries guard refuses
// either where it reads a rendering (tests/core/test_filesystem_queries_guard.cpp).

namespace dss::core {

// How a spelling is USED, which decides what it names on Windows (above).
enum class Use : std::uint8_t {
    Leaf,        // the object the whole spelling names
    Container,   // the directory every `container \ name` lies in
};

class HostPath;
struct HostPathAccess;

// A REFUSAL OF THE HOME, BY NAME (P69 H6c2, T4 and H1): what an operation answers where its value names nothing for
// it -- never a throw.
enum class PathRefusal : std::uint8_t {
    // From 1: a refusal's error code (`pathRefusalCode`) is never 0, the value every `if (ec)` reads as success.
    NoLastName = 1,      // the value names no last name: a volume root (`C:\`, `C:\x\..`), a last `.` or `..` (POSIX)
    IllFormedSpelling,   // a GIVEN narrow text that is no UTF-8 (Windows): it names nothing
    HostAnsweredNoReason,   // a host call refused and left no reason (`runtimeReason`): said so, never guessed
};
[[nodiscard]] constexpr char const* pathRefusalName(PathRefusal refusal) noexcept {
    switch (refusal) {
        case PathRefusal::NoLastName: return "it names no last name";
        case PathRefusal::IllFormedSpelling: return "its spelling is no UTF-8 text, so it names nothing";
        case PathRefusal::HostAnsweredNoReason: return "the host refused the call and answered no reason";
    }
    return "?";   // unreachable: the switch names every refusal
}
// The refusal as an error code (its own category, its message `pathRefusalName`): what an operation that answers
// with an error code sets for it.
[[nodiscard]] DSS_EXPORT std::error_code pathRefusalCode(PathRefusal refusal);

// A GIVEN NARROW TEXT AS THIS HOST READS IT (P69 H6c2, T4): UTF-8 on Windows, through the one decoder
// (`decodeUtf8ToWide`, windows_command_line.hpp), never the library's narrow conversion (MS STL reads the code page,
// libstdc++ UTF-8 and throws on an ill-formed sequence: the two Windows libraries disagree); the bytes themselves on
// POSIX. An ill-formed sequence answers `illFormed`, its text the bytes escaped (`\xNN`) for people.
struct GivenText {
    std::filesystem::path text;
    bool                  illFormed = false;
};
[[nodiscard]] DSS_EXPORT GivenText givenText(std::string_view narrow);

// The text of a value, for PEOPLE: a diagnostic, a report, a key compared as text. It has NO conversion to a path and
// no path accessor (see the header note).
class PathText {
public:
    // The lossless text, UTF-8, with the host's own separators.
    [[nodiscard]] DSS_EXPORT std::string display() const;
    // The text with `/` for every separator of a DOS spelling. A VERBATIM text (`\\?\...`) keeps its `\`: under
    // `\\?\` a `/` is a character, not a separator, so turning one generic would rename the file.
    [[nodiscard]] DSS_EXPORT std::string generic() const;
    [[nodiscard]] bool empty() const noexcept { return text_.empty(); }

private:
    friend class HostPath;
    explicit PathText(std::filesystem::path text) : text_(std::move(text)) {}
    std::filesystem::path text_;
};

// The text `display()` renders.
DSS_EXPORT std::ostream& operator<<(std::ostream& out, PathText const& text);

// The names below a walked root, as LISTED: a matching key (a glob, an ignore list, a descriptor's name). It composes
// only through `HostPath::operator/`, name by name, LITERALLY -- a listed name is the OS's own answer and is never
// resolved again.
class ListedRelative {
public:
    ListedRelative() = default;
    // `a/b/e.`, UTF-8, `/` between the names: for matching only.
    [[nodiscard]] std::string const& generic() const noexcept { return generic_; }
    [[nodiscard]] std::size_t        size() const noexcept { return names_.size(); }
    [[nodiscard]] bool               empty() const noexcept { return names_.empty(); }

private:
    friend class HostPath;
    friend struct HostPathAccess;
    std::vector<std::filesystem::path::string_type> names_;
    std::string                                     generic_;
};

class HostPath {
public:
    enum class Origin : std::uint8_t { Given, Listed, Composed, Answered };

    HostPath() = default;   // the empty spelling: `.` as a left side and a container, refused as a leaf
    // A GIVEN spelling -- implicit: a spelling a person, a document or DSS itself gives means what the reference rule
    // makes of it at each use.
    // ★ NARROW TEXT IS UTF-8 (T4): a narrow source is read by `givenText` -- an ill-formed one makes a value every
    // operation refuses (`PathRefusal::IllFormedSpelling`); any other source is the library's path of it.
    template <class Source>
        requires(!std::same_as<std::remove_cvref_t<Source>, HostPath>
                 && std::constructible_from<std::filesystem::path, Source const&>)
    HostPath(Source const& given) {   // NOLINT(google-explicit-constructor): GIVEN, by design
        if constexpr (std::is_convertible_v<Source const&, std::string_view>) {
            GivenText read = givenText(std::string_view{given});
            text_          = std::move(read.text);
            illFormed_     = read.illFormed;
        } else {
            text_ = std::filesystem::path(given);
        }
    }

    // The lossless text, for people (no path conversion).
    [[nodiscard]] PathText spelling() const { return PathText{text_}; }

    // A child. A RELATIVE right side TYPED in (a document's, an `#include` name, DSS's own literal) takes the OS's
    // plain rule by position below a DOS-text left -- a middle name loses a single trailing period, the last name is
    // resolved by use -- and is LITERAL below a verbatim-text left, as the whole verbatim spelling is (no reference
    // reads one there: MEASURED, cl 19.52 and gcc 13.2 open nothing below a verbatim include directory). A leading
    // `..` is the one cut of the left. A right side WITH a root replaces the left by the home's own root parse
    // (`rootSpelling`): an absolute one is that right side, GIVEN; a rooted `\x` the left's volume root extended by
    // `x`; a drive-relative `D:x` the left extended by `x` on D, the GIVEN `D:x` on another drive.
    [[nodiscard]] DSS_EXPORT HostPath operator/(std::filesystem::path const& right) const;
    // A child named by LISTED names: literally, name by name.
    [[nodiscard]] DSS_EXPORT HostPath operator/(ListedRelative const& listed) const;
    // The sibling whose last name is this value's last name followed by `suffix` (a staging name beside a target),
    // cut from the leaf reference and extended literally -- or `NoLastName` when the value names no last name (H1:
    // until H6c2 this threw, uncaught, out of the PATH probe and the writer).
    [[nodiscard]] DSS_EXPORT std::expected<HostPath, PathRefusal>
    withSuffix(std::filesystem::path::string_type const& suffix) const;

    // The last name, its stem and its extension, as TEXT to match (`.c`) -- never a path to hand on.
    [[nodiscard]] std::filesystem::path filename() const { return text_.filename(); }
    [[nodiscard]] std::filesystem::path stem() const { return text_.stem(); }
    [[nodiscard]] std::filesystem::path extension() const { return text_.extension(); }
    [[nodiscard]] bool                  empty() const noexcept { return text_.empty(); }
    [[nodiscard]] Origin                origin() const noexcept { return origin_; }

private:
    friend struct HostPathAccess;
    std::filesystem::path text_;
    // Windows: the references of a LISTED / COMPOSED / ANSWERED value, fixed when it was made (`\\?\` spellings; the
    // container's without a trailing separator but at a volume root). Empty for a GIVEN value and on every POSIX
    // host, where both uses are the text.
    std::filesystem::path::string_type leafRef_;
    std::filesystem::path::string_type containerRef_;
    Origin                             origin_ = Origin::Given;
    // A GIVEN narrow text that was no UTF-8 (Windows): the value names nothing, and every operation refuses it.
    bool illFormed_ = false;
};

// ════ THE HOME'S OPERATIONS, AS ONE LIST (P69 H6c2, T3) ════
//
// Every operation the home declares -- in every substrate header whose declarations take or answer a host path
// (a `HostPath`, a `PathText`, a `ListedRelative`, a `PathIdentity`, a `std::filesystem::path`): host_path.hpp,
// host_path_access.hpp, path_identity.hpp, checked_file_read.hpp, process_spawn.hpp, found BY DATA by the guard
// (tests/core/test_filesystem_queries_guard.cpp, `EveryOperationOfTheHomeIsInTheList`), which fails on a declared
// operation this list does not name and on a row no header declares. A row is X(name, header, family): overloads
// share a row; a member of the value types (`HostPath`, `PathText`, `ListedRelative`, `PathIdentity`) is an operation
// whatever its signature, any other declaration one where its signature names a path type.
// THE FAMILY says what the operation asks of the host, and so which universe cells it runs on
// (tests/core/substrate/host_path_universe.hpp): a REFERENCE family hands the host a value's reference and runs over
// the WITNESS set, where reference and text differ (T5).
//   Spelling   -- reads the text alone; asks the host nothing (a rendering, a parse, a split)
//   Composing  -- makes a value from values and names; asks the host nothing but a GIVEN left's reference
//   Reading    -- REFERENCE: asks the host what an object is (kind, size, time, identity, a listing)
//   Making     -- REFERENCE: changes the host (makes, removes, renames, copies, opens to write)
//   Opening    -- REFERENCE: opens an object for the caller (a stream, a handle, a descriptor, a read)
//   Answering  -- the host hands a path out, as an ANSWERED value
//   Spawning   -- REFERENCE: a process is started on a value's image and directory forms
//   Access     -- the substrate's own access to a value's references (host_path_access.hpp; never exported)
//   StoreRoute -- the fenced text forms H6d deletes
//   Seam       -- a test's seam, named under tests/ alone
#define DSS_HOST_PATH_OPERATIONS(X)                                         \
    X("HostPath", host_path, Composing)                                     \
    X("spelling", host_path, Spelling)                                      \
    X("display", host_path, Spelling)                                       \
    X("generic", host_path, Spelling)                                       \
    X("empty", host_path, Spelling)                                         \
    X("size", host_path, Spelling)                                          \
    X("operator<<", host_path, Spelling)                                    \
    X("operator/", host_path, Composing)                                    \
    X("withSuffix", host_path, Composing)                                   \
    X("filename", host_path, Spelling)                                      \
    X("stem", host_path, Spelling)                                          \
    X("extension", host_path, Spelling)                                     \
    X("origin", host_path, Spelling)                                        \
    X("PathText", host_path, Spelling)                                      \
    X("ListedRelative", host_path, Spelling)                                \
    X("reference", host_path_access, Access)                                \
    X("text", host_path_access, Access)                                     \
    X("listed", host_path_access, Access)                                   \
    X("extend", host_path_access, Access)                                   \
    X("answered", host_path_access, Access)                                 \
    X("made", host_path_access, Access)                                     \
    X("illFormed", host_path_access, Access)                                \
    X("inheritIllFormed", host_path_access, Access)                         \
    X("spawnDirectory", host_path_access, Spawning)                         \
    X("spawnImage", host_path_access, Spawning)                             \
    X("genericSpelling", path_identity, Spelling)                           \
    X("genericSpellingU8", path_identity, Spelling)                         \
    X("rootSpelling", path_identity, Spelling)                              \
    X("parentWithinRoot", path_identity, Spelling)                          \
    X("typedNameBelow", path_identity, Spelling)                            \
    X("containingDirectory", path_identity, Composing)                      \
    X("openFileStream", path_identity, Opening)                             \
    X("openInputStream", path_identity, Opening)                            \
    X("openOutputStream", path_identity, Opening)                           \
    X("openHandle", path_identity, Opening)                                 \
    X("openDescriptor", path_identity, Opening)                             \
    X("fileAttributes", path_identity, Reading)                             \
    X("createDirectories", path_identity, Making)                           \
    X("createDirectoriesFor", path_identity, Making)                        \
    X("removeEntry", path_identity, Making)                                 \
    X("copyFile", path_identity, Making)                                    \
    X("commitByHandle", path_identity, Making)                              \
    X("renameEntry", path_identity, Making)                                 \
    X("currentDirectory", path_identity, Answering)                         \
    X("temporaryDirectory", path_identity, Answering)                       \
    X("runningExecutable", path_identity, Answering)                        \
    X("processImage", path_identity, Answering)                             \
    X("absoluteKeepingRoot", path_identity, Reading)                        \
    X("normalizeKeepingRoot", path_identity, Reading)                       \
    X("canonicalIdentityKey", path_identity, Reading)                       \
    X("of", path_identity, Reading)                                         \
    X("path", path_identity, Spelling)                                      \
    X("string", path_identity, Spelling)                                    \
    X("view", path_identity, Spelling)                                      \
    X("operator==", path_identity, Spelling)                                \
    X("operator<=>", path_identity, Spelling)                               \
    X("operator()", path_identity, Spelling)                                \
    X("entryKind", path_identity, Reading)                                  \
    X("reachedKind", path_identity, Reading)                                \
    X("reachedPipe", path_identity, Reading)                                \
    X("reachedFileSize", path_identity, Reading)                            \
    X("reachedPath", path_identity, Reading)                                \
    X("reachedRelative", path_identity, Reading)                            \
    X("reachedWriteTime", path_identity, Reading)                           \
    X("reachedSameFile", path_identity, Reading)                            \
    X("addReachedPermissions", path_identity, Making)                       \
    X("sameEntry", path_identity, Reading)                                  \
    X("storedEntryName", path_identity, Reading)                            \
    X("directoryFoldsCase", path_identity, Reading)                         \
    X("walkTree", path_identity, Reading)                                   \
    X("removeTree", path_identity, Making)                                  \
    X("storeRouteNormalized", path_identity, StoreRoute)                    \
    X("storeRouteAbsolute", path_identity, StoreRoute)                      \
    X("storeRouteReachedPath", path_identity, StoreRoute)                   \
    X("storeRouteCurrentDirectory", path_identity, StoreRoute)              \
    X("storeRouteRunningExecutable", path_identity, StoreRoute)             \
    X("storeRouteTemporaryDirectory", path_identity, StoreRoute)            \
    X("setRemoveTreeEntryHook", path_identity, Seam)                        \
    X("referenceForTest", path_identity, Seam)                              \
    X("readFileChecked", checked_file_read, Opening)                        \
    X("resolveExecutableOnPath", process_spawn, Answering)                  \
    X("programPathFromUtf8", process_spawn, Spelling)                       \
    X("spawnAndWaitInherit", process_spawn, Spawning)                       \
    X("spawnAndWaitRedirectStdout", process_spawn, Spawning)                \
    X("startImageInheriting", process_spawn, Spawning)

enum class HostPathOperationFamily : std::uint8_t {
    Spelling, Composing, Reading, Making, Opening, Answering, Spawning, Access, StoreRoute, Seam,
};

// The list as data: each operation's name, the header that declares it (its stem under src/core/substrate/), its
// family.
struct HostPathOperation {
    char const*             name;
    char const*             header;
    HostPathOperationFamily family;
};
inline constexpr HostPathOperation kHostPathOperations[] = {
#define DSS_HOST_PATH_OPERATION_ROW(name, header, family) {name, #header, HostPathOperationFamily::family},
    DSS_HOST_PATH_OPERATIONS(DSS_HOST_PATH_OPERATION_ROW)
#undef DSS_HOST_PATH_OPERATION_ROW
};

// Whether an operation of `family` hands the host a value's REFERENCE -- and so runs over the witness set (T5).
[[nodiscard]] constexpr bool takesAReference(HostPathOperationFamily family) noexcept {
    switch (family) {
        case HostPathOperationFamily::Reading:
        case HostPathOperationFamily::Making:
        case HostPathOperationFamily::Opening:
        case HostPathOperationFamily::Spawning: return true;
        case HostPathOperationFamily::Spelling:
        case HostPathOperationFamily::Composing:
        case HostPathOperationFamily::Answering:
        case HostPathOperationFamily::Access:
        case HostPathOperationFamily::StoreRoute:
        case HostPathOperationFamily::Seam: return false;
    }
    return false;   // unreachable: the switch names every family
}

}  // namespace dss::core
