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
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>   // mkfifo: the special file a POSIX host can make
#endif

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
TEST(DirectoryTree, TheRemovalDeletesARootHoldingAFileAndADirectory) {
    ScratchDir     scratch{Location::Temp, "directory_tree_remove_plain"};
    fs::path const root = scratch.path() / "root";
    writeFile(root / "file.json");
    writeFile(root / "sub" / "inner.json");
    auto const removal = dss::core::removeTree(root);
    EXPECT_EQ(removal.failures, 0u) << "the removal failed " << removal.failures << " time(s), the first at "
                                    << removal.firstPath.generic_string() << ": " << removal.firstError.message();
    EXPECT_EQ(kindOf(root), "absent") << "the removal left the root holding a file and a directory";
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
// link's own; a dangling link and nothing at all answer nothing. The file is dated one tick past the library's
// file clock's own epoch (1601 under MS STL, 2174 under libstdc++, 1970 under libc++ -- not the epoch itself: a
// FILETIME of zero tells SetFileTime to change nothing), centuries from the link's own time, the moment the case
// makes it: a value of the file clock's own, with no duration written and no wall clock read.
TEST(DirectoryTree, AFileLinkWasWrittenWhenItsFileWas) {
    ScratchDir     scratch{Location::Temp, "directory_tree_write_time"};
    fs::path const dir = scratch.path();
    writeFile(dir / "old.bin", "OLD");
    fs::file_time_type const dated{fs::file_time_type::duration{1}};
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
