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
#include <sys/stat.h>   // mkfifo: the special file a POSIX host can make
#endif
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

[[nodiscard]] std::string kindOf(fs::path const& p) {
    std::error_code ec;
    EntryKind const k = dss::core::entryKind(p, ec);
    return ec ? std::string("error: ") + ec.message() : std::string(kindName(k));
}

[[nodiscard]] std::string reachOf(fs::path const& p) {
    std::error_code ec;
    EntryKind const k = dss::core::reachedKind(p, ec);
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
        out[e.path.lexically_relative(root).generic_string()] =
            std::string(kindName(e.kind)) + "/" + kindName(e.reaches);
        return WalkStep::Continue;
    });
    EXPECT_FALSE(walk.error) << walk.where.generic_string() << ": " << walk.error.message();
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
        return dss::core::PathIdentity::of(a) == dss::core::PathIdentity::of(b) ? "one identity" : "two identities";
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
    fs::path const  base = dss::core::reachedPath(dir, ec);
    ASSERT_FALSE(ec) << ec.message();
    auto const reaches = [&](fs::path const& p) {
        std::error_code rec;
        fs::path const  r = dss::core::reachedPath(p, rec);
        return rec ? "error: " + rec.message() : r.lexically_relative(base).generic_string();
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
        std::string const rel = e.path.lexically_relative(dir).generic_string();
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

// A root the walk cannot list is an error naming it -- never an empty, finished walk.
TEST(DirectoryTree, TheWalkNamesTheRootItCannotList) {
    ScratchDir     scratch{Location::Temp, "directory_tree_no_root"};
    fs::path const missing = scratch.path() / "not-here";
    std::size_t    handed = 0;
    auto const     walk = dss::core::walkTree(missing, [&](TreeEntry const&) {
        ++handed;
        return WalkStep::Continue;
    });
    EXPECT_TRUE(walk.error) << "a walk of a root that is not there reported a complete walk";
    EXPECT_EQ(walk.where, missing);
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
    EXPECT_EQ(removal.failures, 0u) << removal.firstPath.generic_string() << ": " << removal.firstError.message();
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
                                    << removal.firstPath.generic_string() << ": " << removal.firstError.message();
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
                                    << removal.firstPath.generic_string() << ": " << removal.firstError.message()
                                    << ")";
    EXPECT_EQ(removal.firstPath, held) << "the failure named " << removal.firstPath.generic_string();
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
    EXPECT_EQ(removal.failures, 0u) << removal.firstPath.generic_string() << ": " << removal.firstError.message();
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
                                 << kept.firstPath.generic_string() << ": " << kept.firstError.message() << ")";
    EXPECT_EQ(kept.firstPath, stuck) << "the failure named " << kept.firstPath.generic_string();
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
                                    << " failure(s), the first " << removal.firstPath.generic_string() << ": "
                                    << removal.firstError.message() << ")";
    EXPECT_EQ(removal.firstPath, victim);
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
    EXPECT_EQ(walk.where, dir / "loop") << "the walk ended at " << walk.where.generic_string();

    std::map<std::string, std::string> seen;
    auto const handedOut = dss::core::walkTree(
        dir,
        [&](TreeEntry const& e) {
            seen[e.path.filename().string()] = std::string(kindName(e.kind)) + "/" + kindName(e.reaches)
                                             + (e.reachError ? "/unreadable" : "");
            return WalkStep::Continue;
        },
        dss::core::TreeWalkOptions{.handOutUnreadableLinks = true});
    EXPECT_FALSE(handedOut.error) << handedOut.where.generic_string() << ": " << handedOut.error.message();
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
        auto const      t = dss::core::reachedWriteTime(p, ec);
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
        fs::path const  reached = dss::core::reachedPath(asked, ec);
        std::string const what  = tail.empty() ? "the volume's root" : "a name the volume's root does not hold";
        ASSERT_FALSE(ec) << what << ": reachedPath refused it -- " << ec.message();
        std::wstring const spelled = reached.wstring();
        EXPECT_TRUE(spelled.starts_with(L"\\\\?\\GLOBALROOT\\Device\\"))
            << what << " was reached as " << reached.string() << ", not through the volume's NT name";
        if (!tail.empty()) {
            EXPECT_EQ(reached.filename(), fs::path{tail}) << "the name that does not exist was not appended as written";
        } else {
            // The answer is a path the OS opens, and it reaches itself: the volume's root again.
            std::error_code again;
            fs::path const  twice = dss::core::reachedPath(reached, again);
            EXPECT_FALSE(again) << "the answer " << reached.string() << " is no path the OS opens: " << again.message();
            EXPECT_EQ(twice, reached) << "the answer " << reached.string() << " reaches " << twice.string();
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
    fs::path const  expectedAbsent = dss::core::reachedPath(root / absent / L"x.txt", ec);
    ASSERT_FALSE(ec) << "the drive spelling was refused: " << ec.message();
    ASSERT_EQ(expectedAbsent.parent_path().parent_path(), dss::core::reachedPath(root, ec))
        << "the drive spelling walked up to no root";
    fs::path const expectedHeld = dss::core::reachedPath(scratch.path() / "held.txt", ec);
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
        fs::path const     reached = dss::core::reachedPath(asked, askedEc);
        ASSERT_FALSE(askedEc) << "refused -- " << askedEc.message() << " (the walk stopped at the volume's device)";
        EXPECT_EQ(reached, expectedAbsent) << "reached " << reached.string() << ", where the drive spelling reaches "
                                           << expectedAbsent.string();
        std::wstring const heldTail = s.forward ? slashed(below + L"\\held.txt") : below + L"\\held.txt";
        fs::path const     heldAsked{s.rootSpelled + heldTail};
        fs::path const     heldReached = dss::core::reachedPath(heldAsked, askedEc);
        ASSERT_FALSE(askedEc) << "the held name was refused -- " << askedEc.message();
        EXPECT_EQ(heldReached, expectedHeld) << "reached " << heldReached.string();
        EXPECT_EQ(dss::core::PathIdentity::of(heldAsked), dss::core::PathIdentity::of(scratch.path() / "held.txt"))
            << "one file, two keys";
        EXPECT_EQ(dss::core::PathIdentity::of(asked), dss::core::PathIdentity::of(root / absent / L"x.txt"))
            << "one not-yet-made file, two keys";
    }
    // Win32's own edges, each as the OS draws it.
    std::error_code edgeEc;
    fs::path const  climbed =
        dss::core::reachedPath(fs::path{L"\\\\.\\" + drive + L"\\..\\" + below + L"\\held.txt"}, edgeEc);
    EXPECT_NE(climbed, expectedHeld) << "a `..` under \\\\.\\ climbs over the volume, and reaches no drive file";
    for (std::wstring const& literal : {L"\\\\?\\" + drive + L"\\" + below + L"\\sub\\..\\held.txt",
                                        L"\\\\?\\" + drive + L"\\" + below + L"\\held.txt."}) {
        fs::path const reached = dss::core::reachedPath(fs::path{literal}, edgeEc);
        ASSERT_FALSE(edgeEc) << edgeEc.message();
        EXPECT_NE(reached, expectedHeld) << "a verbatim rest is literal: " << reached.string();
        EXPECT_TRUE(reached.native().starts_with(L"\\\\?\\"))
            << "a rest the DOS form would rewrite stays verbatim: " << reached.string();
        EXPECT_NE(dss::core::PathIdentity::of(fs::path{literal}),
                  dss::core::PathIdentity::of(scratch.path() / "held.txt"));
    }
    fs::path const trimmed = dss::core::reachedPath(fs::path{scratch.path().native() + L"\\held.txt."}, edgeEc);
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
    fs::path const  expectedHeld   = dss::core::reachedPath(fs::path{share + L"\\held.txt"}, ec);
    ASSERT_FALSE(ec) << ec.message();
    fs::path const expectedAbsent = dss::core::reachedPath(fs::path{share + L"\\absent\\x.txt"}, ec);
    ASSERT_FALSE(ec) << ec.message();
    EXPECT_TRUE(expectedHeld.native().starts_with(L"\\\\localhost\\")) << expectedHeld.string();
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
        EXPECT_EQ(dss::core::reachedPath(held, ec), expectedHeld);
        ASSERT_FALSE(ec) << ec.message();
        EXPECT_EQ(dss::core::reachedPath(gone, ec), expectedAbsent);
        ASSERT_FALSE(ec) << ec.message();
    }
    // The share holds its `..`: `\\localhost\C$\..\<below>\held.txt` is the held file.
    std::wstring const climbed = L"\\\\localhost\\" + local.substr(0, 1) + L"$\\.." + local.substr(2) + L"\\held.txt";
    EXPECT_EQ(dss::core::reachedPath(fs::path{climbed}, ec), expectedHeld);
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
        bool const      s = dss::core::reachedSameFile(a, b, ec);
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
        fs::path const  r = dss::core::reachedRelative(p, base, ec);
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
    EXPECT_FALSE(dss::core::sameEntry(dir / "L.h", dir / "x.h", ec)) << "a link was followed to what it reaches";
    EXPECT_FALSE(ec) << ec.message();
    EXPECT_TRUE(dss::core::sameEntry(dir / "x.h", dir / "x.h", ec));
    EXPECT_EQ(dss::core::sameEntry(dir / "x.h", dir / "X.H", ec), folds) << "the other case of one name";
    EXPECT_FALSE(dss::core::sameEntry(dir / "x.h", dir / "absent.h", ec));
    EXPECT_FALSE(ec) << "an absent entry was an error: " << ec.message();
    auto const stored = [&](char const* name) -> std::string {
        std::optional<fs::path::string_type> const s = dss::core::storedEntryName(dir / name, ec);
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
            std::ofstream out{dss::core::extendedLengthSpelling(file), std::ios::binary};
            out << "abc";
            ASSERT_TRUE(out.good()) << "the file was not written";
        }
        EXPECT_EQ(dss::core::entryKind(file, ec), EntryKind::File) << ec.message();
        EXPECT_EQ(dss::core::reachedKind(dir, ec), EntryKind::Directory) << ec.message();
        auto const size = dss::core::reachedFileSize(file, ec);
        ASSERT_TRUE(size.has_value()) << "the file's size was not read: " << ec.message();
        EXPECT_EQ(*size, 3u);
        std::vector<std::string> names;
        auto const walk = dss::core::walkTree(dir, [&names](TreeEntry const& e) {
            names.push_back(e.path.filename().string());
            return WalkStep::Continue;
        });
        EXPECT_FALSE(walk.error) << "the directory was not listed: " << walk.error.message();
        EXPECT_EQ(names, std::vector<std::string>{"f.txt"});
        {
            std::ifstream in{dss::core::extendedLengthSpelling(file), std::ios::binary};
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
        if (std::FILE* const c = dss::core::openFileStream(copy, "rb")) {
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
        EXPECT_FALSE(dss::core::canonicalIdentityKey(file, ec).empty());
        EXPECT_FALSE(ec) << "the identity was not keyed: " << ec.message();
#ifdef _WIN32
        // The DEVICE spelling of the same file, at the same length: converted, it opens what it means.
        EXPECT_EQ(dss::core::entryKind(fs::path{L"\\\\.\\" + file.native()}, ec), EntryKind::File) << ec.message();
#endif
    }
    auto const removal = dss::core::removeTree(top);
    EXPECT_EQ(removal.failures, 0u) << removal.firstPath.string() << ": " << removal.firstError.message();
    std::error_code ec;
    EXPECT_EQ(dss::core::entryKind(deep, ec), EntryKind::Absent) << "the deep tree was not removed";
}
