// THE TYPED HOST PATH HOME'S UNIVERSE (P69 H6c2, T5): every cell of the product `host_path_universe.hpp` generates
// from the home's own vocabulary is ONE gtest case, `Universe/HostPathCell.Holds/<cell>`, its name printing its axes.
//
// EACH CELL IS JUDGED THREE WAYS, in this order:
//   (1) THE REFERENCE RULE, over the whole product: the object a value's reference opens for its use is the object the
//       host reads for that value;
//   (2) THE OPERATIONS, over the WITNESS cells (`isWitness`): every reference-taking operation a cell can ask touches
//       that object -- the leaf's content, size, kind, handle and identity, the container's kind, listing and identity
//       -- an object it must not name is never the same file or the same key, and a write lands in it;
//   (3) TOTALITY, over the whole product: every operation of the home that takes a reference (`takesAReference` of its
//       row in `DSS_HOST_PATH_OPERATIONS`) runs on the cell's value and answers -- a value, a refusal by name, "not
//       found" -- and never throws. `EveryOperationThatTakesAReferenceIsInvoked` reddens when a row has no invoker.
//
// ★ THE ORACLES ARE THE HOST'S OWN ANSWERS, held independently of the home (the home audit's 4.6):
//   * a GIVEN value names what the host reads for its spelling at that use: the object the plain call
//     (`CreateFileW`, POSIX `open`) opens for the spelling as a LEAF, and for the CONTAINER the object it opens for a
//     child spelled after it by the OS's documented grammar (Microsoft, "Naming Files, Paths, and Namespaces": `C:x`
//     names `x` in drive C's current directory, and a `\\?\` spelling is passed as written) -- never by asking the
//     home's own composition;
//   * a LISTED value names the literal object a walk listed it as;
//   * a COMPOSED value (a typed right side below an answered left, whose text is DOS) names what the plain call opens
//     for the whole DOS spelling (B1);
//   * an ANSWERED value (`reachedPath`) names what its spelling reached -- and nothing for an object with no path (a
//     device, a volume opened as itself).
// Every candidate object carries its own marker ("literal", "twin!", "file!"), so the object a reference opened is
// READ, never inferred from its text; two objects are one when the host says so (the volume serial and file index on
// Windows, the device and inode on POSIX).
// ★ A BINDING THIS HOST REFUSES MAKES A NAMED UNMEASURED SKIP, never a pass: a share the host does not serve, a volume
// that makes no short names, no writable directory on another drive. `TheGuaranteedBindingsHoldOnThisLeg` is RED where
// a binding every leg of its kind guarantees was refused; a scaffold that must hold (a planted object the host does not
// read, a walk that does not list a planted name, a spelling that parses to another shape) is RED in its cell.
// ★ ANOTHER LETTER (the generator's rule (6): a short name's other drive) is a drive letter of the test's own
// (`MappedDrive`), defined onto its scratch: it claims what the LETTER decides, never the volume. Its cells are RED,
// never skipped, where the letter is not bound or its volume made no short name.

#include "host_path_universe.hpp"

#include "core/substrate/checked_file_read.hpp"
#include "core/substrate/path_identity.hpp"
#include "core/substrate/process_spawn.hpp"
#include "os_link.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // the oracle's plain calls, and the objects made through `\\?\` as the OS makes them
#include <cwctype>
#else
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace u  = dss::test_support::universe;
using dss::core::HostPath;
using dss::core::RootShape;
using dss::core::Use;

namespace {

using Native = fs::path::string_type;   // the host's own spelling of a name

[[nodiscard]] Native native(char const* ascii) {
    Native out;
    for (char const* c = ascii; *c != '\0'; ++c) out.push_back(static_cast<Native::value_type>(*c));
    return out;
}
[[nodiscard]] Native native(std::string const& ascii) { return native(ascii.c_str()); }
[[nodiscard]] std::string shown(Native const& s) {
    std::string out;
    for (auto const c : s) out.push_back(static_cast<unsigned>(c) < 0x80 ? static_cast<char>(c) : '?');
    return out;
}
#if defined(_WIN32)
constexpr wchar_t kSep = L'\\';
#else
constexpr char kSep = '/';
#endif
Native const kSepText(1, kSep);

// The TEXT edges' names: a name no ANSI code page holds, and the one no UTF-8 spells (a lone surrogate on Win32,
// bytes that are not UTF-8 on POSIX).
#if defined(_WIN32)
Native const kOutsideCodePage = Native{L'n', static_cast<wchar_t>(0xD83D), static_cast<wchar_t>(0xDE00)};
Native const kNoUtf8Spelling  = Native{L'n', static_cast<wchar_t>(0xD800)};
#else
Native const kOutsideCodePage = "n\xF0\x9F\x98\x80";
Native const kNoUtf8Spelling  = "n\xFF";
#endif

// A relative spelling's generic rendering, made here and not by the home: `/` between names, UTF-8, and a lone
// surrogate as its three WTF-8 bytes (Win32); the bytes as they are (POSIX).
[[nodiscard]] std::string genericOf(Native const& relative) {
#if defined(_WIN32)
    std::string out;
    for (std::size_t i = 0; i < relative.size(); ++i) {
        std::uint32_t cp = relative[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < relative.size() && relative[i + 1] >= 0xDC00
            && relative[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<std::uint32_t>(relative[i + 1]) - 0xDC00);
            ++i;
        }
        if (cp == L'\\') cp = '/';
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
#else
    return relative;
#endif
}

// ── WHAT THE HOST OPENS FOR A SPELLING: the plain call, the object identified, its marker read ────────────────────
struct Opened {
    bool          ok       = false;
    bool          pathless = false;   // a device, a volume opened as itself: an object with no path
    bool          dir      = false;
    unsigned long type     = 0;       // the host's file type of a pathless object
    std::uint64_t volume   = 0;
    std::uint64_t index    = 0;
    std::string   marker;             // a file's bytes (its first 64)
    long          error = 0;

    [[nodiscard]] bool sameAs(Opened const& o) const {
        if (ok != o.ok) return false;
        if (!ok) return true;   // both refused: the host names nothing for either
        if (pathless != o.pathless) return false;
        if (pathless) return type == o.type;
        return volume == o.volume && index == o.index;
    }
    [[nodiscard]] std::string describe() const {
        if (!ok) return "nothing (refused, error " + std::to_string(error) + ")";
        if (pathless) return "an object with no path (type " + std::to_string(type) + ")";
        if (dir) return "a directory (index " + std::to_string(index) + ")";
        return "the file holding \"" + marker + "\" (index " + std::to_string(index) + ")";
    }
};

[[nodiscard]] Opened openPlain(Native const& spelling) {
    Opened out;
#if defined(_WIN32)
    HANDLE const h = ::CreateFileW(spelling.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        out.error = static_cast<long>(::GetLastError());
        return out;
    }
    out.ok = true;
    BY_HANDLE_FILE_INFORMATION info{};
    wchar_t                    probe[8];
    DWORD const                type = ::GetFileType(h);
    if (type == FILE_TYPE_CHAR || !::GetFileInformationByHandle(h, &info)
        || ::GetFinalPathNameByHandleW(h, probe, 8, VOLUME_NAME_DOS) == 0) {
        out.pathless = true;
        out.type     = type;
        ::CloseHandle(h);
        return out;
    }
    out.volume = info.dwVolumeSerialNumber;
    out.index  = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    out.dir    = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (!out.dir) {
        char  buffer[64];
        DWORD got = 0;
        if (::ReadFile(h, buffer, sizeof buffer, &got, nullptr)) out.marker.assign(buffer, got);
    }
    ::CloseHandle(h);
#else
    int const fd = ::open(spelling.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        out.error = errno;
        return out;
    }
    out.ok = true;
    struct stat st{};
    if (::fstat(fd, &st) != 0 || S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) {
        out.pathless = true;
        out.type     = static_cast<unsigned long>(st.st_mode & S_IFMT);
        ::close(fd);
        return out;
    }
    out.volume = static_cast<std::uint64_t>(st.st_dev);
    out.index  = static_cast<std::uint64_t>(st.st_ino);
    out.dir    = S_ISDIR(st.st_mode);
    if (!out.dir) {
        char          buffer[64];
        ssize_t const got = ::read(fd, buffer, sizeof buffer);
        if (got > 0) out.marker.assign(buffer, static_cast<std::size_t>(got));
    }
    ::close(fd);
#endif
    return out;
}

// The child `name` of `spelling` as the OS's documented grammar spells it: no separator after a bare drive-relative
// `X:` (the drive's current directory) or after a spelling that already ends in one; one otherwise.
[[nodiscard]] Native documentedChild(Native const& spelling, Native const& name) {
    bool const bareDrive =
        dss::core::rootSpelling(spelling, dss::core::kHostPathGrammar).shape == RootShape::DriveRelativeBare;
    if (bareDrive || (!spelling.empty() && spelling.back() == kSep)) return spelling + name;
    return spelling + kSepText + name;
}

// What a VALUE's reference opens for `use`: the leaf as itself; the container by the child `child` spelled after it
// (a container reference carries no separator but at a volume root: its caller appends one).
[[nodiscard]] Opened openReference(HostPath const& value, Use use, Native const& child) {
    Native const reference = dss::core::substrate_testing::referenceForTest(value, use);
    if (reference.empty()) return Opened{};
    if (use == Use::Leaf) return openPlain(reference);
    return openPlain(reference.back() == kSep ? reference + child : reference + kSepText + child);
}

// Whether the host LISTS the directory `spelling` names: its own call (FindFirstFileW of `<dir>\*`; POSIX
// opendir), which a walk must agree with.
[[nodiscard]] bool hostLists(Native const& spelling) {
#if defined(_WIN32)
    WIN32_FIND_DATAW data{};
    HANDLE const     h = ::FindFirstFileW((spelling + L"\\*").c_str(), &data);
    if (h == INVALID_HANDLE_VALUE) return ::GetLastError() == ERROR_FILE_NOT_FOUND;   // listed, and empty
    ::FindClose(h);
    return true;
#else
    DIR* const d = ::opendir(spelling.c_str());
    if (d == nullptr) return false;
    ::closedir(d);
    return true;
#endif
}

// ── MAKING THE OBJECTS, literally (`\\?\` on Windows: every name is made as written) ───────────────────────────────
#if defined(_WIN32)
[[nodiscard]] Native literal(Native const& absolute) { return L"\\\\?\\" + absolute; }
bool makeDirectory(Native const& absolute) { return ::CreateDirectoryW(literal(absolute).c_str(), nullptr) != 0; }
bool makeFile(Native const& absolute, std::string const& marker) {
    HANDLE const h = ::CreateFileW(literal(absolute).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD      written = 0;
    BOOL const ok      = ::WriteFile(h, marker.data(), static_cast<DWORD>(marker.size()), &written, nullptr);
    ::CloseHandle(h);
    return ok != 0 && written == marker.size();
}
// The 8.3 alias the volume made for the last name of `absolute`, or empty when it made none.
[[nodiscard]] Native shortNameOf(Native const& absolute) {
    Native const whole = literal(absolute);
    DWORD const  n     = ::GetShortPathNameW(whole.c_str(), nullptr, 0);
    if (n == 0) return {};
    Native      out(n, L'\0');
    DWORD const written = ::GetShortPathNameW(whole.c_str(), out.data(), n);
    if (written == 0 || written >= n) return {};
    out.resize(written);
    return out.substr(out.find_last_of(L'\\') + 1);
}
#else
[[nodiscard]] Native literal(Native const& absolute) { return absolute; }
bool makeDirectory(Native const& absolute) { return ::mkdir(absolute.c_str(), 0700) == 0; }
bool makeFile(Native const& absolute, std::string const& marker) {
    int const fd = ::open(absolute.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    bool const ok = ::write(fd, marker.data(), marker.size()) == static_cast<ssize_t>(marker.size());
    ::close(fd);
    return ok;
}
[[nodiscard]] Native shortNameOf(Native const&) { return {}; }
#endif

// ── THE HOST'S BINDINGS, asked once per run (made by the first cell, removed by the environment's TearDown) ───────
constexpr char const* kVolumeChildOfTheCwd = "universe-child";   // what a bare drive's container is judged by

#if defined(_WIN32)
// ── A DRIVE LETTER OF THE TEST'S OWN: `X:` defined onto a directory of this run's scratch ─────────────────────────────
// For the OtherLetter cells (the ShortName edge's other drive, the generator's rule (6)). A drive letter is a name in
// this logon session's DOS device namespace (DefineDosDeviceW; MS Learn, "Defining an MS-DOS Device Name"): the
// drive-relative grammar keys on the LETTER and the state the process keeps for it (its current directory `=X:`), never
// on the volume -- so a letter onto the scratch is another LETTER for that grammar, and NOT another volume: its objects,
// their serial, their final paths and their short-name policy are the scratch volume's.
// ★ ITS NAMESPACE, GUARDED THREE WAYS (the H6d design audit's D9, main's ruling):
//   * the sweep, the choice and the definition are made under ONE host-wide named mutex (`LetterLock`), so the runs of
//     this binary on one host (the two Windows legs) never choose one letter at once; the letter must still be its
//     only definition (a definer outside the lock pushes over it: ours goes, the next letter);
//   * every definition is STAMPED with its owner: its target is `<scratch>\letter-of-<pid>-<start>`, the owner's process
//     id and creation time, inside this test's scratch root;
//   * the SWEEP removes a definition only when it lies inside this test's scratch root, carries a stamp, and its owner
//     is GONE (no process holds the id, the one that does has exited, or it was created at another time: the id
//     reused). A live owner's definition, an unstamped one and every other is never touched.
//   It is defined with DDD_NO_BROADCAST_SYSTEM (no WM_SETTINGCHANGE to the shell) and removed with
//   DDD_EXACT_MATCH_ON_REMOVE (definitions STACK and a removal otherwise matches by prefix). `~MappedDrive` removes it
//   on every exit that runs destructors; a run that dies first leaves it for the next run's sweep.

// The definitions `letter` holds, most recent first (QueryDosDeviceW's list), or none.
[[nodiscard]] std::vector<Native> definitionsOf(Native const& letter) {
    std::vector<wchar_t> buffer(65536);
    DWORD const          n = ::QueryDosDeviceW(letter.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
    std::vector<Native>  out;
    for (DWORD at = 0; at < n && buffer[at] != L'\0';) {
        out.emplace_back(buffer.data() + at);
        at += static_cast<DWORD>(out.back().size()) + 1;
    }
    return out;
}
// Whether no drive and no definition holds `letter`: the host answers "not found" for its name.
[[nodiscard]] bool letterIsFree(Native const& letter) {
    auto const index = static_cast<unsigned>(std::towupper(letter[0])) - static_cast<unsigned>(L'A');
    if ((::GetLogicalDrives() & (DWORD{1} << index)) != 0) return false;
    wchar_t probe[2] = {};
    return ::QueryDosDeviceW(letter.c_str(), probe, 2) == 0 && ::GetLastError() == ERROR_FILE_NOT_FOUND;
}
bool removeDefinition(Native const& letter, Native const& target) {
    return ::DefineDosDeviceW(DDD_RAW_TARGET_PATH | DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE, letter.c_str(),
                              target.c_str())
        != 0;
}
// Defines the first free letter from Z down onto `target` (an NT path, `\??\...`), as that letter's only definition,
// and returns it -- or nothing, with why: the host refused the definition, or no letter was free. Called under the
// `LetterLock`.
[[nodiscard]] std::optional<Native> defineFreeLetter(Native const& target, std::string& why) {
    std::string taken;
    for (wchar_t c = L'Z'; c >= L'D'; --c) {
        Native const name{c, L':'};
        if (!letterIsFree(name)) continue;
        if (!::DefineDosDeviceW(DDD_RAW_TARGET_PATH | DDD_NO_BROADCAST_SYSTEM, name.c_str(), target.c_str())) {
            why = "DefineDosDeviceW refused " + shown(name) + ": error " + std::to_string(::GetLastError());
            return std::nullopt;
        }
        if (definitionsOf(name) == std::vector<Native>{target}) return name;
        static_cast<void>(removeDefinition(name, target));   // another defined it at once: ours goes, the next letter
        taken += " " + shown(name);
    }
    why = "no drive letter was free" + (taken.empty() ? std::string{} : " (defined by another at once:" + taken + ")");
    return std::nullopt;
}

// THE HOST-WIDE LOCK every choice and definition of a letter, and every sweep, is made under.
class LetterLock {
public:
    LetterLock() {
        mutex_ = ::CreateMutexW(nullptr, FALSE, L"Global\\dss-host-path-universe-drive-letters");
        if (mutex_ == nullptr) {
            refused_ = "the host-wide letter lock could not be made: error " + std::to_string(::GetLastError());
            return;
        }
        DWORD const waited = ::WaitForSingleObject(mutex_, 120000);
        held_              = waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED;   // abandoned: its holder died
        if (!held_) refused_ = "the host-wide letter lock was not taken within 120 s (wait " + std::to_string(waited) + ")";
    }
    ~LetterLock() {
        if (held_) static_cast<void>(::ReleaseMutex(mutex_));
        if (mutex_ != nullptr) ::CloseHandle(mutex_);
    }
    LetterLock(LetterLock const&)            = delete;
    LetterLock& operator=(LetterLock const&) = delete;
    [[nodiscard]] std::string const& refused() const noexcept { return refused_; }

private:
    HANDLE      mutex_ = nullptr;
    bool        held_  = false;
    std::string refused_;
};

// A process's creation time, as the 100 ns count its FILETIME holds, or nothing.
[[nodiscard]] std::optional<std::uint64_t> createdAt(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!::GetProcessTimes(process, &created, &exited, &kernel, &user)) return std::nullopt;
    return (std::uint64_t{created.dwHighDateTime} << 32) | created.dwLowDateTime;
}
// THE OWNER STAMP: the last name of a definition's target, `letter-of-<pid>-<start>`.
[[nodiscard]] Native ownerStamp(DWORD pid, std::uint64_t start) {
    return L"letter-of-" + std::to_wstring(pid) + L"-" + std::to_wstring(start);
}
// Whether the owner a stamp names is GONE: no process holds the id (OpenProcess answers ERROR_INVALID_PARAMETER, as
// for the idle process's 0), the one that does has exited, or it was created at another time (the id reused). A
// process this one may not query, or whose creation time it cannot read, is alive.
[[nodiscard]] bool ownerIsGone(DWORD pid, std::uint64_t start) {
    HANDLE const h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (h == nullptr) return ::GetLastError() == ERROR_INVALID_PARAMETER;
    bool gone = ::WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
    if (!gone) {
        std::optional<std::uint64_t> const created = createdAt(h);
        gone                                       = created && *created != start;
    }
    ::CloseHandle(h);
    return gone;
}
// THE SWEEP, under the `LetterLock`: removes every definition inside `scratchRoot` whose stamp names an owner that is
// gone, and returns how many it removed.
std::size_t sweepStaleDefinitions(Native const& scratchRoot) {
    Native const ours   = L"\\??\\" + scratchRoot + L"\\";
    Native const prefix = L"letter-of-";
    std::size_t  swept  = 0;
    for (wchar_t c = L'A'; c <= L'Z'; ++c) {
        Native const name{c, L':'};
        for (Native const& target : definitionsOf(name)) {
            if (target.size() <= ours.size()
                || ::CompareStringOrdinal(target.c_str(), static_cast<int>(ours.size()), ours.c_str(),
                                          static_cast<int>(ours.size()), TRUE)
                       != CSTR_EQUAL)
                continue;
            Native const stamp = target.substr(target.find_last_of(L'\\') + 1);
            if (stamp.compare(0, prefix.size(), prefix) != 0) continue;   // unstamped: never touched
            Native const      rest = stamp.substr(prefix.size());
            std::size_t const dash = rest.find(L'-');
            if (dash == 0 || dash == Native::npos || dash > 10 || rest.size() - dash - 1 == 0 || rest.size() - dash - 1 > 19
                || rest.find_first_not_of(L"0123456789") != dash
                || rest.find_first_not_of(L"0123456789", dash + 1) != Native::npos)
                continue;
            unsigned long long const pid   = std::stoull(rest.substr(0, dash));
            unsigned long long const start = std::stoull(rest.substr(dash + 1));
            if (pid > MAXDWORD) continue;
            if (ownerIsGone(static_cast<DWORD>(pid), start) && removeDefinition(name, target)) ++swept;
        }
    }
    return swept;
}

struct MappedDrive {
    Native                letter;          // `X:`, chosen when the run starts
    Native                target;          // `\??\<scratch>\letter-of-<pid>-<start>`: the definition, as held
    Native                root;            // `X:\cwd`: the directory its `=X:` names, reached through the letter
    std::optional<Native> cwdWas;          // the `=X:` the process held before
    bool                  cwdSet = false;
    std::string           refused;         // why no letter was bound: the cells that need one are red
    std::size_t           swept = 0;       // stale definitions of earlier runs removed

    explicit MappedDrive(Native const& scratch) {
        std::optional<std::uint64_t> const start = createdAt(::GetCurrentProcess());
        if (!start) {
            refused = "this process's creation time is unreadable: error " + std::to_string(::GetLastError());
            return;
        }
        Native const directory = scratch + L"\\" + ownerStamp(::GetCurrentProcessId(), *start);
        if (!makeDirectory(directory)) {
            refused = "the letter's directory could not be made: error " + std::to_string(::GetLastError());
            return;
        }
        {
            LetterLock const lock;
            if (!lock.refused().empty()) {
                refused = lock.refused();
                return;
            }
            swept = sweepStaleDefinitions(scratch.substr(0, scratch.find_last_of(L'\\')));
            std::optional<Native> const defined = defineFreeLetter(L"\\??\\" + directory, refused);
            if (!defined) return;
            letter = *defined;
            target = L"\\??\\" + directory;
        }
        Native const at = letter + L"\\cwd";
        if (!makeDirectory(at)) {
            refused = "the directory " + shown(at) + " could not be made through the letter";
            return;
        }
        Native const         name = L"=" + letter;
        std::vector<wchar_t> buffer(32768);
        DWORD const          was = ::GetEnvironmentVariableW(name.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
        if (was > 0 && was < buffer.size()) cwdWas = Native{buffer.data(), was};
        cwdSet = ::SetEnvironmentVariableW(name.c_str(), at.c_str()) != 0;
        DWORD const full = ::GetFullPathNameW((letter + L"x").c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
        if (!cwdSet || full == 0 || full >= buffer.size() || Native{buffer.data(), full} != at + L"\\x") {
            refused = "the host does not resolve " + shown(letter) + "x in the letter's own `=X:`";
            return;
        }
        // THE BINDING'S CONTROL: an object made through the letter is the scratch's own.
        if (!makeFile(at + kSepText + native(kVolumeChildOfTheCwd), "child")
            || !openPlain(literal(directory + L"\\cwd" + kSepText + native(kVolumeChildOfTheCwd))).ok) {
            refused = "an object made through " + shown(letter) + " is not in the scratch's directory it is defined onto";
            return;
        }
        root = at;
    }
    ~MappedDrive() {
        if (cwdSet) static_cast<void>(::SetEnvironmentVariableW((L"=" + letter).c_str(), cwdWas ? cwdWas->c_str() : nullptr));
        if (!target.empty() && !removeDefinition(letter, target))
            std::fprintf(stderr, "[ universe ] WARNING: the drive letter %s was not removed (error %lu): the next run sweeps it\n",
                         shown(letter).c_str(), ::GetLastError());
    }
    MappedDrive(MappedDrive const&)            = delete;
    MappedDrive& operator=(MappedDrive const&) = delete;
};
#endif

struct Bindings {
    dss::test_support::ScratchDir scratch{dss::test_support::Location::Temp, "host-path-universe"};
    Native                        root;          // the scratch, absolute (DOS on Windows); the process's directory
    Native                        firstName;     // the first name below the scratch's volume root
    Native                        originalCwd;
    std::string                   refused;       // why the scratch could not be entered: every cell is red
#if defined(_WIN32)
    Native                drive;        // `C:`: the scratch's drive
    Native                volumeGuid;   // `Volume{G}`, or empty
    Native                ntDevice;     // `\Device\HarddiskVolumeN`, or empty
    bool                  share = false;   // `\\localhost\X$\...` reaches the scratch
    Native                otherDrive;      // `D:`: another drive whose `=D:` names `otherRoot`, or empty
    Native                otherRoot;       // a writable directory on it, absolute, or empty
    std::optional<Native> otherDriveWas;   // the `=D:` the process held before
    MappedDrive           mapped{scratch.path().native()};   // after `scratch`: its letter goes before the scratch

    // The drive a cell's drive-relative spelling names and the directory its `=X:` holds: the scratch's own for the
    // same drive, the other volume's for another drive, the letter of the test's own for another LETTER. The
    // directory is empty where this host bound none.
    struct DriveBinding {
        Native letter;
        Native directory;
    };
    [[nodiscard]] DriveBinding driveFor(u::Drive which) const {
        switch (which) {
            case u::Drive::Same: return {drive, root};
            case u::Drive::Other: return {otherDrive, otherRoot};
            case u::Drive::OtherLetter: return {mapped.letter, mapped.root};
        }
        return {};
    }
#endif
    std::size_t next = 0;

    Bindings() {
#if defined(_WIN32)
        root = scratch.path().native();
        std::vector<wchar_t> buffer(32768);
        DWORD const          n = ::GetCurrentDirectoryW(static_cast<DWORD>(buffer.size()), buffer.data());
        originalCwd.assign(buffer.data(), n);
        if (root.size() < 4 || root[1] != L':' || root[2] != L'\\') {
            refused = "the scratch lies on no drive: " + shown(root);
            return;
        }
        if (!::SetCurrentDirectoryW(root.c_str())) {
            refused = "the scratch could not be entered: error " + std::to_string(::GetLastError());
            return;
        }
        drive     = root.substr(0, 2);
        firstName = root.substr(3, root.find(L'\\', 3) == Native::npos ? Native::npos : root.find(L'\\', 3) - 3);
        wchar_t guid[64] = {};
        if (::GetVolumeNameForVolumeMountPointW((drive + L"\\").c_str(), guid, 64)) {
            Native const g{guid};   // `\\?\Volume{G}\`
            if (g.size() > 5 && g.compare(0, 4, L"\\\\?\\") == 0 && g.back() == L'\\') volumeGuid = g.substr(4, g.size() - 5);
        }
        wchar_t device[1024] = {};
        if (::QueryDosDeviceW(drive.c_str(), device, 1024)) {
            Native const d{device};
            if (d.compare(0, 8, L"\\Device\\") == 0) ntDevice = d;
        }
        Native const viaShare = L"\\\\localhost\\" + Native(1, drive[0]) + L"$" + root.substr(2);
        share                 = ::GetFileAttributesW(viaShare.c_str()) != INVALID_FILE_ATTRIBUTES;
        bindAnotherDrive();
#else
        root = scratch.path().native();
        std::vector<char> buffer(4096);
        if (::getcwd(buffer.data(), buffer.size()) != nullptr) originalCwd = buffer.data();
        if (root.size() < 2 || root[0] != '/') {
            refused = "the scratch is not absolute: " + root;
            return;
        }
        if (::chdir(root.c_str()) != 0) {
            refused = "the scratch could not be entered: errno " + std::to_string(errno);
            return;
        }
        firstName = root.substr(1, root.find('/', 1) == Native::npos ? Native::npos : root.find('/', 1) - 1);
#endif
        if (!makeFile(root + kSepText + native(kVolumeChildOfTheCwd), "child")) refused = "the scratch's child could not be made";
    }

#if defined(_WIN32)
    // ANOTHER DRIVE: a writable directory on a drive that is not the scratch's, named by that drive's `=X:` -- the
    // test binary's own directory, else the user's profile. The binding's control: the host resolves `X:x` there.
    void bindAnotherDrive() {
        std::vector<Native>  candidates;
        std::vector<wchar_t> buffer(32768);
        DWORD const          imageLength = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        Native const         image{buffer.data(), imageLength};
        if (image.find_last_of(L'\\') != Native::npos) candidates.push_back(image.substr(0, image.find_last_of(L'\\')));
        for (wchar_t const* variable : {L"LOCALAPPDATA", L"USERPROFILE"}) {
            DWORD const length = ::GetEnvironmentVariableW(variable, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length > 0 && length < buffer.size()) candidates.emplace_back(buffer.data(), length);
        }
        for (Native const& at : candidates) {
            if (at.size() < 3 || at[1] != L':' || std::towupper(at[0]) == std::towupper(drive[0])) continue;
            Native const dir = at + L"\\dss-host-path-universe-" + std::to_wstring(::GetCurrentProcessId());
            if (!::CreateDirectoryW(dir.c_str(), nullptr)) continue;
            Native const letter = at.substr(0, 2);
            Native const name   = L"=" + letter;
            DWORD const  was    = ::GetEnvironmentVariableW(name.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
            std::optional<Native> const before =
                was > 0 && was < buffer.size() ? std::optional<Native>{Native{buffer.data(), was}} : std::nullopt;
            static_cast<void>(::SetEnvironmentVariableW(name.c_str(), dir.c_str()));
            DWORD const full = ::GetFullPathNameW((letter + L"x").c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
            if (Native{buffer.data(), full} == dir + L"\\x" && makeFile(dir + L"\\" + native(kVolumeChildOfTheCwd), "child")) {
                otherDrive    = letter;
                otherRoot     = dir;
                otherDriveWas = before;
                return;
            }
            static_cast<void>(::SetEnvironmentVariableW(name.c_str(), before ? before->c_str() : nullptr));
            static_cast<void>(dss::core::removeTree(HostPath{fs::path{dir}}));
        }
    }
#endif

    ~Bindings() {
#if defined(_WIN32)
        static_cast<void>(::SetCurrentDirectoryW(originalCwd.c_str()));
        if (!otherRoot.empty()) {
            if (dss::core::removeTree(HostPath{fs::path{otherRoot}}).failures != 0)
                std::fprintf(stderr, "[ universe ] WARNING: the other drive's directory was not removed whole\n");
            static_cast<void>(::SetEnvironmentVariableW((L"=" + otherDrive).c_str(),
                                                        otherDriveWas ? otherDriveWas->c_str() : nullptr));
        }
#else
        if (!originalCwd.empty()) static_cast<void>(::chdir(originalCwd.c_str()));
#endif
    }
    Bindings(Bindings const&)            = delete;
    Bindings& operator=(Bindings const&) = delete;
};

std::unique_ptr<Bindings>& bindingsSlot() {
    static std::unique_ptr<Bindings> slot;
    return slot;
}
Bindings& bindings() {
    std::unique_ptr<Bindings>& slot = bindingsSlot();
    if (!slot) slot = std::make_unique<Bindings>();
    return *slot;
}
std::map<std::string, std::chrono::steady_clock::duration>& timings();   // the budget's measurement, below

class UniverseEnvironment : public ::testing::Environment {
public:
    void TearDown() override {
        std::vector<std::pair<std::chrono::steady_clock::duration, std::string>> spent;
        for (auto const& [what, d] : timings()) spent.emplace_back(d, what);
        std::sort(spent.rbegin(), spent.rend());
        for (std::size_t i = 0; i < spent.size() && i < 16; ++i)
            std::cout << "[ universe ] time " << std::chrono::duration_cast<std::chrono::milliseconds>(spent[i].first).count()
                      << " ms: " << spent[i].second << "\n";
        bindingsSlot().reset();
    }
};
[[maybe_unused]] ::testing::Environment* const kUniverseEnvironment =
    ::testing::AddGlobalTestEnvironment(new UniverseEnvironment);

// ── A CELL'S SPELLING: the absolute `path` (DOS or POSIX, below the scratch or the other drive's directory) in
// `shape` -- or, for a shape that spells its volume alone or a named device, that spelling -- or nothing, with why,
// where this host cannot bind it.
struct Spelled {
    std::optional<Native> text;
    std::string           unmeasured;
};

[[nodiscard]] Spelled spell(RootShape shape, Native const& path, u::Drive drive) {
    Bindings& b = bindings();
    using S     = RootShape;
#if defined(_WIN32)
    Bindings::DriveBinding const named = b.driveFor(drive);
    Native const base      = named.directory;
    Native const tail      = path.size() > 2 ? path.substr(2) : Native{};   // `\Users\...`
    Native const shareName = Native(1, b.drive[0]) + L"$";
    Native const unc       = L"UNC\\localhost\\" + shareName;
    auto const   noShare   = [&] {
        return Spelled{std::nullopt, "UNMEASURED: this host serves no \\\\localhost\\" + shown(shareName) + " share"};
    };
    auto const share = [&](Native const& text) { return b.share ? Spelled{text, {}} : noShare(); };
    auto const guid  = [&](Native const& prefix, Native const& rest) {
        return b.volumeGuid.empty() ? Spelled{std::nullopt, "UNMEASURED: the scratch's volume answers no GUID name"}
                                    : Spelled{prefix + b.volumeGuid + rest, {}};
    };
    auto const device = [&](Native const& prefix, Native const& rest) {
        return b.ntDevice.empty() ? Spelled{std::nullopt, "UNMEASURED: the scratch's drive answers no NT device"}
                                  : Spelled{prefix + L"GLOBALROOT" + b.ntDevice + rest, {}};
    };
    if (base.empty())   // an OtherLetter cell's letter unbound is red in its fixture first (`makeFixture`)
        return {std::nullopt, "UNMEASURED: no writable directory on another drive whose `=X:` the host honours"};
    switch (shape) {
        case S::Relative: return {path.substr(b.root.size() + 1), {}};
        case S::DriveRelativeName: return {named.letter + path.substr(base.size() + 1), {}};
        case S::Rooted: return {tail, {}};
        case S::Drive: return {path, {}};
        case S::ShareName: return share(L"\\\\localhost\\" + shareName + tail);
        case S::DeviceDrive: return {L"\\\\.\\" + b.drive + tail, {}};
        case S::DeviceVolume: return guid(L"\\\\.\\", tail);
        case S::DeviceUnc: return share(L"\\\\.\\" + unc + tail);
        case S::DeviceGlobalRoot: return device(L"\\\\.\\", tail);
        case S::VerbatimDrive: return {L"\\\\?\\" + b.drive + tail, {}};
        case S::VerbatimVolume: return guid(L"\\\\?\\", tail);
        case S::VerbatimUnc: return share(L"\\\\?\\" + unc + tail);
        case S::VerbatimGlobalRoot: return device(L"\\\\?\\", tail);
        case S::NtDrive: return {L"\\??\\" + b.drive + tail, {}};
        case S::NtVolume: return guid(L"\\??\\", tail);
        case S::NtUnc: return share(L"\\??\\" + unc + tail);
        case S::NtGlobalRoot: return device(L"\\??\\", tail);
        // ── a volume alone: `path` is not read ──
        case S::DriveRelativeBare: return {named.letter, {}};
        case S::RootedBare: return {L"\\", {}};
        case S::DriveRoot: return {b.drive + L"\\", {}};
        case S::ShareServer: return share(L"\\\\localhost");
        case S::ShareBare: return share(L"\\\\localhost\\" + shareName);
        case S::ShareRoot: return share(L"\\\\localhost\\" + shareName + L"\\");
        case S::DeviceDriveBare: return {L"\\\\.\\" + b.drive, {}};
        case S::VerbatimDriveBare: return {L"\\\\?\\" + b.drive, {}};
        case S::NtDriveBare: return {L"\\??\\" + b.drive, {}};
        case S::DeviceVolumeBare: return guid(L"\\\\.\\", {});
        case S::VerbatimVolumeBare: return guid(L"\\\\?\\", {});
        case S::NtVolumeBare: return guid(L"\\??\\", {});
        case S::DeviceUncBare: return share(L"\\\\.\\" + unc);
        case S::VerbatimUncBare: return share(L"\\\\?\\" + unc);
        case S::NtUncBare: return share(L"\\??\\" + unc);
        case S::DeviceGlobalRootBare: return device(L"\\\\.\\", {});
        case S::VerbatimGlobalRootBare: return device(L"\\\\?\\", {});
        case S::NtGlobalRootBare: return device(L"\\??\\", {});
        // ── a named device: the null device, present on every Win32 host ──
        case S::DeviceNamedBare: return {L"\\\\.\\NUL", {}};
        case S::DeviceNamed: return {L"\\\\.\\NUL\\x", {}};
        case S::VerbatimNamedBare: return {L"\\\\?\\NUL", {}};
        case S::VerbatimNamed: return {L"\\\\?\\NUL\\x", {}};
        case S::NtNamedBare: return {L"\\??\\NUL", {}};
        case S::NtNamed: return {L"\\??\\NUL\\x", {}};
        case S::PosixRelative:
        case S::PosixRoot:
        case S::PosixRooted:
        case S::PosixAuthorityRoot:
        case S::PosixAuthority: return {std::nullopt, "a POSIX shape on a Win32 host"};
    }
    return {std::nullopt, "unbound"};
#else
    static_cast<void>(drive);
    switch (shape) {
        case S::PosixRelative: return {path.substr(b.root.size() + 1), {}};
        case S::PosixRooted: return {path, {}};
        case S::PosixAuthority: return {"/" + path, {}};
        case S::PosixRoot: return {"/", {}};
        case S::PosixAuthorityRoot: return {"//", {}};
        default: return {std::nullopt, "a Win32 shape on a POSIX host"};
    }
#endif
}

// The child a VOLUME-ALONE container is judged by: an entry the documented directory holds -- the first name below
// the scratch's volume root, the scratch's own child for a bare drive (its current directory), the share for a server.
[[nodiscard]] Native volumeChild(RootShape shape) {
    Bindings& b = bindings();
    if (shape == RootShape::DriveRelativeBare) return native(kVolumeChildOfTheCwd);
#if defined(_WIN32)
    if (shape == RootShape::ShareServer) return Native(1, b.drive[0]) + L"$";
#endif
    return b.firstName;
}

// ── ONE CELL'S OBJECTS ───────────────────────────────────────────────────────────────────────────────────────────────
// Below the cell's own directory: the LITERAL object the edge names (marker "literal") and, where the DOS rule trims
// the name, its TWIN (marker "twin!") -- each a file (Leaf) or a directory holding the child `c` and `only-<which>`
// (Container), at the last position or holding `x` in the middle. A stream's file holds "file!"; a link's target is `E`.
struct Fixture {
    Native                        dir;           // absolute: the cell's own directory
    Native                        spelledTail;   // what the cell spells below `dir`
    std::map<std::string, Native> objects;       // marker -> the object, at the cell's use, that carries it (absolute)
    std::string                   unmeasured;
    std::string                   failure;
    bool                          made = false;
};

[[nodiscard]] char const* onlyName(std::string const& marker) { return marker == "literal" ? "only-literal" : "only-twin"; }

[[nodiscard]] Fixture makeFixture(u::Cell const& c) {
    Bindings& b = bindings();
    Fixture   f;
#if defined(_WIN32)
    Native const base = b.driveFor(c.drive).directory;
    if (base.empty() && c.drive == u::Drive::OtherLetter) {
        f.failure = "the drive letter of the test's own was not bound: " + b.mapped.refused;
        return f;
    }
#else
    Native const base = b.root;
#endif
    if (base.empty()) {
        f.unmeasured = "UNMEASURED: no writable directory on another drive whose `=X:` the host honours";
        return f;
    }
    f.dir = base + kSepText + native("c" + std::to_string(b.next++));
    if (!makeDirectory(f.dir)) {
        f.failure = "the cell's directory could not be made: " + shown(f.dir);
        return f;
    }
    if (u::formOf(c.shape) != u::Form::Named) {
        f.made = true;
        return f;
    }
    // The object below `at`, carrying `marker`, recorded at the cell's use.
    auto const plant = [&](Native const& at, std::string const& marker) {
        Native const object = c.position == u::Position::Last ? at : at + kSepText + native("x");
        bool         ok     = c.position == u::Position::Last || makeDirectory(at);
        if (c.use == Use::Leaf) {
            ok = ok && makeFile(object, marker);
        } else {
            ok = ok && makeDirectory(object) && makeFile(object + kSepText + native("c"), marker)
              && makeFile(object + kSepText + native(onlyName(marker)), marker);
        }
        f.objects[marker] = object;
        return ok;
    };
    bool   ok = true;
    Native name;
    switch (c.edge) {
        case u::Edge::Plain: ok = plant(f.dir + kSepText + (name = native("n")), "literal"); break;
        case u::Edge::TrailingPeriod:
            ok = plant(f.dir + kSepText + (name = native("n.")), "literal") && plant(f.dir + kSepText + native("n"), "twin!");
            break;
        case u::Edge::TrailingSpace:
            ok = plant(f.dir + kSepText + (name = native("n ")), "literal") && plant(f.dir + kSepText + native("n"), "twin!");
            break;
        case u::Edge::ReservedStem: ok = plant(f.dir + kSepText + (name = native("NUL")), "literal"); break;
        case u::Edge::ShortName: {
            ok                 = plant(f.dir + kSepText + native("longcomponentname"), "literal");
            Native const alias = shortNameOf(f.dir + kSepText + native("longcomponentname"));
            if (ok && (alias.empty() || alias == native("longcomponentname"))) {
                if (c.drive == u::Drive::OtherLetter) {   // the letter of the test's own is bound for this claim
                    f.failure = "the volume below the drive letter of the test's own made no short name for a long "
                                "one: its OtherLetter cell cannot measure its claim";
                    return f;
                }
                f.unmeasured = "UNMEASURED: this volume made no short name for a long one";
                return f;
            }
            name = alias;
            break;
        }
        case u::Edge::Stream:   // a file's stream: Leaf, Last (the generator's rule 4)
            // ⚠ NEVER A ONE-LETTER STEM: a typed right side `n:s` is drive N's relative `s` -- the C++ standard's
            // append ([fs.path.append]: a right side with another root name replaces the left), which the home keeps
            // -- so it would measure the drive grammar, not the stream.
            name = native("file:s");
            ok   = makeFile(f.dir + kSepText + native("file"), "file!") && makeFile(f.dir + kSepText + name, "literal");
            f.objects["literal"] = f.dir + kSepText + name;
            f.objects["file!"]   = f.dir + kSepText + native("file");
            break;
        case u::Edge::OutsideCodePage: ok = plant(f.dir + kSepText + (name = kOutsideCodePage), "literal"); break;
        case u::Edge::UnpairedSurrogate:
        case u::Edge::NotUtf8:
            ok = plant(f.dir + kSepText + (name = kNoUtf8Spelling), "literal");
#if !defined(_WIN32)
            if (!ok && errno == EILSEQ) {   // a file system that stores UTF-8 names alone (APFS)
                f.unmeasured = "UNMEASURED: this file system refuses a name that is not UTF-8";
                return f;
            }
#endif
            break;
        case u::Edge::Link: {   // a directory link in the middle (rule 5): `lnk` reaches `E`
            ok = plant(f.dir + kSepText + native("E"), "literal");
            fs::path const target{f.dir + kSepText + native("E")};
            fs::path const link{f.dir + kSepText + native("lnk")};
#if defined(_WIN32)
            ok = ok && !dss::test_support::makeJunction(target, link);
#else
            ok = ok && !dss::test_support::makeSymbolicLink(target, link, dss::test_support::LinkTo::Directory);
#endif
            name = native("lnk");
            break;
        }
    }
    if (!ok) {
        f.failure = "the cell's objects could not be made below " + shown(f.dir);
        return f;
    }
    // THE FIXTURE'S CONTROL: every planted object opens by its literal spelling -- so a spelling of a cell the host
    // refuses is the host's answer for that spelling, never a scaffold that planted nothing.
    for (auto const& [which, object] : f.objects) {
        if (!openPlain(literal(object)).ok) {
            f.failure = "the planted object \"" + which + "\" does not open by its literal spelling " + shown(object);
            return f;
        }
    }
    f.spelledTail = c.position == u::Position::Middle ? name + kSepText + native("x") : name;
    f.made        = true;
    return f;
}

// ── A CELL: its value, the object the host reads, the object the value's reference opens ─────────────────────────
struct Judged {
    std::optional<HostPath> value;
    Opened                  expected;
    Opened                  actual;
    std::string             unmeasured;   // a binding this host refused: the cell is skipped by that name
    std::string             failure;      // a scaffold that must hold did not: the cell is red
    std::string             spelled;
    Opened                  itself;       // what the host opens for the value's object ITSELF (a container as a directory)
    std::string             walkRefused;  // a walk the host refuses too: judged by that refusal, no value to ask
};

// The answer of an ANSWERED value: what the spelling reached, and nothing for an object with no path.
[[nodiscard]] Opened answeredOracle(Opened given) { return given.pathless ? Opened{} : given; }

// Whether the host answered "nothing is there" -- not a refusal: the OS's own not-found answers (Win32: no file, no
// path, no network path or name, a name it cannot hold, a bad path name, no directory; POSIX: ENOENT, ENOTDIR).
[[nodiscard]] bool nothingThere(Opened const& o) {
    if (o.ok) return false;
#if defined(_WIN32)
    switch (o.error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_NET_NAME:
        case ERROR_INVALID_NAME:
        case ERROR_BAD_PATHNAME:
        case ERROR_DIRECTORY: return true;
        default: return false;
    }
#else
    return o.error == ENOENT || o.error == ENOTDIR;
#endif
}

// What an ANSWERED value names: what its spelling REACHES -- the object itself where the host opens it, what the
// spelling names as written where nothing of it is there (the reach's lexical rest), and nothing where the host refuses
// the object itself or it has no path. `given` is what the spelling names at the cell's use.
[[nodiscard]] Opened answeredNames(Opened const& itself, Opened const& given) {
    if (itself.ok && !itself.pathless) return answeredOracle(given);
    if (nothingThere(itself)) return answeredOracle(given);
    return Opened{};
}

#if defined(_WIN32)
// The final path of the object `spelling` names, as the host answers it (GetFinalPathNameByHandleW, VOLUME_NAME_DOS:
// `\\?\C:\...`, or `\\?\UNC\server\share\...` through a share -- the root it is REACHED through kept, since a share's
// server stands between this process and every name below it, and answers for them). Nothing, with the host's error
// where it opens nothing (0 where it answers no final path), otherwise.
[[nodiscard]] std::optional<Native> finalPathOf(Native const& spelling, long& error) {
    HANDLE const h = ::CreateFileW(spelling.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = static_cast<long>(::GetLastError());
        return std::nullopt;
    }
    std::vector<wchar_t> buffer(32768);
    DWORD const n = ::GetFinalPathNameByHandleW(h, buffer.data(), static_cast<DWORD>(buffer.size()), VOLUME_NAME_DOS);
    ::CloseHandle(h);
    if (n == 0 || n >= buffer.size()) return std::nullopt;
    return Native{buffer.data(), n};
}

// The DOS spelling of a final path: the documented `\\?\` prefix dropped, a share's `\\?\UNC\` read as `\\`.
[[nodiscard]] Native dosOfFinal(Native const& final) {
    Native const unc = L"\\\\?\\UNC\\";
    if (final.compare(0, unc.size(), unc) == 0) return L"\\\\" + final.substr(unc.size());
    Native const verbatim = L"\\\\?\\";
    return final.compare(0, verbatim.size(), verbatim) == 0 ? final.substr(verbatim.size()) : final;
}
#endif

// The directory that HOLDS the object `childSpelling` names, as the host answers it: the child opened, its final path
// asked, the last name cut, and that directory opened literally -- a `NUL` directory is a directory here, never the
// device the DOS rule makes of `...\NUL` read as a last name. Nothing where the host opens no child.
[[nodiscard]] Opened directoryHolding(Native const& childSpelling) {
#if defined(_WIN32)
    Opened                      refused;
    std::optional<Native> const final = finalPathOf(childSpelling, refused.error);
    if (!final) return refused;
    Native parent = final->substr(0, final->find_last_of(L'\\'));
    if (u::formOf(dss::core::rootSpelling(parent, dss::core::kHostPathGrammar).shape) == u::Form::VolumeAlone)
        parent += L'\\';   // a volume root keeps its separator: without it `\\?\C:` is the volume's device
    return openPlain(parent);
#else
    return openPlain(childSpelling.substr(0, childSpelling.find_last_of('/')));
#endif
}

[[nodiscard]] Judged judge(u::Cell const& c, Fixture const& f) {
    Judged       j;
    Native const child = c.use == Use::Container ? native("c") : Native{};
    auto const   bind  = [&](Native const& path) -> std::optional<Native> {
        Spelled const s = spell(c.shape, path, c.drive);
        if (!s.text) {
            j.unmeasured = s.unmeasured;
            return std::nullopt;
        }
        RootShape const parsed = dss::core::rootSpelling(*s.text, dss::core::kHostPathGrammar).shape;
        if (parsed != c.shape) {
            j.failure = "the cell's spelling `" + shown(*s.text) + "` parses as " + dss::core::shapeName(parsed);
            return std::nullopt;
        }
        return s.text;
    };
    // ── a volume alone, or a named device: one spelling, its container judged by a child it holds ──
    if (u::formOf(c.shape) != u::Form::Named) {
        std::optional<Native> const s = bind({});
        if (!s) return j;
        j.spelled            = shown(*s);
        Native const childOf = u::formOf(c.shape) == u::Form::VolumeAlone ? volumeChild(c.shape) : native("c");
        Opened const given   = openPlain(c.use == Use::Leaf ? *s : documentedChild(*s, childOf));
        j.itself = c.use == Use::Leaf ? given
                                      : openPlain(u::formOf(c.shape) == u::Form::VolumeAlone ? documentedChild(*s, {}) : *s);
        HostPath const g{fs::path{*s}};
        if (c.origin == u::Origin::Given) {
            j.value    = g;
            j.expected = given;
        } else {   // Answered (the generator's rule 2): what the object itself reaches, nothing where it has no path
            std::error_code ec;
            HostPath const  reached = dss::core::reachedPath(g, c.use, ec);
            j.expected              = answeredNames(j.itself, given);
            if (!ec) j.value = reached;
        }
        j.actual = j.value ? openReference(*j.value, c.use, c.use == Use::Container ? childOf : Native{}) : Opened{};
        return j;
    }
    // ── a name below its volume ──
    Native const                whole = f.dir + kSepText + f.spelledTail;   // absolute, the base's own form
    std::optional<Native> const s     = bind(whole);
    if (!s) return j;
    j.spelled                = shown(*s);
    Opened const givenOracle = openPlain(c.use == Use::Leaf ? *s : documentedChild(*s, child));
    j.itself                 = c.use == Use::Leaf ? givenOracle : directoryHolding(documentedChild(*s, child));
    switch (c.origin) {
        case u::Origin::Given:
            j.value    = HostPath{fs::path{*s}};
            j.expected = givenOracle;
            break;
        case u::Origin::Answered: {
            std::error_code ec;
            HostPath const  reached = dss::core::reachedPath(HostPath{fs::path{*s}}, c.use, ec);
            j.expected              = answeredNames(j.itself, givenOracle);
            if (!ec) j.value = reached;
            break;
        }
        case u::Origin::Composed: {
            std::optional<Native> const left = bind(f.dir);
            if (!left) return j;
            std::error_code ec;
            HostPath const  answered = dss::core::reachedPath(HostPath{fs::path{*left}}, Use::Container, ec);
            if (ec) {
                j.failure = "the cell's own directory `" + shown(*left) + "` was not reached: " + ec.message();
                return j;
            }
            j.value = answered / fs::path{f.spelledTail};
            // The plain rule (B1) over the text the host ANSWERS for the cell's own directory: its final path in DOS
            // form, the root it is reached through kept -- a name below a share is read as the server answers it,
            // never by the local volume's device rule (`...\NUL` the device there).
#if defined(_WIN32)
            long                        refusal = 0;
            std::optional<Native> const final   = finalPathOf(*left, refusal);
            if (!final) {
                j.failure = "the host answers no final path for the cell's own directory `" + shown(*left)
                          + "` (error " + std::to_string(refusal) + ")";
                return j;
            }
            Native const composed = dosOfFinal(*final) + kSepText + f.spelledTail;
#else
            Native const composed = whole;
#endif
            j.expected = openPlain(c.use == Use::Leaf ? composed : documentedChild(composed, child));
            j.itself   = c.use == Use::Leaf ? j.expected : directoryHolding(documentedChild(composed, child));
            break;
        }
        case u::Origin::Listed: {
            std::optional<Native> const walked = bind(f.dir);
            if (!walked) return j;
            Native const lit = f.objects.at("literal");
            std::string const want = genericOf(lit.substr(f.dir.size() + 1));
            auto const walk = dss::core::walkTree(HostPath{fs::path{*walked}}, [&](dss::core::TreeEntry const& e) {
                if (e.relative.generic() != want) return dss::core::WalkStep::Continue;
                j.value = e.at;
                return dss::core::WalkStep::Stop;
            });
            if (!j.value) {
                // A walk that did not list it must have named a refusal -- and the host must refuse that listing too:
                // the directory that holds the literal, spelled in the cell's shape, listed by the host's own call.
                std::optional<Native> const holder = bind(lit.substr(0, lit.find_last_of(kSep)));
                if (walk.error && holder && !hostLists(*holder)) {
                    j.walkRefused = "the walk of `" + shown(*walked) + "` named its refusal (" + walk.error.message()
                                  + "), and the host lists `" + shown(*holder) + "` no more than it";
                    return j;
                }
                j.failure = "the walk of `" + shown(*walked) + "` listed no `" + want + "` (" + walk.error.message() + ")";
                return j;
            }
            // The object the walk listed, opened literally THROUGH THE ROOT THE WALK WALKED: the walked directory's
            // final path and the listed name below it -- a share's server answers for the names below it (it
            // refuses a `NUL` name itself that the local volume opens).
#if defined(_WIN32)
            long                        refusal = 0;
            std::optional<Native> const final   = finalPathOf(*walked, refusal);
            if (!final) {
                j.failure = "the host answers no final path for the walked directory `" + shown(*walked) + "` (error "
                          + std::to_string(refusal) + ")";
                return j;
            }
            Native const listedAt = *final + kSepText + lit.substr(f.dir.size() + 1);
#else
            Native const listedAt = literal(lit);
#endif
            j.expected = openPlain(c.use == Use::Leaf ? listedAt : listedAt + kSepText + child);
            j.itself   = openPlain(listedAt);
            break;
        }
    }
    j.actual = j.value ? openReference(*j.value, c.use, child) : Opened{};
    return j;
}

// ── (2) THE OPERATIONS OVER A WITNESS CELL: each touches the object the host reads ────────────────────────────────
void judgeTheOperations(u::Cell const& c, Fixture const& f, Judged const& j) {
    HostPath const&    v = *j.value;
    std::error_code    ec;
    std::string const& marker = j.expected.marker;
    auto const         at     = f.objects.find(marker);
    // An object the value must NOT name: another planted object that is not one file with it (a stream shares its
    // file's identity, by design).
    std::vector<Native> others;
    if (c.edge != u::Edge::Stream)
        for (auto const& [which, path] : f.objects)
            if (which != marker) others.push_back(path);
    auto const objectValue = [](Native const& absolute) { return HostPath{fs::path{literal(absolute)}}; };
    if (c.use == Use::Leaf) {
        if (j.expected.dir) return;   // the leaf the host reads is a directory: no content to read (totality asks it)
        std::ifstream     in = dss::core::openInputStream(v, std::ios::binary).value_or(std::ifstream{});
        std::string const streamed{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        EXPECT_EQ(streamed, marker) << "openInputStream read another object";
        auto const whole = dss::core::readFileChecked(v);
        EXPECT_TRUE(whole.has_value() && *whole == marker) << "readFileChecked read another object";
        auto const size = dss::core::reachedFileSize(v, ec);
        EXPECT_TRUE(size.has_value() && *size == marker.size()) << "reachedFileSize sized another object: " << ec.message();
        EXPECT_EQ(dss::core::reachedKind(v, Use::Leaf, ec), dss::core::EntryKind::File) << ec.message();
        if (std::FILE* const file = dss::core::openFileStream(v, "rb").value_or(nullptr)) {
            char              buffer[64];
            std::size_t const got = std::fread(buffer, 1, sizeof buffer, file);
            std::fclose(file);
            EXPECT_EQ(std::string(buffer, got), marker) << "openFileStream read another object";
        } else {
            ADD_FAILURE() << "openFileStream opened nothing";
        }
#if defined(_WIN32)
        auto const handle = dss::core::openHandle(v, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
        if (handle) {
            char  buffer[64];
            DWORD got = 0;
            static_cast<void>(::ReadFile(static_cast<HANDLE>(*handle), buffer, sizeof buffer, &got, nullptr));
            ::CloseHandle(static_cast<HANDLE>(*handle));
            EXPECT_EQ(std::string(buffer, got), marker) << "openHandle opened another object";
        } else {
            ADD_FAILURE() << "openHandle opened nothing: " << dss::core::reasonText(handle.error());
        }
#else
        auto const fd = dss::core::openDescriptor(v, O_RDONLY, 0);
        if (fd) {
            char          buffer[64];
            ssize_t const got = ::read(*fd, buffer, sizeof buffer);
            ::close(*fd);
            EXPECT_EQ(std::string(buffer, got > 0 ? static_cast<std::size_t>(got) : 0u), marker)
                << "openDescriptor opened another object";
        } else {
            ADD_FAILURE() << "openDescriptor opened nothing: " << dss::core::reasonText(fd.error());
        }
#endif
    } else {
        if (!j.itself.ok || j.itself.pathless) {
            // The host opens no directory for the container ITSELF (a share's server refuses a `NUL` name, while
            // it opens a child below it): every question of the directory itself answers a refusal by name.
            EXPECT_EQ(dss::core::reachedKind(v, Use::Container, ec), dss::core::EntryKind::Absent);
            EXPECT_TRUE(ec) << "the host refuses the directory itself; reachedKind answered without its refusal";
            auto const refused = dss::core::walkTree(v, [](dss::core::TreeEntry const&) { return dss::core::WalkStep::Stop; });
            EXPECT_TRUE(refused.error) << "the host refuses the directory itself; the walk named no refusal";
            return;
        }
        EXPECT_EQ(dss::core::reachedKind(v, Use::Container, ec), dss::core::EntryKind::Directory) << ec.message();
        std::set<std::string> names;
        auto const            walk = dss::core::walkTree(v, [&](dss::core::TreeEntry const& e) {
            names.insert(e.relative.generic());
            return dss::core::WalkStep::Prune;
        });
        EXPECT_FALSE(walk.error) << "walkTree did not list the directory: " << walk.error.message();
        if (at != f.objects.end()) EXPECT_EQ(names.count(onlyName(marker)), 1u) << "walkTree listed another directory";
    }
    if (at != f.objects.end()) {
        HostPath const object = objectValue(at->second);
        EXPECT_TRUE(dss::core::reachedSameFile(v, object, c.use, ec)) << "reachedSameFile: " << ec.message();
        for (Native const& other : others) {
            HostPath const wrong = objectValue(other);
            EXPECT_FALSE(dss::core::reachedSameFile(v, wrong, c.use, ec)) << "reachedSameFile took " << shown(other);
            EXPECT_NE(dss::core::canonicalIdentityKey(v, c.use), dss::core::canonicalIdentityKey(wrong, c.use))
                << "canonicalIdentityKey keyed it as " << shown(other);
        }
        if (c.edge == u::Edge::ShortName && c.use == Use::Container) {
            // A name not made yet below a short alias is keyed as the same name below its LONG directory (R10): the
            // identity of an 8.3 spelling is its long object, the part that is not there yet included.
            // Its long directory is spelled in the cell's OWN shape: a share's key keeps its authority, by the
            // home's rule, and is never a local drive's.
            fs::path const notYet{native("not-made-yet.o")};
            Spelled const  longSpelled = spell(c.shape, at->second, c.drive);
            ASSERT_TRUE(longSpelled.text.has_value()) << longSpelled.unmeasured;
            EXPECT_EQ(dss::core::canonicalIdentityKey(v / notYet, Use::Leaf),
                      dss::core::canonicalIdentityKey(HostPath{fs::path{*longSpelled.text}} / notYet, Use::Leaf))
                << "a name below the short alias keyed apart from the same name below its long directory";
        }
        if (c.use == Use::Leaf) {
            // The level above it names the directory that holds the object the host reads (H4 (iii)): never the
            // level of its TEXT re-read as a whole, another directory for a name that ends in a space or a period.
            // Asked through the level's CONTAINER reference, under the object's own last name: the host's answer for
            // a level it serves only children of (a share's server refusing `...\NUL` itself) is the same.
            Native const   name  = at->second.substr(at->second.find_last_of(kSep) + 1);
            HostPath const above = dss::core::containingDirectory(v);
            Opened const   held  = openReference(above, Use::Container, name);
            EXPECT_TRUE(held.sameAs(j.expected)) << "containingDirectory names a directory holding " << held.describe()
                                                 << " under `" << shown(name) << "`, not the object the host reads";
        }
        if (c.use == Use::Leaf) {   // the write last: it lands in the object the host reads
            {
                std::ofstream out =
                    dss::core::openOutputStream(v, std::ios::binary | std::ios::trunc).value_or(std::ofstream{});
                out << "written";
            }
            EXPECT_EQ(openPlain(literal(at->second)).marker, "written") << "openOutputStream wrote another object";
        }
    }
}

// ── (3) TOTALITY: one invoker per operation of the home that takes a reference ────────────────────────────────────
struct Asked {
    u::Cell const&  c;
    Fixture const&  f;
    HostPath const& v;
    std::error_code ec;
    // The operations that MAKE or WRITE run only on a value below the cell's own directory -- never on a volume.
    [[nodiscard]] bool below() const { return u::formOf(c.shape) == u::Form::Named; }
    [[nodiscard]] bool belowLeaf() const { return below() && c.use == Use::Leaf; }
};
struct Invoker {
    char const*                 name;
    char const*                 absentHere;   // non-null: the operation is not on this host, and why
    std::function<void(Asked&)> run;
};

// An image that is not there, named ABSOLUTELY below the cell's own directory: a bare name is searched on PATH, which
// cost the run 80 s of 140 (MEASURED, run u-win-2) and asks nothing of the cell's value.
[[nodiscard]] HostPath noSuchImage(Asked const& a) {
    return HostPath{fs::path{a.f.dir + kSepText + native("no-such-image.exe")}};
}

[[nodiscard]] std::vector<Invoker> const& invokers() {
    using namespace dss::core;
    static std::vector<Invoker> const table = {
        // ── Reading ──
        {"entryKind", nullptr, [](Asked& a) { static_cast<void>(entryKind(a.v, a.c.use, a.ec)); }},
        {"reachedKind", nullptr, [](Asked& a) { static_cast<void>(reachedKind(a.v, a.c.use, a.ec)); }},
        {"reachedPipe", nullptr, [](Asked& a) { static_cast<void>(reachedPipe(a.v, a.ec)); }},
        {"reachedFileSize", nullptr, [](Asked& a) { static_cast<void>(reachedFileSize(a.v, a.ec)); }},
        {"reachedPath", nullptr, [](Asked& a) { static_cast<void>(reachedPath(a.v, a.c.use, a.ec)); }},
        {"reachedRelative", nullptr,
         [](Asked& a) { static_cast<void>(reachedRelative(a.v, a.c.use, HostPath{fs::path{a.f.dir}}, a.ec)); }},
        {"reachedWriteTime", nullptr, [](Asked& a) { static_cast<void>(reachedWriteTime(a.v, a.c.use, a.ec)); }},
        {"reachedSameFile", nullptr, [](Asked& a) { static_cast<void>(reachedSameFile(a.v, a.v, a.c.use, a.ec)); }},
        {"sameEntry", nullptr, [](Asked& a) { static_cast<void>(sameEntry(a.v, a.v, a.c.use, a.ec)); }},
        {"storedEntryName", nullptr, [](Asked& a) { static_cast<void>(storedEntryName(a.v, a.c.use, a.ec)); }},
        {"directoryFoldsCase", nullptr, [](Asked& a) { static_cast<void>(directoryFoldsCase(a.v, a.ec)); }},
        {"absoluteKeepingRoot", nullptr, [](Asked& a) { static_cast<void>(absoluteKeepingRoot(a.v, a.c.use, a.ec)); }},
        {"normalizeKeepingRoot", nullptr, [](Asked& a) { static_cast<void>(normalizeKeepingRoot(a.v, a.c.use)); }},
        {"canonicalIdentityKey", nullptr,
         [](Asked& a) { static_cast<void>(canonicalIdentityKey(a.v, a.c.use, a.ec)); }},
        {"of", nullptr,
         [](Asked& a) { static_cast<void>(PathIdentity::of(a.v, a.c.use)); }},
        {"walkTree", nullptr,
         [](Asked& a) { static_cast<void>(walkTree(a.v, [](TreeEntry const&) { return WalkStep::Stop; })); }},
#if defined(_WIN32)
        {"fileAttributes", nullptr, [](Asked& a) { static_cast<void>(fileAttributes(a.v)); }},
#else
        {"fileAttributes", "Windows alone", {}},
#endif
        // ── Opening ──
        {"openFileStream", nullptr,
         [](Asked& a) {
             if (std::FILE* const file = openFileStream(a.v, "rb").value_or(nullptr)) std::fclose(file);
         }},
        {"openInputStream", nullptr, [](Asked& a) { static_cast<void>(openInputStream(a.v, std::ios::binary).has_value()); }},
        {"readFileChecked", nullptr,
         [](Asked& a) {
             static_cast<void>(readFileChecked(a.v));
             static_cast<void>(readFileChecked(a.v, 64));
         }},
#if defined(_WIN32)
        {"openHandle", nullptr,
         [](Asked& a) {
             auto const h = openHandle(a.v, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
             if (h) ::CloseHandle(*h);
         }},
        {"openDescriptor", "POSIX alone", {}},
#else
        {"openHandle", "Windows alone", {}},
        {"openDescriptor", nullptr,
         [](Asked& a) {
             auto const fd = openDescriptor(a.v, O_RDONLY | O_NONBLOCK, 0);
             if (fd) ::close(*fd);
         }},
#endif
        {"openOutputStream", nullptr,
         [](Asked& a) {
             if (a.belowLeaf()) static_cast<void>(openOutputStream(a.v, std::ios::app).has_value());
         }},
        // ── Composing (no reference of their own, asked for totality all the same) ──
        {"containingDirectory", nullptr, [](Asked& a) { static_cast<void>(containingDirectory(a.v)); }},
        {"withSuffix", nullptr, [](Asked& a) { static_cast<void>(a.v.withSuffix(native(".tmp"))); }},
        // ── Spelling: no reference either; every rendering of the home is total (T4) ──
        {"display", nullptr, [](Asked& a) { static_cast<void>(a.v.spelling().display()); }},
        {"generic", nullptr, [](Asked& a) { static_cast<void>(a.v.spelling().generic()); }},
        {"operator<<", nullptr,
         [](Asked& a) {
             std::ostringstream out;
             out << a.v.spelling();
         }},
        {"filename", nullptr, [](Asked& a) { static_cast<void>(a.v.filename()); }},
        {"stem", nullptr, [](Asked& a) { static_cast<void>(a.v.stem()); }},
        {"extension", nullptr, [](Asked& a) { static_cast<void>(a.v.extension()); }},
        {"genericSpelling", nullptr, [](Asked& a) { static_cast<void>(genericSpelling(a.v.filename())); }},
        {"genericSpellingU8", nullptr, [](Asked& a) { static_cast<void>(genericSpellingU8(a.v.filename())); }},
        // ── Spawning: an image that is not there, or a planted marker file that is no image ──
        {"spawnDirectory", nullptr,
         [](Asked& a) { static_cast<void>(dss::substrate::spawnAndWaitInherit(noSuchImage(a), {"universe"}, a.v)); }},
        {"spawnImage", nullptr,
         [](Asked& a) {
             if (a.belowLeaf()) static_cast<void>(dss::substrate::spawnAndWaitInherit(a.v, {"universe"}));
         }},
        {"spawnAndWaitInherit", nullptr,
         [](Asked& a) {
             if (a.belowLeaf())
                 static_cast<void>(dss::substrate::spawnAndWaitInherit(a.v, {"universe"}, HostPath{fs::path{a.f.dir}}));
         }},
        {"spawnAndWaitRedirectStdout", nullptr,
         [](Asked& a) {
             if (a.belowLeaf())
                 static_cast<void>(dss::substrate::spawnAndWaitRedirectStdout(noSuchImage(a), {"universe"}, {}, a.v));
         }},
#if defined(_WIN32)
        {"startImageInheriting", nullptr,
         [](Asked& a) {
             if (!a.belowLeaf()) return;
             STARTUPINFOW        startup{};
             PROCESS_INFORMATION process{};
             startup.cb = sizeof startup;
             if (dss::substrate::startImageInheriting(a.v, L"universe", &startup, &process) == 0) {
                 ::TerminateProcess(process.hProcess, 1);
                 ::WaitForSingleObject(process.hProcess, INFINITE);
                 ::CloseHandle(process.hThread);
                 ::CloseHandle(process.hProcess);
                 ADD_FAILURE() << "a planted marker file started as a process";
             }
         }},
#else
        {"startImageInheriting", "Windows alone", {}},
#endif
        // ── Making: below the cell's own directory alone, the removals last ──
        {"createDirectoriesFor", nullptr,
         [](Asked& a) {
             if (a.below()) static_cast<void>(createDirectoriesFor(a.v, a.ec));
         }},
        {"createDirectories", nullptr,
         [](Asked& a) {
             if (a.below() && a.c.use == Use::Container) static_cast<void>(createDirectories(a.v, a.ec));
         }},
        {"addReachedPermissions", nullptr,
         [](Asked& a) {
             if (a.below()) addReachedPermissions(a.v, fs::perms::owner_read, a.ec);
         }},
        {"copyFile", nullptr,
         [](Asked& a) {
             if (a.belowLeaf()) copyFile(a.v, HostPath{fs::path{a.f.dir + kSepText + native("copied")}}, a.ec);
         }},
#if defined(_WIN32)
        {"commitByHandle", nullptr,
         [](Asked& a) {
             if (!a.belowLeaf()) return;
             Native const staged = a.f.dir + kSepText + native("staged");
             HANDLE const h      = ::CreateFileW(literal(staged).c_str(), DELETE | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                                 FILE_ATTRIBUTE_NORMAL, nullptr);
             if (h == INVALID_HANDLE_VALUE) {
                 ADD_FAILURE() << "the staged file could not be made: error " << ::GetLastError();
                 return;
             }
             static_cast<void>(commitByHandle(h, a.v, true));
             ::CloseHandle(h);
         }},
        {"renameEntry", "POSIX alone", {}},
#else
        {"commitByHandle", "Windows alone", {}},
        {"renameEntry", nullptr,
         [](Asked& a) {
             if (!a.belowLeaf()) return;
             Native const staged = a.f.dir + kSepText + native("staged");
             if (!makeFile(staged, "staged")) {
                 ADD_FAILURE() << "the staged file could not be made: errno " << errno;
                 return;
             }
             renameEntry(HostPath{fs::path{staged}}, a.v, a.ec);
         }},
#endif
        {"removeEntry", nullptr,
         [](Asked& a) {
             if (a.below()) static_cast<void>(removeEntry(a.v, a.ec));
         }},
        {"removeTree", nullptr,
         [](Asked& a) {
             if (a.below()) static_cast<void>(removeTree(a.v));
         }},
    };
    return table;
}

// THE BUDGET'S MEASUREMENT: where the run's time goes, per phase and per operation, printed when the run ends.
std::map<std::string, std::chrono::steady_clock::duration>& timings() {
    static std::map<std::string, std::chrono::steady_clock::duration> spent;
    return spent;
}
struct Timed {
    std::string                           what;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~Timed() { timings()[what] += std::chrono::steady_clock::now() - start; }
};

void judgeTotality(u::Cell const& c, Fixture const& f, HostPath const& v, std::string const& spelled) {
    Asked asked{c, f, v, {}};
    for (Invoker const& invoker : invokers()) {
        if (invoker.absentHere != nullptr) continue;
        Timed const timed{std::string{"totality "} + invoker.name};
        try {
            invoker.run(asked);
        } catch (std::exception const& e) {
            ADD_FAILURE() << invoker.name << " threw on `" << spelled << "`: " << e.what();
        } catch (...) {
            ADD_FAILURE() << invoker.name << " threw on `" << spelled << "`";
        }
    }
}

class HostPathCell : public ::testing::TestWithParam<u::Cell> {};

TEST_P(HostPathCell, Holds) {
    u::Cell const c = GetParam();
    ASSERT_TRUE(bindings().refused.empty()) << bindings().refused;
    Fixture const f = [&] {
        Timed const timed{"fixture"};
        return makeFixture(c);
    }();
    ASSERT_TRUE(f.failure.empty()) << f.failure;
    if (!f.made) GTEST_SKIP() << f.unmeasured;
    Judged const j = [&] {
        Timed const timed{"reference rule"};
        return judge(c, f);
    }();
    ASSERT_TRUE(j.failure.empty()) << j.failure;
    if (!j.unmeasured.empty()) GTEST_SKIP() << j.unmeasured;
    if (!j.walkRefused.empty()) {
        std::cout << "[ universe ] " << u::cellName(c) << ": " << j.walkRefused << "\n";
        return;   // judged: the walk's refusal is the host's
    }
    // (1) the reference rule
    EXPECT_TRUE(j.actual.sameAs(j.expected))
        << "`" << j.spelled << "` as a " << u::nameOf(c.use) << " (" << u::nameOf(c.origin) << "): the host reads "
        << j.expected.describe() << ", the value's reference opens " << j.actual.describe();
    // (2) the operations, on a witness cell whose object is a file or a directory
    if (u::isWitness(c) && j.value && j.expected.ok && !j.expected.pathless) {
        Timed const timed{"operations"};
        judgeTheOperations(c, f, j);
    }
    // (3) totality
    if (j.value) judgeTotality(c, f, *j.value, j.spelled);
}

INSTANTIATE_TEST_SUITE_P(Universe, HostPathCell, ::testing::ValuesIn(u::generatedCells()),
                         [](::testing::TestParamInfo<u::Cell> const& info) { return u::cellName(info.param); });

// THE GENERATOR: one cell per member of the product the rules leave, every name apart, every shape of the host's
// grammar with a cell, and the count printed.
TEST(HostPathUniverse, TheProductIsGeneratedOnceAndEveryCellIsNamedApart) {
    auto const            cells = u::generatedCells();
    std::set<std::string> names;
    for (u::Cell const& c : cells) EXPECT_TRUE(names.insert(u::cellName(c)).second) << u::cellName(c);
    std::cout << "[ universe ] " << cells.size() << " cells on this host\n";
    for (dss::core::RootShapeRow const& row : dss::core::kRootShapes) {
        if (row.grammar != dss::core::kHostPathGrammar) continue;
        bool any = false;
        for (u::Cell const& c : cells) any = any || c.shape == row.shape;
        EXPECT_TRUE(any) << row.name << " has no cell";
    }
}

// THE BINDINGS EVERY LEG OF ITS KIND GUARANTEES (the home audit's 4.7): each spells its shape on this host, so a cell
// of it is never a quiet skip; the bindings a host may lawfully lack are printed by name.
TEST(HostPathUniverse, TheGuaranteedBindingsHoldOnThisLeg) {
    Bindings& b = bindings();
    ASSERT_TRUE(b.refused.empty()) << b.refused;
    for (dss::core::RootShapeRow const& row : dss::core::kRootShapes) {
        if (row.grammar != dss::core::kHostPathGrammar) continue;
        if (row.grammar == dss::core::PathGrammar::Win32 && dss::core::namesAnAuthority(row.shape)) continue;   // a share
        Spelled const s = spell(row.shape, b.root + kSepText + native(kVolumeChildOfTheCwd), u::Drive::Same);
        EXPECT_TRUE(s.text.has_value()) << row.name << " was not bound: " << s.unmeasured;
    }
#if defined(_WIN32)
    // ANOTHER DRIVE: guaranteed on both Windows legs (MEASURED, the home audit's u6 P4: a second drive is present).
    EXPECT_FALSE(b.otherRoot.empty())
        << "no directory on another drive was bound by its `=X:`: the OtherDrive cells did not run";
    // SHORT NAMES: guaranteed where the scratch lies on the system volume (MEASURED, u6 P1: C: makes 8.3 names on both
    // Windows legs); another volume may lawfully make none, and its ShortName cells are named UNMEASURED skips.
    std::vector<wchar_t> windows(32768);
    UINT const           length = ::GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
    bool const systemVolume     = length >= 2 && std::towupper(windows[0]) == std::towupper(b.drive[0]);
    Native const probe          = b.root + L"\\guaranteed-short-name-probe";
    ASSERT_TRUE(makeDirectory(probe));
    Native const alias = shortNameOf(probe);
    bool const   made  = !alias.empty() && alias != L"guaranteed-short-name-probe";
    if (systemVolume) {
        EXPECT_TRUE(made) << "the system volume made no short name: the ShortName cells did not run";
    } else if (!made) {
        std::cout << "[ universe ] UNMEASURED: the scratch's volume makes no short names; the ShortName cells skipped\n";
    }
    // A DRIVE LETTER OF THE TEST'S OWN, onto the scratch: guaranteed on both Windows legs (a name of the logon session,
    // chosen among the free letters); the OtherLetter cells need it bound, and short names made below it.
    EXPECT_TRUE(b.mapped.refused.empty()) << "the drive letter of the test's own was not bound: " << b.mapped.refused;
    if (!b.mapped.root.empty()) {
        Native const viaLetter = b.mapped.root + L"\\guaranteed-short-name-probe";
        ASSERT_TRUE(makeDirectory(viaLetter));
        Native const mappedAlias = shortNameOf(viaLetter);
        EXPECT_TRUE(!mappedAlias.empty() && mappedAlias != L"guaranteed-short-name-probe")
            << "no short name was made below " << shown(b.mapped.letter) << ": its OtherLetter cells are red";
        std::cout << "[ universe ] the drive letter of the test's own: " << shown(b.mapped.letter) << " onto "
                  << shown(b.mapped.target) << "; " << b.mapped.swept << " stale definition(s) of earlier runs swept\n";
    }
    // SHARES: not guaranteed (the audit's 4.7: claimed once a leg is MEASURED serving one); what this leg did, said.
    std::cout << "[ universe ] \\\\localhost\\" << shown(b.drive.substr(0, 1)) << "$ "
              << (b.share ? "served: the Share and Unc cells ran\n" : "not served: the Share and Unc cells skipped\n");
#endif
}

#if defined(_WIN32)
// One definition of a free drive letter, made under the host-wide lock and removed when it goes: the sweep's fixture.
struct Definition {
    Native letter;
    Native target;
    Definition(Native const& onto, std::string& why) : target(onto) {
        if (std::optional<Native> const defined = defineFreeLetter(onto, why)) letter = *defined;
    }
    ~Definition() {
        if (!letter.empty()) static_cast<void>(removeDefinition(letter, target));
    }
    Definition(Definition const&)            = delete;
    Definition& operator=(Definition const&) = delete;
};
#endif

// THE SWEEP REMOVES ONLY A DEFINITION OF THIS TEST'S OWN WHOSE OWNER IS GONE: stamped inside this test's scratch root
// with an owner no process is (the idle process's id 0; this process's id under another creation time, the id reused)
// -- and never a live owner's (a suspended copy of this binary, this run's own), an unstamped one, or one outside the
// root (a root whose name only begins with this one's).
TEST(HostPathUniverse, TheSweepRemovesOnlyADriveLetterWhoseOwnerIsGone) {
#if !defined(_WIN32)
    GTEST_SKIP() << "a POSIX host names no drive letters";
#else
    Bindings& b = bindings();
    ASSERT_TRUE(b.mapped.refused.empty()) << b.mapped.refused;
    Native const scratch     = b.scratch.path().native();
    Native const scratchRoot = scratch.substr(0, scratch.find_last_of(L'\\'));
    // A LIVE OWNER: a suspended copy of this binary -- its thread never starts.
    std::vector<wchar_t> buffer(32768);
    DWORD const          imageLength = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    ASSERT_TRUE(imageLength > 0 && imageLength < buffer.size());
    Native const        image{buffer.data(), imageLength};
    STARTUPINFOW        startup{};
    PROCESS_INFORMATION sibling{};
    startup.cb = sizeof startup;
    ASSERT_TRUE(::CreateProcessW(image.c_str(), nullptr, nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr,
                                 &startup, &sibling))
        << "error " << ::GetLastError();
    struct Ended {
        PROCESS_INFORMATION const& process;
        ~Ended() {
            static_cast<void>(::TerminateProcess(process.hProcess, 0));
            static_cast<void>(::WaitForSingleObject(process.hProcess, 10000));
            ::CloseHandle(process.hThread);
            ::CloseHandle(process.hProcess);
        }
    } const ended{sibling};
    std::optional<std::uint64_t> const siblingStart = createdAt(sibling.hProcess);
    std::optional<std::uint64_t> const ownStart     = createdAt(::GetCurrentProcess());
    ASSERT_TRUE(siblingStart.has_value() && ownStart.has_value()) << "error " << ::GetLastError();
    ASSERT_FALSE(ownerIsGone(sibling.dwProcessId, *siblingStart));

    LetterLock const lock;
    ASSERT_TRUE(lock.refused().empty()) << lock.refused();
    Native const     in = L"\\??\\" + scratch + L"\\";
    std::string      why;
    Definition const noOwner{in + ownerStamp(0, 1), why};
    ASSERT_FALSE(noOwner.letter.empty()) << why;
    Definition const reused{in + ownerStamp(::GetCurrentProcessId(), *ownStart + 1), why};
    ASSERT_FALSE(reused.letter.empty()) << why;
    Definition const live{in + ownerStamp(sibling.dwProcessId, *siblingStart), why};
    ASSERT_FALSE(live.letter.empty()) << why;
    Definition const unstamped{in + L"0-0", why};
    ASSERT_FALSE(unstamped.letter.empty()) << why;
    Definition const outside{L"\\??\\" + scratchRoot + L"-elsewhere\\" + ownerStamp(0, 1), why};
    ASSERT_FALSE(outside.letter.empty()) << why;

    // (Its count is not asserted: a definition of an earlier run's may be swept with these.)
    static_cast<void>(sweepStaleDefinitions(scratchRoot));
    EXPECT_TRUE(definitionsOf(noOwner.letter).empty()) << "a definition whose owner no process is was left";
    EXPECT_TRUE(definitionsOf(reused.letter).empty()) << "a definition whose owner's id was reused was left";
    EXPECT_EQ(definitionsOf(live.letter), std::vector<Native>{live.target}) << "a live owner's definition was removed";
    EXPECT_EQ(definitionsOf(unstamped.letter), std::vector<Native>{unstamped.target})
        << "an unstamped definition was removed";
    EXPECT_EQ(definitionsOf(outside.letter), std::vector<Native>{outside.target})
        << "a definition outside this test's scratch root was removed";
    EXPECT_EQ(definitionsOf(b.mapped.letter), std::vector<Native>{b.mapped.target}) << "this run's own letter was removed";
#endif
}

// EVERY OPERATION THAT TAKES A REFERENCE IS INVOKED BY THE CELLS (T5): the rows of the list whose family takes a
// reference, against the invokers' names -- a new operation with no invoker reddens here, and so does an invoker that
// names no row.
TEST(HostPathUniverse, EveryOperationThatTakesAReferenceIsInvoked) {
    std::set<std::string> invoked;
    for (Invoker const& invoker : invokers()) invoked.insert(invoker.name);
    std::set<std::string> rows;
    for (dss::core::HostPathOperation const& op : dss::core::kHostPathOperations) {
        rows.insert(op.name);
        if (dss::core::takesAReference(op.family)) EXPECT_EQ(invoked.count(op.name), 1u) << op.name << " has no invoker";
    }
    for (std::string const& name : invoked) EXPECT_EQ(rows.count(name), 1u) << name << " is no operation of the home";
}

}  // namespace
