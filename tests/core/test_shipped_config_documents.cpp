// Tests for `shippedConfigDocuments` (config_path_walk), THE ONE OWNER of "which files ARE the documents of
// one config kind" — [[D-CONFIG-STRAY-FILE-NAMED-AFTER-A-LANGUAGE-LOADS-AS-A-SECOND-DOCUMENT]].
//
// ★ THE DEFECT IT ENDS WAS A DISAGREEMENT BETWEEN ENUMERATIONS OF ONE DIRECTORY. The driver's shipped-source
// realization listed `sources/` by SUBSTRING, so `zz.lang.json.bak` was a second language and a same-stem
// `c.lang.json.orig` was silently READ; the LSP listed the same directory by exact suffix. Every arm below
// plants the shapes an editor, a merge or a `cp` leaves beside real documents and asserts the owner's ANSWER,
// not a property of it, so an owner that drifted back to a substring match — or to a partial listing read as
// the corpus — reds here by name.
//
// HERMETIC: every tree is planted under a scratch directory, so the arms answer the same in-tree and out.

#include "core/types/config_path_walk.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

using dss::ShippedConfigDocument;
using dss::shippedConfigDocuments;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

void plant(fs::path const& file) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    ASSERT_FALSE(ec) << "cannot create " << file.parent_path().generic_string() << ": " << ec.message();
    std::ofstream out{file, std::ios::binary};
    out << "{}\n";
    ASSERT_TRUE(out.good()) << "cannot write " << file.generic_string();
}

[[nodiscard]] std::vector<std::string> stemsOf(std::vector<ShippedConfigDocument> const& docs) {
    std::vector<std::string> out;
    for (auto const& d : docs) out.push_back(d.stem);
    return out;
}

} // namespace

// ★★ THE ROW'S OWN SHAPES. Exactly `<stem><suffix>` regular files are documents, SORTED BY STEM; every
// artifact beside them is not — and the owner names no artifact suffix, it defines the accepted form.
TEST(ShippedConfigDocuments, OnlyExactStemSuffixRegularFilesAreDocumentsSortedByStem) {
    ScratchDir const scratch{Location::Temp, "shipped-config-documents"};
    fs::path const dir = scratch.path() / "sources";
    for (char const* leaf : {"zeta.lang.json", "c.lang.json", "asm-x.lang.json",
                             // the strays an editor, a merge or a copy leaves:
                             "c.lang.json.orig", "zz.lang.json.bak", "c.lang.json.rej",
                             "c.lang.json.tmp-1234", "c.lang.json~",
                             // neither a document of this kind nor a stray of one:
                             ".lang.json", "notes.txt", "c.target.json"}) {
        plant(dir / leaf);
    }
    // A DIRECTORY named like a document is not one.
    std::error_code ec;
    fs::create_directories(dir / "d.lang.json", ec);
    ASSERT_FALSE(ec) << ec.message();

    auto const got = shippedConfigDocuments(dir, ".lang.json");
    ASSERT_TRUE(got.has_value()) << got.error();
    EXPECT_EQ(stemsOf(*got), (std::vector<std::string>{"asm-x", "c", "zeta"}));
    for (auto const& d : *got) {
        EXPECT_EQ(d.path, dir / (d.stem + ".lang.json"))
            << "each document's PATH is the file it was read from";
    }
}

// The same owner answers for every kind: the suffix is an argument, never a branch.
TEST(ShippedConfigDocuments, TheSuffixSelectsTheKind) {
    ScratchDir const scratch{Location::Temp, "shipped-config-documents"};
    fs::path const dir = scratch.path() / "object-formats";
    for (char const* leaf : {"pe64-x86_64-windows-exec.format.json", "pe64-x86_64-windows-exec-dll.format.json",
                             "pe64-x86_64-windows-exec.format.json.orig", "x.lang.json"}) {
        plant(dir / leaf);
    }
    auto const got = shippedConfigDocuments(dir, ".format.json");
    ASSERT_TRUE(got.has_value()) << got.error();
    // SORTED BY STEM: a stem that is a prefix of another sorts first, whatever the separator after it.
    EXPECT_EQ(stemsOf(*got), (std::vector<std::string>{"pe64-x86_64-windows-exec",
                                                        "pe64-x86_64-windows-exec-dll"}));
}

// An EMPTY directory is an empty answer, not an error: "no documents" is a fact the caller judges.
TEST(ShippedConfigDocuments, AnEmptyDirectoryIsAnEmptyAnswer) {
    ScratchDir const scratch{Location::Temp, "shipped-config-documents"};
    std::error_code ec;
    fs::create_directories(scratch.path() / "empty", ec);
    ASSERT_FALSE(ec) << ec.message();
    auto const got = shippedConfigDocuments(scratch.path() / "empty", ".lang.json");
    ASSERT_TRUE(got.has_value()) << got.error();
    EXPECT_TRUE(got->empty());
}

// ⚠ A LISTING THAT CANNOT HAPPEN IS REFUSED, NAMING THE DIRECTORY — never an empty (or partial) corpus. Two
// portable ways to make the listing itself fail: a directory that does not exist, and a path that is a FILE.
TEST(ShippedConfigDocuments, ADirectoryThatCannotBeListedIsRefusedByName) {
    ScratchDir const scratch{Location::Temp, "shipped-config-documents"};
    fs::path const missing = scratch.path() / "no-such-dir";
    auto const gone = shippedConfigDocuments(missing, ".lang.json");
    ASSERT_FALSE(gone.has_value()) << "a missing directory must not read as an empty corpus";
    EXPECT_NE(gone.error().find(missing.generic_string()), std::string::npos) << gone.error();
    EXPECT_NE(gone.error().find("could not be listed"), std::string::npos) << gone.error();

    fs::path const file = scratch.path() / "a-file";
    plant(file);
    auto const notDir = shippedConfigDocuments(file, ".lang.json");
    ASSERT_FALSE(notDir.has_value()) << "a FILE where the directory should be must not read as a corpus";
    EXPECT_NE(notDir.error().find(file.generic_string()), std::string::npos) << notDir.error();
}
