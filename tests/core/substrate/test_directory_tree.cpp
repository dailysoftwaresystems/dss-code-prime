// THE KIND OF A DIRECTORY ENTRY, AND THE WALK AND THE REMOVAL THAT ASK IT
// (src/core/substrate/path_identity.{hpp,cpp}: `entryKind`, `reachedKind`, `reachedFileSize`, `reachedPath`,
// `reachedRelative`, `reachedWriteTime`, `reachedSameFile`, `addReachedPermissions`, `walkTree`, `removeTree`).
//
// ★ EVERY LINK HERE IS MADE BY THE OS (`tests/test_support/os_link.hpp`), and that is the point of
// these cases: on Windows, libstdc++ can neither make a link nor see one (its `symlink_status`
// reads what the link reaches), so a case that made its link through the library never ran on the
// MinGW build -- the build whose walks followed links and whose removals emptied a link's target.
// Every assertion is about the ENTRY the OS recorded, asked of the substrate, never the library.

#include "core/substrate/path_identity.hpp"
#include "os_link.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <dirent.h>     // opendir: the OS's own listing, the unlistable root's control
#include <sys/stat.h>   // mkfifo: the special file a POSIX host can make; chmod
#include <unistd.h>     // geteuid
#include <cerrno>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // the walk universe's objects, made through `\\?\` as the OS makes them
#include <sddl.h>      // ConvertStringSecurityDescriptorToSecurityDescriptorW: the unlistable root's DACL
#endif
#include <cstring>
#include <iostream>
#include <optional>
#include <system_error>

namespace fs = std::filesystem;
using dss::core::EntryKind;
using dss::core::TreeEntry;
using dss::core::WalkStep;
using dss::test_support::LinkTo;
using dss::test_support::Location;
using dss::test_support::makeJunction;
using dss::test_support::makeSymbolicLink;
using dss::test_support::ScratchDir;

namespace {

void writeFile(fs::path const& p, std::string const& text = "x\n") {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << text;
}

[[nodiscard]] char const* kindName(EntryKind k) {
    switch (k) {
        case EntryKind::Absent: return "absent";
        case EntryKind::Directory: return "directory";
        case EntryKind::Link: return "link";
        case EntryKind::File: return "file";
        case EntryKind::Other: return "other";
    }
    return "?";
}

// The text of a spelling, UTF-8 -- what a value's rendering is compared with (H6c).
[[nodiscard]] std::string utf8Of(fs::path const& p) {
    std::u8string const u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

// An ENTRY asked as the whole spelling names it (Leaf, H6c): every name these cases ask is benign, so the two uses
// name one object here (the walk-up universe below asks both).
[[nodiscard]] std::string kindOf(fs::path const& p) {
    std::error_code ec;
    EntryKind const k = dss::core::entryKind(p, dss::core::Use::Leaf, ec);
    return ec ? std::string("error: ") + ec.message() : std::string(kindName(k));
}

[[nodiscard]] std::string reachOf(fs::path const& p) {
    std::error_code ec;
    EntryKind const k = dss::core::reachedKind(p, dss::core::Use::Leaf, ec);
    return ec ? std::string("error: ") + ec.message() : std::string(kindName(k));
}

// How many bytes the file `p` reaches is, as `N bytes`, or `no file`.
[[nodiscard]] std::string sizeOf(fs::path const& p) {
    std::error_code ec;
    auto const      size = dss::core::reachedFileSize(p, ec);
    if (ec) return std::string("error: ") + ec.message();
    return size ? std::to_string(*size) + " bytes" : std::string("no file");
}

// Every entry a walk of `root` yields, as `relative path -> kind/reaches`, in generic spelling.
[[nodiscard]] std::map<std::string, std::string> walked(fs::path const& root) {
    std::map<std::string, std::string> out;
    auto const                          walk = dss::core::walkTree(root, [&](TreeEntry const& e) {
        out[e.relative.generic()] =   // the names below the root, as LISTED (H6c)
            std::string(kindName(e.kind)) + "/" + kindName(e.reaches);
        return WalkStep::Continue;
    });
    EXPECT_FALSE(walk.error) << walk.where.spelling().generic() << ": " << walk.error.message();
    return out;
}

// Every write bit: what a read-only entry has none of.
constexpr fs::perms kAllWrite = fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write;

// 2001-09-09 00:00:00 UTC, to the second, on the library's file clock: a date every file system a leg's temp
// directory may sit on can store -- FAT's from 1980, and the 32-bit times of ext4's small inodes, XFS without
// bigtime and HFS+, which end in 2038 and 2040. Never the file clock's own epoch: that is 1601 under MS STL and
// 2174 under libstdc++, and a file system without the range fails the set, or clamps it, with no defect to see.
// Reached from NOW on both clocks -- the distance back to that date on the system clock, taken from the file
// clock's now -- because the three libraries share no conversion: libstdc++'s file clock has `from_sys`, MS STL's
// only `from_utc`, and libc++'s (AppleClang 21) neither.
[[nodiscard]] fs::file_time_type storableDate() {
    using FileTicks = fs::file_time_type::duration;
    auto const back =
        std::chrono::system_clock::now() - std::chrono::sys_days{std::chrono::year{2001} / 9 / 9};
    auto const dated = fs::file_time_type::clock::now() - std::chrono::duration_cast<FileTicks>(back);
    return std::chrono::time_point_cast<FileTicks>(std::chrono::floor<std::chrono::seconds>(dated));
}

} // namespace

// A real directory, a file, a directory link, a dangling file link: each read as the ENTRY it is.
TEST(DirectoryTree, AnOsMadeLinkIsALinkNeverWhatItReaches) {
    ScratchDir     scratch{Location::Temp, "directory_tree_kinds"};
    fs::path const dir = scratch.path();
    writeFile(dir / "real" / "x.json");
    DSS_ASSERT_LINK_MADE(makeSymbolicLink(dir / "real", dir / "dir-link", LinkTo::Directory));
    DSS_ASSERT_LINK_MADE(makeSymbolicLink(dir / "real" / "x.json", dir / "file-link", LinkTo::File));
    DSS_ASSERT_LINK_MADE(makeSymbolicLink(dir / "no-such", dir / "dangling", LinkTo::File));

    EXPECT_EQ(kindOf(dir / "real"), "directory");
    EXPECT_EQ(kindOf(dir / "real" / "x.json"), "file");
    EXPECT_EQ(kindOf(dir / "dir-link"), "link")
        << "the directory link reads as " << kindOf(dir / "dir-link") << ", not as the link it is";
    EXPECT_EQ(kindOf(dir / "file-link"), "link")
        << "the file link reads as " << kindOf(dir / "file-link") << ", not as the link it is";
    EXPECT_EQ(kindOf(dir / "dangling"), "link")
        << "the dangling link reads as " << kindOf(dir / "dangling") << ", not as the link it is";
    EXPECT_EQ(kindOf(dir / "nothing-here"), "absent");

    EXPECT_EQ(reachOf(dir / "dir-link"), "directory");
    EXPECT_EQ(reachOf(dir / "file-link"), "file");
    EXPECT_EQ(reachOf(dir / "dangling"), "absent")
        << "the dangling link reaches " << reachOf(dir / "dangling") << ", where it reaches nothing";
    EXPECT_EQ(reachOf(dir / "real"), "directory");
}

// A junction (Windows' own directory link) is a link too, as MS STL and the Python reader read it.
TEST(DirectoryTree, AJunctionIsALink) {
#if !defined(_WIN32)
    GTEST_SKIP() << "a junction exists only on Windows; this host has none to read";
#else
    ScratchDir     scratch{Location::Temp, "directory_tree_junction"};
    fs::path const dir = scratch.path();
    writeFile(dir / "real" / "x.json");
    DSS_ASSERT_LINK_MADE(makeJunction(dir / "real", dir / "junction"));
    EXPECT_EQ(kindOf(dir / "junction"), "link")
        << "the junction reads as " << kindOf(dir / "junction") << ", not as the link it is";
    EXPECT_EQ(reachOf(dir / "junction"), "directory");
    auto const seen = walked(dir);
    EXPECT_EQ(seen.count("junction/x.json"), 0u) << "the walk went through the junction";
    EXPECT_EQ(seen.count("junction"), 1u) << "the junction itself is an entry the walk yields";
#endif
}

// A link and what it reaches are ONE file, and the substrate's identity -- the key the include-once registry, the
// pre-scan memo and the shipped-source claims compare -- says so on every build: a file link and its file, and a
// file named through a directory link and the same file named directly.
TEST(DirectoryTree, ALinkAndWhatItReachesAreOneIdentity) {
    ScratchDir     scratch{Location::Temp, "directory_tree_identity"};
    fs::path const dir = scratch.path();
    writeFile(dir / "real" / "h.h");
    DSS_ASSERT_LINK_MADE(makeSymbolicLink(dir / "real" / "h.h", dir / "file-link.h", LinkTo::File));
    DSS_ASSERT_LINK_MADE(makeSymbolicLink(dir / "real", dir / "dir-link", LinkTo::Directory));
    auto const identities = [](fs::path const& a, fs::path const& b) -> std::string {
        return dss::core::PathIdentity::of(a, dss::core::Use::Leaf) == dss::core::PathIdentity::of(b, dss::core::Use::Leaf)
                   ? "one identity"
                   : "two identities";
    };
    EXPECT_EQ(identities(dir / "file-link.h", dir / "real" / "h.h"), "one identity")
        << "the file link and its file are " << identities(dir / "file-link.h", dir / "real" / "h.h");
    EXPECT_EQ(identities(dir / "dir-link" / "h.h", dir / "real" / "h.h"), "one identity")
        << "the file named through the directory link and the file are "
        << identities(dir / "dir-link" / "h.h", dir / "real" / "h.h");
}

// A file is sized by what a path REACHES, through every link on the way: a link to a three-byte file is three
// bytes, and a dangling link, a directory and nothing at all are no file.
TEST(DirectoryTree, AFileLinkIsSizedByTheFileItReaches) {
    ScratchDir     scratch{Location::Temp, "directory_tree_size"};
    fs::path const dir = scratch.path();
    writeFile(dir / "real.bin", "ABC");
    DSS_ASSERT_LINK_MADE(makeSymbolicLink(dir / "real.bin", dir / "file-link", LinkTo::File));
    DSS_ASSERT_LINK_MADE(makeSymbolicLink(dir / "no-such", dir / "dangling", LinkTo::File));

    EXPECT_EQ(sizeOf(dir / "real.bin"), "3 bytes");
    EXPECT_EQ(sizeOf(dir / "file-link"), "3 bytes") << "the file link is sized " << sizeOf(dir / "file-link");
    EXPECT_EQ(sizeOf(dir / "dangling"), "no file") << "the dangling link is sized " << sizeOf(dir / "dangling");
    EXPECT_EQ(sizeOf(dir), "no file") << "the directory is sized " << sizeOf(dir);
    EXPECT_EQ(sizeOf(dir / "nothing-here"), "no file");
}

// A path through a link REACHES what the link names, on every build: a file link reaches its file, a file named
// through a directory link reaches the same file, and a tail that does not exist is kept as written after the
// resolved part that does.
TEST(DirectoryTree, APathThroughALinkReachesWhatTheLinkNames) {
    ScratchDir     scratch{Location::Temp, "directory_tree_reached_path"};
    fs::path const dir = scratch.path();
    writeFile(dir / "real" / "h.h");
    DSS_ASSERT_LINK_MADE(makeSymbolicLink(dir / "real" / "h.h", dir / "file-link.h", LinkTo::File));
    DSS_ASSERT_LINK_MADE(makeSymbolicLink(dir / "real", dir / "dir-link", LinkTo::Directory));
    std::error_code ec;
    // The answers are VALUES (H6c), compared by their renderings: the base a directory (Container), each path a
    // file or a tail (Leaf).
    std::string const baseText = dss::core::reachedPath(dir, dss::core::Use::Container, ec).spelling().generic();
    ASSERT_FALSE(ec) << ec.message();
    auto const reaches = [&](fs::path const& p) -> std::string {
        std::error_code   rec;
        std::string const r = dss::core::reachedPath(p, dss::core::Use::Leaf, rec).spelling().generic();
        if (rec) return "error: " + rec.message();
        return r.starts_with(baseText + "/") ? r.substr(baseText.size() + 1) : "outside " + baseText + ": " + r;
    };
    EXPECT_EQ(reaches(dir / "file-link.h"), "real/h.h") << "the file link reaches " << reaches(dir / "file-link.h");
    EXPECT_EQ(reaches(dir / "dir-link" / "h.h"), "real/h.h")
        << "the file named through the directory link reaches " << reaches(dir / "dir-link" / "h.h");
    EXPECT_EQ(reaches(dir / "dir-link" / "no-such" / "x.h"), "real/no-such/x.h")
        << "the missing tail through the directory link reaches " << reaches(dir / "dir-link" / "no-such" / "x.h");
}

// The walk yields every entry, descends every real directory, and never goes through a link.
TEST(DirectoryTree, TheWalkNeverDescendsALinkAndYieldsIt) {
    ScratchDir     scratch{Location::Temp, "directory_tree_walk"};
    ScratchDir     outside{Location::Temp, "directory_tree_walk_outside"};
    fs::path const dir = scratch.path();
    writeFile(dir / "real" / "x.json");
    writeFile(dir / "real" / "deeper" / "y.json");
    writeFile(outside.path() / "elsewhere.json");
    DSS_ASSERT_LINK_MADE(makeSymbolicLink(dir / "real", dir / "back-in", LinkTo::Directory));
    DSS_ASSERT_LINK_MADE(makeSymbolicLink(outside.path(), dir / "out", LinkTo::Directory));
    DSS_ASSERT_LINK_MADE(makeSymbolicLink(dir / "gone", dir / "dangling", LinkTo::File));

    auto const seen = walked(dir);
    std::map<std::string, std::string> const expected{
        {"back-in", "link/directory"},     {"dangling", "link/absent"},
        {"out", "link/directory"},         {"real", "directory/directory"},
        {"real/deeper", "directory/directory"}, {"real/deeper/y.json", "file/file"},
        {"real/x.json", "file/file"},
    };
    EXPECT_EQ(seen, expected) << "the walk yielded " << seen.size() << " of the " << expected.size()
                              << " entries: each link is yielded as the entry it is, and nothing behind one is read";
}

// The order a walk hands entries out in, its depth, a pruned directory and a stop.
TEST(DirectoryTree, TheWalkHandsADirectoryOutBeforeItsContentsAndObeysPruneAndStop) {
    ScratchDir     scratch{Location::Temp, "directory_tree_order"};
    fs::path const dir = scratch.path();
    writeFile(dir / "a" / "b" / "c.json");
    writeFile(dir / "a" / "skip" / "hidden.json");

    std::vector<std::string> order;
    auto const walk = dss::core::walkTree(dir, [&](TreeEntry const& e) {
        std::string const rel = e.relative.generic();   // the names below the root, as LISTED (H6c)
        order.push_back(rel + "@" + std::to_string(e.depth));
        return rel == "a/skip" ? WalkStep::Prune : WalkStep::Continue;
    });
    EXPECT_FALSE(walk.error);
    EXPECT_FALSE(walk.stopped);
    auto const at = [&](std::string const& s) {
        return std::find(order.begin(), order.end(), s) - order.begin();
    };
    ASSERT_EQ(order.size(), 4u) << "a, a/b, a/b/c.json, a/skip -- and nothing under the pruned a/skip";
    EXPECT_LT(at("a@0"), at("a/b@1"));
    EXPECT_LT(at("a/b@1"), at("a/b/c.json@2"));
    EXPECT_NE(at("a/skip@1"), static_cast<std::ptrdiff_t>(order.size()));
    EXPECT_EQ(at("a/skip/hidden.json@2"), static_cast<std::ptrdiff_t>(order.size())) << "a pruned directory was read";

    std::size_t handed = 0;
    auto const  stopped = dss::core::walkTree(dir, [&](TreeEntry const&) {
        ++handed;
        return WalkStep::Stop;
    });
    EXPECT_TRUE(stopped.stopped);
    EXPECT_EQ(handed, 1u) << "the walk went on after the visitor ended it";
}

// A root that is NOT THERE is an error naming it -- never an empty, finished walk. The walk reads its root's kind
// before any listing (H6c, A2), so this case is answered there; the root it cannot LIST is the next case's.
TEST(DirectoryTree, TheWalkNamesARootThatIsNotThere) {
    ScratchDir     scratch{Location::Temp, "directory_tree_no_root"};
    fs::path const missing = scratch.path() / "not-here";
    std::size_t    handed = 0;
    auto const     walk = dss::core::walkTree(missing, [&](TreeEntry const&) {
        ++handed;
        return WalkStep::Continue;
    });
    EXPECT_TRUE(walk.error) << "a walk of a root that is not there reported a complete walk";
    EXPECT_EQ(walk.where.spelling().display(), utf8Of(missing));
    EXPECT_EQ(walk.root, dss::core::WalkRoot::Absent) << "the walk reports its root (H6c, A2)";
    EXPECT_EQ(handed, 0u);
}

// A ROOT DIRECTORY THE WALK CANNOT LIST (P69 H6c2, ledger 628): it is there and it is a directory -- its kind is read
// -- but the OS refuses to list it. The walk ends with the OS's error, the root named and called Unreadable, and hands
// out nothing; never an empty, finished walk (a glob matching nothing, an input directory holding no source).
// - POSIX: a mode-0 directory. It stats as a directory and `opendir` refuses it EACCES, for a process that is not the
//   superuser (MEASURED 2026-11-16, u10l: macos-arm64-debug euid 501, linux-x86_64-debug euid 1000). The superuser
//   lists it, so there the cell is UNMEASURED and says so.
// - Windows: a DACL that denies FILE_LIST_DIRECTORY to everyone and allows the rest, so the kind is still read. Neither
//   Windows leg holds the backup or restore privilege that would bypass it (MEASURED u6 P2).
// The OS's own refusal is asserted first, so the cell never passes over a directory the OS would have listed.
TEST(DirectoryTree, TheWalkNamesARootDirectoryItCannotList) {
    ScratchDir     scratch{Location::Temp, "directory_tree_unlistable_root"};
    fs::path const dir = scratch.path() / "unlistable";
    writeFile(dir / "inside.json");
#if defined(_WIN32)
    // Deny FILE_LIST_DIRECTORY (0x1) to everyone on this directory alone, allow everything else; restored before the
    // scratch is removed (the removal lists it).
    auto const setDacl = [&dir](wchar_t const* sddl) {
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) return false;
        BOOL const set = ::SetFileSecurityW(dir.c_str(), DACL_SECURITY_INFORMATION, sd);
        ::LocalFree(sd);
        return set != FALSE;
    };
    struct Restore {
        decltype(setDacl)& set;
        ~Restore() { static_cast<void>(set(L"D:(A;OICI;FA;;;WD)")); }
    } const restore{setDacl};
    ASSERT_TRUE(setDacl(L"D:P(D;;0x1;;;WD)(A;OICI;FA;;;WD)")) << "the DACL was not set: " << ::GetLastError();
    {
        WIN32_FIND_DATAW found{};
        HANDLE const     search = ::FindFirstFileW((dir / L"*").c_str(), &found);
        DWORD const      error  = search == INVALID_HANDLE_VALUE ? ::GetLastError() : 0;
        if (search != INVALID_HANDLE_VALUE) ::FindClose(search);
        ASSERT_EQ(error, static_cast<DWORD>(ERROR_ACCESS_DENIED)) << "the OS lists the directory: the cell asks nothing";
    }
#else
    if (::geteuid() == 0) GTEST_SKIP() << "UNMEASURED: the superuser lists a mode-0 directory";
    struct Restore {
        fs::path const& dir;
        ~Restore() { static_cast<void>(::chmod(dir.c_str(), 0700)); }
    } const restore{dir};
    ASSERT_EQ(::chmod(dir.c_str(), 0), 0) << "the mode was not set: " << std::strerror(errno);
    {
        DIR* const listing = ::opendir(dir.c_str());
        int const  error   = listing == nullptr ? errno : 0;
        if (listing != nullptr) ::closedir(listing);
        ASSERT_EQ(error, EACCES) << "the OS lists the directory: the cell asks nothing";
    }
#endif
    std::size_t handed = 0;
    auto const  walk   = dss::core::walkTree(dir, [&](TreeEntry const&) {
        ++handed;
        return WalkStep::Continue;
    });
    EXPECT_TRUE(walk.error) << "a walk of a root directory it cannot list reported a complete walk";
    EXPECT_EQ(walk.root, dss::core::WalkRoot::Unreadable) << "the walk reports its root (H6c, A2)";
    EXPECT_EQ(walk.where.spelling().display(), utf8Of(dir));
    EXPECT_EQ(handed, 0u);
}

// The removal takes every link as itself: what a link reaches -- outside the tree or inside it -- stays.
TEST(DirectoryTree, TheRemovalTakesALinkAsItselfAndNeverEmptiesItsTarget) {
    ScratchDir     scratch{Location::Temp, "directory_tree_remove"};
    ScratchDir     outside{Location::Temp, "directory_tree_remove_outside"};
    fs::path const tree = scratch.path() / "tree";
    writeFile(tree / "own" / "mine.json");
    writeFile(outside.path() / "kept" / "theirs.json");
    writeFile(outside.path() / "kept.json");
    DSS_ASSERT_LINK_MADE(makeSymbolicLink(outside.path() / "kept", tree / "dir-link", LinkTo::Directory));
    DSS_ASSERT_LINK_MADE(makeSymbolicLink(outside.path() / "kept.json", tree / "own" / "file-link", LinkTo::File));
    DSS_ASSERT_LINK_MADE(makeSymbolicLink(tree / "gone", tree / "dangling", LinkTo::File));
#if defined(_WIN32)
    DSS_ASSERT_LINK_MADE(makeJunction(outside.path() / "kept", tree / "junction"));
#endif

    auto const removal = dss::core::removeTree(tree);
    EXPECT_EQ(removal.failures, 0u) << removal.firstPath.spelling().generic() << ": " << removal.firstError.message();
    EXPECT_EQ(kindOf(tree), "absent") << "the tree is still there";
    EXPECT_EQ(kindOf(outside.path() / "kept" / "theirs.json"), "file")
        << "the file behind the directory link is now " << kindOf(outside.path() / "kept" / "theirs.json")
        << ": the removal went through the link and deleted what it reaches";
    EXPECT_EQ(kindOf(outside.path() / "kept.json"), "file")
        << "the file behind the file link is now " << kindOf(outside.path() / "kept.json");

    // A link given as the target is removed as itself.
    DSS_ASSERT_LINK_MADE(makeSymbolicLink(outside.path() / "kept", scratch.path() / "lone-link", LinkTo::Directory));
    auto const lone = dss::core::removeTree(scratch.path() / "lone-link");
    EXPECT_EQ(lone.failures, 0u) << lone.firstError.message();
    EXPECT_EQ(kindOf(scratch.path() / "lone-link"), "absent");
    EXPECT_EQ(kindOf(outside.path() / "kept" / "theirs.json"), "file") << "removing a link emptied its target";

    // Nothing there is nothing to do.
    EXPECT_EQ(dss::core::removeTree(scratch.path() / "never-was").failures, 0u);
}

// The plainest tree a removal meets: a root holding a file and a directory that holds one. Every directory is
// opened ONCE for both its listing and its deletion -- on Windows a listing handle reopened from one opened for
// the deletion alone is refused, which left every such root listed as nothing and never emptied.
// ★ AND READ-ONLY ENTRIES, which a git checkout leaves (its pack files): a read-only file and a read-only
// directory are removed as MS STL's `remove` removes them -- on Windows the read-only attribute does not stop the
// deletion (the extended disposition that ignores it, or the attribute cleared first), and on POSIX a file's own
// write bit was never asked. (On POSIX the read-only directory is EMPTY: an entry of a directory without its write
// bit cannot be unlinked by anyone but root, the same refusal every library's `remove_all` meets. On Windows the
// read-only directory also holds a read-only file.)
TEST(DirectoryTree, TheRemovalDeletesARootHoldingAFileAndADirectory) {
    ScratchDir     scratch{Location::Temp, "directory_tree_remove_plain"};
    fs::path const root = scratch.path() / "root";
    writeFile(root / "file.json");
    writeFile(root / "sub" / "inner.json");
    writeFile(root / "read-only.json");
    fs::create_directories(root / "read-only-dir");
#if defined(_WIN32)
    writeFile(root / "read-only-dir" / "read-only-inner.json");
    fs::permissions(root / "read-only-dir" / "read-only-inner.json", kAllWrite, fs::perm_options::remove);
#endif
    for (fs::path const& readOnly : {root / "read-only.json", root / "read-only-dir"}) {
        std::error_code pec;
        fs::permissions(readOnly, kAllWrite, fs::perm_options::remove, pec);
        ASSERT_FALSE(pec) << readOnly.generic_string() << ": " << pec.message();
        ASSERT_EQ(fs::status(readOnly).permissions() & fs::perms::owner_write, fs::perms::none)
            << readOnly.generic_string() << " is still writable: the case asks nothing about a read-only entry";
    }
    auto const removal = dss::core::removeTree(root);
    EXPECT_EQ(removal.failures, 0u) << "the removal failed " << removal.failures << " time(s), the first at "
                                    << removal.firstPath.spelling().generic() << ": " << removal.firstError.message();
    EXPECT_EQ(kindOf(root), "absent") << "the removal left the root holding a file and a directory";
}

// A DIRECTORY ANOTHER HOLDER KEEPS OPEN WITHOUT DELETE SHARING -- a child process's current directory is one --
// is left WHOLE, and the removal names it with the OS's refusal. ✔MEASURED 2026-10-28 (MSVC 14.51's STL, one
// probe: a directory held by a handle sharing read|write only, and one that is a child process's current
// directory): `std::filesystem::remove_all` removes NOTHING under it and fails with ERROR_SHARING_VIOLATION. The
// removal opens the directory once to list and delete it, so the same refusal leaves the same tree.
TEST(DirectoryTree, TheRemovalLeavesADirectoryHeldWithoutDeleteSharingWholeAndNamesIt) {
#if !defined(_WIN32)
    GTEST_SKIP() << "a POSIX directory has no sharing mode: an open one is removed; the Windows legs ask this";
#else
    ScratchDir     scratch{Location::Temp, "directory_tree_remove_held"};
    fs::path const held = scratch.path() / "held";
    writeFile(held / "a.json");
    writeFile(held / "sub" / "b.json");
    HANDLE const holder = ::CreateFileW(held.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    ASSERT_NE(holder, INVALID_HANDLE_VALUE) << "the directory could not be held: " << ::GetLastError();
    auto const removal = dss::core::removeTree(held);
    ::CloseHandle(holder);
    EXPECT_EQ(removal.failures, 1u) << "the held directory is one failure (" << removal.failures << ", the first "
                                    << removal.firstPath.spelling().generic() << ": " << removal.firstError.message()
                                    << ")";
    EXPECT_EQ(removal.firstPath.spelling().display(), utf8Of(held))
        << "the failure named " << removal.firstPath.spelling().generic();
    EXPECT_EQ(removal.firstError.value(), static_cast<int>(ERROR_SHARING_VIOLATION))
        << "the refusal was " << removal.firstError.message();
    for (char const* rel : {"a.json", "sub/b.json"}) {
        EXPECT_EQ(kindOf(held / rel), "file") << rel << " is " << kindOf(held / rel)
                                              << ": MS STL leaves a held directory whole, and so does the removal";
    }
#endif
}

// `lastEntry`, THE MARKER OF A HALF-REMOVED TREE: the entry the options name goes LAST, after every other entry of
// the target -- and when another entry could not be removed it STAYS, and with it the target, so a tree the removal
// could not finish is still recognisable by its marker (a kept root's `run-info.txt`). Twenty-one entries beside the
// marker, so a listing that hands the marker out anywhere but last is seen whatever order the file system lists in.
// The entry that stays is made by the substrate's seam: a file added to `stuck` after its listing, so `stuck` is not
// empty when its turn comes -- the same on every host, whoever runs the test.
TEST(DirectoryTree, TheRemovalTakesTheLastEntryLastAndKeepsItWhenAnotherEntryStays) {
    ScratchDir     scratch{Location::Temp, "directory_tree_remove_last_entry"};
    fs::path const whole = scratch.path() / "whole";
    for (int i = 0; i < 20; ++i) writeFile(whole / ("e" + std::to_string(100 + i) + ".json"));
    writeFile(whole / "run-info.txt", "marker\n");
    writeFile(whole / "zz" / "inner.json");
    std::vector<std::string> reached;
    dss::core::substrate_testing::setRemoveTreeEntryHook([&](fs::path const& entry) {
        if (entry.parent_path() == whole) reached.push_back(entry.filename().generic_string());
    });
    auto const removal = dss::core::removeTree(whole, dss::core::RemovalOptions{fs::path{"run-info.txt"}});
    dss::core::substrate_testing::setRemoveTreeEntryHook({});
    std::string order;
    for (auto const& name : reached) order += " " + name;
    EXPECT_EQ(removal.failures, 0u) << removal.firstPath.spelling().generic() << ": " << removal.firstError.message();
    ASSERT_EQ(reached.size(), 22u) << "the removal reached:" << order;
    EXPECT_EQ(reached.back(), "run-info.txt") << "the marker was not the last entry the removal reached:" << order;
    EXPECT_EQ(kindOf(whole), "absent") << "a removal with nothing left over leaves the target " << kindOf(whole);

    fs::path const half  = scratch.path() / "half";
    fs::path const stuck = half / "stuck";
    writeFile(half / "run-info.txt", "marker\n");
    writeFile(stuck / "a.json");
    for (int i = 0; i < 20; ++i) writeFile(half / ("e" + std::to_string(100 + i) + ".json"));
    bool added = false;
    dss::core::substrate_testing::setRemoveTreeEntryHook([&](fs::path const& entry) {
        if (entry != stuck / "a.json" || added) return;
        writeFile(stuck / "late.json");   // not in the listing the removal holds: `stuck` stays
        added = true;
    });
    auto const kept = dss::core::removeTree(half, dss::core::RemovalOptions{fs::path{"run-info.txt"}});
    dss::core::substrate_testing::setRemoveTreeEntryHook({});
    ASSERT_TRUE(added) << "the seam never saw stuck/a.json";
    EXPECT_EQ(kept.failures, 1u) << "the one entry that stays is `stuck` (" << kept.failures << " failure(s), the first "
                                 << kept.firstPath.spelling().generic() << ": " << kept.firstError.message() << ")";
    EXPECT_EQ(kept.firstPath.spelling().display(), utf8Of(stuck))
        << "the failure named " << kept.firstPath.spelling().generic();
    EXPECT_EQ(kindOf(half / "run-info.txt"), "file")
        << "the marker is " << kindOf(half / "run-info.txt") << ": a tree the removal could not finish lost the entry "
           "that says what it is";
    EXPECT_EQ(kindOf(stuck / "late.json"), "file");
    EXPECT_EQ(kindOf(half / "e100.json"), "absent") << "every other entry still goes";
}

// THE RACE, MADE DETERMINISTIC: a directory the removal LISTED as a directory is swapped for a link to a tree
// outside before the removal opens it (the substrate's test seam runs between the listing and the open). The
// removal opens each entry relative to its directory's handle without following a link, so it finds the LINK
// and removes it as itself: the tree outside keeps every file. (The real directory, renamed away by the swap,
// is not in the listing the removal holds, so its parent cannot be deleted -- one failure, named.)
TEST(DirectoryTree, TheRemovalNeverFollowsALinkSwappedInAfterTheListing) {
    ScratchDir     scratch{Location::Temp, "directory_tree_remove_race"};
    ScratchDir     outside{Location::Temp, "directory_tree_remove_race_outside"};
    fs::path const victim = scratch.path() / "victim";
    writeFile(victim / "sub" / "inner.json");
    writeFile(victim / "top.json");
    writeFile(outside.path() / "keep.json", "KEEP");
    fs::path const sub = victim / "sub";
    std::string    swapped;
    dss::core::substrate_testing::setRemoveTreeEntryHook([&](fs::path const& entry) {
        if (entry != sub || !swapped.empty()) return;
        std::error_code rec;
        fs::rename(sub, victim / "sub.moved", rec);
        std::error_code const made = makeSymbolicLink(outside.path(), sub, LinkTo::Directory);
        swapped = rec ? "rename: " + rec.message() : made ? "link: " + made.message() : "swapped";
    });
    auto const removal = dss::core::removeTree(victim);
    dss::core::substrate_testing::setRemoveTreeEntryHook({});
    ASSERT_EQ(swapped, "swapped") << "the seam never saw victim/sub, or the swap failed";
    EXPECT_EQ(kindOf(outside.path() / "keep.json"), "file")
        << "the file outside is now " << kindOf(outside.path() / "keep.json")
        << ": the removal went through the link swapped in after its listing";
    EXPECT_EQ(kindOf(sub), "absent") << "the swapped-in link is " << kindOf(sub) << ": it was not removed as itself";
    EXPECT_EQ(kindOf(victim / "top.json"), "absent");
    EXPECT_EQ(removal.failures, 1u) << "the one leftover is the directory renamed away (" << removal.failures
                                    << " failure(s), the first " << removal.firstPath.spelling().generic() << ": "
                                    << removal.firstError.message() << ")";
    EXPECT_EQ(removal.firstPath.spelling().display(), utf8Of(victim));
}

// A link whose reach cannot be examined -- here one that names itself, a loop -- ENDS the walk, naming the link,
// so no caller reads a tree it did not read; a visitor that asks for such links gets it, with the reason.
TEST(DirectoryTree, AnUnreadableLinkEndsTheWalkNamingItUnlessHandedOut) {
    ScratchDir     scratch{Location::Temp, "directory_tree_loop"};
    fs::path const dir = scratch.path();
    writeFile(dir / "a.json");
    DSS_ASSERT_LINK_MADE(makeSymbolicLink(dir / "loop", dir / "loop", LinkTo::File));
    EXPECT_EQ(kindOf(dir / "loop"), "link");
    std::size_t handed = 0;
    auto const  walk = dss::core::walkTree(dir, [&](TreeEntry const&) {
        ++handed;
        return WalkStep::Continue;
    });
    EXPECT_TRUE(walk.error) << "a walk over a link it cannot examine reported a complete walk (" << handed
                            << " entries handed out)";
    EXPECT_EQ(walk.where.spelling().display(), utf8Of(dir / "loop")) << "the walk ended at "
                                                                    << walk.where.spelling().generic();

    std::map<std::string, std::string> seen;
    auto const handedOut = dss::core::walkTree(
        dir,
        [&](TreeEntry const& e) {
            seen[e.at.filename().string()] = std::string(kindName(e.kind)) + "/" + kindName(e.reaches)
                                             + (e.reachError ? "/unreadable" : "");
            return WalkStep::Continue;
        },
        dss::core::TreeWalkOptions{.handOutUnreadableLinks = true});
    EXPECT_FALSE(handedOut.error) << handedOut.where.spelling().generic() << ": " << handedOut.error.message();
    EXPECT_EQ(seen["loop"], "link/absent/unreadable") << "the looping link was handed out as " << seen["loop"];
    EXPECT_EQ(seen["a.json"], "file/file");
}

// When a file was written is asked of what a path REACHES: a link to a file answers the file's time, never the
// link's own; a dangling link and nothing at all answer nothing. The file is dated 2001-09-09 (`storableDate`: a
// date every file system a leg's temp directory may sit on can store), decades from the link's own time, the
// moment the case makes it -- no duration written, the distance read off the clocks themselves.
TEST(DirectoryTree, AFileLinkWasWrittenWhenItsFileWas) {
    ScratchDir     scratch{Location::Temp, "directory_tree_write_time"};
    fs::path const dir = scratch.path();
    writeFile(dir / "old.bin", "OLD");
    fs::file_time_type const dated = storableDate();
    std::error_code          set;
    fs::last_write_time(dir / "old.bin", dated, set);
    ASSERT_FALSE(set) << set.message();
    DSS_ASSERT_LINK_MADE(makeSymbolicLink(dir / "old.bin", dir / "file-link", LinkTo::File));
    DSS_ASSERT_LINK_MADE(makeSymbolicLink(dir / "no-such", dir / "dangling", LinkTo::File));
    // A time to the second (a file system may keep less than the clock's tick, MinGW's library sets whole seconds).
    auto const spelledTime = [](fs::file_time_type t) {
        return std::to_string(std::chrono::floor<std::chrono::seconds>(t).time_since_epoch().count())
             + " s past the file clock's epoch";
    };
    auto const written = [&spelledTime](fs::path const& p) -> std::string {
        std::error_code ec;
        auto const      t = dss::core::reachedWriteTime(p, dss::core::Use::Leaf, ec);
        if (ec) return "error: " + ec.message();
        if (!t) return "nothing";
        return spelledTime(*t);
    };
    std::string const wanted = spelledTime(dated);
    EXPECT_EQ(written(dir / "old.bin"), wanted) << "the file was not dated, so the link's answer below proves nothing";
    EXPECT_EQ(written(dir / "file-link"), wanted) << "the file link was written " << written(dir / "file-link")
                                                 << ": the link's own time, not its file's";
    EXPECT_EQ(written(dir / "dangling"), "nothing") << "the dangling link was written " << written(dir / "dangling");
    EXPECT_EQ(written(dir / "nothing-here"), "nothing");
}

// A PATH ON A VOLUME WITH NO DOS NAME -- no drive letter, no folder it is mounted in -- is reached through the
// volume's NT name, as MS STL's `canonical` reaches it. ✔MEASURED 2026-10-28 (one probe, both Windows libraries,
// this host's two such volumes): `GetFinalPathNameByHandleW` answers such a path's DOS name with
// ERROR_PATH_NOT_FOUND and its NT name with `\Device\HarddiskVolumeN\...`; MS STL's `canonical` answers
// `\\?\GLOBALROOT\Device\HarddiskVolumeN\...` (and its own `weakly_canonical` refuses a tail that does not exist
// there); MinGW's library re-roots the path onto the C: drive. A host with no such volume, or one this process
// may not open, has nothing to ask and says so.
TEST(DirectoryTree, APathOnAVolumeWithNoDosNameIsReachedThroughItsNtName) {
#if !defined(_WIN32)
    GTEST_SKIP() << "every POSIX path has one spelling from its root; the Windows legs ask this";
#else
    std::wstring volume;
    {
        std::wstring name(MAX_PATH, L'\0');
        HANDLE const find = ::FindFirstVolumeW(name.data(), MAX_PATH);
        ASSERT_NE(find, INVALID_HANDLE_VALUE) << "the volumes could not be listed: " << ::GetLastError();
        do {
            std::wstring const candidate = name.c_str();
            wchar_t            mounts[2048]{};
            DWORD              length = 0;
            if (!::GetVolumePathNamesForVolumeNameW(candidate.c_str(), mounts, 2048, &length) || mounts[0] != L'\0')
                continue;
            HANDLE const root = ::CreateFileW(candidate.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                              nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (root == INVALID_HANDLE_VALUE) continue;
            ::CloseHandle(root);
            volume = candidate;
            break;
        } while (::FindNextVolumeW(find, name.data(), MAX_PATH));
        ::FindVolumeClose(find);
    }
    if (volume.empty()) GTEST_SKIP() << "this host has no volume without a DOS name that this process may open";
    for (std::wstring const& tail : {std::wstring{}, std::wstring{L"not-there.txt"}}) {
        fs::path const  asked = fs::path{volume} / tail;
        std::error_code ec;
        // A VALUE (H6c): the root a directory (Container), a name below it a file (Leaf); compared by its rendering.
        dss::core::HostPath const reached = dss::core::reachedPath(asked, tail.empty() ? dss::core::Use::Container : dss::core::Use::Leaf, ec);
        std::string const what  = tail.empty() ? "the volume's root" : "a name the volume's root does not hold";
        ASSERT_FALSE(ec) << what << ": reachedPath refused it -- " << ec.message();
        std::string const spelled = reached.spelling().display();
        EXPECT_TRUE(spelled.starts_with("\\\\?\\GLOBALROOT\\Device\\"))
            << what << " was reached as " << spelled << ", not through the volume's NT name";
        if (!tail.empty()) {
            EXPECT_EQ(reached.filename(), fs::path{tail}) << "the name that does not exist was not appended as written";
        } else {
            // The answer is a path the OS opens, and it reaches itself: the volume's root again.
            std::error_code again;
            std::string const twice = dss::core::reachedPath(reached, dss::core::Use::Container, again).spelling().display();
            EXPECT_FALSE(again) << "the answer " << spelled << " is no path the OS opens: " << again.message();
            EXPECT_EQ(twice, spelled) << "the answer " << spelled << " reaches " << twice;
        }
    }
#endif
}

// THE WALK UP OF A PATH STOPS AT ITS VOLUME'S ROOT DIRECTORY, WHATEVER WIN32 SPELLING NAMES THE VOLUME (P69 H5, H6).
// ✔MEASURED 2026-11-03 (probe-reference-cc run 20261104-021850-b279d3e9, both Windows legs, MinGW's library and
// MS STL's): the library's `parent_path()` of `\\?\Volume{G}\x`, `\\?\C:\x` and
// `\\?\GLOBALROOT\Device\HarddiskVolumeN\x` is
// the volume WITHOUT its separator -- the name the OS opens as the volume's DEVICE, whose handle has no file
// information and no final path (ERROR_INVALID_FUNCTION) -- and the same name WITH its separator is the volume's root
// directory; the walk stops there because `rootSpelling` parses where the root ends (H6), never by an error.
// THE TABLE IS THE UNIVERSE of the spellings of one volume (✔MEASURED 2026-11-04, runs 20261104-204243-d5c02d5f and
// 20261104-205708-1394b8a2: each of them opens): its drive, `\\?\` and `\\.\` with its drive, the forward-slash
// `//?/` and `//./` (both NORMALIZED device paths), its GUID name under `\\?\` and `\\.\` and in lower case, its NT
// name under `\\?\GLOBALROOT` in either case -- on the volume this test's scratch lies on, for a name the scratch
// holds and one its volume root does not: each reaches where the drive spelling does, and keys alike. And the edges
// Win32 itself draws: a `..` under `\\.\` climbs over the volume (`\\.\C:\..\x` is `\\.\x`, which opens nothing), a
// verbatim rest is literal (`\\?\...\sub\..\held.txt` and `\\?\...\held.txt.` name no file the drive spelling
// names, and are answered in the verbatim form), and the DOS spelling trims what Win32 trims (`held.txt.` is
// `held.txt`).
TEST(DirectoryTree, EverySpellingOfAVolumeWalksUpToItsRootDirectory) {
#if !defined(_WIN32)
    GTEST_SKIP() << "a POSIX path has one spelling of its root; the Windows legs ask this";
#else
    ScratchDir     scratch{Location::Temp, "directory_tree_volume_spellings"};
    fs::path const root = scratch.path().root_path();
    if (root.native().size() != 3u || root.native()[1] != L':')
        GTEST_SKIP() << "this host's scratch lies on no drive letter (" << root.string() << "): no spelling to compare";
    std::wstring const absent = L"dss-absent-" + std::to_wstring(::GetCurrentProcessId());
    ASSERT_FALSE(fs::exists(root / absent)) << "the name this case asks for is held by the volume's root";
    writeFile(scratch.path() / "held.txt");
    fs::create_directories(scratch.path() / "sub");
    std::error_code ec;
    // The answers are VALUES (H6c), compared by their renderings.
    std::string const expectedAbsent = dss::core::reachedPath(root / absent / L"x.txt", dss::core::Use::Leaf, ec).spelling().display();
    ASSERT_FALSE(ec) << "the drive spelling was refused: " << ec.message();
    ASSERT_EQ(dss::core::containingDirectory(dss::core::containingDirectory(
                  dss::core::reachedPath(root / absent / L"x.txt", dss::core::Use::Leaf, ec))).spelling().display(),
              dss::core::reachedPath(root, dss::core::Use::Container, ec).spelling().display())
        << "the drive spelling walked up to no root";
    std::string const expectedHeld = dss::core::reachedPath(scratch.path() / "held.txt", dss::core::Use::Leaf, ec).spelling().display();
    ASSERT_FALSE(ec) << "the held name's drive spelling was refused: " << ec.message();
    wchar_t guid[MAX_PATH]{};
    ASSERT_TRUE(::GetVolumeNameForVolumeMountPointW(root.c_str(), guid, MAX_PATH)) << "no GUID name: "
                                                                                  << ::GetLastError();
    std::wstring const guidName = std::wstring{guid}.substr(4);   // `Volume{G}\`
    std::wstring nt(1024, L'\0');
    {
        HANDLE const h = ::CreateFileW(root.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                       OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        ASSERT_NE(h, INVALID_HANDLE_VALUE) << "the root did not open: " << ::GetLastError();
        DWORD const n = ::GetFinalPathNameByHandleW(h, nt.data(), static_cast<DWORD>(nt.size()),
                                                    FILE_NAME_NORMALIZED | VOLUME_NAME_NT);
        ::CloseHandle(h);
        ASSERT_NE(n, 0u) << "the root's NT name was not given: " << ::GetLastError();
        nt.resize(n);
    }
    std::wstring const drive = root.native().substr(0, 2);
    std::wstring const below = scratch.path().native().substr(3);   // the scratch below the drive's root
    auto const         slashed = [](std::wstring s) {
        std::replace(s.begin(), s.end(), L'\\', L'/');
        return s;
    };
    struct Spelling { char const* what; std::wstring rootSpelled; bool forward; };
    Spelling const spellings[] = {
        {"\\\\?\\ and the drive", L"\\\\?\\" + drive + L"\\", false},
        {"\\\\.\\ and the drive", L"\\\\.\\" + drive + L"\\", false},
        {"//?/ and the drive", L"//?/" + drive + L"/", true},
        {"//./ and the drive", L"//./" + drive + L"/", true},
        {"the GUID name", std::wstring{guid}, false},
        {"the GUID name under \\\\.\\", L"\\\\.\\" + guidName, false},
        {"the GUID name in lower case", L"\\\\?\\volume" + guidName.substr(6), false},
        {"the NT name", L"\\\\?\\GLOBALROOT" + nt, false},
        {"the NT name in lower case", L"\\\\?\\globalroot" + nt, false},
    };
    for (Spelling const& s : spellings) {
        SCOPED_TRACE(s.what);
        std::wstring const tail    = s.forward ? slashed(absent + L"\\x.txt") : absent + L"\\x.txt";
        fs::path const     asked   = fs::path{s.rootSpelled + tail};
        std::error_code    askedEc;
        std::string const  reached = dss::core::reachedPath(asked, dss::core::Use::Leaf, askedEc).spelling().display();
        ASSERT_FALSE(askedEc) << "refused -- " << askedEc.message() << " (the walk stopped at the volume's device)";
        EXPECT_EQ(reached, expectedAbsent) << "reached " << reached << ", where the drive spelling reaches "
                                           << expectedAbsent;
        std::wstring const heldTail = s.forward ? slashed(below + L"\\held.txt") : below + L"\\held.txt";
        fs::path const     heldAsked{s.rootSpelled + heldTail};
        std::string const  heldReached = dss::core::reachedPath(heldAsked, dss::core::Use::Leaf, askedEc).spelling().display();
        ASSERT_FALSE(askedEc) << "the held name was refused -- " << askedEc.message();
        EXPECT_EQ(heldReached, expectedHeld) << "reached " << heldReached;
        EXPECT_EQ(dss::core::PathIdentity::of(heldAsked, dss::core::Use::Leaf),
                  dss::core::PathIdentity::of(scratch.path() / "held.txt", dss::core::Use::Leaf))
            << "one file, two keys";
        EXPECT_EQ(dss::core::PathIdentity::of(asked, dss::core::Use::Leaf), dss::core::PathIdentity::of(root / absent / L"x.txt", dss::core::Use::Leaf))
            << "one not-yet-made file, two keys";
    }
    // Win32's own edges, each as the OS draws it.
    std::error_code edgeEc;
    std::string const climbed = dss::core::reachedPath(
        fs::path{L"\\\\.\\" + drive + L"\\..\\" + below + L"\\held.txt"}, dss::core::Use::Leaf, edgeEc).spelling().display();
    // The OS's answer, asserted as a VALUE (P69 H6b, the review of H6's N6): GetFullPathNameW climbs the `..` over
    // the volume to `\\.\<below>\held.txt`, which opens nothing -- so nothing of it is reached and the answer is that
    // whole, with no error. (Any answer but the held file passed the NE this replaced, an error included.)
    // ★ ITS TEXT IS ITS REFERENCE'S (H6c): the `\\.\` spelling's `\\?\` twin -- one NT namespace, the same object.
    ASSERT_FALSE(edgeEc) << edgeEc.message();
    EXPECT_EQ(climbed, utf8Of(fs::path{L"\\\\?\\" + below + L"\\held.txt"}))
        << "a `..` under \\\\.\\ climbs over the volume, and reaches no drive file: reached " << climbed;
    EXPECT_NE(climbed, expectedHeld);
    for (std::wstring const& literal : {L"\\\\?\\" + drive + L"\\" + below + L"\\sub\\..\\held.txt",
                                        L"\\\\?\\" + drive + L"\\" + below + L"\\held.txt."}) {
        std::string const reached = dss::core::reachedPath(fs::path{literal}, dss::core::Use::Leaf, edgeEc).spelling().display();
        ASSERT_FALSE(edgeEc) << edgeEc.message();
        EXPECT_NE(reached, expectedHeld) << "a verbatim rest is literal: " << reached;
        EXPECT_TRUE(reached.starts_with("\\\\?\\"))
            << "a rest the DOS form would rewrite stays verbatim: " << reached;
        EXPECT_NE(dss::core::PathIdentity::of(fs::path{literal}, dss::core::Use::Leaf),
                  dss::core::PathIdentity::of(scratch.path() / "held.txt", dss::core::Use::Leaf));
    }
    std::string const trimmed =
        dss::core::reachedPath(fs::path{scratch.path().native() + L"\\held.txt."}, dss::core::Use::Leaf, edgeEc).spelling().display();
    ASSERT_FALSE(edgeEc) << edgeEc.message();
    EXPECT_EQ(trimmed, expectedHeld) << "the DOS spelling trims a trailing dot, as Win32 does";
#endif
}

// THE SAME WALK UNDER A SHARE: `\\server\share\` is its volume root, holding its `..`, in each spelling of the share
// -- `\\localhost\C$`, `//localhost/C$`, `\\?\UNC\localhost\C$`, `\\?\unc\` (✔MEASURED: each opens on both Windows legs,
// run 20261104-204243-d5c02d5f) -- for a name the scratch holds and one it does not.
TEST(DirectoryTree, EverySpellingOfAShareWalksUpToItsShare) {
#if !defined(_WIN32)
    GTEST_SKIP() << "a POSIX path has no share; the Windows legs ask this";
#else
    ScratchDir scratch{Location::Temp, "directory_tree_share_spellings"};
    writeFile(scratch.path() / "held.txt");
    std::wstring const local = scratch.path().native();
    if (local.size() < 3 || local[1] != L':') GTEST_SKIP() << "the scratch lies on no drive letter";
    std::wstring const share = L"\\\\localhost\\" + local.substr(0, 1) + L"$" + local.substr(2);
    {
        HANDLE const h = ::CreateFileW(share.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            GTEST_SKIP() << "this host's administrative share does not open (" << ::GetLastError()
                         << "): the share walk is UNMEASURED on this leg, not passing";
        ::CloseHandle(h);
    }
    std::error_code ec;
    // The answers are VALUES (H6c), compared by their renderings.
    std::string const expectedHeld = dss::core::reachedPath(fs::path{share + L"\\held.txt"}, dss::core::Use::Leaf, ec).spelling().display();
    ASSERT_FALSE(ec) << ec.message();
    std::string const expectedAbsent =
        dss::core::reachedPath(fs::path{share + L"\\absent\\x.txt"}, dss::core::Use::Leaf, ec).spelling().display();
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_TRUE(expectedHeld.starts_with("\\\\localhost\\")) << expectedHeld;
    std::wstring forward = share;
    std::replace(forward.begin(), forward.end(), L'\\', L'/');
    struct Spelling { char const* what; std::wstring spelled; };
    Spelling const spellings[] = {{"//localhost/", forward},
                                  {"\\\\?\\UNC\\", L"\\\\?\\UNC" + share.substr(1)},
                                  {"\\\\?\\unc\\", L"\\\\?\\unc" + share.substr(1)}};
    for (auto const& [what, spelled] : spellings) {
        SCOPED_TRACE(what);
        bool const     slashed = spelled.starts_with(L"//");
        fs::path const held{spelled + (slashed ? L"/held.txt" : L"\\held.txt")};
        fs::path const gone{spelled + (slashed ? L"/absent/x.txt" : L"\\absent\\x.txt")};
        EXPECT_EQ(dss::core::reachedPath(held, dss::core::Use::Leaf, ec).spelling().display(), expectedHeld);
        ASSERT_FALSE(ec) << ec.message();
        EXPECT_EQ(dss::core::reachedPath(gone, dss::core::Use::Leaf, ec).spelling().display(), expectedAbsent);
        ASSERT_FALSE(ec) << ec.message();
    }
    // The share holds its `..`: `\\localhost\C$\..\<below>\held.txt` is the held file.
    std::wstring const climbed = L"\\\\localhost\\" + local.substr(0, 1) + L"$\\.." + local.substr(2) + L"\\held.txt";
    EXPECT_EQ(dss::core::reachedPath(fs::path{climbed}, dss::core::Use::Leaf, ec).spelling().display(), expectedHeld);
#endif
}

// Two paths reach ONE file when a link, a hard link or a second spelling leads to it; a dangling link reaches
// no file, so it is the same as nothing -- `false`, and no error, on every build.
TEST(DirectoryTree, ALinkAndAHardLinkReachTheSameFile) {
    ScratchDir     scratch{Location::Temp, "directory_tree_same_file"};
    fs::path const dir = scratch.path();
    writeFile(dir / "real" / "h.h");
    writeFile(dir / "other.h");
    DSS_ASSERT_LINK_MADE(makeSymbolicLink(dir / "real" / "h.h", dir / "file-link.h", LinkTo::File));
    DSS_ASSERT_LINK_MADE(makeSymbolicLink(dir / "real", dir / "dir-link", LinkTo::Directory));
    DSS_ASSERT_LINK_MADE(makeSymbolicLink(dir / "no-such", dir / "dangling", LinkTo::File));
    std::error_code hard;
    fs::create_hard_link(dir / "real" / "h.h", dir / "hard.h", hard);
    ASSERT_FALSE(hard) << hard.message();
    auto const same = [](fs::path const& a, fs::path const& b) -> std::string {
        std::error_code ec;
        bool const      s = dss::core::reachedSameFile(a, b, dss::core::Use::Leaf, ec);
        return ec ? "error: " + ec.message() : s ? "one file" : "two files";
    };
    fs::path const h = dir / "real" / "h.h";
    EXPECT_EQ(same(dir / "file-link.h", h), "one file")
        << "the file link and its file are " << same(dir / "file-link.h", h);
    EXPECT_EQ(same(dir / "dir-link" / "h.h", h), "one file");
    EXPECT_EQ(same(dir / "hard.h", h), "one file") << "the hard link and its file are " << same(dir / "hard.h", h);
    EXPECT_EQ(same(dir / "other.h", h), "two files");
    EXPECT_EQ(same(dir / "dangling", h), "two files")
        << "the dangling link and a file are " << same(dir / "dangling", h);
    EXPECT_EQ(same(dir / "nothing-here", h), "two files");
}

// Where one path lies from another is asked of what each REACHES: a file under a link that leads out of the base
// lies outside it, and a path through a link to the base itself lies in it -- `std::filesystem::relative`'s
// answer under MS STL and libc++, which libstdc++ on Windows does not give (it compares the spellings).
TEST(DirectoryTree, WhereAPathLiesIsReadThroughEveryLink) {
    ScratchDir     scratch{Location::Temp, "directory_tree_relative"};
    fs::path const dir = scratch.path();
    writeFile(dir / "base" / "y");
    writeFile(dir / "outside" / "x.txt");
    DSS_ASSERT_LINK_MADE(makeSymbolicLink(dir / "outside", dir / "base" / "lnk", LinkTo::Directory));
    DSS_ASSERT_LINK_MADE(makeSymbolicLink(dir / "base", dir / "baselink", LinkTo::Directory));
    auto const relative = [](fs::path const& p, fs::path const& base) -> std::string {
        std::error_code ec;
        fs::path const  r = dss::core::reachedRelative(p, dss::core::Use::Leaf, base, ec);
        return ec ? "error: " + ec.message() : r.generic_string();
    };
    EXPECT_EQ(relative(dir / "base" / "lnk" / "x.txt", dir / "base"), "../outside/x.txt")
        << "a file under a link out of the base was spelled as if it lay inside it";
    EXPECT_EQ(relative(dir / "baselink" / "y", dir / "base"), "y") << "a path through a link to the base lies in it";
    EXPECT_EQ(relative(dir / "base" / "y", dir / "baselink"), "y");
    EXPECT_EQ(relative(dir / "base" / "not-yet" / "z", dir / "base"), "not-yet/z")
        << "a path that does not exist yet is spelled as written below what does";
}

// A permission added through a link is added to the file the link reaches. POSIX: the execute bits; Windows:
// the one permission it records, write (the read-only attribute cleared).
TEST(DirectoryTree, APermissionAddedThroughALinkIsAddedToItsFile) {
    ScratchDir     scratch{Location::Temp, "directory_tree_permissions"};
    fs::path const dir = scratch.path();
    writeFile(dir / "real.bin", "X");
    std::error_code prep;
    fs::permissions(dir / "real.bin", fs::perms::owner_read, fs::perm_options::replace, prep);
    ASSERT_FALSE(prep) << prep.message();
    DSS_ASSERT_LINK_MADE(makeSymbolicLink(dir / "real.bin", dir / "file-link", LinkTo::File));
#if defined(_WIN32)
    fs::perms const add = fs::perms::owner_write;
#else
    fs::perms const add = fs::perms::owner_exec;
#endif
    std::error_code ec;
    dss::core::addReachedPermissions(dir / "file-link", add, ec);
    ASSERT_FALSE(ec) << ec.message();
    auto const held = fs::status(dir / "real.bin").permissions();
    EXPECT_NE(held & add, fs::perms::none) << "the permission went to the link, not to the file it reaches";
    std::error_code restore;
    fs::permissions(dir / "real.bin", fs::perms::owner_all, fs::perm_options::add, restore);
    std::error_code dangling;
    DSS_ASSERT_LINK_MADE(makeSymbolicLink(dir / "no-such", dir / "dangling", LinkTo::File));
    dss::core::addReachedPermissions(dir / "dangling", add, dangling);
    EXPECT_TRUE(dangling) << "a permission added through a dangling link reported success";
}

// A FIFO is neither a file nor a directory: it reads as OTHER -- as itself and as what it reaches -- and the
// walk hands it out as such, so a site that takes only regular files (a glob, an input directory) keeps
// refusing it, as it did when each asked the library for `regular`. POSIX only: Windows makes no FIFO in its
// filesystem.
TEST(DirectoryTree, AFifoIsOtherNeverAFile) {
#if defined(_WIN32)
    GTEST_SKIP() << "Windows makes no FIFO in its filesystem; the POSIX legs read it";
#else
    ScratchDir     scratch{Location::Temp, "directory_tree_fifo"};
    fs::path const dir = scratch.path();
    ASSERT_EQ(::mkfifo((dir / "pipe").c_str(), 0600), 0) << "this host made no FIFO";
    EXPECT_EQ(kindOf(dir / "pipe"), "other")
        << "the FIFO reads as " << kindOf(dir / "pipe") << ", not as the special file it is";
    EXPECT_EQ(reachOf(dir / "pipe"), "other") << "the FIFO reaches " << reachOf(dir / "pipe");
    auto const seen = walked(dir);
    ASSERT_EQ(seen.count("pipe"), 1u) << "the walk did not hand the FIFO out";
    EXPECT_EQ(seen.at("pipe"), "other/other") << "the walk handed the FIFO out as " << seen.at("pipe");
#endif
}

// AN ENTRY IS ASKED AS ITSELF: ITS IDENTITY, ITS STORED NAME AND ITS DIRECTORY'S CASE RULE, WITH NO LISTING
// (path_identity.hpp's table). `sameEntry` never follows a link -- a link and what it reaches are two entries -- and
// a folding directory's two spellings of one name are one; `storedEntryName` answers the link's OWN name, never its
// target's, and on a folding host the stored case of a name asked under the other -- on the Mac also for an entry
// whose open is refused (mode 0); `directoryFoldsCase` answers the directory's own rule where the host has the
// question; `reachedPipe` tells a named pipe from every other kind. The header search asks these where a listing is
// refused (test_header_name_matching.cpp).
TEST(DirectoryTree, AnEntrysIdentityStoredNameAndCaseRuleAreAskedOfTheEntry) {
    ScratchDir     scratch{Location::Temp, "directory_tree_stored_name"};
    fs::path const dir = scratch.path();
    writeFile(dir / "x.h");
    writeFile(dir / "z.h");
    fs::create_directories(dir / "Sub");
    DSS_ASSERT_LINK_MADE(makeSymbolicLink(dir / "x.h", dir / "L.h", LinkTo::File));
    bool const folds = fs::exists(dir / "X.H");   // the host's own lookup of the other spelling, asked once
    SCOPED_TRACE(folds ? "this host folds case" : "this host keeps case");
    std::error_code ec;
    EXPECT_FALSE(dss::core::sameEntry(dir / "L.h", dir / "x.h", dss::core::Use::Leaf, ec)) << "a link was followed to what it reaches";
    EXPECT_FALSE(ec) << ec.message();
    EXPECT_TRUE(dss::core::sameEntry(dir / "x.h", dir / "x.h", dss::core::Use::Leaf, ec));
    EXPECT_EQ(dss::core::sameEntry(dir / "x.h", dir / "X.H", dss::core::Use::Leaf, ec), folds) << "the other case of one name";
    EXPECT_FALSE(dss::core::sameEntry(dir / "x.h", dir / "absent.h", dss::core::Use::Leaf, ec));
    EXPECT_FALSE(ec) << "an absent entry was an error: " << ec.message();
    auto const stored = [&](char const* name) -> std::string {
        std::optional<fs::path::string_type> const s = dss::core::storedEntryName(dir / name, dss::core::Use::Leaf, ec);
        if (ec) return "error: " + ec.message();
        return s ? fs::path{*s}.generic_string() : "nothing";
    };
#if defined(_WIN32) || defined(__APPLE__)
    EXPECT_EQ(stored("x.h"), "x.h");
    EXPECT_EQ(stored("L.h"), "L.h") << "the link's own name";
    EXPECT_EQ(stored("absent.h"), "nothing");
    if (folds) {
        EXPECT_EQ(stored("X.H"), "x.h");
        EXPECT_EQ(stored("SUB"), "Sub");
        EXPECT_EQ(stored("l.H"), "L.h") << "the link itself, never what it reaches";
    }
#if defined(__APPLE__)
    // No descriptor is asked for (getattrlist): an entry whose open is refused still answers.
    fs::permissions(dir / "z.h", fs::perms::none, fs::perm_options::replace);
    EXPECT_EQ(stored(folds ? "Z.H" : "z.h"), "z.h") << "a mode-0 file";
    fs::permissions(dir / "z.h", fs::perms::owner_all, fs::perm_options::replace);
#endif
#else
    EXPECT_EQ(stored("x.h"), "nothing") << "this host tells no stored name (path_identity.hpp)";
#endif
    std::optional<bool> const rule = dss::core::directoryFoldsCase(dir, ec);
#if defined(_WIN32) || defined(__APPLE__)
    ASSERT_FALSE(ec) << "the directory's own case rule was refused: " << ec.message();
    ASSERT_TRUE(rule.has_value()) << "no case rule on a host that has the question";
    EXPECT_EQ(*rule, folds) << "the directory's own rule disagrees with its lookup";
#else
    EXPECT_FALSE(rule.has_value()) << "a case rule on a host that has no such question";
    EXPECT_FALSE(ec) << ec.message();
#endif
    EXPECT_FALSE(dss::core::reachedPipe(dir / "x.h", ec)) << "a file read as a pipe";
    EXPECT_FALSE(dss::core::reachedPipe(dir / "Sub", ec)) << "a directory read as a pipe";
#if !defined(_WIN32)
    ASSERT_EQ(::mkfifo((dir / "pipe").c_str(), 0600), 0) << "this host made no FIFO";
    EXPECT_TRUE(dss::core::reachedPipe(dir / "pipe", ec)) << "a FIFO not read as a pipe";
    EXPECT_FALSE(dss::core::reachedPipe("/dev/null", ec)) << "a device read as a pipe";
#endif
}

// A DIRECTORY A USER MADE CASE-SENSITIVE SAYS SO (Windows). ✔MEASURED 2026-10-28 (probe-reference-cc, -D_WIN32_WINNT
// =0x0A00): through the documented class (23) both Windows toolchains let a user make a directory case-sensitive and
// then answer 1 for it; MinGW-w64 13.2's header enumerator is 21, another class, which a query refuses (error 87) --
// the reason the substrate spells the class by value.
TEST(DirectoryTree, ADirectoryMadeCaseSensitiveAnswersThatItKeepsCase) {
#if !defined(_WIN32)
    GTEST_SKIP() << "a per-directory case rule a user sets is Windows'; the Mac's is its volume's";
#else
    ScratchDir     scratch{Location::Temp, "directory_tree_case_sensitive"};
    fs::path const dir = scratch.path() / "cs";
    fs::create_directories(dir);
    HANDLE const h = ::CreateFileW(dir.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    ASSERT_NE(h, INVALID_HANDLE_VALUE) << "the directory could not be opened: error " << ::GetLastError();
    FILE_CASE_SENSITIVE_INFO on{};
    on.Flags        = FILE_CS_FLAG_CASE_SENSITIVE_DIR;
    BOOL const  set = ::SetFileInformationByHandle(h, static_cast<FILE_INFO_BY_HANDLE_CLASS>(23), &on, sizeof on);
    DWORD const why = set ? 0 : ::GetLastError();
    ::CloseHandle(h);
    if (!set) GTEST_SKIP() << "this host will not let a user make a directory case-sensitive (error " << why << ")";
    std::error_code           ec;
    std::optional<bool> const rule = dss::core::directoryFoldsCase(dir, ec);
    ASSERT_FALSE(ec) << "the directory's own case rule was refused: " << ec.message();
    ASSERT_TRUE(rule.has_value());
    EXPECT_FALSE(*rule) << "a case-sensitive directory answered that it folds";
    std::error_code eca;
    EXPECT_TRUE(dss::core::directoryFoldsCase(scratch.path(), eca).value_or(false)) << "its parent still folds";
#endif
}

// ★ EVERY OPERATION AT ANY LENGTH (P69 H6, [[D-LINK-A-DEEP-OUTPUT-DIRECTORY-FAILS-THE-WINDOWS-IMAGE-WRITE]]): a
// directory made, a file written through a stream, examined, listed, read, copied, opened as a C stream and removed,
// its identity keyed and the tree removed -- at a path of 300 characters or more and, the control, below 260 --
// through the substrate's operations and the extended-length spelling. ✔MEASURED before the fix (probe-reference-cc
// runs 20261104-215014-7ba4f08f and 20261104-215144-4893b98f, RtlAreLongPathsEnabled 0): at 260 or more every plain
// Win32 call, _wfopen and std::filesystem call of either library opens nothing. A host with no such limit holds the
// same cells.
TEST(DirectoryTree, EveryOperationReachesAPathOf300CharactersAsBelow260) {
    ScratchDir scratch{Location::Temp, "directory_tree_long_paths"};
    fs::path const top = scratch.path() / std::string(40, 'd');
    fs::path       deep = top;
    while (deep.native().size() < 300) deep /= std::string(40, 'd');
    fs::path const shallow = scratch.path() / "s";
    ASSERT_LT(shallow.native().size(), 260u) << "the control is not below the limit";
    for (fs::path const& dir : {shallow, deep}) {
        SCOPED_TRACE(std::to_string(dir.native().size()) + " characters");
        std::error_code ec;
        EXPECT_TRUE(dss::core::createDirectories(dir, ec)) << ec.message();
        ASSERT_FALSE(ec) << "the directory was not made: " << ec.message();
        EXPECT_FALSE(dss::core::createDirectories(dir, ec)) << "it exists: there was nothing to make";
        EXPECT_FALSE(ec) << ec.message();
        fs::path const file = dir / "f.txt";
        {
            std::ofstream out = dss::core::openOutputStream(file, std::ios::binary).value();
            out << "abc";
            ASSERT_TRUE(out.good()) << "the file was not written";
        }
        EXPECT_EQ(dss::core::entryKind(file, dss::core::Use::Leaf, ec), EntryKind::File) << ec.message();
        EXPECT_EQ(dss::core::reachedKind(dir, dss::core::Use::Container, ec), EntryKind::Directory) << ec.message();
        auto const size = dss::core::reachedFileSize(file, ec);
        ASSERT_TRUE(size.has_value()) << "the file's size was not read: " << ec.message();
        EXPECT_EQ(*size, 3u);
        std::vector<std::string> names;
        auto const walk = dss::core::walkTree(dir, [&names](TreeEntry const& e) {
            names.push_back(e.at.filename().string());
            return WalkStep::Continue;
        });
        EXPECT_FALSE(walk.error) << "the directory was not listed: " << walk.error.message();
        EXPECT_EQ(names, std::vector<std::string>{"f.txt"});
        {
            std::ifstream in = dss::core::openInputStream(file, std::ios::binary).value_or(std::ifstream{});
            std::string   text;
            in >> text;
            EXPECT_EQ(text, "abc") << "the file was not read back";
        }
        fs::path const copy = dir / "g.txt";
        dss::core::copyFile(file, copy, ec);
        EXPECT_FALSE(ec) << "the copy was refused: " << ec.message();
        dss::core::copyFile(file, copy, ec);
        EXPECT_TRUE(ec) << "a copy onto an existing file must be refused";
        ec.clear();
        if (std::FILE* const c = dss::core::openFileStream(copy, "rb").value_or(nullptr)) {
            char bytes[4] = {};
            EXPECT_EQ(std::fread(bytes, 1, 3, c), 3u);
            std::fclose(c);
            EXPECT_STREQ(bytes, "abc");
        } else {
            ADD_FAILURE() << "the C stream was not opened";
        }
        EXPECT_TRUE(dss::core::removeEntry(copy, ec)) << ec.message();
        EXPECT_FALSE(dss::core::removeEntry(copy, ec)) << "nothing is there any more";
        EXPECT_FALSE(ec) << "absence is no error: " << ec.message();
        EXPECT_FALSE(dss::core::canonicalIdentityKey(file, dss::core::Use::Leaf, ec).empty());
        EXPECT_FALSE(ec) << "the identity was not keyed: " << ec.message();
#ifdef _WIN32
        // The DEVICE spelling of the same file, at the same length: converted, it opens what it means.
        EXPECT_EQ(dss::core::entryKind(fs::path{L"\\\\.\\" + file.native()}, dss::core::Use::Leaf, ec), EntryKind::File) << ec.message();
#endif
    }
    auto const removal = dss::core::removeTree(top);
    EXPECT_EQ(removal.failures, 0u) << removal.firstPath.spelling().display() << ": " << removal.firstError.message();
    std::error_code ec;
    EXPECT_EQ(dss::core::entryKind(deep, dss::core::Use::Container, ec), EntryKind::Absent) << "the deep tree was not removed";
}

// ════ THE WALK-UP UNIVERSE, EVERY MEASURED CELL (P69 H6b) ════
// The 256 cells the probe measured -- kinds {drive, cwd-relative, drive-relative, rooted, share, `\\?\`, `\\.\`,
// `\??\`} x the step's last name {`d `, `e.`, `f\..`, `NUL`} x separators {`\`, `/`, mixed, trailing} x {short,
// past MAX_PATH} -- each asked of the substrate's walks in BOTH uses, its expected values the measured answers of
// the one-cut reference (walk_universe_cells.inc, GENERATED from the probe's log, never from this code):
//   LEAF      the child `<step>\x.txt`: reachedPath of a name absent beside it reaches the directory `ref`;
//             containingDirectory(child) is that directory; createDirectoriesFor(child) makes `made`;
//   CONTAINER the step itself: walkTree lists the directory `step`; createDirectories makes `step` (dotdot: the
//             area itself, nothing).
// The objects are made through `\\?\` beside their trimmed twins (`d ` beside `d`, `e.` beside `e`), the review of
// H6's scenario. A cell whose whole names nothing (the OS refused its reference) owes no level; the share's NUL
// directory is the SMB server's refusal (ERROR_ACCESS_DENIED) in every spelling, pinned as that boundary.
#ifdef _WIN32
namespace {

struct WalkCell {
    int         cell;
    bool        isLong;
    char const* kind;
    char const* name;
    char const* sep;
    char const* ref;
    char const* step;
    char const* made;
    int         refused;
};

constexpr WalkCell kWalkCells[] = {
#include "walk_universe_cells.inc"
};

[[nodiscard]] std::wstring wide(char const* s) { return std::wstring(s, s + std::strlen(s)); }
[[nodiscard]] std::wstring verbatimOf(std::wstring const& abs) { return L"\\\\?\\" + abs; }

void makeVerbatim(std::wstring const& abs) { ::CreateDirectoryW(verbatimOf(abs).c_str(), nullptr); }
void putVerbatim(std::wstring const& abs, char const* text) {
    HANDLE const h = ::CreateFileW(verbatimOf(abs).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    ::WriteFile(h, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
    ::CloseHandle(h);
}
// The names a directory holds, read through `\\?\` (the OS's own listing, no name re-resolved).
[[nodiscard]] std::vector<std::wstring> namesIn(std::wstring const& absDir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW          found{};
    HANDLE const h = ::FindFirstFileExW((verbatimOf(absDir) + L"\\*").c_str(), FindExInfoBasic, &found,
                                        FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring const n = found.cFileName;
        if (n != L"." && n != L"..") out.push_back(n);
    } while (::FindNextFileW(h, &found));
    ::FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}
// The cell's spelling of `rel` under the base `base` (an absolute drive path) in kind `kind`; `cwdRel` is the same
// object spelled relative to the process's directory -- the probe's own construction.
[[nodiscard]] std::wstring spelled(std::wstring const& kind, std::wstring const& base, std::wstring const& rel,
                                   std::wstring const& cwdRel) {
    std::wstring const noDrive = base.substr(2);
    if (kind == L"abs") return base + L"\\" + rel;
    if (kind == L"cwd") return cwdRel;
    if (kind == L"drv") return base.substr(0, 2) + cwdRel;
    if (kind == L"root") return noDrive + L"\\" + rel;
    if (kind == L"share") return L"\\\\localhost\\" + base.substr(0, 1) + L"$" + noDrive + L"\\" + rel;
    if (kind == L"verb") return L"\\\\?\\" + base + L"\\" + rel;
    if (kind == L"dev") return L"\\\\.\\" + base + L"\\" + rel;
    return L"\\??\\" + base + L"\\" + rel;
}
[[nodiscard]] std::wstring separated(std::wstring s, std::wstring const& sep, std::size_t at) {
    if (sep == L"fs") std::replace(s.begin(), s.end(), L'\\', L'/');
    if (sep == L"mixed" && at < s.size() && s[at] == L'\\') s[at] = L'/';
    return s;
}
// The process's directory, put back however a cell left it.
struct CurrentDirectoryKept {
    std::wstring was;
    CurrentDirectoryKept() {
        std::wstring buf(MAX_PATH, L'\0');
        DWORD const  n = ::GetCurrentDirectoryW(MAX_PATH, buf.data());
        was            = buf.substr(0, n);
    }
    ~CurrentDirectoryKept() { ::SetCurrentDirectoryW(was.c_str()); }
};
[[nodiscard]] std::wstring markOf(std::wstring const& dir) {
    if (dir.empty()) return L"base.mark";
    if (dir == L"d ") return L"space.mark";
    if (dir == L"d") return L"plain-d.mark";
    if (dir == L"e.") return L"dot.mark";
    if (dir == L"e") return L"plain-e.mark";
    if (dir == L"NUL") return L"nul.mark";
    return dir + L".mark";
}

}  // namespace
#endif

// ★ A CASE ON EVERY LEG (P69 ledger 576): the universe is Win32's -- its spellings (`\\?\`, `\\.\`, `\??\`, a
// drive, a share) and its trims (`d `, `e.`, a last `NUL`) exist on no POSIX host -- so it SKIPS there, saying why,
// and its helpers and table stay Windows-only. A case compiled out on a leg was a mutation arm's pass read as a
// violation (ckpt 42's Mac sweep: 18 arms).
TEST(DirectoryTree, EveryWalkReachesTheWholesLevelInEveryMeasuredCell) {
#if !defined(_WIN32)
    GTEST_SKIP() << "the walk-up universe is Win32's: POSIX resolves no name by its position, and has none of its "
                    "spellings (`\\\\?\\`, `\\\\.\\`, `\\??\\`, a drive, a share) -- the Windows legs ask it";
#else
    ScratchDir           scratch{Location::Temp, "directory_tree_walk_universe"};
    CurrentDirectoryKept kept;
    std::wstring const   S = scratch.path().native();
    ASSERT_TRUE(S.size() >= 3 && S[1] == L':' && S.size() < 200) << "the scratch lies on no drive, or too deep";
    std::wstring chain;
    for (int i = 0; i < 28; ++i) chain += (i < 10 ? L"\\long-level-0" : L"\\long-level-") + std::to_wstring(i);
    std::wstring const L = S + chain;
    for (std::size_t at = 1; at <= chain.size(); ++at)
        if (at == chain.size() || chain[at] == L'\\') makeVerbatim(S + chain.substr(0, at));
    struct Made { wchar_t const* dir; char const* tag; };
    for (std::wstring const& base : {S, L}) {
        putVerbatim(base + L"\\base.mark", "BASE");
        for (Made const m : {Made{L"d ", "SPACE"}, Made{L"d", "PLAIN-D"}, Made{L"e.", "DOT"}, Made{L"e", "PLAIN-E"},
                             Made{L"NUL", "NUL"}, Made{L"f", "F"}, Made{L"g", "G"}}) {
            makeVerbatim(base + L"\\" + m.dir);
            putVerbatim(base + L"\\" + m.dir + L"\\x.txt", m.tag);
            putVerbatim(base + L"\\" + m.dir + L"\\" + markOf(m.dir), m.tag);
        }
        makeVerbatim(base + L"\\mk");
    }
    // The share reaches this scratch only where the host serves its drive's administrative share; the share's cells
    // are asked where it does, and counted where it does not.
    std::error_code    shareEc;
    bool const         shareServed = dss::core::reachedKind(fs::path{spelled(L"share", S, L"d", L"")}, dss::core::Use::Container, shareEc)
                             == EntryKind::Directory;
    std::size_t        asked = 0, owedNothing = 0, shareNotServed = 0, serverRefused = 0;
    for (WalkCell const& c : kWalkCells) {
        SCOPED_TRACE("cell " + std::to_string(c.cell) + " " + (c.isLong ? "long " : "short ") + c.kind + " " + c.name
                     + " " + c.sep);
        std::wstring const kind = wide(c.kind), name = wide(c.name), sep = wide(c.sep);
        if (kind == L"share" && !shareServed) {
            ++shareNotServed;
            continue;
        }
        if (c.ref == nullptr) {   // the whole names nothing: the OS refused its reference, so no level is owed
            ++owedNothing;
            continue;
        }
        bool const         dotdot   = name == L"dotdot";
        std::wstring const step     = dotdot ? L"f\\.." : (name == L"space" ? L"d " : name == L"dot" ? L"e." : L"NUL");
        std::wstring const childRel = dotdot ? L"f\\..\\g\\x.txt" : step + L"\\x.txt";
        std::wstring const base     = c.isLong ? L : S;
        // A cell spelled under `under`, relative to the process's directory `cwd` (a long base cannot be one).
        auto const cellSpelling = [&](std::wstring const& under, std::wstring const& rel, std::size_t stepSize) {
            std::wstring const cwdPrefix = c.isLong ? under.substr(S.size() + 1) + L"\\" : L"";
            std::wstring const bare      = spelled(kind, under, rel, cwdPrefix + rel);
            return separated(bare, sep, stepSize);
        };
        auto const stepSpelling = [&](std::wstring const& under) {
            std::wstring const cwdPrefix = c.isLong ? under.substr(S.size() + 1) + L"\\" : L"";
            std::wstring const bare      = spelled(kind, under, step, cwdPrefix + step);
            std::wstring       out       = separated(bare, sep, bare.find_last_of(L'\\'));
            if (sep == L"trail") out += L"\\";
            return out;
        };
        auto const stepSize = [&](std::wstring const& under) {
            std::wstring const cwdPrefix = c.isLong ? under.substr(S.size() + 1) + L"\\" : L"";
            return spelled(kind, under, step, cwdPrefix + step).size();
        };
        ::SetCurrentDirectoryW((c.isLong ? S : base).c_str());
        std::wstring const child = cellSpelling(base, childRel, stepSize(base));
        if (c.refused != 0) {
            // THE SERVER'S BOUNDARY: every level is refused with the error the OS gave the probe.
            std::error_code ec;
            static_cast<void>(dss::core::reachedKind(dss::core::containingDirectory(fs::path{child}), dss::core::Use::Container, ec));
            EXPECT_EQ(ec.value(), c.refused) << "the share's refusal of its NUL directory, as measured";
            ++serverRefused;
            continue;
        }
        ++asked;
        std::wstring const ref = wide(c.ref), stepDir = wide(c.step), made = wide(c.made);
        // LEAF: a name absent beside the child is reached through the child's own directory.
        std::wstring absent = child;
        absent.replace(absent.size() - 5, 5, L"absent.txt");
        std::error_code reachEc;
        // A VALUE (H6c): the level it lies in is its own cut (`containingDirectory`), never its text's parent.
        dss::core::HostPath const reached = dss::core::reachedPath(fs::path{absent}, dss::core::Use::Leaf, reachEc);
        EXPECT_FALSE(reachEc) << reachEc.message();
        EXPECT_EQ(dss::core::containingDirectory(reached).filename().native(), ref)
            << "reached " << reached.spelling().display();
        // LEAF: the directory the child is in.
        dss::core::HostPath const in = dss::core::containingDirectory(fs::path{child});
        std::error_code inEc;
        EXPECT_EQ(dss::core::reachedKind(in, dss::core::Use::Container, inEc), EntryKind::Directory)
            << in.spelling().display() << " " << inEc.message();
        EXPECT_EQ(in.filename().native(), ref) << in.spelling().display();
        // CONTAINER: the step's listing names the step's own directory.
        std::vector<std::wstring> marks;
        auto const walk = dss::core::walkTree(fs::path{stepSpelling(base)}, [&](TreeEntry const& e) {
            std::wstring const n = e.at.filename().native();
            if (n.size() > 5 && n.substr(n.size() - 5) == L".mark") marks.push_back(n);
            return e.kind == EntryKind::Directory ? WalkStep::Prune : WalkStep::Continue;
        });
        EXPECT_FALSE(walk.error) << walk.where.spelling().display() << ": " << walk.error.message();
        EXPECT_EQ(marks, std::vector<std::wstring>{markOf(stepDir)});
        // LEAF and CONTAINER creations, each in a fresh area of its own.
        for (bool const container : {false, true}) {
            std::wstring const area = base + L"\\mk\\" + (container ? L"c" : L"p") + std::to_wstring(c.cell);
            makeVerbatim(area);
            if (dotdot) makeVerbatim(area + L"\\f");
            ::SetCurrentDirectoryW((c.isLong ? S : area).c_str());
            std::wstring const spelling = container ? stepSpelling(area) : cellSpelling(area, childRel, stepSize(area));
            std::error_code    mkEc;
            static_cast<void>(container ? dss::core::createDirectories(fs::path{spelling}, mkEc)
                                        : dss::core::createDirectoriesFor(fs::path{spelling}, mkEc));
            ::SetCurrentDirectoryW(S.c_str());
            EXPECT_FALSE(mkEc) << (container ? "createDirectories " : "createDirectoriesFor ") << mkEc.message();
            std::vector<std::wstring> held = namesIn(area);
            if (dotdot) held.erase(std::remove(held.begin(), held.end(), std::wstring{L"f"}), held.end());
            std::vector<std::wstring> const expected =
                container ? (dotdot ? std::vector<std::wstring>{} : std::vector<std::wstring>{stepDir})
                          : std::vector<std::wstring>{made};
            EXPECT_EQ(held, expected) << (container ? "the container made " : "the creation for the file made ")
                                      << held.size() << " name(s)";
        }
    }
    // DESCENT: a LISTED directory is walked by its own verbatim spelling -- its parent's, `\`, the listed name -- never
    // by its path re-resolved: each directory of either base, listed one level down, holds its own mark alone (`d `
    // re-resolved lists `d`'s, and NUL re-resolved is the device).
    for (std::wstring const& base : {S, L}) {
        SCOPED_TRACE(base == S ? "descent below the short base" : "descent below the long base");
        std::vector<std::pair<std::wstring, std::wstring>> found;   // (the directory listed one level down, a mark in it)
        auto const walk = dss::core::walkTree(fs::path{base}, [&](TreeEntry const& e) {
            std::wstring const n = e.at.filename().native();
            if (e.depth == 0) return e.kind == EntryKind::Directory && n != L"mk" ? WalkStep::Continue : WalkStep::Prune;
            if (n.size() > 5 && n.substr(n.size() - 5) == L".mark") found.emplace_back(dss::core::containingDirectory(e.at).filename().native(), n);
            return WalkStep::Prune;
        });
        EXPECT_FALSE(walk.error) << walk.where.spelling().display() << ": " << walk.error.message();
        std::vector<std::pair<std::wstring, std::wstring>> expected;
        for (wchar_t const* d : {L"d ", L"d", L"e.", L"e", L"NUL", L"f", L"g"}) expected.emplace_back(d, markOf(d));
        std::sort(found.begin(), found.end());
        std::sort(expected.begin(), expected.end());
        EXPECT_EQ(found, expected) << "a directory listed one level down was walked as another directory";
    }
    // Every cell is accounted for: asked, owed nothing, the server's boundary, or a share this host does not serve.
    EXPECT_EQ(asked + owedNothing + serverRefused + shareNotServed, std::size(kWalkCells));
    EXPECT_EQ(owedNothing, 32u) << "the measured table holds 32 cells whose whole names nothing";
    EXPECT_GE(asked, 192u) << "every non-share owed cell is asked";
    std::cout << "walk universe: " << asked << " cell(s) asked, " << owedNothing << " owe nothing, " << serverRefused
              << " the server's refusal, " << shareNotServed << " share cell(s) on a host that serves no share\n";
    // The scratch holds names plain Win32 cannot remove: the substrate's removal takes them (through handles).
    ::SetCurrentDirectoryW(kept.was.c_str());
    auto const removal = dss::core::removeTree(scratch.path());
    EXPECT_EQ(removal.failures, 0u) << removal.firstPath.spelling().display() << ": " << removal.firstError.message();
#endif
}

// ════ THE LISTED-NAME UNIVERSE (P69 H6c): a name the OS LISTS is the object it names ════
// The 286 MEASURED cells of listed_name_cells.inc (GENERATED from the probe's logs, never from this code): a walked
// root spelled {a drive path, cwd-relative, drive-relative, rooted, a share}, a listed directory or file named
// {`e.`, `e..`, `...`, `d `, `NUL`, `CON`, `COM1`, `AUX`, `n`} beside its trimmed twin of the SAME kind, of the OTHER
// kind, or none -- under a short base and a 469-character one. The table says what each name's PLAIN spelling
// reaches (the twin, the device, the parent, nothing); the walk's VALUE (`TreeEntry::at`) must reach the listed
// object itself in every operation, whatever the plain spelling does:
//   READ       a directory's listing holds its own mark, a file reads its own tag (`at`'s fixed references);
//   KIND       as a leaf and as a container, the listed object's kind;
//   MIDDLE     a typed `x.txt` below a listed directory is the listed directory's `x.txt` (operator/);
//   COPY       `dst / entry.relative` makes the listed NAME literally in a fresh tree (operator/(ListedRelative));
//   SUFFIX     `withSuffix` makes the listed name's sibling, never the twin's;
//   IDENTITY   PathIdentity of the value is the verbatim spelling's, never the twin's;
//   NORMAL     normalizeKeepingRoot keeps the listed name (as a container);
//   RENDERING  a verbatim text renders with its `\` kept (generic());
//   REMOVAL    removing the value removes the listed object and leaves its twin.
// And the GIVEN side (R7): the plain spelling, a GIVEN value, is resolved by the reference rule at each use -- its
// kind as a leaf and as a container are the MEASURED plain answers, which differ beside a twin of the other kind.
#ifdef _WIN32
namespace {

struct ListedCell {
    char const* root;
    bool        isLong;
    char        twin;
    bool        dir;
    char const* name;
    char const* plainLeafKind;
    char const* plainMiddle;
    char const* plainContainerKind;
    char const* plainContainerReach;
};

constexpr ListedCell kListedCells[] = {
#include "listed_name_cells.inc"
};

// The name a leaf trims the listed name to (its TWIN), or "" where it trims to no other name of its directory.
[[nodiscard]] std::wstring twinOf(std::wstring const& name) {
    if (name == L"e." || name == L"e..") return L"e";
    if (name == L"d ") return L"d";
    return L"";
}

// The whole of a file, read through `\\?\` (the OS's own open of the spelling, no name re-resolved).
[[nodiscard]] std::string readVerbatim(std::wstring const& abs) {
    HANDLE const h = ::CreateFileW(verbatimOf(abs).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
                                   nullptr);
    if (h == INVALID_HANDLE_VALUE) return "<unopened>";
    std::string out(256, '\0');
    DWORD       got = 0;
    ::ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
    ::CloseHandle(h);
    out.resize(got);
    return out;
}

[[nodiscard]] std::string readValue(dss::core::HostPath const& v) {
    std::ifstream in = dss::core::openInputStream(v, std::ios::binary).value_or(std::ifstream{});
    if (!in) return "<unopened>";
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] EntryKind kindNamed(char const* measured) {
    std::string const m{measured};
    return m == "dir" ? EntryKind::Directory : m == "file" ? EntryKind::File : EntryKind::Absent;
}

}  // namespace
#endif

TEST(DirectoryTree, EveryListedNameIsTheObjectItNamesInEveryMeasuredCell) {
#if !defined(_WIN32)
    GTEST_SKIP() << "the listed-name universe is Win32's: POSIX trims no name and has none of its spellings -- the "
                    "Windows legs ask it";
#else
    ScratchDir           scratch{Location::Temp, "directory_tree_listed_universe"};
    CurrentDirectoryKept kept;
    std::wstring const   S = scratch.path().native();
    ASSERT_TRUE(S.size() >= 3 && S[1] == L':' && S.size() < 200) << "the scratch lies on no drive, or too deep";
    std::wstring chain;
    for (int i = 0; i < 28; ++i) chain += (i < 10 ? L"\\long-level-0" : L"\\long-level-") + std::to_wstring(i);
    std::wstring const L = S + chain;
    for (std::size_t at = 1; at <= chain.size(); ++at)
        if (at == chain.size() || chain[at] == L'\\') makeVerbatim(S + chain.substr(0, at));
    for (std::wstring const& base : {S, L}) {
        makeVerbatim(base + L"\\LN");
        makeVerbatim(base + L"\\LD");
    }
    std::error_code shareEc;
    bool const      shareServed = dss::core::reachedKind(fs::path{spelled(L"share", S, L"LN", L"")},
                                                         dss::core::Use::Container, shareEc) == EntryKind::Directory;
    std::size_t asked = 0, shareNotServed = 0, serverRefused = 0, givenAsked = 0, plainDiffers = 0;
    int         k = 0;
    for (ListedCell const& c : kListedCells) {
        ++k;
        std::wstring const root = wide(c.root), name = wide(c.name), twin = twinOf(name);
        SCOPED_TRACE("cell " + std::to_string(k) + " " + c.root + (c.isLong ? " long " : " short ") + c.twin + " "
                     + (c.dir ? "dir " : "file ") + "'" + c.name + "'");
        if (root == L"share" && !shareServed) {
            ++shareNotServed;
            continue;
        }
        ++asked;
        std::wstring const base = c.isLong ? L : S;
        std::wstring const rel  = L"LN\\c" + std::to_wstring(k);
        std::wstring const area = base + L"\\" + rel;
        makeVerbatim(area);
        std::string const lit = "lit-" + std::to_string(k), tw = "twin-" + std::to_string(k);
        if (c.dir) {
            makeVerbatim(area + L"\\" + name);
            putVerbatim(area + L"\\" + name + L"\\lit.mark", lit.c_str());
            putVerbatim(area + L"\\" + name + L"\\x.txt", (lit + "-x").c_str());
        } else {
            putVerbatim(area + L"\\" + name, lit.c_str());
        }
        bool const twinMade = c.twin != 'A' && !twin.empty();
        bool const twinIsDir = c.twin == 'P' ? c.dir : !c.dir;
        if (twinMade && twinIsDir) {
            makeVerbatim(area + L"\\" + twin);
            putVerbatim(area + L"\\" + twin + L"\\twin.mark", tw.c_str());
            putVerbatim(area + L"\\" + twin + L"\\x.txt", (tw + "-x").c_str());
        } else if (twinMade) {
            putVerbatim(area + L"\\" + twin, tw.c_str());
        }
        ::SetCurrentDirectoryW(S.c_str());
        std::wstring const rootSpelling = c.isLong ? area : spelled(root, S, rel, rel);
        std::wstring const given        = rootSpelling + L"\\" + name;

        // THE LISTING: the walk hands the listed name out as a value.
        std::optional<TreeEntry> listed;
        auto const walk = dss::core::walkTree(fs::path{rootSpelling}, [&](TreeEntry const& e) {
            if (e.depth == 0 && e.at.filename().native() == name) listed = e;
            return WalkStep::Prune;
        });
        ASSERT_FALSE(walk.error) << walk.where.spelling().display() << ": " << walk.error.message();
        ASSERT_TRUE(listed.has_value()) << "the walk did not list the name";
        dss::core::HostPath const v = listed->at;
        bool const viaShare = !c.isLong && root == L"share";
        // THE SERVER REFUSES A DEVICE NAME WHATEVER ITS SPELLING (H6c run1, both Windows legs, MEASURED: every
        // operation on the listed `NUL`, `CON`, `COM1` or `AUX` through the share answered ERROR_ACCESS_DENIED, its
        // verbatim UNC text too -- the share's server applies Win32's device names on its side; the table's plain
        // column measured the same 5). The home reports the refusal for the listed value, in both uses, and names
        // no other object; the cell is counted, as the walk universe counts its server's boundary.
        // ⚠ THE VERDICT IS THE SERVER'S, MEASURED ON THIS HOST's (the drive's administrative share, served by the
        // host itself). A server that serves a device name as an ordinary name answers its kind with no error, and
        // one that refuses it otherwise answers another value: either way both EXPECTs below fail, naming the cell
        // -- never a pass -- and the table is measured again against that server. Nothing here passes vacuously:
        // the count below pins all 24 cells refused wherever the share is served.
        if (viaShare && (name == L"NUL" || name == L"CON" || name == L"COM1" || name == L"AUX")) {
            EXPECT_EQ(std::string{c.plainLeafKind}, "err 5") << "the table measured another answer";
            for (dss::core::Use const use : {dss::core::Use::Leaf, dss::core::Use::Container}) {
                std::error_code rec;
                EXPECT_EQ(dss::core::reachedKind(v, use, rec), EntryKind::Absent);
                EXPECT_EQ(rec.value(), 5) << "the server's refusal of a device name, as measured: " << rec.message();
            }
            ++serverRefused;
            continue;
        }
        EXPECT_EQ(listed->kind, c.dir ? EntryKind::Directory : EntryKind::File);

        // READ and KIND.
        std::error_code ec;
        EXPECT_EQ(dss::core::reachedKind(v, dss::core::Use::Leaf, ec), c.dir ? EntryKind::Directory : EntryKind::File)
            << "as a leaf: " << ec.message();
        EXPECT_EQ(dss::core::reachedKind(v, dss::core::Use::Container, ec),
                  c.dir ? EntryKind::Directory : EntryKind::File)
            << "as a container: " << ec.message();
        if (c.dir) {
            std::vector<std::wstring> marks;
            auto const inner = dss::core::walkTree(v, [&](TreeEntry const& e) {
                std::wstring const n = e.at.filename().native();
                if (n.size() > 5 && n.substr(n.size() - 5) == L".mark") marks.push_back(n);
                return WalkStep::Prune;
            });
            EXPECT_FALSE(inner.error) << inner.where.spelling().display() << ": " << inner.error.message();
            EXPECT_EQ(marks, std::vector<std::wstring>{L"lit.mark"}) << "the listed directory's listing";
            // MIDDLE: a typed name below the listed directory.
            EXPECT_EQ(readValue(v / "x.txt"), lit + "-x") << "a typed name below the listed directory";
        } else {
            EXPECT_EQ(readValue(v), lit) << "the listed file's bytes";
        }

        // COPY: `dst / entry.relative` names the listed name literally in a fresh tree.
        std::wstring const dst = base + L"\\LD\\d" + std::to_wstring(k);
        makeVerbatim(dst);
        dss::core::HostPath const copied = dss::core::HostPath{fs::path{c.isLong ? L"\\\\?\\" + dst : dst}} / listed->relative;
        if (c.dir) {
            EXPECT_TRUE(dss::core::createDirectories(copied, ec)) << ec.message();
        } else {
            dss::core::copyFile(v, copied, ec);
            EXPECT_FALSE(ec) << ec.message();
            EXPECT_EQ(readVerbatim(dst + L"\\" + name), lit) << "the copy's bytes";
        }
        EXPECT_EQ(namesIn(dst), std::vector<std::wstring>{name}) << "the copy names the listed name";

        // SUFFIX: the sibling of the listed name, never of its twin.
        if (!c.dir) {
            dss::core::openOutputStream(v.withSuffix(L".tmp").value(), std::ios::binary).value() << "sibling";
            EXPECT_EQ(readVerbatim(area + L"\\" + name + L".tmp"), "sibling") << "withSuffix named another sibling";
        }

        // IDENTITY: the verbatim spelling's, never the twin's -- through the ROOT the listing walked: an identity is
        // the final path the OS answers for a handle, and a share's (`\\?\UNC\...`) is another text than its
        // drive's (H6c run1, MEASURED: every share cell's identity differed from the drive's verbatim one).
        std::wstring const verbatimArea =
            viaShare ? L"\\\\?\\UNC\\" + rootSpelling.substr(2) : L"\\\\?\\" + area;
        dss::core::HostPath const verbatimLiteral = fs::path{verbatimArea + L"\\" + name};
        EXPECT_EQ(dss::core::PathIdentity::of(v, dss::core::Use::Leaf),
                  dss::core::PathIdentity::of(verbatimLiteral, dss::core::Use::Leaf));
        EXPECT_EQ(dss::core::PathIdentity::of(v, dss::core::Use::Container),
                  dss::core::PathIdentity::of(verbatimLiteral, dss::core::Use::Container));
        if (twinMade) {
            dss::core::HostPath const verbatimTwin = fs::path{verbatimArea + L"\\" + twin};
            EXPECT_NE(dss::core::PathIdentity::of(v, dss::core::Use::Leaf),
                      dss::core::PathIdentity::of(verbatimTwin, dss::core::Use::Leaf))
                << "the listed value's identity is its twin's";
        }

        // NORMAL and RENDERING.
        EXPECT_EQ(dss::core::normalizeKeepingRoot(v, dss::core::Use::Container).filename().native(), name);
        std::string const shown = v.spelling().display();
        if (shown.rfind("\\\\?\\", 0) == 0)
            EXPECT_EQ(v.spelling().generic(), shown) << "a verbatim text renders with its separators kept";

        // GIVEN (R7): the plain spelling's kinds are the measured plain answers -- but the device's: a last `NUL`
        // reaches the DOS device, whose kind the home answers as its own, where the probe read its attributes.
        std::string const leafKind{c.plainLeafKind}, contKind{c.plainContainerKind};
        if (!c.isLong && name != L"NUL" && (leafKind == "dir" || leafKind == "file")) {
            ++givenAsked;
            std::error_code gec;
            EXPECT_EQ(dss::core::reachedKind(fs::path{given}, dss::core::Use::Leaf, gec), kindNamed(c.plainLeafKind))
                << "the GIVEN spelling's kind as a leaf: " << gec.message();
            if (contKind == "dir")
                EXPECT_EQ(dss::core::reachedKind(fs::path{given}, dss::core::Use::Container, gec), EntryKind::Directory)
                    << "the GIVEN spelling's kind as a container: " << gec.message();
        }
        bool const differs = std::string{c.plainMiddle} == "twin" || std::string{c.plainContainerReach} == "twin"
                             || kindNamed(c.plainLeafKind) != (c.dir ? EntryKind::Directory : EntryKind::File);
        if (differs) ++plainDiffers;

        // REMOVAL: the listed object goes, its twin stays.
        if (c.dir) {
            auto const removal = dss::core::removeTree(v);
            EXPECT_EQ(removal.failures, 0u) << removal.firstPath.spelling().display();
        } else {
            EXPECT_TRUE(dss::core::removeEntry(v, ec)) << ec.message();
            EXPECT_TRUE(dss::core::removeEntry(v.withSuffix(L".tmp").value(), ec)) << ec.message();
        }
        EXPECT_EQ(namesIn(area), twinMade ? std::vector<std::wstring>{twin} : std::vector<std::wstring>{})
            << "the removal took another object than the listed one";
    }
    EXPECT_EQ(asked + shareNotServed, std::size(kListedCells));
    EXPECT_EQ(serverRefused, shareNotServed == 0 ? 24u : 0u)
        << "where the share is served, its 24 device-name cells (4 names x 3 twins x 2 kinds) are the server's "
           "refusal";
    EXPECT_GE(asked, 230u) << "every non-share cell is asked";
    // 92 cells outside the share (132 with it) where the plain spelling reaches another object -- counted from the
    // table when it was generated: the universe is not vacuous.
    EXPECT_GE(plainDiffers, 92u) << "the cells where the plain spelling reaches another object were not all asked";
    std::cout << "listed universe: " << asked << " cell(s) asked (" << plainDiffers
              << " where the plain spelling reaches another object), " << givenAsked << " GIVEN kind cell(s), "
              << serverRefused << " the server's refusal, " << shareNotServed
              << " share cell(s) on a host that serves no share\n";
    ::SetCurrentDirectoryW(kept.was.c_str());
    auto const removal = dss::core::removeTree(scratch.path());
    EXPECT_EQ(removal.failures, 0u) << removal.firstPath.spelling().display() << ": " << removal.firstError.message();
#endif
}

// ════ THE TYPED VALUE'S OWN CELLS (P69 H6c, plan section 6: #35, #36 and #11, #20, #21, #7, #8, #22) ════
//
// Each asserts WHICH OBJECT a value names -- its identity key against the expected object's (and, where the two
// differ, against the other object's), a mark read back, a relative path, a walk's report -- never a spelling alone.
#ifdef _WIN32
namespace {
// The OS's own reading of a whole plain spelling: GetFullPathNameW, asked by the TEST -- an oracle independent of the
// home, which reads a GIVEN value by `normalizedSpelling` (pinned equal to it over F3's universe, H6).
[[nodiscard]] std::wstring fullPathByTheOs(std::wstring const& spelling) {
    DWORD const n = ::GetFullPathNameW(spelling.c_str(), 0, nullptr, nullptr);
    if (n == 0) return {};
    std::wstring out(n, L'\0');
    out.resize(::GetFullPathNameW(spelling.c_str(), n, out.data(), nullptr));
    return out;
}
// A full DOS answer as the verbatim spelling of the same object; a device answer (`\\.\NUL`) as it is.
[[nodiscard]] std::wstring verbatimOfAnswer(std::wstring const& full) {
    return full.size() >= 3 && full[1] == L':' ? verbatimOf(full) : full;
}
[[nodiscard]] dss::core::PathIdentity leafKeyOf(std::wstring const& spelling) {
    return dss::core::PathIdentity::of(dss::core::HostPath{fs::path{spelling}}, dss::core::Use::Leaf);
}
// An ASCII name for a trace.
[[nodiscard]] std::string narrowAscii(std::wstring_view w) {
    std::string out;
    for (wchar_t const c : w) out += static_cast<char>(c);
    return out;
}
}  // namespace
#endif

// ★ [B1] A TYPED RIGHT SIDE NAMES WHAT THE OS READS BY POSITION (#35): `parent / right` for the typed right sides
// {`x.h.`, `x.h `, `NUL` last; `e./x.h`} below three parents -- a GIVEN drive directory, a LISTED `e.` (a walk's
// entry) and a GIVEN `\\?\` directory -- each right side's LITERAL object made beside its trimmed TWIN (present /
// absent), asked as a leaf and as a container: 48 cells.
//   * below the drive parent the expected object is the OS's: GetFullPathNameW of the whole plain spelling, asked at
//     run time (as a container: of the spelling with a separator after it);
//   * below the two verbatim-text parents the LITERAL object -- the names compose as typed (DECIDED, plan section 1
//     B1: no reference reads a typed name below a verbatim directory, u4; a verbatim text cannot name two objects by
//     use).
// Every object is a DIRECTORY, so one object answers both uses. `NUL` has no twin object (the device always stands):
// its two twin rows ask the same objects.
TEST(DirectoryTree, ATypedRightSideNamesTheObjectTheOsReadsByPositionInEveryCell) {
#if !defined(_WIN32)
    GTEST_SKIP() << "only Win32 reads a typed name by its position: on POSIX a name is its bytes, which every POSIX "
                    "case of this file composes";
#else
    ScratchDir         scratch{Location::Temp, "directory_tree_typed_right"};
    std::wstring const S = scratch.path().native();
    ASSERT_TRUE(S.size() >= 3 && S[1] == L':' && S.size() < 180) << "the scratch lies on no drive, or too deep";
    struct Right {
        wchar_t const* typed;
        wchar_t const* literal;   // the object as made below the parent, `\` between its names
        wchar_t const* twin;      // its trimmed twin, or none
    };
    Right const rights[] = {{L"x.h.", L"x.h.", L"x.h"}, {L"x.h ", L"x.h ", L"x.h"}, {L"NUL", L"NUL", nullptr},
                            {L"e./x.h", L"e.\\x.h", L"e\\x.h"}};
    enum class Parent { Drive, Listed, Verbatim };
    std::size_t k = 0, asked = 0;
    for (Parent const parent : {Parent::Drive, Parent::Listed, Parent::Verbatim}) {
        for (Right const& r : rights) {
            for (bool const twin : {true, false}) {
                std::wstring const cell = S + L"\\t" + std::to_wstring(++k);
                makeVerbatim(cell);
                std::wstring const at = parent == Parent::Listed ? cell + L"\\e." : cell;   // the parent on disk
                if (parent == Parent::Listed) makeVerbatim(at);
                auto const makeChain = [&](std::wstring const& rel) {
                    for (std::size_t i = 0; i <= rel.size(); ++i)
                        if (i == rel.size() || rel[i] == L'\\') makeVerbatim(at + L"\\" + rel.substr(0, i));
                };
                makeChain(r.literal);
                if (twin && r.twin != nullptr) makeChain(r.twin);
                dss::core::HostPath left;
                if (parent == Parent::Drive) left = fs::path{at};
                if (parent == Parent::Verbatim) left = fs::path{verbatimOf(at)};
                if (parent == Parent::Listed) {
                    std::size_t const walked = [&] {
                        std::size_t n = 0;
                        static_cast<void>(dss::core::walkTree(fs::path{cell}, [&](TreeEntry const& e) {
                            if (e.at.filename() == fs::path{L"e."}) {
                                left = e.at;
                                ++n;
                            }
                            return WalkStep::Prune;
                        }));
                        return n;
                    }();
                    ASSERT_EQ(walked, 1u) << "the walk of the cell listed no `e.`";
                    ASSERT_EQ(left.origin(), dss::core::HostPath::Origin::Listed);
                }
                for (dss::core::Use const use : {dss::core::Use::Leaf, dss::core::Use::Container}) {
                    bool const leaf = use == dss::core::Use::Leaf;
                    std::string const parentName = parent == Parent::Drive    ? "drive"
                                                 : parent == Parent::Listed ? "listed e."
                                                                            : "verbatim";
                    SCOPED_TRACE("cell " + std::to_string(k) + ": " + parentName + " / `" + narrowAscii(r.typed)
                                 + "`, twin " + (twin ? "present" : "absent") + (leaf ? ", leaf" : ", container"));
                    dss::core::HostPath const composed = left / fs::path{r.typed};
                    std::wstring              expected;
                    if (parent == Parent::Drive) {
                        std::wstring answer = fullPathByTheOs(at + L"\\" + r.typed + (leaf ? L"" : L"\\"));
                        ASSERT_FALSE(answer.empty()) << "GetFullPathNameW refused the plain spelling";
                        if (!leaf && answer.size() > 3 && answer.back() == L'\\') answer.pop_back();
                        expected = verbatimOfAnswer(answer);
                    } else {
                        expected = verbatimOf(at + L"\\" + r.literal);
                    }
                    std::wstring const literalAt = verbatimOf(at + L"\\" + r.literal);
                    std::wstring const twinAt    = r.twin != nullptr ? verbatimOf(at + L"\\" + r.twin) : literalAt;
                    std::wstring const other     = expected == literalAt ? twinAt : literalAt;
                    std::error_code    gotEc, wantEc;
                    auto const         got  = dss::core::PathIdentity::of(composed, use, gotEc);
                    auto const         want = dss::core::PathIdentity::of(dss::core::HostPath{fs::path{expected}},
                                                                          dss::core::Use::Leaf, wantEc);
                    EXPECT_EQ(static_cast<bool>(gotEc), static_cast<bool>(wantEc))
                        << "the composed value: " << gotEc.message() << "; the expected object: " << wantEc.message();
                    EXPECT_EQ(got, want) << "the composed value names `" << got.string()
                                         << "`, the expected object is `" << want.string() << "`";
                    if (other != expected) EXPECT_NE(got, leafKeyOf(other)) << "it names the other object";
                    ++asked;
                }
            }
        }
    }
    EXPECT_EQ(asked, 48u) << "3 parents x 4 right sides x twin {present, absent} x {leaf, container}";
#endif
}

// ★ [B2, A8] THE EMPTY SPELLING (#36, #11): as a left side and a container it is the current directory -- a walk of it
// lists the current directory, `"" / "a.c"` reads `<cwd>/a.c` -- and as a whole LEAF it is refused, naming the
// invalid argument (✔MEASURED, u3 part 6: GetFullPathNameW refuses "" with ERROR_INVALID_NAME; both libraries'
// `status("")` answer not-found -- never "the current directory"). On every leg: the rule is the home's, on each host.
TEST(TypedHostPath, AnEmptySpellingIsTheCurrentDirectoryAsALeftSideAndRefusedAsALeaf) {
    ScratchDir scratch{Location::Temp, "typed_host_path_empty"};
    {
        std::ofstream out{scratch.path() / "a.c", std::ios::binary};
        out << "EMPTY_LEFT\n";
    }
    struct CwdKept {
        fs::path was = fs::current_path();
        ~CwdKept() {
            std::error_code ec;
            fs::current_path(was, ec);
        }
    } const kept;
    fs::current_path(scratch.path());
    dss::core::HostPath const empty;
    std::error_code           leafEc;
    static_cast<void>(dss::core::reachedKind(empty, dss::core::Use::Leaf, leafEc));
    EXPECT_TRUE(leafEc == std::errc::invalid_argument) << "the empty leaf was asked: " << leafEc.message();
    std::error_code idEc;
    static_cast<void>(dss::core::PathIdentity::of(empty, dss::core::Use::Leaf, idEc));
    EXPECT_TRUE(idEc == std::errc::invalid_argument) << "the empty leaf's identity was taken: " << idEc.message();
    EXPECT_FALSE(dss::core::openInputStream(empty).has_value()) << "a stream was opened on the empty leaf";
    std::error_code kindEc;
    EXPECT_EQ(dss::core::reachedKind(empty, dss::core::Use::Container, kindEc), EntryKind::Directory)
        << "the empty container is not the current directory: " << kindEc.message();
    std::vector<std::string> listed;
    auto const               walk = dss::core::walkTree(empty, [&](TreeEntry const& e) {
        listed.push_back(e.at.filename().string());
        return WalkStep::Prune;
    });
    EXPECT_FALSE(walk.error) << walk.error.message();
    EXPECT_EQ(listed, std::vector<std::string>{"a.c"}) << "a walk of the empty spelling lists the current directory";
    std::ifstream in = dss::core::openInputStream(empty / fs::path{"a.c"}).value_or(std::ifstream{});
    std::string   line;
    std::getline(in, line);
    EXPECT_EQ(line, "EMPTY_LEFT") << "`\"\" / \"a.c\"` does not read the current directory's a.c";
}

// ★ [A9a] A ROOTED RIGHT SIDE REPLACES THE LEFT BY THE HOME'S ROOT PARSE (#20): an absolute right side IS the child
// (`C:\x`, `\\?\C:\x`); a rooted `\x` is the left's VOLUME ROOT extended by `x` (a share keeps `\\server\share\`); a
// drive-relative `D:x` on the left's drive extends the left, on another drive it is the GIVEN `D:x` -- never appended.
TEST(TypedHostPath, ARootedRightSideReplacesTheLeftByItsRoot) {
#if !defined(_WIN32)
    GTEST_SKIP() << "a POSIX right side has one root, `/`, which every POSIX case composes as the replacement";
#else
    ScratchDir         scratch{Location::Temp, "typed_host_path_rooted"};
    std::wstring const S = scratch.path().native();
    ASSERT_TRUE(S.size() >= 3 && S[1] == L':') << "the scratch lies on no drive";
    makeVerbatim(S + L"\\d");
    putVerbatim(S + L"\\d\\m", "IN-D");
    putVerbatim(S + L"\\m", "TOP");
    dss::core::HostPath const left = fs::path{S + L"\\d"};
    auto const                key  = [](dss::core::HostPath const& p) {
        return dss::core::PathIdentity::of(p, dss::core::Use::Leaf);
    };
    auto const top = leafKeyOf(verbatimOf(S + L"\\m"));
    auto const inD = leafKeyOf(verbatimOf(S + L"\\d\\m"));
    EXPECT_EQ(key(left / fs::path{S + L"\\m"}), top) << "an absolute right side";
    EXPECT_EQ(key(left / fs::path{verbatimOf(S + L"\\m")}), top) << "a verbatim right side";
    EXPECT_EQ(key(left / fs::path{S.substr(2) + L"\\m"}), top) << "a rooted right side: the left's volume root";
    EXPECT_EQ(key(left / fs::path{S.substr(0, 2) + L"m"}), inD) << "drive-relative on the left's drive";
    wchar_t const             other      = (S[0] == L'Q' || S[0] == L'q') ? L'R' : L'Q';
    dss::core::HostPath const elsewhere  = left / fs::path{std::wstring{other, L':', L'm'}};
    std::string const         given{static_cast<char>(other), ':', 'm'};
    EXPECT_EQ(elsewhere.spelling().display(), given)
        << "drive-relative on another drive is that GIVEN spelling";
    EXPECT_EQ(elsewhere.origin(), dss::core::HostPath::Origin::Given);
    // The share keeps its volume root where the host serves this drive's administrative share.
    std::wstring const shareD = L"\\\\localhost\\" + S.substr(0, 1) + L"$" + S.substr(2) + L"\\d";
    std::error_code    shareEc;
    if (dss::core::reachedKind(fs::path{shareD}, dss::core::Use::Container, shareEc) == EntryKind::Directory) {
        dss::core::HostPath const shareLeft = fs::path{shareD};
        EXPECT_EQ(key(shareLeft / fs::path{S.substr(2) + L"\\m"}),
                  key(fs::path{L"\\\\localhost\\" + S.substr(0, 1) + L"$" + S.substr(2) + L"\\m"}))
            << "a rooted right side below a share: the share's own volume root";
    }
#endif
}

// ★ A LEVEL REPORTED MADE IS CONFIRMED A DIRECTORY (#21): CreateDirectoryW below a DEVICE succeeds and makes nothing
// (✔MEASURED, `\\?\NUL\x`: "made", nothing there), so success alone proves no directory.
TEST(TypedHostPath, AMadeLevelIsConfirmedADirectory) {
#if !defined(_WIN32)
    GTEST_SKIP() << "a POSIX mkdir that succeeds made the directory; the Windows legs ask this";
#else
    std::error_code ec;
    bool const      made = dss::core::createDirectories(fs::path{L"\\\\?\\NUL\\x"}, ec);
    EXPECT_FALSE(made) << "a level below the NUL device was reported made";
    EXPECT_TRUE(ec) << "making a directory below the NUL device reported no failure";
#endif
}

// ★ [A2] A RELATIVE PATH'S BASE IS A CONTAINER (#7): the base `S\root ` (a trailing space) beside its twin `root` is
// the directory `root ` -- a container keeps the space -- so a file in it is `a.c` from there.
TEST(TypedHostPath, ARelativeBaseIsAskedAsAContainer) {
#if !defined(_WIN32)
    GTEST_SKIP() << "POSIX resolves no name by its position: `root ` is its bytes in both uses";
#else
    ScratchDir         scratch{Location::Temp, "typed_host_path_relative_base"};
    std::wstring const S = scratch.path().native();
    makeVerbatim(S + L"\\root ");
    makeVerbatim(S + L"\\root");
    putVerbatim(S + L"\\root \\a.c", "LIT");
    putVerbatim(S + L"\\root\\a.c", "TWIN");
    std::error_code ec;
    fs::path const  rel = dss::core::reachedRelative(fs::path{verbatimOf(S + L"\\root \\a.c")}, dss::core::Use::Leaf,
                                                     fs::path{S + L"\\root "}, ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_EQ(rel, fs::path{L"a.c"}) << "the relative path is `" << rel.string() << "`: the base was not `root `";
#endif
}

// ★ [A2] THE WALK REPORTS ITS ROOT (#8): a FILE root is no directory -- the walk says so, names it, and hands nothing.
TEST(DirectoryTree, AWalkOfAFileRootReportsNoDirectoryAndNamesIt) {
    ScratchDir     scratch{Location::Temp, "directory_tree_file_root"};
    fs::path const file = scratch.path() / "plain.txt";
    writeFile(file);
    std::size_t handed = 0;
    auto const  walk   = dss::core::walkTree(file, [&](TreeEntry const&) {
        ++handed;
        return WalkStep::Continue;
    });
    EXPECT_EQ(walk.root, dss::core::WalkRoot::NotADirectory) << "the walk reports its root (H6c, A2)";
    EXPECT_TRUE(walk.error) << "a walk of a file reported a complete walk";
    EXPECT_EQ(walk.where.spelling().display(), utf8Of(file));
    EXPECT_EQ(handed, 0u);
}

// ★ A FAILED ENTRY IS ASKED WHETHER IT IS GONE BY ITS OWN REFERENCE (#22): a LISTED `e.` / `d ` held open without
// delete sharing (its trimmed twin absent) stays -- and the removal names IT as its first failure, never the twin's
// absence read as "gone".
TEST(DirectoryTree, TheRemovalAsksAHeldListedEntryWhetherItIsGoneByItsOwnReference) {
#if !defined(_WIN32)
    GTEST_SKIP() << "a POSIX directory has no sharing mode and no trimmed twin; the Windows legs ask this";
#else
    ScratchDir         scratch{Location::Temp, "directory_tree_remove_held_listed"};
    std::wstring const S = scratch.path().native();
    for (wchar_t const* name : {L"e.", L"d "}) {
        SCOPED_TRACE(std::string{"held `"} + (name[0] == L'e' ? "e." : "d ") + "`");
        std::wstring const target = S + L"\\t" + (name[0] == L'e' ? L"1" : L"2");
        std::wstring const held   = target + L"\\" + name;
        makeVerbatim(target);
        makeVerbatim(held);
        putVerbatim(held + L"\\x.txt", "X");
        HANDLE const holder = ::CreateFileW(verbatimOf(held).c_str(), FILE_LIST_DIRECTORY,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                            FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        ASSERT_NE(holder, INVALID_HANDLE_VALUE) << "the entry could not be held: " << ::GetLastError();
        auto const removal = dss::core::removeTree(fs::path{target});
        ::CloseHandle(holder);
        ASSERT_GE(removal.failures, 1u) << "the held entry was reported gone";
        EXPECT_EQ(dss::core::PathIdentity::of(removal.firstPath, dss::core::Use::Leaf), leafKeyOf(verbatimOf(held)))
            << "the first failure is `" << removal.firstPath.spelling().display() << "`, not the held entry";
    }
#endif
}
