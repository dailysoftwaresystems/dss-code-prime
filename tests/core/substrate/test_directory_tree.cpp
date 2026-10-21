// THE KIND OF A DIRECTORY ENTRY, AND THE WALK AND THE REMOVAL THAT ASK IT
// (src/core/substrate/path_identity.{hpp,cpp}: `entryKind`, `reachedKind`, `walkTree`, `removeTree`).
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
    ASSERT_FALSE(makeSymbolicLink(dir / "real", dir / "dir-link", LinkTo::Directory));
    ASSERT_FALSE(makeSymbolicLink(dir / "real" / "x.json", dir / "file-link", LinkTo::File));
    ASSERT_FALSE(makeSymbolicLink(dir / "no-such", dir / "dangling", LinkTo::File));

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
    ASSERT_FALSE(makeJunction(dir / "real", dir / "junction")) << "mklink /J could not make the junction";
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
    ASSERT_FALSE(makeSymbolicLink(dir / "real" / "h.h", dir / "file-link.h", LinkTo::File));
    ASSERT_FALSE(makeSymbolicLink(dir / "real", dir / "dir-link", LinkTo::Directory));
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
    ASSERT_FALSE(makeSymbolicLink(dir / "real.bin", dir / "file-link", LinkTo::File));
    ASSERT_FALSE(makeSymbolicLink(dir / "no-such", dir / "dangling", LinkTo::File));

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
    ASSERT_FALSE(makeSymbolicLink(dir / "real" / "h.h", dir / "file-link.h", LinkTo::File));
    ASSERT_FALSE(makeSymbolicLink(dir / "real", dir / "dir-link", LinkTo::Directory));
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
    ASSERT_FALSE(makeSymbolicLink(dir / "real", dir / "back-in", LinkTo::Directory));
    ASSERT_FALSE(makeSymbolicLink(outside.path(), dir / "out", LinkTo::Directory));
    ASSERT_FALSE(makeSymbolicLink(dir / "gone", dir / "dangling", LinkTo::File));

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
    ASSERT_FALSE(makeSymbolicLink(outside.path() / "kept", tree / "dir-link", LinkTo::Directory));
    ASSERT_FALSE(makeSymbolicLink(outside.path() / "kept.json", tree / "own" / "file-link", LinkTo::File));
    ASSERT_FALSE(makeSymbolicLink(tree / "gone", tree / "dangling", LinkTo::File));
#if defined(_WIN32)
    ASSERT_FALSE(makeJunction(outside.path() / "kept", tree / "junction"));
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
    ASSERT_FALSE(makeSymbolicLink(outside.path() / "kept", scratch.path() / "lone-link", LinkTo::Directory));
    auto const lone = dss::core::removeTree(scratch.path() / "lone-link");
    EXPECT_EQ(lone.failures, 0u) << lone.firstError.message();
    EXPECT_EQ(kindOf(scratch.path() / "lone-link"), "absent");
    EXPECT_EQ(kindOf(outside.path() / "kept" / "theirs.json"), "file") << "removing a link emptied its target";

    // Nothing there is nothing to do.
    EXPECT_EQ(dss::core::removeTree(scratch.path() / "never-was").failures, 0u);
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
