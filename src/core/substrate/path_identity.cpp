#include "core/substrate/path_identity.hpp"

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace dss::core {
namespace {

namespace fs = std::filesystem;

#ifdef _WIN32

// `GetLongPathNameW` for ONE path, or empty when Windows will not answer.
//
// ★ WHY THIS CALL AND NOT `GetFinalPathNameByHandleW`. Both expand 8.3, but the
// handle-based one ALSO rewrites junctions and mount points and prefixes `\\?\`.
// These keys are compared as strings and some of them are printed to operators,
// so the minimal transformation is the correct one: expand short components,
// touch nothing else.
[[nodiscard]] std::wstring longNameOrEmpty(std::wstring const& in) {
    if (in.empty()) return {};
    DWORD const n = ::GetLongPathNameW(in.c_str(), nullptr, 0);
    if (n == 0) return {};
    std::wstring out(n, L'\0');
    DWORD const written = ::GetLongPathNameW(in.c_str(), out.data(), n);
    // `written >= n` means the path changed under us between the two calls.
    if (written == 0 || written >= n) return {};
    out.resize(written);
    return out;
}

// Expand the longest EXISTING prefix and re-attach the remainder verbatim.
//
// ★★★ THIS WALK IS MANDATORY, NOT DEFENSIVE — ✔MEASURED 2026-08-18 on this
// machine, with a standalone probe, before this function was written:
//
//     DSS-SC~1                   ->  dss-scratch-spelling-probe-longname   (expanded)
//     DSS-SC~1\not\yet\built.o   ->  DSS-SC~1\not\yet\built.o              (UNCHANGED)
//
// `GetLongPathNameW` fails WHOLESALE when any component is absent. Calling it on
// the full path alone therefore no-ops on exactly the paths that do not exist
// yet — every output artifact, every not-yet-generated source — while looking
// perfectly correct on every input path that happens to exist. That is a false
// green of the family this project keeps catching: the property appears to hold
// because the cases that would refute it were never exercised.
[[nodiscard]] std::wstring expandShortComponents(std::wstring const& in) {
    if (auto const whole = longNameOrEmpty(in); !whole.empty()) return whole;

    fs::path const p{in};
    fs::path       head = p;
    fs::path       tail;
    // Walk up until Windows answers for the head. `parent_path()` of a root is
    // the root itself, so compare against the previous value to terminate —
    // `has_parent_path()` alone would spin forever on `C:\`.
    while (true) {
        fs::path const parent = head.parent_path();
        if (parent.empty() || parent == head) return in;  // nothing existed
        // ⚠ NOT `head.filename() / tail` UNCONDITIONALLY: `operator/` with an
        // EMPTY right operand APPENDS A SEPARATOR, so on the first turn of this
        // loop the tail became `notyet.o\` and every key built through this walk
        // carried a trailing separator that the whole-path fast path above does
        // not produce. ✔MEASURED, ONE path, two keys, differing only in whether
        // the file was there yet:
        //     before it exists : 'C:/.../idtmp/notyet.o/'
        //     after  it exists : 'C:/.../idtmp/notyet.o'
        // That is precisely the two-keys-for-one-file defect this type exists to
        // prevent, on the not-yet-created inputs the header's own note calls a
        // FIRST-CLASS case (output artifacts, a `path` dependency resolved
        // before its directory is confirmed, a cycle key needed for the reject's
        // own message).
        tail = tail.empty() ? head.filename() : (head.filename() / tail);
        head = parent;
        if (auto const expanded = longNameOrEmpty(head.wstring());
            !expanded.empty()) {
            fs::path out{expanded};
            out /= tail;
            return out.wstring();
        }
    }
}

#endif  // _WIN32

[[nodiscard]] constexpr bool isSeparator(fs::path::value_type c) {
    return c == fs::path::preferred_separator
        || c == fs::path::value_type{'/'};
}

// How many separators does this spelling START with? A run of 2 or more is the
// only place a "redundant" separator carries meaning, and both transforms below
// are about not destroying it.
[[nodiscard]] std::size_t leadingSeparatorRun(fs::path::string_type const& s) {
    std::size_t n = 0;
    while (n < s.size() && isSeparator(s[n])) ++n;
    return n;
}

}  // namespace

// `lexically_normal()` WITHOUT letting it eat the leading separator RUN.
//
// ★★★ ✔MEASURED, and it is the same loss `generic_string()` performs one step
// later, so the two together destroyed a UNC path twice over
// ([[D-CPP-QUOTE-INCLUDE-UNC-DIRECTORY-UNRESOLVED]]). Printed with `.string()`,
// so no print-side transform can be blamed:
//     input            '\\wsl.localhost\Ubuntu\home\<user>\p\uncprobe.h'
//     lexically_normal '\wsl.localhost\Ubuntu\home\<user>\p\uncprobe.h'
// One separator gone: the authority is now an ordinary directory on the local
// drive root, and the key names a file that does not exist. A run of TWO OR MORE
// leading separators is the one place a "redundant" separator is not redundant,
// on every path model that does not give the authority a `root_name()` of its
// own -- so the run is preserved verbatim (spelled with the model's own
// preferred separator, which is what makes the `//` and `\\` spellings of ONE
// file converge on ONE key) and only the part BELOW it is normalised.
//
// ★★ THIS IS NOT A WINDOWS QUIRK, AND THAT WAS MEASURED RATHER THAN ASSUMED --
// a fix reasoned on one host is a portability claim. On Linux/libstdc++ 13.3.0
// the same path answers `is_absolute()` TRUE with an EMPTY `root_name()`,
// `lexically_normal()` collapses the run just the same, and `weakly_canonical`
// SUCCEEDS returning the collapsed form; the shipped derivation therefore gave
// `//x/share/h` and `/x/share/h` the SAME key there too. POSIX makes a leading
// run of exactly two separators implementation-defined, so keeping the two
// apart is the safe direction on that host as well: a duplicate key costs
// duplicate work, a merged one costs wrong contents.
//
// ⚠ A run of 0 or 1 is untouched -- a drive path and a root-relative path both
// take `lexically_normal()` exactly as before, so this changes no local key.
fs::path normalizeKeepingRoot(fs::path const& p) {
    fs::path::string_type const& s    = p.native();
    std::size_t const            lead = leadingSeparatorRun(s);
    if (lead < 2) return p.lexically_normal();
    fs::path::string_type out(lead, fs::path::preferred_separator);
    out += fs::path{s.substr(lead)}.lexically_normal().native();
    return fs::path{out};
}

std::string genericSpelling(fs::path const& p) {
    std::string out = p.string();
    if constexpr (fs::path::preferred_separator != fs::path::value_type{'/'}) {
        std::replace(out.begin(), out.end(),
                     static_cast<char>(fs::path::preferred_separator), '/');
    }
    return out;
}

std::string genericSpelling(std::string_view spelling) {
    // Through the `fs::path` form, never a second `std::replace` here: two
    // spellings of one rule are two owners of it, and the one that gets
    // forgotten is the one that stops matching. `fs::path`'s narrow constructor
    // does not touch the bytes, so a stored native spelling round-trips into
    // the same substitution the path form performs.
    return genericSpelling(fs::path{spelling});
}

std::u8string genericSpellingU8(fs::path const& p) {
    // `u8string()` and NOT `generic_u8string()` -- the NATIVE spelling carries
    // the leading run, and the substitution below is the same normalisation
    // without the generic-format conversion that eats it. See the header.
    std::u8string out = p.u8string();
    if constexpr (fs::path::preferred_separator != fs::path::value_type{'/'}) {
        // Every path separator is ASCII, so it is ONE UTF-8 code unit and a
        // per-unit substitution cannot land inside a multi-byte sequence.
        std::replace(out.begin(), out.end(),
                     static_cast<char8_t>(fs::path::preferred_separator),
                     char8_t{'/'});
    }
    return out;
}

fs::path absoluteKeepingRoot(fs::path const& p, std::error_code& ec) {
    // A multi-separator prefix names an AUTHORITY, not a location on the current
    // drive, so there is nothing here to make absolute. See the header for the
    // measurement: `fs::absolute` does not merely fail on these, it succeeds
    // having re-rooted the path onto the local drive.
    if (leadingSeparatorRun(p.native()) >= 2) {
        ec.clear();
        return p;
    }
    return fs::absolute(p, ec);
}

std::string canonicalIdentityKey(fs::path const& p) {
    std::error_code ec;
    return canonicalIdentityKey(p, ec);
}

std::string canonicalIdentityKey(fs::path const& p, std::error_code& ec) {
    ec.clear();
    // The canonicalisation's OWN failure, kept separate from the `ec` this
    // function reports: the two answer different questions (see below), and
    // folding them was what let a model failure masquerade as a host refusal.
    //
    // ★ THE PATH A LINK LEADS TO IS ASKED OF `reachedPath`, NEVER OF THE LIBRARY: libstdc++ on Windows
    // resolves no link (its `weakly_canonical` hands one back unchanged), so the MinGW build keyed a link
    // and the file it reaches as TWO identities where every other build keys one
    // ([[D-CORE-MINGW-BUILDS-WALKS-AND-REMOVALS-THAT-FOLLOW-A-LINK]]). On Windows `reachedPath` is the final
    // path of the opened file's handle -- what MS STL's `weakly_canonical` asks too.
    std::error_code canonEc;
    fs::path const c = reachedPath(p, canonEc);
    if (canonEc) {
        // ⚠ A `weakly_canonical` FAILURE IS NOT EVIDENCE THAT THE FILESYSTEM
        // REFUSED, and this overload's whole contract is that `ec` means exactly
        // that. ✔MEASURED: `weakly_canonical` fails `ENOENT` 200 times out of
        // 200 on every spelling of a UNC path whose `exists()` is TRUE, whose
        // `file_size()` answers, and whose parent enumerates. The library's path
        // model cannot walk the authority; the OS has no such trouble.
        // Forwarding that as a refusal made every caller that fails loud on `ec`
        // -- the project-source de-duplication is one, and its own comment
        // asserts "`weakly_canonical` does NOT error merely because a path does
        // not exist, so this arm means a genuine filesystem failure" -- reject a
        // perfectly readable source. So ASK THE FILESYSTEM DIRECTLY: if it
        // answers at all, whatever the answer, it did not refuse.
        std::error_code probe;
        static_cast<void>(fs::exists(p, probe));
        ec = probe;
    }
    // ★★★ AND A SUCCESS IS NOT EVIDENCE THAT IT ANSWERED FOR THE PATH IT WAS
    // ASKED ABOUT. ✔MEASURED, the OTHER branch of the same model gap, and this
    // one is the collision the row asked about:
    //     input             '//no-such-server/share/x.h'   (exists: false)
    //     weakly_canonical  'C:\no-such-server\share\x.h'  ec: <none>
    // With the authority unmodelled, `weakly_canonical` treats the path as
    // drive-relative and RELOCATES it onto the local drive, reporting success.
    // The key it yields is then BYTE-IDENTICAL to the key of a real local file
    // at `C:\no-such-server\share\x.h` -- two distinct files, ONE identity --
    // which on any of the maps this type keys is a wrong-content hazard rather
    // than a duplicate-work one. (A UNC path that EXISTS takes the `ENOENT`
    // branch above instead, so the two branches together are the whole
    // exposure.) A canonicalisation that changed the leading separator RUN
    // changed which machine the path names, so it is refused here and the
    // lexical normalisation -- which cannot relocate anything -- is used.
    bool const relocated = leadingSeparatorRun(p.native()) >= 2
                        && leadingSeparatorRun(c.native())
                               != leadingSeparatorRun(p.native());
    fs::path const r = (canonEc || relocated) ? normalizeKeepingRoot(p) : c;
#ifdef _WIN32
    // ONE separator convention for every key in the compiler: two sites that key
    // the same file with different separators disagree about identity for a
    // reason no diagnostic can explain. `genericSpelling` and NOT
    // `generic_string()` -- see the header for what the latter deletes.
    return genericSpelling(fs::path{expandShortComponents(r.wstring())});
#else
    return genericSpelling(r);
#endif
}

// ════ THE KIND OF A DIRECTORY ENTRY ITSELF, AND THE WALK AND THE REMOVAL THAT ASK IT ════
// (the header states why the OS is asked on Windows, with the measurement)

namespace {

#ifdef _WIN32

// The answers Windows gives for "nothing is there" -- the set MS STL's `status` reads as `not_found`.
[[nodiscard]] bool winNotFound(DWORD e) {
    return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_INVALID_NAME
        || e == ERROR_INVALID_DRIVE || e == ERROR_BAD_NETPATH || e == ERROR_BAD_NET_NAME;
}

[[nodiscard]] std::error_code winError(DWORD e) {
    return std::error_code{static_cast<int>(e), std::system_category()};
}

// What `p` REACHES, read through a handle opened WITHOUT `FILE_FLAG_OPEN_REPARSE_POINT` -- a handle to
// what every link on the way reaches. Nothing when nothing is reached; `ec` set only for a failure
// that is not "nothing there".
[[nodiscard]] std::optional<BY_HANDLE_FILE_INFORMATION> reachedInformation(fs::path const& p, std::error_code& ec) {
    HANDLE const reached = ::CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                         FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (reached == INVALID_HANDLE_VALUE) {
        DWORD const e = ::GetLastError();
        if (!winNotFound(e)) ec = winError(e);
        return std::nullopt;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    BOOL const  read = ::GetFileInformationByHandle(reached, &info);
    DWORD const e    = read ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(reached);
    if (!read) {
        ec = winError(e);
        return std::nullopt;
    }
    return info;
}

// One entry's kind from what the OS recorded for it: its attributes and, for a reparse point, its tag.
[[nodiscard]] EntryKind kindOfAttributes(DWORD attributes, DWORD reparseTag) {
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
        && (reparseTag == static_cast<DWORD>(IO_REPARSE_TAG_SYMLINK)
            || reparseTag == static_cast<DWORD>(IO_REPARSE_TAG_MOUNT_POINT))) {
        return EntryKind::Link;
    }
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? EntryKind::Directory : EntryKind::File;
}

[[nodiscard]] bool isPermissionDenied(std::error_code const& ec) {
    return ec == std::errc::permission_denied
        || (ec.category() == std::system_category() && ec.value() == static_cast<int>(ERROR_ACCESS_DENIED));
}

// Every entry of `dir` but `.` and `..`, each classified from its own find record -- the record
// carries the attributes AND a reparse point's tag, so no entry costs a second call.
void listEntries(fs::path const& dir, std::size_t depth, std::vector<TreeEntry>& out, std::error_code& ec) {
    ec.clear();
    fs::path const   pattern = dir / L"*";
    WIN32_FIND_DATAW found{};
    HANDLE const     search = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &found, FindExSearchNameMatch,
                                                 nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (search == INVALID_HANDLE_VALUE) {
        ec = winError(::GetLastError());
        return;
    }
    do {
        std::wstring_view const name{found.cFileName};
        if (name == L"." || name == L"..") continue;
        DWORD const     tag  = (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ? found.dwReserved0 : 0;
        EntryKind const kind = kindOfAttributes(found.dwFileAttributes, tag);
        out.push_back(TreeEntry{dir / fs::path{std::wstring{name}}, kind, kind, depth});
    } while (::FindNextFileW(search, &found));
    DWORD const last = ::GetLastError();
    ::FindClose(search);
    if (last != ERROR_NO_MORE_FILES) ec = winError(last);
}

#else

[[nodiscard]] EntryKind kindOfStatus(fs::file_type type) {
    switch (type) {
        case fs::file_type::not_found: return EntryKind::Absent;
        case fs::file_type::directory: return EntryKind::Directory;
        case fs::file_type::symlink: return EntryKind::Link;
        case fs::file_type::regular: return EntryKind::File;
        default: return EntryKind::Other;
    }
}

[[nodiscard]] bool isPermissionDenied(std::error_code const& ec) {
    return ec == std::errc::permission_denied;
}

// Every entry of `dir`, each classified by its own `symlink_status` (the listing's cached type
// where the platform records one). The `ec` test comes BEFORE the end test: a failed increment may
// leave the iterator equal to the end, and a loop that asked "finished?" first would read a listing
// error as a complete listing.
void listEntries(fs::path const& dir, std::size_t depth, std::vector<TreeEntry>& out, std::error_code& ec) {
    ec.clear();
    fs::directory_iterator       it{dir, ec};
    fs::directory_iterator const end;
    while (true) {
        if (ec) return;
        if (it == end) return;
        std::error_code kec;
        auto const      type = it->symlink_status(kec).type();
        // Gone since it was listed is `Absent`; any other failure to read it ends the listing.
        if (kec && type != fs::file_type::not_found) {
            ec = kec;
            return;
        }
        EntryKind const kind = kindOfStatus(type);
        out.push_back(TreeEntry{it->path(), kind, kind, depth});
        it.increment(ec);
    }
}

#endif

}  // namespace

EntryKind entryKind(fs::path const& p, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &data)) {
        DWORD const e = ::GetLastError();
        if (winNotFound(e)) return EntryKind::Absent;
        ec = winError(e);
        return EntryKind::Other;
    }
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) return kindOfAttributes(data.dwFileAttributes, 0);
    // A reparse point: its TAG decides, read from a handle to the entry itself (never what it reaches).
    HANDLE const entry = ::CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (entry == INVALID_HANDLE_VALUE) {
        DWORD const e = ::GetLastError();
        if (winNotFound(e)) return EntryKind::Absent;
        ec = winError(e);
        return EntryKind::Other;
    }
    FILE_ATTRIBUTE_TAG_INFO info{};
    BOOL const  read = ::GetFileInformationByHandleEx(entry, FileAttributeTagInfo, &info, sizeof info);
    DWORD const e    = read ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(entry);
    if (!read) {
        ec = winError(e);
        return EntryKind::Other;
    }
    return kindOfAttributes(info.FileAttributes, info.ReparseTag);
#else
    auto const type = fs::symlink_status(p, ec).type();
    if (type == fs::file_type::not_found) {
        ec.clear();
        return EntryKind::Absent;
    }
    return ec ? EntryKind::Other : kindOfStatus(type);
#endif
}

EntryKind reachedKind(fs::path const& p, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    auto const info = reachedInformation(p, ec);
    if (!info) return EntryKind::Absent;
    return (info->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? EntryKind::Directory : EntryKind::File;
#else
    auto const type = fs::status(p, ec).type();
    if (type == fs::file_type::not_found) {
        ec.clear();
        return EntryKind::Absent;
    }
    if (ec) return EntryKind::Absent;
    return type == fs::file_type::directory ? EntryKind::Directory
         : type == fs::file_type::regular   ? EntryKind::File
                                            : EntryKind::Other;
#endif
}

std::optional<std::uintmax_t> reachedFileSize(fs::path const& p, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    auto const info = reachedInformation(p, ec);
    if (!info || (info->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return std::nullopt;
    return (std::uintmax_t{info->nFileSizeHigh} << 32) | info->nFileSizeLow;
#else
    auto const reached = fs::status(p, ec).type();
    if (reached == fs::file_type::not_found) {
        ec.clear();
        return std::nullopt;
    }
    if (ec || reached != fs::file_type::regular) return std::nullopt;
    auto const size = fs::file_size(p, ec);
    if (ec) return std::nullopt;
    return size;
#endif
}

fs::path reachedPath(fs::path const& p, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    // Absolute and normal WITHOUT re-rooting an authority onto the local drive (the header measures
    // what `fs::absolute` and `lexically_normal` do to a multi-separator root).
    fs::path const whole = normalizeKeepingRoot(absoluteKeepingRoot(p, ec));
    if (ec) return {};
    // The longest leading part that exists, resolved by the OS; what follows it is appended as written.
    fs::path head = whole;
    fs::path rest;
    for (;;) {
        HANDLE const reached = ::CreateFileW(head.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                             nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (reached != INVALID_HANDLE_VALUE) {
            std::wstring resolved(MAX_PATH, L'\0');
            DWORD        n = ::GetFinalPathNameByHandleW(reached, resolved.data(), static_cast<DWORD>(resolved.size()),
                                                         FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (n >= resolved.size()) {
                resolved.resize(n);
                n = ::GetFinalPathNameByHandleW(reached, resolved.data(), static_cast<DWORD>(resolved.size()),
                                                FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            }
            DWORD const e = n == 0 ? ::GetLastError() : ERROR_SUCCESS;
            ::CloseHandle(reached);
            if (n == 0 || n >= resolved.size()) {
                ec = winError(n == 0 ? e : ERROR_INSUFFICIENT_BUFFER);
                return {};
            }
            resolved.resize(n);
            // The final path is spelled in the OS's own long form: `\\?\C:\...` or `\\?\UNC\server\share\...`.
            if (resolved.starts_with(L"\\\\?\\UNC\\")) resolved = L"\\\\" + resolved.substr(8);
            else if (resolved.starts_with(L"\\\\?\\")) resolved = resolved.substr(4);
            fs::path out{resolved};
            if (!rest.empty()) out /= rest;
            return normalizeKeepingRoot(out);
        }
        DWORD const e = ::GetLastError();
        if (!winNotFound(e)) {
            ec = winError(e);
            return {};
        }
        if (!head.has_relative_path()) return whole;   // nothing of it exists
        rest = rest.empty() ? head.filename() : head.filename() / rest;
        head = head.parent_path();
    }
#else
    return fs::weakly_canonical(p, ec);
#endif
}

TreeWalk walkTree(fs::path const& root, std::function<WalkStep(TreeEntry const&)> const& visit,
                  TreeWalkOptions options) {
    TreeWalk walk;
    // One listed directory: its entries, and how many of them the walk has handed out.
    struct Listed {
        std::vector<TreeEntry> entries;
        std::size_t            next = 0;
    };
    std::vector<Listed> work;
    // List `dir` onto the stack; false ends the walk (its error recorded).
    auto const descend = [&](fs::path const& dir, std::size_t depth) {
        Listed          listed;
        std::error_code ec;
        listEntries(dir, depth, listed.entries, ec);
        if (ec) {
            if (options.skipPermissionDenied && isPermissionDenied(ec)) return true;
            walk.error = ec;
            walk.where = dir;
            return false;
        }
        for (auto& entry : listed.entries) {
            if (entry.kind != EntryKind::Link) continue;
            std::error_code reachEc;   // a link whose reach cannot be read reaches nothing usable
            entry.reaches = reachedKind(entry.path, reachEc);
        }
        work.push_back(std::move(listed));
        return true;
    };
    if (!descend(root, 0)) return walk;
    while (!work.empty()) {
        Listed& top = work.back();
        if (top.next == top.entries.size()) {
            work.pop_back();
            continue;
        }
        // A COPY: descending grows `work`, which would leave a reference into it dangling.
        TreeEntry const entry = top.entries[top.next++];
        WalkStep const  step  = visit(entry);
        if (step == WalkStep::Stop) {
            walk.stopped = true;
            return walk;
        }
        if (step == WalkStep::Prune || entry.kind != EntryKind::Directory) continue;
        if (!descend(entry.path, entry.depth + 1)) return walk;
    }
    return walk;
}

TreeRemoval removeTree(fs::path const& target) {
    TreeRemoval out;
    // A failure on an entry that is now GONE was beaten to the outcome wanted: it is no failure.
    auto const note = [&out](fs::path const& p, std::error_code const& ec) {
        std::error_code gec;
        if (entryKind(p, gec) == EntryKind::Absent && !gec) return;
        if (out.failures++ == 0) {
            out.firstPath  = p;
            out.firstError = ec;
        }
    };
    // `fs::remove` takes one entry: a link as itself (the directory flavour of a link included), a
    // file, or an EMPTY directory.
    auto const removeOne = [&note](fs::path const& p) {
        std::error_code ec;
        fs::remove(p, ec);
        if (ec) note(p, ec);
    };
    std::error_code tec;
    EntryKind const kind = entryKind(target, tec);
    if (tec) {
        note(target, tec);
        return out;
    }
    if (kind == EntryKind::Absent) return out;
    if (kind != EntryKind::Directory) {
        removeOne(target);
        return out;
    }
    // An explicit work stack: a directory is LISTED once, its entries handled, and it is removed
    // once every entry of that listing has been. Nothing is deleted under a live listing.
    struct Pending {
        fs::path dir;
        bool     listed = false;
    };
    std::vector<Pending> work;
    work.push_back({target, false});
    while (!work.empty()) {
        if (work.back().listed) {
            fs::path const dir = std::move(work.back().dir);
            work.pop_back();
            removeOne(dir);
            continue;
        }
        work.back().listed = true;
        fs::path const         dir = work.back().dir;   // a COPY: `work` grows below
        std::vector<TreeEntry> entries;
        std::error_code        ec;
        listEntries(dir, 0, entries, ec);
        if (ec) note(dir, ec);
        for (auto& entry : entries) {
            if (entry.kind == EntryKind::Absent) continue;   // already gone
            if (entry.kind == EntryKind::Directory) {
                work.push_back({std::move(entry.path), false});
                continue;
            }
            removeOne(entry.path);
        }
    }
    return out;
}

}  // namespace dss::core
