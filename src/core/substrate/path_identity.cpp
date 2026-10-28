#include "core/substrate/path_identity.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
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
    #include <winternl.h>   // NtOpenFile's argument types: the one relative open Win32 does not offer
#else
    #include <cerrno>
    #include <dirent.h>
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

namespace dss::core {
namespace {

namespace fs = std::filesystem;

#ifdef _WIN32

// `GetLongPathNameW` for ONE path, or empty when Windows will not answer.
//
// ★ WHAT IS LEFT FOR THIS CALL TO DO. `canonicalIdentityKey` resolves every key through `reachedPath`
// FIRST, and on Windows that is `GetFinalPathNameByHandleW` -- which already rewrites junctions, mount
// points and links and spells every component it resolved in its long form. So for the part of a path
// that EXISTS this expansion changes nothing (and a not-yet-existing tail has no long form to expand
// to). What it still expands is a key `reachedPath` did not produce: the lexical fallback taken when the
// resolution failed or would have relocated an authority. It touches nothing but 8.3 components.
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

// The answers Windows gives for "nothing is there" -- exactly the set MS STL's `status` reads as
// `not_found` (READ 2026-10-21: `__std_is_file_not_found` in xfilesystem_abi.h, MSVC 14.51.36231), so
// the MinGW build and the MSVC build call the same answers absence; every other refusal is an error.
[[nodiscard]] bool winNotFound(DWORD e) {
    return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_BAD_NETPATH
        || e == ERROR_INVALID_NAME || e == ERROR_BAD_PATHNAME || e == ERROR_DIRECTORY
        || e == ERROR_NETNAME_DELETED;
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

// ── THE HANDLE-RELATIVE REMOVAL'S PIECES (the header states why the removal never goes by path) ──

// One owned handle.
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h == INVALID_HANDLE_VALUE ? nullptr : h) {}
    Handle(Handle&& o) noexcept : h_(std::exchange(o.h_, nullptr)) {}
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) {
            reset();
            h_ = std::exchange(o.h_, nullptr);
        }
        return *this;
    }
    Handle(Handle const&)            = delete;
    Handle& operator=(Handle const&) = delete;
    ~Handle() { reset(); }
    [[nodiscard]] HANDLE get() const { return h_; }
    [[nodiscard]] explicit operator bool() const { return h_ != nullptr; }
    void reset() {
        if (h_ != nullptr) ::CloseHandle(h_);
        h_ = nullptr;
    }

private:
    HANDLE h_ = nullptr;
};

// `NtOpenFile` and the status translation, from ntdll (loaded in every process): Win32 has no call that
// opens a name RELATIVE to a directory handle, and that relative open is what keeps a swapped-in link
// out of the removal.
struct NtCalls {
    using OpenFile  = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG, ULONG);
    using ToWin32   = ULONG(NTAPI*)(LONG);
    OpenFile openFile = nullptr;
    ToWin32  toWin32  = nullptr;
};

[[nodiscard]] NtCalls const& ntCalls() {
    static NtCalls const calls = [] {
        NtCalls c;
        if (HMODULE const ntdll = ::GetModuleHandleW(L"ntdll.dll"); ntdll != nullptr) {
            c.openFile = reinterpret_cast<NtCalls::OpenFile>(reinterpret_cast<void*>(::GetProcAddress(ntdll, "NtOpenFile")));
            c.toWin32  = reinterpret_cast<NtCalls::ToWin32>(
                reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlNtStatusToDosError")));
        }
        return c;
    }();
    return calls;
}

// The NT open options (winternl.h's names, spelled here because not every SDK this builds with defines
// them all): never follow a reparse point, open for backup intent (a directory too), synchronous I/O, and
// the open's own demand on what it finds -- a directory, or anything but one.
constexpr ULONG kNtOpenReparsePoint     = 0x00200000;
constexpr ULONG kNtOpenForBackupIntent  = 0x00004000;
constexpr ULONG kNtSynchronousIo        = 0x00000020;
constexpr ULONG kNtDirectoryFile        = 0x00000001;
constexpr ULONG kNtNonDirectoryFile     = 0x00000040;
constexpr ULONG kNtObjCaseInsensitive   = 0x00000040;

// Open the entry `name` (one component) RELATIVE to the directory handle `dir`, NEVER following a link
// -- the entry itself is what is opened -- demanding `demand` of it (`kNtDirectoryFile`, which a link to a
// directory meets as the link, or `kNtNonDirectoryFile`). Not opened: `error` says why -- ERROR_DIRECTORY
// when the entry is not a directory a directory open demanded.
[[nodiscard]] Handle openEntryRelative(HANDLE dir, std::wstring const& name, ACCESS_MASK access, ULONG demand,
                                       DWORD& error) {
    NtCalls const& nt = ntCalls();
    if (nt.openFile == nullptr || nt.toWin32 == nullptr) {
        error = ERROR_PROC_NOT_FOUND;
        return {};
    }
    UNICODE_STRING us{};
    us.Buffer        = const_cast<PWSTR>(name.c_str());
    us.Length        = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    us.MaximumLength = us.Length;
    OBJECT_ATTRIBUTES oa{};
    oa.Length        = sizeof oa;
    oa.RootDirectory = dir;
    oa.ObjectName    = &us;
    oa.Attributes    = kNtObjCaseInsensitive;   // as every Win32 open names a file
    IO_STATUS_BLOCK io{};
    HANDLE          h = nullptr;
    LONG const status = nt.openFile(&h, access | SYNCHRONIZE, &oa, &io,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    kNtOpenReparsePoint | kNtOpenForBackupIntent | kNtSynchronousIo | demand);
    if (status < 0) {
        error = nt.toWin32(status);
        return {};
    }
    return Handle{h};
}

// The kind of what the handle OPENED (opened without following a link, so a link is a Link).
[[nodiscard]] EntryKind kindOfHandle(HANDLE h, DWORD& error) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!::GetFileInformationByHandleEx(h, FileAttributeTagInfo, &info, sizeof info)) {
        error = ::GetLastError();
        return EntryKind::Other;
    }
    return kindOfAttributes(info.FileAttributes, info.ReparseTag);
}

// Every name the directory `dir` holds but `.` and `..`, listed THROUGH its handle -- the one handle the
// removal also deletes it through, opened with FILE_LIST_DIRECTORY for both -- so the listing reads the
// directory that was opened, whatever its path names now. (A listing handle reopened from a handle opened
// for removal alone is refused: ReOpenFile answers ACCESS_DENIED for a directory, on both Windows builds.)
[[nodiscard]] std::vector<std::wstring> namesThrough(HANDLE dir, DWORD& error) {
    std::vector<std::wstring> names;
    std::vector<LONGLONG>      buffer(8 * 1024);   // 64 KiB, aligned for the records
    FILE_INFO_BY_HANDLE_CLASS  cls = FileIdBothDirectoryRestartInfo;
    for (;;) {
        if (!::GetFileInformationByHandleEx(dir, cls, buffer.data(),
                                            static_cast<DWORD>(buffer.size() * sizeof(LONGLONG)))) {
            DWORD const e = ::GetLastError();
            if (e != ERROR_NO_MORE_FILES) error = e;
            return names;
        }
        cls = FileIdBothDirectoryInfo;
        auto const* at = reinterpret_cast<unsigned char const*>(buffer.data());
        for (;;) {
            auto const* record = reinterpret_cast<FILE_ID_BOTH_DIR_INFO const*>(at);
            std::wstring_view const name{record->FileName, record->FileNameLength / sizeof(wchar_t)};
            if (name != L"." && name != L"..") names.emplace_back(name);
            if (record->NextEntryOffset == 0) break;
            at += record->NextEntryOffset;
        }
    }
}

// Delete the entry the handle opened, through the handle (it must carry DELETE). POSIX semantics where
// the OS has them (the name goes at once) and a read-only file deleted as MS STL's `remove` deletes one;
// otherwise the classic disposition, the read-only attribute cleared first. 0, or why it was refused.
[[nodiscard]] DWORD deleteThrough(HANDLE h) {
    struct DispositionEx {
        DWORD Flags;
    };
    constexpr DWORD kDelete = 0x1, kPosixSemantics = 0x2, kIgnoreReadOnly = 0x10;
    constexpr auto  kDispositionInfoEx = static_cast<FILE_INFO_BY_HANDLE_CLASS>(21);   // FileDispositionInfoEx
    DispositionEx ex{kDelete | kPosixSemantics | kIgnoreReadOnly};
    if (::SetFileInformationByHandle(h, kDispositionInfoEx, &ex, sizeof ex)) return ERROR_SUCCESS;
    DWORD const e = ::GetLastError();
    // An OS or a file system without the extended disposition answers one of these; anything else is
    // the deletion's own refusal (a directory not empty, a file in use, access denied).
    if (e != ERROR_INVALID_PARAMETER && e != ERROR_NOT_SUPPORTED && e != ERROR_INVALID_FUNCTION) return e;
    FILE_BASIC_INFO basic{};
    if (::GetFileInformationByHandleEx(h, FileBasicInfo, &basic, sizeof basic)
        && (basic.FileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
        basic.FileAttributes &= ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY);
        if (basic.FileAttributes == 0) basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        static_cast<void>(::SetFileInformationByHandle(h, FileBasicInfo, &basic, sizeof basic));
    }
    FILE_DISPOSITION_INFO classic{};
    classic.DeleteFile = TRUE;
    if (::SetFileInformationByHandle(h, FileDispositionInfo, &classic, sizeof classic)) return ERROR_SUCCESS;
    return ::GetLastError();
}

// The access a removal opens an entry with: delete it, read what it is, clear a read-only attribute -- and,
// for a directory, list it through the same handle.
constexpr ACCESS_MASK kRemovalAccess          = DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES;
constexpr ACCESS_MASK kDirectoryRemovalAccess = kRemovalAccess | FILE_LIST_DIRECTORY;

// A count of FILETIME ticks (100 ns since 1601-01-01) as the library's file clock `Clock` reads time. A file
// clock with `from_sys` (libstdc++'s) converts from the system clock, whose epoch is 1970-01-01, 11644473600 s
// later. MS STL's has none: its file clock IS that count -- its `now()` is the system ticks plus
// `__std_fs_file_time_epoch_adjustment`, 1601's distance from 1970 (READ, <chrono>, MSVC 14.51.36231) -- and its
// own `last_write_time` hands the FILETIME count back as it stands.
template <class Clock>
[[nodiscard]] fs::file_time_type fileTimeFromTicks(long long ticksSince1601) {
    using Ticks = std::chrono::duration<long long, std::ratio<1, 10'000'000>>;
    if constexpr (requires { Clock::from_sys(std::chrono::sys_time<Ticks>{}); }) {
        std::chrono::sys_time<Ticks> const sys{Ticks{ticksSince1601 - 116444736000000000LL}};
        return std::chrono::time_point_cast<fs::file_time_type::duration>(Clock::from_sys(sys));
    } else {
        static_assert(std::is_same_v<typename Clock::period, std::ratio<1, 10'000'000>>,
                      "a file clock with no from_sys is read as the FILETIME count, 100 ns ticks");
        return fs::file_time_type{fs::file_time_type::duration{ticksSince1601}};
    }
}

// A FILETIME as the library's file clock reads time.
[[nodiscard]] fs::file_time_type fileTimeOf(FILETIME const& ft) {
    ULARGE_INTEGER u{};
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return fileTimeFromTicks<fs::file_time_type::clock>(static_cast<long long>(u.QuadPart));
}

// A handle to what `p` REACHES (every link on the way followed), or none: nothing reached is no handle
// with `ec` clear.
[[nodiscard]] Handle reachedHandle(fs::path const& p, DWORD access, std::error_code& ec) {
    HANDLE const reached = ::CreateFileW(p.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (reached == INVALID_HANDLE_VALUE) {
        DWORD const e = ::GetLastError();
        if (!winNotFound(e)) ec = winError(e);
        return {};
    }
    return Handle{reached};
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

// ── THE DESCRIPTOR-RELATIVE REMOVAL'S PIECES (the header states why the removal never goes by path) ──

// One owned descriptor.
class Descriptor {
public:
    Descriptor() = default;
    explicit Descriptor(int fd) : fd_(fd) {}
    Descriptor(Descriptor&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    Descriptor& operator=(Descriptor&& o) noexcept {
        if (this != &o) {
            reset();
            fd_ = std::exchange(o.fd_, -1);
        }
        return *this;
    }
    Descriptor(Descriptor const&)            = delete;
    Descriptor& operator=(Descriptor const&) = delete;
    ~Descriptor() { reset(); }
    [[nodiscard]] int  get() const { return fd_; }
    [[nodiscard]] bool valid() const { return fd_ >= 0; }
    void reset() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

private:
    int fd_ = -1;
};

// The flags a removal opens a directory with: a directory and NEVER a link (a link, or anything that is
// not a directory, refuses the open -- ELOOP or ENOTDIR), and never a wait on a FIFO.
constexpr int kOpenDirectoryOnly = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK;

[[nodiscard]] std::error_code posixError(int e) {
    return std::error_code{e, std::generic_category()};
}

// Every name the directory open as `fd` holds but `.` and `..`, listed THROUGH the descriptor (a
// duplicate of it, which `fdopendir` takes), so the listing reads the directory that was opened.
[[nodiscard]] std::vector<std::string> namesThrough(int fd, int& error) {
    std::vector<std::string> names;
    int const listFd = ::dup(fd);
    if (listFd < 0) {
        error = errno;
        return names;
    }
    DIR* const stream = ::fdopendir(listFd);
    if (stream == nullptr) {
        error = errno;
        ::close(listFd);
        return names;
    }
    for (;;) {
        errno                    = 0;
        struct dirent const* ent = ::readdir(stream);
        if (ent == nullptr) {
            if (errno != 0) error = errno;
            break;
        }
        std::string_view const name{ent->d_name};
        if (name != "." && name != "..") names.emplace_back(name);
    }
    ::closedir(stream);
    return names;
}

#endif

// The removal's test seam (the header): empty in every build unless a test sets it.
[[nodiscard]] std::function<void(fs::path const&)>& removeTreeEntryHook() {
    static std::function<void(fs::path const&)> hook;
    return hook;
}

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

fs::path reachedRelative(fs::path const& p, fs::path const& base, std::error_code& ec) {
    fs::path const to = reachedPath(p, ec);
    if (ec) return {};
    fs::path const from = reachedPath(base, ec);
    if (ec) return {};
    return to.lexically_relative(from);
}

std::optional<fs::file_time_type> reachedWriteTime(fs::path const& p, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    Handle const reached = reachedHandle(p, FILE_READ_ATTRIBUTES, ec);
    if (!reached) return std::nullopt;
    FILETIME written{};
    if (!::GetFileTime(reached.get(), nullptr, nullptr, &written)) {
        ec = winError(::GetLastError());
        return std::nullopt;
    }
    return fileTimeOf(written);
#else
    auto const t = fs::last_write_time(p, ec);
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory) ec.clear();
        return std::nullopt;
    }
    return t;
#endif
}

bool reachedSameFile(fs::path const& a, fs::path const& b, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    Handle const first = reachedHandle(a, FILE_READ_ATTRIBUTES, ec);
    if (!first) return false;
    Handle const second = reachedHandle(b, FILE_READ_ATTRIBUTES, ec);
    if (!second) return false;
    // The 128-bit identity first (ReFS needs it), the 64-bit one where the file system has no other. The
    // record and its class are winbase.h's FILE_ID_INFO / FileIdInfo (Windows 8 on), spelled here because
    // MinGW-w64's headers declare them only for a _WIN32_WINNT this build does not set; an OS without the
    // class refuses the call, and the 64-bit identity answers.
    struct FileIdRecord {
        ULONGLONG VolumeSerialNumber;
        BYTE      Identifier[16];
    };
    constexpr auto kFileIdInfo = static_cast<FILE_INFO_BY_HANDLE_CLASS>(18);   // FileIdInfo
    FileIdRecord idA{}, idB{};
    if (::GetFileInformationByHandleEx(first.get(), kFileIdInfo, &idA, sizeof idA)
        && ::GetFileInformationByHandleEx(second.get(), kFileIdInfo, &idB, sizeof idB)) {
        return idA.VolumeSerialNumber == idB.VolumeSerialNumber
            && std::equal(std::begin(idA.Identifier), std::end(idA.Identifier), std::begin(idB.Identifier));
    }
    BY_HANDLE_FILE_INFORMATION infoA{}, infoB{};
    if (!::GetFileInformationByHandle(first.get(), &infoA) || !::GetFileInformationByHandle(second.get(), &infoB)) {
        ec = winError(::GetLastError());
        return false;
    }
    return infoA.dwVolumeSerialNumber == infoB.dwVolumeSerialNumber && infoA.nFileIndexHigh == infoB.nFileIndexHigh
        && infoA.nFileIndexLow == infoB.nFileIndexLow;
#else
    for (fs::path const* side : {&a, &b}) {
        auto const reached = fs::status(*side, ec).type();
        if (reached == fs::file_type::not_found) {
            ec.clear();
            return false;
        }
        if (ec) return false;
    }
    bool const same = fs::equivalent(a, b, ec);
    return !ec && same;
#endif
}

void addReachedPermissions(fs::path const& p, fs::perms add, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    Handle const reached = reachedHandle(p, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, ec);
    if (!reached) {
        if (!ec) ec = winError(ERROR_FILE_NOT_FOUND);
        return;
    }
    // Windows records one permission, the read-only attribute: a write bit added clears it, as MS STL
    // does; the other bits have nothing to set.
    if ((add & (fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write)) == fs::perms::none) return;
    FILE_BASIC_INFO basic{};
    if (!::GetFileInformationByHandleEx(reached.get(), FileBasicInfo, &basic, sizeof basic)) {
        ec = winError(::GetLastError());
        return;
    }
    if ((basic.FileAttributes & FILE_ATTRIBUTE_READONLY) == 0) return;
    basic.FileAttributes &= ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY);
    if (basic.FileAttributes == 0) basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
    if (!::SetFileInformationByHandle(reached.get(), FileBasicInfo, &basic, sizeof basic))
        ec = winError(::GetLastError());
#else
    fs::permissions(p, add, fs::perm_options::add, ec);   // follows every link (POSIX `chmod`)
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
            // A link whose reach cannot be examined reaches nothing usable, and the walk does not know
            // what that entry is: it ends here naming the link, unless the visitor asked to answer it.
            entry.reaches = reachedKind(entry.path, entry.reachError);
            if (entry.reachError && !options.handOutUnreadableLinks) {
                walk.error = entry.reachError;
                walk.where = entry.path;
                return false;
            }
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
    auto const& hook = removeTreeEntryHook();
    // An explicit work stack of OPEN directories: each is listed once, through its own handle, its
    // entries handled one by one -- each opened RELATIVE to it, never following a link, and judged by
    // what was opened -- and it is deleted once every entry of that listing has been. A directory's
    // handle stays open while its entries are handled, so every relative open names an entry of the
    // directory that was listed, whatever its path names by then.
#ifdef _WIN32
    struct Open {
        Handle                    dir;
        fs::path                  path;
        std::vector<std::wstring> names;
        std::size_t               next   = 0;
        bool                      listed = false;
    };
    // The target itself, opened as the entry it is (a link is opened as the link) -- a directory by ONE
    // opening that both lists and deletes it. An entry that refuses the listing access is opened again for
    // its deletion alone (a file this process may delete but not read, or one another process holds
    // without sharing reads); a DIRECTORY opened only so cannot be listed, and that refusal is the failure.
    auto const openTarget = [&target](ACCESS_MASK access, DWORD& error) {
        HANDLE const h = ::CreateFileW(target.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING,
                                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        error = h == INVALID_HANDLE_VALUE ? ::GetLastError() : ERROR_SUCCESS;
        return Handle{h};
    };
    DWORD  listingRefusal = ERROR_SUCCESS;
    Handle root           = openTarget(kDirectoryRemovalAccess, listingRefusal);
    if (!root) {
        if (winNotFound(listingRefusal)) return out;
        DWORD again = ERROR_SUCCESS;
        root        = openTarget(kRemovalAccess, again);
        if (!root) {
            if (!winNotFound(again)) note(target, winError(again));
            return out;
        }
    }
    DWORD     kindError = ERROR_SUCCESS;
    EntryKind kind      = kindOfHandle(root.get(), kindError);
    if (kindError != ERROR_SUCCESS) {
        note(target, winError(kindError));
        return out;
    }
    if (kind != EntryKind::Directory) {
        if (DWORD const e = deleteThrough(root.get()); e != ERROR_SUCCESS) note(target, winError(e));
        return out;
    }
    if (listingRefusal != ERROR_SUCCESS) {
        note(target, winError(listingRefusal));
        return out;
    }
    std::vector<Open> work;
    work.push_back(Open{std::move(root), target, {}, 0, false});
    while (!work.empty()) {
        Open& top = work.back();
        if (!top.listed) {
            top.listed      = true;
            DWORD listError = ERROR_SUCCESS;
            top.names       = namesThrough(top.dir.get(), listError);
            if (listError != ERROR_SUCCESS) note(top.path, winError(listError));
            continue;
        }
        if (top.next == top.names.size()) {
            // Every entry of its listing handled: the directory itself, through its own handle.
            DWORD const    e    = deleteThrough(top.dir.get());
            fs::path const path = top.path;
            work.pop_back();   // closes the handle: a classic disposition deletes it now
            if (e != ERROR_SUCCESS) note(path, winError(e));
            continue;
        }
        std::wstring const name  = top.names[top.next++];
        fs::path const     entry = top.path / name;
        if (hook) hook(entry);
        // A directory is opened ONCE for its listing and its deletion (a link to a directory meets that open
        // as the link, and is judged a link below); an entry that is no directory refuses it, and is opened
        // for its deletion alone.
        DWORD  openError = ERROR_SUCCESS;
        Handle opened    = openEntryRelative(top.dir.get(), name, kDirectoryRemovalAccess, kNtDirectoryFile, openError);
        if (!opened && openError == ERROR_DIRECTORY)
            opened = openEntryRelative(top.dir.get(), name, kRemovalAccess, kNtNonDirectoryFile, openError);
        if (!opened) {
            if (!winNotFound(openError)) note(entry, winError(openError));   // gone already is no failure
            continue;
        }
        kindError = ERROR_SUCCESS;
        kind      = kindOfHandle(opened.get(), kindError);
        if (kindError != ERROR_SUCCESS) {
            note(entry, winError(kindError));
            continue;
        }
        if (kind == EntryKind::Directory) {
            work.push_back(Open{std::move(opened), entry, {}, 0, false});   // `top` is not used past here
            continue;
        }
        // A file, or a link -- the link itself, which is what was opened.
        if (DWORD const e = deleteThrough(opened.get()); e != ERROR_SUCCESS) note(entry, winError(e));
    }
#else
    struct Open {
        Descriptor               dir;
        fs::path                 path;
        std::vector<std::string> names;
        std::size_t              next   = 0;
        bool                     listed = false;
    };
    // The target itself: a directory opened as one, or -- refused as a link or as no directory -- the
    // entry removed as itself (`unlink` never follows a link).
    int const first = ::open(target.c_str(), kOpenDirectoryOnly);
    if (first < 0) {
        int const e = errno;
        if (e == ENOENT) return out;
        if (e == ELOOP || e == ENOTDIR) {
            if (::unlink(target.c_str()) != 0 && errno != ENOENT) note(target, posixError(errno));
            return out;
        }
        note(target, posixError(e));
        return out;
    }
    std::vector<Open> work;
    work.push_back(Open{Descriptor{first}, target, {}, 0, false});
    while (!work.empty()) {
        Open& top = work.back();
        if (!top.listed) {
            top.listed    = true;
            int listError = 0;
            top.names     = namesThrough(top.dir.get(), listError);
            if (listError != 0) note(top.path, posixError(listError));
            continue;
        }
        if (top.next == top.names.size()) {
            // Every entry of its listing handled: the directory itself, relative to its parent's
            // descriptor -- the root by its path (`rmdir` never follows a link).
            fs::path const path = top.path;
            work.pop_back();   // closes its descriptor
            int const rc = work.empty() ? ::rmdir(path.c_str())
                                        : ::unlinkat(work.back().dir.get(), path.filename().c_str(), AT_REMOVEDIR);
            if (rc != 0 && errno != ENOENT) note(path, posixError(errno));
            continue;
        }
        std::string const name  = top.names[top.next++];
        fs::path const    entry = top.path / name;
        if (hook) hook(entry);
        int const opened = ::openat(top.dir.get(), name.c_str(), kOpenDirectoryOnly);
        if (opened >= 0) {
            work.push_back(Open{Descriptor{opened}, entry, {}, 0, false});   // `top` is not used past here
            continue;
        }
        int const e = errno;
        if (e == ENOENT) continue;   // gone already is no failure
        if (e != ELOOP && e != ENOTDIR) {
            note(entry, posixError(e));
            continue;
        }
        // A file, a link (the link itself), a FIFO, a device: removed as itself.
        if (::unlinkat(top.dir.get(), name.c_str(), 0) != 0 && errno != ENOENT) note(entry, posixError(errno));
    }
#endif
    return out;
}

namespace substrate_testing {
void setRemoveTreeEntryHook(std::function<void(fs::path const&)> hook) {
    removeTreeEntryHook() = std::move(hook);
}
}  // namespace substrate_testing

}  // namespace dss::core
