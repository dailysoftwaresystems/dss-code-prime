#pragma once

// ★★★ THE SUBSTRATE'S ALONE (P69 H6c, A5). Nothing here is exported, and the filesystem-queries guard refuses this
// header's inclusion anywhere outside src/core/substrate/ (tests/core/test_filesystem_queries_guard.cpp): a reference
// spelling is what a Win32 call is handed, and a caller holding one is a caller re-resolving it. It offers references
// of VALUES only -- never of plain text -- and the reference RULE itself stays in path_identity.cpp.

#include "core/substrate/host_path.hpp"

#include <filesystem>

namespace dss::core {

struct HostPathAccess {
    using String = std::filesystem::path::string_type;

    // The spelling every host call on `value` takes for `use`: on Windows its reference (`\\?\...`, at any length; a
    // GIVEN value's taken now by the rule, a made value's fixed one); on POSIX its text. The CONTAINER form carries no
    // trailing separator but at a volume root (a caller that hands a directory on with its separator -- the spawn --
    // appends it). EMPTY when the value names nothing for that use: the empty spelling as a LEAF (refused by every
    // leaf operation, `std::errc::invalid_argument`), and a spelling the OS refuses to resolve stays as written.
    [[nodiscard]] static String reference(HostPath const& value, Use use);

    // The text as it was given or made, for a call that must hand a caller-visible spelling on (a diagnostic's).
    [[nodiscard]] static std::filesystem::path const& text(HostPath const& value) { return value.text_; }
    // Whether the value is a GIVEN narrow text that was no UTF-8 (T4): it names nothing, so every operation refuses it
    // (`PathRefusal::IllFormedSpelling`), and its references are EMPTY.
    [[nodiscard]] static bool illFormed(HostPath const& value) noexcept { return value.illFormed_; }
    // A value made from `value` by a text operation (a composition, a level): ill-formed when `value` is.
    static void inheritIllFormed(HostPath& made, HostPath const& value) noexcept {
        made.illFormed_ = made.illFormed_ || value.illFormed_;
    }

    // A LISTED value: the entry `name` of the directory whose container reference is `parentReference` and whose
    // text is `parentText`.
    [[nodiscard]] static HostPath listed(std::filesystem::path const& parentText, String const& parentReference,
                                         String const& name);
    // A ListedRelative grown by one listed name.
    static void extend(ListedRelative& relative, String const& name);
    // An ANSWERED value: the OS's own answer `reference` (a `\\?\` spelling on Windows; any absolute spelling on
    // POSIX), its text the DOS form exactly where that names the same object, verbatim otherwise.
    [[nodiscard]] static HostPath answered(String const& reference);
    // A MADE value: its text and its two references, fixed (on POSIX the text alone: both references are empty).
    [[nodiscard]] static HostPath made(std::filesystem::path text, String leafReference, String containerReference,
                                       HostPath::Origin origin);

    // THE SPAWN'S TWO FORMS (Windows; the text elsewhere), each the one CreateProcessW is handed:
    //   * `spawnDirectory`: the working directory -- the DOS spelling of its CONTAINER reference WITH the separator
    //     after it, wherever that reference has a DOS form (a drive's, a share's); the verbatim container reference
    //     with its separator where it has none (a volume with no DOS name). ✔MEASURED, H6c u3 parts 2-4, both Windows
    //     libraries: the DOS form with its separator starts the child in exactly the directory the verbatim
    //     reference starts it in, in every row of every DOS kind x 7 names x {last, middle} x twin {present, absent}
    //     -- and it starts up to 258 characters INCLUDING that separator in every form, where the verbatim one loses
    //     the 4 (drive) or 6 (share) characters of its prefix.
    //   * `spawnImage`: the image -- its LEAF reference in the DOS form exactly where that names the same file
    //     (`dosSpellingNamesTheSameFile` over its tail) and is under MAX_PATH; the verbatim reference otherwise.
    //     ✔MEASURED, H6c u3 part 4: a plain spelling through a listed `e.` directory starts the TWIN's image, or
    //     nothing (ERR 3); the verbatim spelling starts the literal image.
    // EMPTY when the value names nothing for that use (the empty spelling as an image).
    [[nodiscard]] static String spawnDirectory(HostPath const& cwd);
    [[nodiscard]] static String spawnImage(HostPath const& image);
};

}  // namespace dss::core
