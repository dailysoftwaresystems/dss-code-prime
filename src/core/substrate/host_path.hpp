#pragma once

#include "core/export.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <string>
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
    template <class Source>
        requires(!std::same_as<std::remove_cvref_t<Source>, HostPath>
                 && std::constructible_from<std::filesystem::path, Source const&>)
    HostPath(Source const& given) : text_(given) {}   // NOLINT(google-explicit-constructor): GIVEN, by design

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
    // cut from the leaf reference and extended literally.
    [[nodiscard]] DSS_EXPORT HostPath withSuffix(std::filesystem::path::string_type const& suffix) const;

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
};

}  // namespace dss::core
