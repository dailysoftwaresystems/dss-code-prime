// The shipped runtime's content-addressed object cache
// (`src/program/runtime_object_cache.{hpp,cpp}`).
// [[D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF]]
//
// ── WHAT EACH GROUP PINS, AND WHAT GOES RED IF IT IS REMOVED ────────────────
//
// SIBLING LOOKUP. Every one of the seven shipped exec/pie formats must resolve
// to its EXACT archive-writing sibling BY NAME over the REAL shipped
// `src/dss-config/object-formats` tree. Then the two failure shapes:
// ZERO candidates (spirv/wasm — kinds that ship no archive format at all) and
// MORE THAN ONE. The >1 arm is the load-bearing one: it is constructed here,
// because the shipped tree deliberately has no ambiguous case, and a rule that
// FIRST-MATCHED would pass every other test in this file while silently
// deciding the answer by `directory_iterator` order — sorted on NTFS,
// hash-ordered on ext4.
//
// ★ The >1 arm is run as a TWO-PHASE experiment in one scratch directory: with
// the single verbatim copy present the resolve SUCCEEDS, and adding the second
// document is the ONLY variable that changes between the two calls. Without
// that first phase, a refusal caused by the scratch directory itself (a bad
// path, an unloadable copy) would read exactly like a refusal caused by
// ambiguity.
//
// KEY. Three claims that must hold TOGETHER, and the third is the one a broken
// implementation passes without: (a) determinism, (b) a descriptor change
// invalidates, and (c) an OUT-OF-SCOPE change does NOT. A key that hashed the
// whole config tree — or the wall clock — would satisfy (a) and (b) and fail
// only (c). A test suite holding only (a) and (b) would look identical to one
// covering a cache that invalidates on everything, which is a cache that never
// hits.
//
// ⚠ (b) IS RUN AT CONSTANT BYTE LENGTH, and that is a MEASURED trap rather
// than a precaution: a real shipped-descriptor mutation in this repo was 9,149
// bytes before and 9,149 bytes after. A key built from (path, size) — or a
// test that could be satisfied by a size or line-count check — would have been
// green through exactly that edit. The two descriptor revisions below differ
// in ONE BYTE at the SAME LENGTH, and the test asserts the lengths are equal
// before asserting the digests differ.
//
// STORE / LOOKUP. Miss-then-hit, idempotent re-store, no temp file left
// behind, superseded siblings pruned, and — the isolation control — a store
// under a DIFFERENT key leaving the first artifact untouched at its own path.
//
// ROOTS / READ-THROUGH. The two-root resolution: the override's precedence over
// every platform default, the platform chain itself, compiler identity in the
// per-user root PATH, and the two shapes read-through has to get right — a
// shipped artifact the cache NEVER WROTE being found, and a store landing ONLY
// in the per-user root with the shipped root's file count unmoved. Then the two
// refusals: an UNWRITABLE per-user root and NO per-user root at all. Both assert
// on the MESSAGE, because "it failed" is not the property — the property is that
// a user can fix it from what the message says.
//
// ⚠⚠ EVERY CASE THAT STORES PINS THE WRITABLE ROOT AT ITS OWN SCRATCH DIRECTORY
// (`ScopedUserCacheRoot`). Without it the default resolution is the DEVELOPER'S
// REAL cache — `%LOCALAPPDATA%` or `~/.cache` — which this suite has no business
// writing to, and one case's leftovers would become another case's HIT. A cache
// test that populates the machine it runs on is a test whose second run asserts
// something different from its first.
//
// ⚠⚠ THE WINDOWS PATH BUDGET, RE-MEASURED 2026-08-18 AFTER THE PATH INDEX
// REPLACED THE 64-HEX DIGEST IN THE FILENAME. This block records the arithmetic
// because it is the reason the cache's filename shape is what it is, and
// `RuntimeObjectCachePathBudget` below pins every number in it as a real
// assertion so the two cannot drift.
//
//     %TEMP%/dss-test-scratch/<group>/<pid>-<n>/uc/<stamp>/<config>/<slug>/<temp>
//       33   +      17       + G+1  +    8    + 3+   42  +   6    +  31  + T+1
//
// The three biggest components are the SUBJECT and cannot be shrunk: the
// build-stamp segment is 41 (a dirty tree stamps `0.0.2+g<12>.dirty<16>`), the
// target slug is 30, and the LONGEST NAME is a temp file — the destination's own
// name plus `.` plus `.tmp-<pid>-<n>`, thirteen characters more.
//
//   BEFORE (`<stem>-<64 hex>.a`)     artifact 71, longest temp 84
//   AFTER  (`<stem>-<16 base32>.a`   artifact 23, key document 25,
//           + `<stem>-<16 base32>.key`)              longest temp 38
//
// ⇒ 46 characters recovered, and the longest name is now the KEY DOCUMENT's
// temp rather than the artifact's. ✔MEASURED, this tree, 5-digit pid, `debug`,
// the arm64 elf slug: with the ORIGINAL 38-character group labels the temp path
// was 264 and every store failed at once at `MAX_PATH` (260) — `create_dir`
// had succeeded at 188, which is why it surfaced as "could not open the
// temporary file" rather than as a directory error. The same labels now compose
// 218, and the abbreviated `roc-…` labels compose 198.
//
// ⓘ This is not a fiction of the test tree: the same arithmetic bounds a real
// INSTALLED compiler on Windows, and that is the number that matters. With the
// default `%LOCALAPPDATA%` root, a 5-character username, a 4-character unit
// stem, `release` and the arm64 elf slug, the longest composed path was 223
// (37 characters of headroom) and is now 168 — **92 characters of headroom**.
// ★ THE LAST 9 OF THOSE 92 WERE BOUGHT BY A RENAME THAT WAS NOT ABOUT PATH
// LENGTH AT ALL: the vendor directory went from `dss-code-prime/runtime-cache`
// to `dsscp/runtime-cache` (2026-08-24, cycle P32), and this case is what
// MEASURED it — it red on the SHORTENING, which is the direction nobody
// thinks to check, and is the argument for asserting an exact figure rather
// than an upper bound.
// It still fails LOUD when it bites, and the refusal now names the composed
// path, its length, and a MEASURED verdict on whether the length was the cause
// (see `composedPathNote`), plus the remedy: point `DSS_RUNTIME_CACHE_DIR` at a
// short directory.
//
// ⚠ THE GROUP LABELS STAY SHORT ANYWAY. 62 characters of slack is not licence
// to spend it: this suite runs on hosts whose `%TEMP%` is longer than this one's
// 33 characters (a domain account's roaming profile is routinely 20+ characters
// longer), and a label is free.
//
// ══ WHAT THE KEY-DOCUMENT GROUP PINS ════════════════════════════════════════
// The filename now carries a 16-character INDEX, not the key, so the full key
// document beside the artifact is what makes a hit an identity rather than a
// probability. Four shapes, and the last two are the ones a naive
// implementation passes without: a verified hit, a MISSING key document
// refusing, a key document that DIFFERS refusing (the collision), and a store
// refusing to write its artifact over a colliding entry.

#include "core/crypto/sha256.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"
#include "program/runtime_object_cache.hpp"

#include "repo_root.hpp"
#include "scoped_env.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace dss;
using namespace dss::runtime;
using dss::test_support::Location;
using dss::test_support::ScopedEnv;
using dss::test_support::ScratchDir;

namespace {

namespace fs = std::filesystem;

// ── Fixture text ────────────────────────────────────────────────────────────
//
// Two descriptor revisions of IDENTICAL byte length differing in exactly one
// byte (`"alpha"` → `"alpho"`). See the constant-length note in the file
// docblock — this is the shape that defeats a size-based key.
constexpr std::string_view kDescriptorV1 =
    "{\"symbol\":\"opendir\",\"layout\":\"alpha\",\"size\":8}\n";
constexpr std::string_view kDescriptorV2 =
    "{\"symbol\":\"opendir\",\"layout\":\"alpho\",\"size\":8}\n";

constexpr std::string_view kUnitText =
    "int dss_runtime_probe(void) { return 42; }\n";

constexpr std::string_view kUnrelatedV1 = "unrelated-document-first-revision\n";
constexpr std::string_view kUnrelatedV2 =
    "unrelated-document-second-revision-which-is-a-different-length\n";

constexpr std::string_view kSourcePath     = "runtime/platform/src/unit.c";
constexpr std::string_view kDescriptorPath = "shippedLibs/probe.json";
constexpr std::string_view kUnrelatedPath  = "languages/not-in-the-key.json";

// Two well-formed 64-lowercase-hex digests, fixed so the expected key document
// can be written out literally.
constexpr std::string_view kLanguageDigest =
    "1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kFormatDigest =
    "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";

void writeFile(fs::path const& path, std::string_view contents) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good()) << "could not open " << path.generic_string();
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out.flush();
    ASSERT_TRUE(out.good()) << "could not write " << path.generic_string();
}

[[nodiscard]] std::string readFile(fs::path const& path) {
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.good()) << "could not open " << path.generic_string();
    return std::string{std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>()};
}

// Populate a scratch directory as a self-contained config root. Returns void +
// asserts, so a fixture that could not be laid down FAILS rather than leaving
// the test to assert against absent files.
void layDownConfigRoot(fs::path const& configRoot,
                       std::string_view descriptorText) {
    writeFile(configRoot / kSourcePath, kUnitText);
    writeFile(configRoot / kDescriptorPath, descriptorText);
    writeFile(configRoot / kUnrelatedPath, kUnrelatedV1);
    ASSERT_TRUE(fs::is_regular_file(configRoot / kSourcePath));
    ASSERT_TRUE(fs::is_regular_file(configRoot / kDescriptorPath));
    ASSERT_TRUE(fs::is_regular_file(configRoot / kUnrelatedPath));
}

// ⚠ The two loaded documents are handed over in (language, format) order while
// the key document must emit them in (format, language) order — the sort by
// (label, path) is therefore EXERCISED, not merely present. Hand them over
// pre-sorted and the sort's red-on-disable disappears.
[[nodiscard]] RuntimeObjectRequest
makeRequest(fs::path const&  configRoot,
            std::string_view configName = "debug",
            std::string_view targetSpec = "arm64:elf64-aarch64-linux-exec") {
    RuntimeObjectRequest request;
    request.configRoot        = configRoot;
    request.descriptorPath    = std::string{kDescriptorPath};
    request.sourcePath        = std::string{kSourcePath};
    request.targetSpec        = std::string{targetSpec};
    request.buildFormatName   = "elf64-aarch64-linux-exec";
    request.siblingFormatName = "elf64-aarch64-linux-staticlib";
    request.configName        = std::string{configName};
    request.loadedDocuments   = {
        {"language", "languages/c.lang.json",
         std::string{kLanguageDigest}},
        {"format", "object-formats/elf64-aarch64-linux-exec.format.json",
         std::string{kFormatDigest}},
    };
    return request;
}

[[nodiscard]] std::size_t countEntries(fs::path const& directory) {
    std::size_t n = 0;
    std::error_code ec;
    for (auto const& entry : fs::directory_iterator(directory, ec)) {
        (void)entry;
        ++n;
    }
    return n;
}

// Regular files anywhere under `directory`. Used to prove the SHIPPED root is
// left alone by a store, so it has to see the whole subtree — a store writes
// two levels down (`<config>/<slug>/`), and a count of the top level only would
// be unchanged no matter what the store did.
[[nodiscard]] std::size_t countFilesRecursive(fs::path const& directory) {
    std::size_t     n = 0;
    std::error_code ec;
    for (auto const& entry : fs::recursive_directory_iterator(directory, ec)) {
        std::error_code typeEc;
        if (entry.is_regular_file(typeEc) && !typeEc) ++n;
    }
    return n;
}

[[nodiscard]] bool contains(std::string const& haystack,
                            std::string_view   needle) {
    return haystack.find(needle) != std::string::npos;
}

// ── Lookup, in the two shapes the cases actually want ───────────────────────
//
// `lookupRuntimeObject` answers THREE ways now — hit, miss, and REFUSE (an
// entry that cannot be shown to be this key's). These two helpers keep every
// case saying which of the three it expects, so a case that meant "miss" can
// never be satisfied by a refusal it forgot to distinguish.
[[nodiscard]] std::optional<fs::path> lookupExpectingNoRefusal(
    RuntimeObjectKey const& key) {
    auto found = lookupRuntimeObject(key);
    EXPECT_TRUE(found.has_value())
        << "the lookup REFUSED where the case expected a hit or a miss: "
        << (found.has_value() ? std::string{} : found.error());
    if (!found.has_value()) return std::nullopt;
    return *found;
}

// The key document a store writes beside an artifact, laid down by hand — used
// wherever a case has to construct a cache entry the cache itself did not
// write (a packaged `dist/`, a hand-planted collision).
void layDownEntry(fs::path const&  artifactPath,
                  std::string_view artifactBytes,
                  std::string_view keyDocument) {
    writeFile(artifactPath, artifactBytes);
    writeFile(runtimeKeyDocumentPath(artifactPath), keyDocument);
}

// RFC 4648's base32 alphabet, lowercased — spelled out HERE, independently of
// `dss::crypto`, so a case asserting the path index's alphabet is not asserting
// against the encoder's own copy of it.
constexpr std::string_view kBase32Alphabet = "abcdefghijklmnopqrstuvwxyz234567";

[[nodiscard]] bool isPathIndex(std::string_view text) {
    if (text.size() != 16u) return false;
    for (char const c : text) {
        if (kBase32Alphabet.find(c) == std::string_view::npos) return false;
    }
    return true;
}

// ── The writable-root pin ───────────────────────────────────────────────────

constexpr char const* kCacheDirVar = "DSS_RUNTIME_CACHE_DIR";

// The environment variables that supply the PLATFORM defaults, in the order
// `resolveRuntimeCacheRoots` walks them. Listed once so the chain case and the
// no-root case cannot drift apart on what "every candidate" means.
constexpr char const* kLocalAppDataVar = "LOCALAPPDATA";
constexpr char const* kXdgCacheHomeVar = "XDG_CACHE_HOME";
constexpr char const* kHomeVar         = "HOME";

// Pins the cache's ONLY writable root at a scratch subdirectory for the whole
// test body. See the ⚠⚠ note in the file docblock — this is not tidiness, it is
// what keeps the suite from writing into the developer's real per-user cache.
//
// ⓘ It does NOT create the directory. The first store after install is exactly
// the case where the root does not exist yet, so leaving it absent keeps every
// store case exercising that path rather than a pre-made one.
//
// ⓘ Call sites pass `<scratch>/uc` — a two-character component, for the
// `MAX_PATH` reason in the file docblock. It is spelled at the call site rather
// than appended in here because one case deliberately roots it somewhere else
// entirely (under a regular file, to make the root unwritable).
class ScopedUserCacheRoot {
public:
    explicit ScopedUserCacheRoot(fs::path base)
        : base_(std::move(base)), env_(kCacheDirVar, base_.string()) {}

    ScopedUserCacheRoot(ScopedUserCacheRoot const&)            = delete;
    ScopedUserCacheRoot& operator=(ScopedUserCacheRoot const&) = delete;

    [[nodiscard]] fs::path const& base() const noexcept { return base_; }

private:
    // ⚠ DECLARATION ORDER IS LOAD-BEARING: `base_` must be initialized before
    // `env_`, which reads it. Reordering these two silently overrides the
    // variable with an empty string.
    fs::path  base_;
    ScopedEnv env_;
};

} // namespace

// ═══ SIBLING LOOKUP — the seven shipped exec/pie formats ════════════════════

namespace {

struct SiblingCase {
    char const* buildFormat;
    char const* target;
    char const* expectedSibling;
};

// EVERY exec/pie format the tree ships, each with the archive-writing sibling
// it must resolve to. Wrong-sibling is the failure this pins: an elf64 arm64
// build compiled against the x86_64 staticlib format links clean and produces
// an archive for the wrong machine.
constexpr SiblingCase kSiblingCases[] = {
    {"elf64-x86_64-linux-exec",   "x86_64", "elf64-x86_64-linux-staticlib"},
    {"elf64-x86_64-linux-pie",    "x86_64", "elf64-x86_64-linux-staticlib"},
    {"elf64-aarch64-linux-exec",  "arm64",  "elf64-aarch64-linux-staticlib"},
    {"elf64-aarch64-linux-pie",   "arm64",  "elf64-aarch64-linux-staticlib"},
    {"macho64-x86_64-darwin-exec", "x86_64",
     "macho64-x86_64-darwin-staticlib"},
    {"macho64-arm64-darwin-exec", "arm64", "macho64-arm64-darwin-staticlib"},
    {"pe64-x86_64-windows-exec",  "x86_64", "pe64-x86_64-windows-staticlib"},
};

} // namespace

TEST(RuntimeObjectCacheSibling, EveryShippedExecFormatResolvesItsExactSibling) {
    auto const root = dss::test::findRepoRoot();
    ASSERT_TRUE(root.has_value()) << dss::test::repoRootDiagnostic();
    fs::path const formatsDir = *root / "src" / "dss-config" / "object-formats";
    ASSERT_TRUE(fs::is_directory(formatsDir))
        << "the shipped object-format tree is the SUBJECT of this test; "
           "without it the case would silently pass over an empty scan: "
        << formatsDir.generic_string();

    for (auto const& testCase : kSiblingCases) {
        auto const format = ObjectFormatSchema::loadShipped(testCase.buildFormat);
        ASSERT_TRUE(format.has_value())
            << "shipped build format failed to load: " << testCase.buildFormat;
        auto const target = TargetSchema::loadShipped(testCase.target);
        ASSERT_TRUE(target.has_value())
            << "shipped target failed to load: " << testCase.target;

        auto const sibling =
            resolveArchiveSiblingFormat(**format, **target, formatsDir,
            dss::runtime::kRuntimeCacheSiblingRequester);
        ASSERT_TRUE(sibling.has_value())
            << testCase.buildFormat << " refused: " << sibling.error();
        EXPECT_EQ(*sibling, testCase.expectedSibling)
            << "wrong archive sibling for " << testCase.buildFormat;
    }
}

// ── ZERO candidates: the kinds that ship no archive format at all ───────────
//
// ⓘ The header calls this out explicitly: spirv/wasm have no archive sibling
// and no machine identity, so their candidate set is EMPTY. That must be a
// LOUD refusal — a silent "nothing to do" would report success having compiled
// nothing.

TEST(RuntimeObjectCacheSibling, SpirvHasNoArchiveSiblingAndRefusesByName) {
    auto const root = dss::test::findRepoRoot();
    ASSERT_TRUE(root.has_value()) << dss::test::repoRootDiagnostic();
    fs::path const formatsDir = *root / "src" / "dss-config" / "object-formats";
    ASSERT_TRUE(fs::is_directory(formatsDir)) << formatsDir.generic_string();

    auto const format = ObjectFormatSchema::loadShipped("spirv-1.6");
    ASSERT_TRUE(format.has_value());
    auto const target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());

    auto const sibling =
        resolveArchiveSiblingFormat(**format, **target, formatsDir,
            dss::runtime::kRuntimeCacheSiblingRequester);
    ASSERT_FALSE(sibling.has_value())
        << "spirv resolved a sibling it cannot have: " << *sibling;
    EXPECT_TRUE(contains(sibling.error(), "'spirv-1.6'")) << sibling.error();
    EXPECT_TRUE(contains(sibling.error(), "kind 'spirv'")) << sibling.error();
    EXPECT_TRUE(contains(sibling.error(), "NO "
                                          "archive-writing sibling"))
        << sibling.error();
}

TEST(RuntimeObjectCacheSibling, WasmHasNoArchiveSiblingAndRefusesByName) {
    auto const root = dss::test::findRepoRoot();
    ASSERT_TRUE(root.has_value()) << dss::test::repoRootDiagnostic();
    fs::path const formatsDir = *root / "src" / "dss-config" / "object-formats";
    ASSERT_TRUE(fs::is_directory(formatsDir)) << formatsDir.generic_string();

    auto const format = ObjectFormatSchema::loadShipped("wasm32-v1");
    ASSERT_TRUE(format.has_value());
    auto const target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());

    auto const sibling =
        resolveArchiveSiblingFormat(**format, **target, formatsDir,
            dss::runtime::kRuntimeCacheSiblingRequester);
    ASSERT_FALSE(sibling.has_value())
        << "wasm resolved a sibling it cannot have: " << *sibling;
    EXPECT_TRUE(contains(sibling.error(), "'wasm32-v1'")) << sibling.error();
    EXPECT_TRUE(contains(sibling.error(), "kind 'wasm'")) << sibling.error();
}

// ── MORE THAN ONE: the arm a first-match rule would pass ────────────────────

TEST(RuntimeObjectCacheSibling, TwoAgreeingArchiveFormatsRefuseAndNameBoth) {
    auto const root = dss::test::findRepoRoot();
    ASSERT_TRUE(root.has_value()) << dss::test::repoRootDiagnostic();
    fs::path const shippedStaticlib = *root / "src" / "dss-config"
                                    / "object-formats"
                                    / "elf64-x86_64-linux-staticlib.format.json";
    ASSERT_TRUE(fs::is_regular_file(shippedStaticlib))
        << "the document this case CLONES is its subject; without it the test "
           "would construct no ambiguity at all: "
        << shippedStaticlib.generic_string();

    std::string const original = readFile(shippedStaticlib);
    ASSERT_FALSE(original.empty());

    // The clone differs ONLY in its declared `format.name`, so it agrees with
    // the same target on kind, container and machine — the exact shape that
    // makes two candidates indistinguishable to a first-match rule.
    constexpr std::string_view kQuotedName = "\"elf64-x86_64-linux-staticlib\"";
    auto const namePos = original.find(kQuotedName);
    ASSERT_NE(namePos, std::string::npos)
        << "the shipped staticlib document no longer spells its name as "
        << kQuotedName << " — the clone below would be a byte-identical copy "
           "and this case would assert nothing.";
    ASSERT_EQ(original.find(kQuotedName, namePos + 1), std::string::npos)
        << "the name string occurs more than once; a blind replace would edit "
           "something other than `format.name`.";
    std::string clone = original;
    clone.replace(namePos, kQuotedName.size(),
                  "\"elf64-x86_64-linux-staticlib-clone\"");

    auto const format = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(format.has_value());
    auto const target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());

    ScratchDir scratch{Location::Temp, "roc-ambiguity"};
    fs::path const formatsDir = scratch.path() / "object-formats";
    ASSERT_NO_FATAL_FAILURE(writeFile(
        formatsDir / "elf64-x86_64-linux-staticlib.format.json", original));

    // ── PHASE 1: exactly one candidate ⇒ SUCCESS ────────────────────────────
    // The control that makes phase 2 mean what it claims: it proves the
    // scratch directory, the copied document and the loader all work, so the
    // phase-2 refusal can only be caused by the second document.
    {
        auto const unique =
            resolveArchiveSiblingFormat(**format, **target, formatsDir,
            dss::runtime::kRuntimeCacheSiblingRequester);
        ASSERT_TRUE(unique.has_value())
            << "the one-candidate control refused: " << unique.error();
        EXPECT_EQ(*unique, "elf64-x86_64-linux-staticlib");
    }

    // ── PHASE 2: add the clone ⇒ AMBIGUOUS ──────────────────────────────────
    ASSERT_NO_FATAL_FAILURE(writeFile(
        formatsDir / "elf64-x86_64-linux-staticlib-clone.format.json", clone));

    auto const ambiguous =
        resolveArchiveSiblingFormat(**format, **target, formatsDir,
            dss::runtime::kRuntimeCacheSiblingRequester);
    ASSERT_FALSE(ambiguous.has_value())
        << "two agreeing archive formats resolved to one answer ('"
        << (ambiguous.has_value() ? *ambiguous : std::string{})
        << "') — a first-match rule decides this by filesystem order.";

    // BOTH names, each quoted: `elf64-x86_64-linux-staticlib` is a prefix of
    // `elf64-x86_64-linux-staticlib-clone`, so an unquoted substring test
    // would be satisfied by the clone's name alone.
    EXPECT_TRUE(contains(ambiguous.error(), "'elf64-x86_64-linux-staticlib'"))
        << ambiguous.error();
    EXPECT_TRUE(
        contains(ambiguous.error(), "'elf64-x86_64-linux-staticlib-clone'"))
        << ambiguous.error();
    EXPECT_TRUE(contains(ambiguous.error(), "AMBIGUOUS")) << ambiguous.error();
    EXPECT_TRUE(contains(ambiguous.error(), "'elf64-x86_64-linux-exec'"))
        << ambiguous.error();
}

TEST(RuntimeObjectCacheSibling, MissingObjectFormatDirectoryRefuses) {
    auto const format = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(format.has_value());
    auto const target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());

    ScratchDir scratch{Location::Temp, "roc-missing-dir"};
    fs::path const absent = scratch.path() / "no-such-object-formats";
    ASSERT_FALSE(fs::exists(absent));

    auto const sibling = resolveArchiveSiblingFormat(**format, **target, absent,
            dss::runtime::kRuntimeCacheSiblingRequester);
    ASSERT_FALSE(sibling.has_value());
    EXPECT_TRUE(contains(sibling.error(), absent.generic_string()))
        << sibling.error();
}

// ═══ THE KEY ════════════════════════════════════════════════════════════════

TEST(RuntimeObjectCacheKey, DocumentHasTheExactLineShapeAndOrder) {
    ScratchDir scratch{Location::Temp, "roc-key-shape"};
    ASSERT_NO_FATAL_FAILURE(layDownConfigRoot(scratch.path(), kDescriptorV1));

    auto const key = computeRuntimeObjectKey(makeRequest(scratch.path()));
    ASSERT_TRUE(key.has_value()) << key.error();

    std::vector<std::string> lines;
    {
        std::string current;
        for (char const c : key->document) {
            if (c == '\n') {
                lines.push_back(current);
                current.clear();
            } else {
                current.push_back(c);
            }
        }
        // Every line is LF-TERMINATED, so nothing may remain unterminated.
        EXPECT_TRUE(current.empty())
            << "the key document does not end with LF; trailing fragment: '"
            << current << "'";
    }

    ASSERT_EQ(lines.size(), 12u) << key->document;
    EXPECT_EQ(lines[0], "dss-runtime-object-cache-key/1");

    // ⓘ `compiler=` is the ONE term this test cannot pin to a literal: its
    // value is a build-time stamp macro compiled into the library, and the
    // test binary does not carry that macro. Pinned as far as it can be — the
    // key, the presence of a non-empty value, and (by the determinism case
    // below) its stability.
    ASSERT_TRUE(lines[1].starts_with("compiler="));
    EXPECT_GT(lines[1].size(), std::string_view{"compiler="}.size())
        << "the compiler stamp term is EMPTY — the key would not move when the "
           "compiler does.";

    EXPECT_EQ(lines[2], "target=arm64:elf64-aarch64-linux-exec");
    EXPECT_EQ(lines[3], "format=elf64-aarch64-linux-exec");
    EXPECT_EQ(lines[4], "sibling=elf64-aarch64-linux-staticlib");
    EXPECT_EQ(lines[5], "config=debug");
    EXPECT_EQ(lines[6], "unit=runtime/platform/src/unit.c");
    EXPECT_EQ(lines[7], "unit-sha256=" + dss::crypto::sha256Hex(kUnitText));
    EXPECT_EQ(lines[8], "descriptor=shippedLibs/probe.json");
    EXPECT_EQ(lines[9],
              "descriptor-sha256=" + dss::crypto::sha256Hex(kDescriptorV1));

    // Handed over as (language, format); emitted as (format, language). The
    // sort by (label, path) is what makes the key a function of the SET rather
    // than of the loaders' traversal order.
    EXPECT_EQ(lines[10],
              "doc=format:object-formats/elf64-aarch64-linux-exec.format.json:"
                  + std::string{kFormatDigest});
    EXPECT_EQ(lines[11], "doc=language:languages/c.lang.json:"
                             + std::string{kLanguageDigest});

    // The digest is the SHA-256 of exactly those bytes — no side channel.
    EXPECT_EQ(key->digest, dss::crypto::sha256Hex(key->document));
    EXPECT_EQ(key->digest.size(), 64u);
}

TEST(RuntimeObjectCacheKey, ThePathIndexIsTheFirstEightyBitsOfTheIdentity) {
    // ★★★ THE IDENTITY / INDEX RELATIONSHIP, ASSERTED THROUGH AN INDEPENDENT
    // ORACLE. `key->digest` is the identity; `key->pathDigest` is 16 characters
    // of lowercase base32 over the FIRST TEN BYTES of that same digest. The
    // oracle below re-derives it from the HEX STRING via a hex decoder and a
    // base32 encoder written HERE — so a bug in `dss::crypto::toBase32Lower`
    // cannot satisfy this case by also being present in the expectation.
    ScratchDir scratch{Location::Temp, "roc-key-index"};
    ASSERT_NO_FATAL_FAILURE(layDownConfigRoot(scratch.path(), kDescriptorV1));

    auto const key = computeRuntimeObjectKey(makeRequest(scratch.path()));
    ASSERT_TRUE(key.has_value()) << key.error();
    ASSERT_EQ(key->digest.size(), 64u);

    // Hex → the first ten bytes, by hand.
    std::array<std::uint8_t, 10> prefix{};
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        auto const nibble = [](char c) -> unsigned {
            return c <= '9' ? static_cast<unsigned>(c - '0')
                            : static_cast<unsigned>(c - 'a') + 10u;
        };
        prefix[i] = static_cast<std::uint8_t>(nibble(key->digest[i * 2]) * 16u
                                              + nibble(key->digest[i * 2 + 1]));
    }
    // Ten bytes → sixteen base32 characters, by hand, MSB first.
    std::string expected;
    for (std::size_t bit = 0; bit < 80u; bit += 5u) {
        unsigned value = 0;
        for (std::size_t k = 0; k < 5u; ++k) {
            std::size_t const at = bit + k;
            unsigned const    b =
                (prefix[at / 8u] >> (7u - at % 8u)) & 1u;
            value = (value << 1) | b;
        }
        expected.push_back(kBase32Alphabet[value]);
    }

    EXPECT_EQ(key->pathDigest, expected)
        << "the path index is not the first 80 bits of the identity digest — "
           "the filename and the key document would describe different things.";
    EXPECT_EQ(key->pathDigest.size(), 16u);
    EXPECT_TRUE(isPathIndex(key->pathDigest)) << key->pathDigest;

    // ⛔ AND IT IS NOT THE IDENTITY. A reader (or a future edit) that collapsed
    // the two would silently reintroduce the 64-hex filename this whole change
    // exists to remove — or, worse, make the 80-bit form the thing a hit is
    // verified on.
    EXPECT_NE(key->pathDigest, key->digest);
    EXPECT_EQ(key->digest, dss::crypto::sha256Hex(key->document))
        << "the IDENTITY is still the full SHA-256 of the key document.";
}

TEST(RuntimeObjectCacheKey, SameInputsProduceAByteIdenticalDocumentAndDigest) {
    ScratchDir scratch{Location::Temp, "roc-key-determ"};
    ASSERT_NO_FATAL_FAILURE(layDownConfigRoot(scratch.path(), kDescriptorV1));

    auto const first  = computeRuntimeObjectKey(makeRequest(scratch.path()));
    auto const second = computeRuntimeObjectKey(makeRequest(scratch.path()));
    ASSERT_TRUE(first.has_value()) << first.error();
    ASSERT_TRUE(second.has_value()) << second.error();

    EXPECT_EQ(first->document, second->document);
    EXPECT_EQ(first->digest, second->digest);
    EXPECT_EQ(first->shippedArtifactPath.generic_string(),
              second->shippedArtifactPath.generic_string());
    EXPECT_EQ(first->userArtifactPath.generic_string(),
              second->userArtifactPath.generic_string());
}

TEST(RuntimeObjectCacheKey, ArtifactPathIsTheKeyAndTheSlugIsSanitized) {
    ScratchDir scratch{Location::Temp, "roc-key-path"};
    ASSERT_NO_FATAL_FAILURE(layDownConfigRoot(scratch.path(), kDescriptorV1));
    ScopedUserCacheRoot userRoot{scratch.path() / "uc"};

    auto const key = computeRuntimeObjectKey(
        makeRequest(scratch.path(), "release", "arm64:elf64-aarch64-linux-exec"));
    ASSERT_TRUE(key.has_value()) << key.error();

    // `:` is outside [A-Za-z0-9._-] and becomes `_`; `-` and `.` survive.
    // ★ THE FILENAME CARRIES THE 16-CHARACTER INDEX, NEVER THE 64-HEX IDENTITY
    // — that is the whole path-budget change, and this is the assertion that
    // goes red if anyone puts the full digest back.
    ASSERT_TRUE(isPathIndex(key->pathDigest)) << key->pathDigest;
    fs::path const relative = fs::path{"release"}
                            / "arm64_elf64-aarch64-linux-exec"
                            / ("unit-" + key->pathDigest + ".a");
    EXPECT_EQ(key->relativePath.generic_string(), relative.generic_string());
    EXPECT_FALSE(contains(key->relativePath.generic_string(), key->digest))
        << "the 64-character identity digest is back in the PATH: "
        << key->relativePath.generic_string();

    // The key document is the artifact's own name with `.a` replaced — NOT
    // appended, which would make the longest name in the tree longer again.
    EXPECT_EQ(runtimeKeyDocumentPath(key->userArtifactPath).filename().string(),
              "unit-" + key->pathDigest + ".key");

    // ★ THE TWO-ROOTS PROPERTY, ASSERTED DIRECTLY: ONE relative path, anchored
    // twice. This is what makes a hit in either root the same artifact — if the
    // two absolute paths could disagree below the root, "shipped first" would
    // become a rule about which BYTES are correct instead of which copy to
    // read, and read-through would need a precedence rule that decided
    // something.
    fs::path const shippedRoot = scratch.path() / "runtime" / "platform" / "dist";
    EXPECT_EQ(key->shippedArtifactPath.generic_string(),
              (shippedRoot / relative).generic_string());
    EXPECT_EQ(key->userArtifactPath.generic_string(),
              (userRoot.base() / runtimeCacheBuildStampSegment() / relative)
                  .generic_string());
}

TEST(RuntimeObjectCacheKey, OneByteDescriptorChangeAtEqualLengthMovesTheKey) {
    ScratchDir scratch{Location::Temp, "roc-key-descr"};
    ASSERT_NO_FATAL_FAILURE(layDownConfigRoot(scratch.path(), kDescriptorV1));

    // The trap this case exists for: equal length, one differing byte.
    ASSERT_EQ(kDescriptorV1.size(), kDescriptorV2.size())
        << "the two descriptor revisions must be the SAME LENGTH, or a "
           "size-based key would pass this test.";
    ASSERT_NE(kDescriptorV1, kDescriptorV2);

    auto const before = computeRuntimeObjectKey(makeRequest(scratch.path()));
    ASSERT_TRUE(before.has_value()) << before.error();
    auto const sizeBefore = fs::file_size(scratch.path() / kDescriptorPath);

    ASSERT_NO_FATAL_FAILURE(
        writeFile(scratch.path() / kDescriptorPath, kDescriptorV2));
    auto const sizeAfter = fs::file_size(scratch.path() / kDescriptorPath);
    ASSERT_EQ(sizeBefore, sizeAfter)
        << "the mutation changed the file SIZE; this case no longer proves the "
           "key reads content rather than metadata.";

    auto const after = computeRuntimeObjectKey(makeRequest(scratch.path()));
    ASSERT_TRUE(after.has_value()) << after.error();

    EXPECT_NE(before->digest, after->digest)
        << "editing the descriptor did NOT move the key — a build would link "
           "an archive compiled against the OLD declarations.";
    EXPECT_NE(before->shippedArtifactPath.generic_string(),
              after->shippedArtifactPath.generic_string());
    EXPECT_NE(before->relativePath.generic_string(),
              after->relativePath.generic_string())
        << "the key moved but the relative path did not — the stale artifact "
           "is still reachable in BOTH roots.";
}

TEST(RuntimeObjectCacheKey, OutOfScopeDocumentChangeLeavesTheKeyUnmoved) {
    // THE CONTROL THAT PROVES THE KEY IS SCOPED. A key that hashed the whole
    // config root would pass every invalidation case in this file and fail
    // only here — and it would be a cache that never hits.
    ScratchDir scratch{Location::Temp, "roc-key-scope"};
    ASSERT_NO_FATAL_FAILURE(layDownConfigRoot(scratch.path(), kDescriptorV1));

    auto const before = computeRuntimeObjectKey(makeRequest(scratch.path()));
    ASSERT_TRUE(before.has_value()) << before.error();

    // A file inside the config root that is NOT the unit, NOT the descriptor,
    // and NOT named by `loadedDocuments`.
    ASSERT_TRUE(fs::is_regular_file(scratch.path() / kUnrelatedPath));
    ASSERT_NE(kUnrelatedV1, kUnrelatedV2);
    ASSERT_NO_FATAL_FAILURE(
        writeFile(scratch.path() / kUnrelatedPath, kUnrelatedV2));
    EXPECT_EQ(readFile(scratch.path() / kUnrelatedPath), kUnrelatedV2)
        << "the out-of-scope mutation did not land, so this control asserts "
           "nothing.";

    auto const after = computeRuntimeObjectKey(makeRequest(scratch.path()));
    ASSERT_TRUE(after.has_value()) << after.error();

    EXPECT_EQ(before->document, after->document);
    EXPECT_EQ(before->digest, after->digest)
        << "an out-of-scope document moved the key — the key is not scoped to "
           "its declared inputs.";
}

TEST(RuntimeObjectCacheKey, EmptyLoadedDocumentDigestRefusesNamingLabelAndPath) {
    ScratchDir scratch{Location::Temp, "roc-key-empty"};
    ASSERT_NO_FATAL_FAILURE(layDownConfigRoot(scratch.path(), kDescriptorV1));

    auto request = makeRequest(scratch.path());
    ASSERT_EQ(request.loadedDocuments.size(), 2u);
    ASSERT_EQ(request.loadedDocuments[0].label, "language");
    request.loadedDocuments[0].digest.clear();

    auto const key = computeRuntimeObjectKey(request);
    ASSERT_FALSE(key.has_value())
        << "an empty content digest was accepted; the key would cover an "
           "unknown input.";
    // Label AND path, both quoted — `language` is a prefix of `languages/...`,
    // so a bare substring test would be satisfied by the path alone.
    EXPECT_TRUE(contains(key.error(),
                         "'language' at 'languages/c.lang.json'"))
        << key.error();
    EXPECT_TRUE(contains(key.error(), "EMPTY")) << key.error();
}

TEST(RuntimeObjectCacheKey, MalformedLoadedDocumentDigestRefuses) {
    ScratchDir scratch{Location::Temp, "roc-key-malform"};
    ASSERT_NO_FATAL_FAILURE(layDownConfigRoot(scratch.path(), kDescriptorV1));

    // 63 hex chars — a truncation, which is exactly what a half-migrated
    // loader produces and what an "is it hex?" check would wave through.
    {
        auto request = makeRequest(scratch.path());
        request.loadedDocuments[0].digest = std::string(63u, 'a');
        auto const key = computeRuntimeObjectKey(request);
        ASSERT_FALSE(key.has_value());
        EXPECT_TRUE(contains(key.error(),
                             "'language' at 'languages/c.lang.json'"))
            << key.error();
        EXPECT_TRUE(contains(key.error(), "MALFORMED")) << key.error();
    }
    // UPPERCASE hex — a different digest spelling for the same bytes, so two
    // loaders disagreeing on case would produce two keys for one input.
    {
        auto request = makeRequest(scratch.path());
        request.loadedDocuments[0].digest = std::string(64u, 'A');
        auto const key = computeRuntimeObjectKey(request);
        ASSERT_FALSE(key.has_value());
        EXPECT_TRUE(contains(key.error(), "MALFORMED")) << key.error();
    }
}

TEST(RuntimeObjectCacheKey, MissingUnitOrDescriptorRefuses) {
    ScratchDir scratch{Location::Temp, "roc-key-missing"};
    ASSERT_NO_FATAL_FAILURE(layDownConfigRoot(scratch.path(), kDescriptorV1));

    ASSERT_TRUE(fs::remove(scratch.path() / kDescriptorPath));
    auto const key = computeRuntimeObjectKey(makeRequest(scratch.path()));
    ASSERT_FALSE(key.has_value())
        << "a missing descriptor produced a key anyway.";
    EXPECT_TRUE(contains(key.error(), "shippedLibs/probe.json")) << key.error();
}

// ═══ STORE / LOOKUP ═════════════════════════════════════════════════════════

namespace {

std::vector<std::uint8_t> const kBytesA{'!', '<', 'a', 'r', 'c', 'h', '>', '\n'};
std::vector<std::uint8_t> const kBytesB{'!', '<', 'a', 'r', 'c', 'h', '>', '\n',
                                        'X'};

} // namespace

TEST(RuntimeObjectCacheStore, LookupMissesBeforeStoreAndHitsAfter) {
    ScratchDir scratch{Location::Temp, "roc-store-hit"};
    ASSERT_NO_FATAL_FAILURE(layDownConfigRoot(scratch.path(), kDescriptorV1));
    ScopedUserCacheRoot userRoot{scratch.path() / "uc"};

    auto const key = computeRuntimeObjectKey(makeRequest(scratch.path()));
    ASSERT_TRUE(key.has_value()) << key.error();
    ASSERT_FALSE(key->userArtifactPath.empty())
        << "the pinned writable root did not resolve, so this case would be "
           "asserting against the refusal arm instead of the store arm.";

    EXPECT_FALSE(lookupExpectingNoRefusal(*key).has_value())
        << "a cold cache reported a hit.";

    auto const stored = storeRuntimeObject(*key, kBytesA);
    ASSERT_TRUE(stored.has_value()) << stored.error();
    EXPECT_EQ(stored->generic_string(), key->userArtifactPath.generic_string());

    auto const hit = lookupExpectingNoRefusal(*key);
    ASSERT_TRUE(hit.has_value()) << "store succeeded but lookup missed.";
    EXPECT_EQ(hit->generic_string(), key->userArtifactPath.generic_string());

    EXPECT_EQ(readFile(*hit),
              std::string(reinterpret_cast<char const*>(kBytesA.data()),
                          kBytesA.size()));

    // ★ THE KEY DOCUMENT LANDED TOO, BYTE FOR BYTE. Without it the hit above
    // would be an 80-bit guess; this is the file that makes it an identity.
    fs::path const document = runtimeKeyDocumentPath(key->userArtifactPath);
    ASSERT_TRUE(fs::is_regular_file(document)) << document.generic_string();
    EXPECT_EQ(readFile(document), key->document)
        << "the stored key document is not the key document.";

    // Exactly two entries: the artifact and its key document. The temp files
    // are dot-prefixed, so a leaked one would be counted here.
    EXPECT_EQ(countEntries(key->userArtifactPath.parent_path()), 2u)
        << "the artifact directory holds something besides the artifact and "
           "its key document — a temporary file was left behind.";
}

TEST(RuntimeObjectCacheStore, StoringTheSameKeyTwiceSucceedsAndLeavesOneFile) {
    ScratchDir scratch{Location::Temp, "roc-store-twice"};
    ASSERT_NO_FATAL_FAILURE(layDownConfigRoot(scratch.path(), kDescriptorV1));
    ScopedUserCacheRoot userRoot{scratch.path() / "uc"};

    auto const key = computeRuntimeObjectKey(makeRequest(scratch.path()));
    ASSERT_TRUE(key.has_value()) << key.error();

    auto const first = storeRuntimeObject(*key, kBytesA);
    ASSERT_TRUE(first.has_value()) << first.error();

    // "Already exists" is SUCCESS, not a conflict: same key ⇒ same bytes.
    auto const second = storeRuntimeObject(*key, kBytesA);
    ASSERT_TRUE(second.has_value())
        << "re-storing an identical key reported a conflict: " << second.error();
    EXPECT_EQ(second->generic_string(), key->userArtifactPath.generic_string());

    EXPECT_TRUE(fs::is_regular_file(key->userArtifactPath));
    EXPECT_EQ(countEntries(key->userArtifactPath.parent_path()), 2u)
        << "the second store left a temporary file behind.";
}

TEST(RuntimeObjectCacheStore, StoreUnderADifferentKeyLeavesTheFirstArtifact) {
    // KEY-AS-PATH ISOLATION. Two keys differing only in `config` land in
    // DIFFERENT directories, so neither store can reach the other's artifact.
    ScratchDir scratch{Location::Temp, "roc-store-isol"};
    ASSERT_NO_FATAL_FAILURE(layDownConfigRoot(scratch.path(), kDescriptorV1));
    ScopedUserCacheRoot userRoot{scratch.path() / "uc"};

    auto const debugKey =
        computeRuntimeObjectKey(makeRequest(scratch.path(), "debug"));
    auto const releaseKey =
        computeRuntimeObjectKey(makeRequest(scratch.path(), "release"));
    ASSERT_TRUE(debugKey.has_value()) << debugKey.error();
    ASSERT_TRUE(releaseKey.has_value()) << releaseKey.error();
    ASSERT_NE(debugKey->digest, releaseKey->digest);
    ASSERT_NE(debugKey->userArtifactPath.generic_string(),
              releaseKey->userArtifactPath.generic_string());

    ASSERT_TRUE(storeRuntimeObject(*debugKey, kBytesA).has_value());
    ASSERT_TRUE(storeRuntimeObject(*releaseKey, kBytesB).has_value());

    EXPECT_TRUE(fs::is_regular_file(debugKey->userArtifactPath))
        << "storing a second key destroyed the first artifact.";
    EXPECT_TRUE(fs::is_regular_file(releaseKey->userArtifactPath));
    EXPECT_TRUE(lookupExpectingNoRefusal(*debugKey).has_value());
    EXPECT_TRUE(lookupExpectingNoRefusal(*releaseKey).has_value());

    // Each holds ITS OWN bytes — the paths did not merely both exist, they
    // carry different content.
    EXPECT_EQ(readFile(debugKey->userArtifactPath).size(), kBytesA.size());
    EXPECT_EQ(readFile(releaseKey->userArtifactPath).size(), kBytesB.size());
}

TEST(RuntimeObjectCacheStore, StoringASupersedingKeyPrunesTheStaleSibling) {
    // Same directory, same unit stem, DIFFERENT digest — the shape pruning is
    // for. The superseded artifact is already unreachable (nothing computes
    // its key); pruning is what stops the directory growing forever.
    ScratchDir scratch{Location::Temp, "roc-store-prune"};
    ASSERT_NO_FATAL_FAILURE(layDownConfigRoot(scratch.path(), kDescriptorV1));
    ScopedUserCacheRoot userRoot{scratch.path() / "uc"};

    auto const staleKey = computeRuntimeObjectKey(makeRequest(scratch.path()));
    ASSERT_TRUE(staleKey.has_value()) << staleKey.error();
    ASSERT_TRUE(storeRuntimeObject(*staleKey, kBytesA).has_value());
    ASSERT_TRUE(fs::is_regular_file(staleKey->userArtifactPath));

    ASSERT_NO_FATAL_FAILURE(
        writeFile(scratch.path() / kDescriptorPath, kDescriptorV2));
    auto const freshKey = computeRuntimeObjectKey(makeRequest(scratch.path()));
    ASSERT_TRUE(freshKey.has_value()) << freshKey.error();
    ASSERT_NE(staleKey->digest, freshKey->digest);
    ASSERT_EQ(staleKey->userArtifactPath.parent_path().generic_string(),
              freshKey->userArtifactPath.parent_path().generic_string());

    // BOTH files of the stale entry exist before the superseding store, or
    // "pruned" below would be satisfied by a file that was never written.
    fs::path const staleDocument =
        runtimeKeyDocumentPath(staleKey->userArtifactPath);
    ASSERT_TRUE(fs::is_regular_file(staleDocument));

    ASSERT_TRUE(storeRuntimeObject(*freshKey, kBytesB).has_value());

    EXPECT_TRUE(fs::is_regular_file(freshKey->userArtifactPath));
    EXPECT_FALSE(fs::exists(staleKey->userArtifactPath))
        << "the superseded artifact was not pruned.";
    // ★ AND ITS KEY DOCUMENT WENT WITH IT. A prune that took only the `.a`
    // would leave the directory growing a `.key` per revision forever, and the
    // orphan would then be the only trace of a key nobody can compute.
    EXPECT_FALSE(fs::exists(staleDocument))
        << "the superseded entry's key document was left behind.";
    EXPECT_EQ(countEntries(freshKey->userArtifactPath.parent_path()), 2u);
    EXPECT_FALSE(lookupExpectingNoRefusal(*staleKey).has_value());
}

TEST(RuntimeObjectCacheStore, PruningMatchesExactlyAndLeavesEveryNearMissAlone) {
    // ⛔ THE MATCHER DELETES FILES, so it is pinned on the near misses rather
    // than on the happy case. Every neighbour below differs from a prunable
    // name in EXACTLY ONE way, and each one is a shape a loosened matcher
    // actually produces:
    //
    //   * a DIFFERENT unit whose stem starts with ours — the prefix-glob bug
    //     (`unit-*.a` deletes `unit-extra-…`, which is not ours to delete);
    //   * an index of the right length in the WRONG ALPHABET (`0`, `1` and `8`
    //     are not in RFC 4648's base32) — what an `[a-z0-9]` check waves
    //     through, and what a stray hex-named leftover looks like;
    //   * an UPPERCASE index — the shape a case-insensitive filesystem can hand
    //     back, and the one a `tolower`-happy matcher would delete;
    //   * a 15-character index — one short, i.e. a truncation;
    //   * the right shape with an unrelated SUFFIX (`.o`).
    ScratchDir scratch{Location::Temp, "roc-store-stems"};
    ASSERT_NO_FATAL_FAILURE(layDownConfigRoot(scratch.path(), kDescriptorV1));
    ScopedUserCacheRoot userRoot{scratch.path() / "uc"};

    auto const key = computeRuntimeObjectKey(makeRequest(scratch.path()));
    ASSERT_TRUE(key.has_value()) << key.error();

    fs::path const directory = key->userArtifactPath.parent_path();
    std::string const other(16u, 'b');   // a WELL-FORMED index that is not ours
    ASSERT_TRUE(isPathIndex(other));
    ASSERT_NE(other, key->pathDigest);

    std::vector<fs::path> const neighbours{
        directory / ("unit-extra-" + other + ".a"),
        directory / ("unit-extra-" + other + ".key"),
        directory / ("unit-" + std::string(16u, '0') + ".a"),
        // ⚠ `C` AND NOT `B`, AND THE REASON IS THIS CHANGE'S WHOLE POINT: on a
        // case-insensitive filesystem `unit-BBBB….a` and the `unit-bbbb….a`
        // control below are ONE FILE, so the case would destroy its own
        // fixture on Windows and macOS while passing on Linux.
        directory / ("unit-" + std::string(16u, 'C') + ".a"),
        directory / ("unit-" + std::string(15u, 'b') + ".a"),
        directory / ("unit-" + other + ".o"),
    };
    for (fs::path const& neighbour : neighbours) {
        ASSERT_NO_FATAL_FAILURE(writeFile(neighbour, "not mine\n"));
        ASSERT_TRUE(fs::is_regular_file(neighbour))
            << neighbour.generic_string();
    }

    // The CONTROL that keeps the case from passing because pruning did nothing
    // at all: a WELL-FORMED superseded sibling of OUR stem, which must go.
    fs::path const prunable = directory / ("unit-" + other + ".a");
    ASSERT_NO_FATAL_FAILURE(writeFile(prunable, "superseded\n"));

    ASSERT_TRUE(storeRuntimeObject(*key, kBytesA).has_value());

    EXPECT_FALSE(fs::exists(prunable))
        << "nothing was pruned at all, so the near misses below survived for "
           "the wrong reason: "
        << prunable.generic_string();
    for (fs::path const& neighbour : neighbours) {
        EXPECT_TRUE(fs::is_regular_file(neighbour))
            << "pruning deleted a file that is NOT this unit's artifact: "
            << neighbour.generic_string();
    }
    EXPECT_TRUE(fs::is_regular_file(key->userArtifactPath));
    EXPECT_TRUE(
        fs::is_regular_file(runtimeKeyDocumentPath(key->userArtifactPath)));
    // 6 survivors + our artifact + our key document.
    EXPECT_EQ(countEntries(directory), neighbours.size() + 2u);
}

// ═══ THE TWO ROOTS ══════════════════════════════════════════════════════════

TEST(RuntimeObjectCacheRoots, OverrideWinsOverEveryPlatformDefault) {
    ScratchDir     scratch{Location::Temp, "roc-roots-over"};
    fs::path const configRoot    = scratch.path() / "config";
    fs::path const overrideRoot  = scratch.path() / "override";
    fs::path const localAppData  = scratch.path() / "localappdata";
    fs::path const xdgCacheHome  = scratch.path() / "xdg";
    fs::path const home          = scratch.path() / "home";

    // ★ ALL FOUR ARE SET, and that is what makes this a precedence case rather
    // than an availability case. With only the override set, a resolution that
    // ignored precedence entirely — "take whichever variable happens to be
    // set" — would pass. Every competitor must be present and lose.
    ScopedEnv ov{kCacheDirVar, overrideRoot.string()};
    ScopedEnv lad{kLocalAppDataVar, localAppData.string()};
    ScopedEnv xdg{kXdgCacheHomeVar, xdgCacheHome.string()};
    ScopedEnv hm{kHomeVar, home.string()};

    std::string const segment = runtimeCacheBuildStampSegment();
    ASSERT_FALSE(segment.empty())
        << "the running compiler's build-stamp segment is EMPTY, so every "
           "assertion below about per-version scoping would hold vacuously.";

    auto const roots = resolveRuntimeCacheRoots(configRoot);

    EXPECT_EQ(roots.shipped.generic_string(),
              (configRoot / "runtime" / "platform" / "dist").generic_string());

    // The override is taken VERBATIM — no vendor tail under it — and still
    // carries the compiler-identity segment, so two installed versions sharing
    // one CI cache directory cannot share one tree.
    EXPECT_EQ(roots.perUser.generic_string(),
              (overrideRoot / segment).generic_string());

    // The identity segment is the LAST component of the root, which is what
    // makes per-version cleanup a single `rm -rf <root>/<segment>` — a segment
    // buried mid-path would not own a deletable subtree.
    EXPECT_EQ(roots.perUser.filename().string(), segment);

    // The losers are NOT chosen…
    EXPECT_FALSE(contains(roots.perUser.generic_string(),
                          localAppData.generic_string()))
        << roots.perUser.generic_string();

    // …but they ARE all reported. The refusal's contract is "every root it
    // tried", so the trail must not stop at the winner.
    EXPECT_TRUE(contains(roots.trail, localAppData.generic_string()))
        << roots.trail;
    EXPECT_TRUE(contains(roots.trail, xdgCacheHome.generic_string()))
        << roots.trail;
    EXPECT_TRUE(contains(roots.trail, home.generic_string())) << roots.trail;
    EXPECT_TRUE(contains(
        roots.trail,
        (configRoot / "runtime" / "platform" / "dist").generic_string()))
        << roots.trail;

    // …and the trail says WHICH one was taken, on the override's own line. A
    // bare "[SELECTED]" somewhere in the string would be satisfied by a trail
    // that marked the wrong candidate.
    EXPECT_TRUE(contains(roots.trail,
                         "$DSS_RUNTIME_CACHE_DIR -> '"
                             + (overrideRoot / segment).generic_string()
                             + "' [SELECTED]"))
        << roots.trail;
}

TEST(RuntimeObjectCacheRoots, PlatformDefaultsFollowTheDocumentedChain) {
    ScratchDir     scratch{Location::Temp, "roc-roots-chain"};
    fs::path const configRoot   = scratch.path() / "config";
    fs::path const localAppData = scratch.path() / "localappdata";
    fs::path const xdgCacheHome = scratch.path() / "xdg";
    fs::path const home         = scratch.path() / "home";

    // Cleared for the WHOLE case: this is the chain WITHOUT an override, and an
    // ambient one (a developer's shell, a CI runner that exports it) would make
    // every phase below silently assert the override instead.
    ScopedEnv noOverride{kCacheDirVar};

    std::string const segment = runtimeCacheBuildStampSegment();
    ASSERT_FALSE(segment.empty());

    fs::path const vendorTail = fs::path{"dsscp"} / "runtime-cache";

    // ── PHASE 1: LOCALAPPDATA present ⇒ it wins over XDG and HOME ───────────
    {
        ScopedEnv lad{kLocalAppDataVar, localAppData.string()};
        ScopedEnv xdg{kXdgCacheHomeVar, xdgCacheHome.string()};
        ScopedEnv hm{kHomeVar, home.string()};
        auto const      roots = resolveRuntimeCacheRoots(configRoot);
        EXPECT_EQ(roots.perUser.generic_string(),
                  (localAppData / vendorTail / segment).generic_string());
    }

    // ── PHASE 2: LOCALAPPDATA absent ⇒ XDG_CACHE_HOME ──────────────────────
    // ⓘ Reachable from a Windows host too — the resolution has no `#ifdef`, so
    // this arm is exercised on every host the suite runs on rather than only on
    // the ones whose platform "owns" it.
    {
        ScopedEnv lad{kLocalAppDataVar};
        ScopedEnv xdg{kXdgCacheHomeVar, xdgCacheHome.string()};
        ScopedEnv hm{kHomeVar, home.string()};
        auto const      roots = resolveRuntimeCacheRoots(configRoot);
        EXPECT_EQ(roots.perUser.generic_string(),
                  (xdgCacheHome / vendorTail / segment).generic_string());
    }

    // ── PHASE 3: both absent ⇒ $HOME/.cache/… (the XDG spec's own default) ──
    {
        ScopedEnv lad{kLocalAppDataVar};
        ScopedEnv xdg{kXdgCacheHomeVar};
        ScopedEnv hm{kHomeVar, home.string()};
        auto const      roots = resolveRuntimeCacheRoots(configRoot);
        EXPECT_EQ(roots.perUser.generic_string(),
                  (home / ".cache" / vendorTail / segment).generic_string());
    }

    // ── PHASE 4: ALL absent ⇒ no writable root, and the trail says so ───────
    {
        ScopedEnv lad{kLocalAppDataVar};
        ScopedEnv xdg{kXdgCacheHomeVar};
        ScopedEnv hm{kHomeVar};
        auto const      roots = resolveRuntimeCacheRoots(configRoot);
        EXPECT_TRUE(roots.perUser.empty()) << roots.perUser.generic_string();
        // The SHIPPED root survives — a host with nowhere to write can still
        // read a packaged `dist/`, which is the whole reason resolution failure
        // is not an error here.
        EXPECT_EQ(roots.shipped.generic_string(),
                  (configRoot / "runtime" / "platform" / "dist").generic_string());
        for (char const* spelling : {"$DSS_RUNTIME_CACHE_DIR", "%LOCALAPPDATA%",
                                     "$XDG_CACHE_HOME", "$HOME"}) {
            EXPECT_TRUE(contains(roots.trail,
                                 std::string{spelling} + ": unset or empty"))
                << spelling << " missing from: " << roots.trail;
        }
    }
}

TEST(RuntimeObjectCacheRoots, AnEmptyOverrideFallsThroughRatherThanRootingAtCwd) {
    // ⚠ THE TWO HOSTS RUN TWO DIFFERENT EXPERIMENTS HERE, AND BOTH ARE REAL —
    // this is not a silent skip. `ScopedEnv` sets via `_putenv_s` on Windows,
    // where `_putenv_s(name, "")` REMOVES the variable (documented), so Windows
    // exercises UNSET while POSIX exercises SET-BUT-EMPTY. The REQUIRED OUTCOME
    // is identical on both — fall through to the platform default — so the
    // assertion below is meaningful either way, and neither host reaches it by
    // skipping.
    //
    // What it forbids: taking `DSS_RUNTIME_CACHE_DIR=` literally. `fs::path{""}`
    // is a RELATIVE path, so the cache would root at the process's current
    // directory and land in whatever tree the build happened to start in.
    ScratchDir     scratch{Location::Temp, "roc-roots-empty"};
    fs::path const configRoot   = scratch.path() / "config";
    fs::path const localAppData = scratch.path() / "localappdata";

    ScopedEnv emptyOverride{kCacheDirVar, ""};
    ScopedEnv lad{kLocalAppDataVar, localAppData.string()};
    ScopedEnv xdg{kXdgCacheHomeVar};
    ScopedEnv hm{kHomeVar};

    auto const roots = resolveRuntimeCacheRoots(configRoot);
    ASSERT_FALSE(roots.perUser.empty()) << roots.trail;
    EXPECT_EQ(roots.perUser.generic_string(),
              (localAppData / "dsscp" / "runtime-cache"
               / runtimeCacheBuildStampSegment())
                  .generic_string());
    EXPECT_TRUE(roots.perUser.is_absolute())
        << "the per-user root is RELATIVE — the cache would follow the build's "
           "current directory: "
        << roots.perUser.generic_string();
}

TEST(RuntimeObjectCacheRoots, DifferentBuildStampsRenderDifferentRootSegments) {
    // Two stamps of the shape `cmake/DssBuildStamp.cmake` actually emits.
    constexpr std::string_view kStampA = "0.0.2+g4095c13b2f1a";
    constexpr std::string_view kStampB = "0.0.2+g8385d46e9c77";
    ASSERT_NE(kStampA, kStampB);

    std::string const segmentA = buildStampPathSegment(kStampA);
    std::string const segmentB = buildStampPathSegment(kStampB);

    // THE CLAIM: two compiler builds get two per-user root PATHS. Without it a
    // user could not delete one version's cache without deleting the other's,
    // and two installed versions would share one tree.
    EXPECT_NE(segmentA, segmentB)
        << "two different build stamps rendered the SAME path segment ('"
        << segmentA << "') — the per-user root is not scoped per compiler "
        << "version.";
    EXPECT_FALSE(segmentA.empty());
    EXPECT_FALSE(segmentB.empty());

    // Each is ONE path component. A segment that kept a separator would silently
    // create a nested tree, and `rm -rf <root>/<segment>` would then delete the
    // wrong depth.
    for (std::string const& segment : {segmentA, segmentB}) {
        EXPECT_EQ(segment.find('/'), std::string::npos) << segment;
        EXPECT_EQ(segment.find('\\'), std::string::npos) << segment;
        EXPECT_EQ(fs::path{segment}.filename().string(), segment) << segment;
    }

    // Path-hostile input is SANITIZED, not passed through. A dirty-tree stamp
    // carries `+`, and the point of the rendering is that no stamp shape can
    // escape its component.
    EXPECT_EQ(buildStampPathSegment("0.0.2+g1/2\\3:4 5"), "0.0.2_g1_2_3_4_5");

    // And the RUNNING compiler's segment is subject to the same rules — this is
    // the value that actually lands in the path.
    std::string const live = runtimeCacheBuildStampSegment();
    EXPECT_FALSE(live.empty())
        << "the running compiler renders an EMPTY segment, which would collapse "
           "the per-user root and un-scope the cache across versions.";
    EXPECT_EQ(fs::path{live}.filename().string(), live) << live;
}

// ═══ READ-THROUGH ═══════════════════════════════════════════════════════════

TEST(RuntimeObjectCacheReadThrough, ShippedArtifactIsFoundThoughTheCacheNeverWroteIt) {
    // The packaged case: a vendor unpacked `dist/` into a read-only install and
    // NOTHING in this process ever wrote the artifact. If the shipped root is
    // not consulted, a build that should hit recompiles instead — silently, and
    // on every invocation.
    ScratchDir scratch{Location::Temp, "roc-rt-shipped"};
    ASSERT_NO_FATAL_FAILURE(layDownConfigRoot(scratch.path(), kDescriptorV1));
    ScopedUserCacheRoot userRoot{scratch.path() / "uc"};

    auto const key = computeRuntimeObjectKey(makeRequest(scratch.path()));
    ASSERT_TRUE(key.has_value()) << key.error();
    ASSERT_FALSE(key->userArtifactPath.empty()) << key->rootTrail;

    ASSERT_FALSE(lookupExpectingNoRefusal(*key).has_value())
        << "a cold cache reported a hit, so the hit below would prove nothing.";

    // The vendor unpacks BOTH files — a packaged entry carries its key document
    // exactly as a locally-stored one does.
    ASSERT_NO_FATAL_FAILURE(layDownEntry(key->shippedArtifactPath,
                                         "shipped-by-the-package-manager\n",
                                         key->document));
    ASSERT_TRUE(fs::is_regular_file(key->shippedArtifactPath));

    auto const hit = lookupExpectingNoRefusal(*key);
    ASSERT_TRUE(hit.has_value())
        << "the shipped root was never consulted — a packaged `dist/` is "
           "unreachable and every install recompiles from scratch.";
    EXPECT_EQ(hit->generic_string(), key->shippedArtifactPath.generic_string());

    // Nothing was copied or promoted into the per-user root behind the caller's
    // back: read-through READS the shipped root, it does not mirror it.
    EXPECT_FALSE(fs::exists(key->userArtifactPath));
}

TEST(RuntimeObjectCacheReadThrough, StoreLandsOnlyInThePerUserRootAndLeavesTheShippedRootUntouched) {
    ScratchDir scratch{Location::Temp, "roc-rt-store"};
    ASSERT_NO_FATAL_FAILURE(layDownConfigRoot(scratch.path(), kDescriptorV1));
    ScopedUserCacheRoot userRoot{scratch.path() / "uc"};

    auto const key = computeRuntimeObjectKey(makeRequest(scratch.path()));
    ASSERT_TRUE(key.has_value()) << key.error();
    ASSERT_FALSE(key->userArtifactPath.empty()) << key->rootTrail;

    // ★ A DECOY, so "the count did not move" is a claim about a NON-EMPTY tree.
    // 0-before and 0-after would also pass, while proving nothing about a store
    // that appends — and it is precisely an appending store this asserts against.
    // It is placed in the artifact's OWN shipped directory, the one directory a
    // shipped-root write or prune would touch.
    fs::path const shippedRoot = scratch.path() / "runtime" / "platform" / "dist";
    fs::path const decoy = key->shippedArtifactPath.parent_path() / "decoy.txt";
    ASSERT_NO_FATAL_FAILURE(writeFile(decoy, "not the cache's to touch\n"));
    ASSERT_TRUE(fs::is_directory(shippedRoot));
    std::size_t const shippedBefore = countFilesRecursive(shippedRoot);
    ASSERT_EQ(shippedBefore, 1u);
    fs::path const userDocument =
        runtimeKeyDocumentPath(key->userArtifactPath);

    auto const stored = storeRuntimeObject(*key, kBytesA);
    ASSERT_TRUE(stored.has_value()) << stored.error();

    // It landed in the PER-USER root — BOTH files…
    EXPECT_EQ(stored->generic_string(), key->userArtifactPath.generic_string());
    EXPECT_TRUE(fs::is_regular_file(key->userArtifactPath));
    EXPECT_EQ(readFile(key->userArtifactPath).size(), kBytesA.size());
    EXPECT_TRUE(fs::is_regular_file(userDocument));

    // …and the SHIPPED root is byte-for-byte as it was. Asserted as a COUNT and
    // not only as "the artifact is absent": a count catches a temp file, a
    // partial write, or a directory the store created on its way to failing.
    // ⚠ The count is what catches the KEY DOCUMENT too — the store now writes
    // TWO files, and "the artifact is absent" would say nothing about the
    // second one landing in the wrong root.
    EXPECT_FALSE(fs::exists(key->shippedArtifactPath))
        << "the store wrote into the READ-ONLY shipped root.";
    EXPECT_FALSE(fs::exists(runtimeKeyDocumentPath(key->shippedArtifactPath)))
        << "the store wrote a KEY DOCUMENT into the READ-ONLY shipped root.";
    EXPECT_EQ(countFilesRecursive(shippedRoot), shippedBefore)
        << "the shipped root's file count moved — it is not being treated as "
           "read-only.";
    EXPECT_TRUE(fs::is_regular_file(decoy))
        << "pruning reached into the shipped root and deleted a file the cache "
           "does not own.";

    auto const hit = lookupExpectingNoRefusal(*key);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->generic_string(), key->userArtifactPath.generic_string());
}

TEST(RuntimeObjectCacheReadThrough, ShippedRootWinsWhenBothRootsHoldTheSameKey) {
    // ⚠ THE FIXTURE IS DELIBERATELY IMPOSSIBLE IN PRODUCTION, and that is what
    // makes it a test rather than a tautology. Same key ⇒ same inputs ⇒ same
    // bytes, so two REAL copies are byte-identical and "which one came back" is
    // unobservable from content alone. Different bytes are written here purely
    // so the answer is witnessed TWICE — once by PATH and once by CONTENT — and
    // a lookup that silently preferred the per-user copy fails both.
    //
    // ⓘ This is exactly why two roots are safe: the case being constructed here
    // cannot arise, so "shipped first" never has to adjudicate a disagreement.
    ScratchDir scratch{Location::Temp, "roc-rt-order"};
    ASSERT_NO_FATAL_FAILURE(layDownConfigRoot(scratch.path(), kDescriptorV1));
    ScopedUserCacheRoot userRoot{scratch.path() / "uc"};

    auto const key = computeRuntimeObjectKey(makeRequest(scratch.path()));
    ASSERT_TRUE(key.has_value()) << key.error();

    ASSERT_TRUE(storeRuntimeObject(*key, kBytesA).has_value());
    // The SAME key document in both roots — the artifacts differ only because
    // the fixture is deliberately impossible; the entries are still THIS key's,
    // so neither root may refuse.
    ASSERT_NO_FATAL_FAILURE(
        layDownEntry(key->shippedArtifactPath, "SHIPPED\n", key->document));

    // BOTH subjects must exist, or "shipped wins" would be satisfied by a root
    // that was simply the only one populated.
    ASSERT_TRUE(fs::is_regular_file(key->shippedArtifactPath));
    ASSERT_TRUE(fs::is_regular_file(key->userArtifactPath));
    ASSERT_NE(readFile(key->shippedArtifactPath),
              readFile(key->userArtifactPath));

    auto const hit = lookupExpectingNoRefusal(*key);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->generic_string(), key->shippedArtifactPath.generic_string())
        << "the per-user root shadowed the shipped one.";
    EXPECT_EQ(readFile(*hit), "SHIPPED\n");
}

// ═══ THE UNWRITABLE-MISS REFUSALS ═══════════════════════════════════════════

TEST(RuntimeObjectCacheStore, UnwritablePerUserRootRefusesAndNamesRootsAndOverride) {
    // ⓘ WHY THE ROOT IS MADE UNWRITABLE BY PARENTING IT ON A REGULAR FILE, and
    // not by `chmod`: making a directory unwritable is not portable. POSIX mode
    // bits are ignored for root (CI containers routinely run as root), and
    // Windows needs an ACL edit a test process may not be entitled to make —
    // either way the "unwritable" root would sometimes be writable and the case
    // would pass by never failing. A path whose PARENT is a REGULAR FILE cannot
    // be created into on ANY host, because a file is not a directory anywhere.
    ScratchDir scratch{Location::Temp, "roc-unwritable"};
    ASSERT_NO_FATAL_FAILURE(layDownConfigRoot(scratch.path(), kDescriptorV1));

    fs::path const blocker = scratch.path() / "blocker";
    ASSERT_NO_FATAL_FAILURE(
        writeFile(blocker, "a regular file, not a directory\n"));
    ASSERT_TRUE(fs::is_regular_file(blocker))
        << "the blocker is the SUBJECT of this case; without it the override "
           "would point somewhere perfectly writable.";

    ScopedUserCacheRoot userRoot{blocker / "uc"};

    auto const key = computeRuntimeObjectKey(makeRequest(scratch.path()));
    ASSERT_TRUE(key.has_value()) << key.error();
    ASSERT_FALSE(key->userArtifactPath.empty())
        << "the override did not resolve, so this case would be exercising the "
           "NO-ROOT arm instead of the UNWRITABLE one.";

    auto const stored = storeRuntimeObject(*key, kBytesA);
    ASSERT_FALSE(stored.has_value())
        << "an unwritable per-user root was accepted — the miss was compiled "
           "and silently discarded, which is the one outcome this refusal "
           "exists to forbid.";

    // (1) EVERY ROOT IT TRIED — the shipped root it read and the per-user root
    // it could not write.
    EXPECT_TRUE(contains(
        stored.error(),
        (scratch.path() / "runtime" / "platform" / "dist").generic_string()))
        << stored.error();
    EXPECT_TRUE(contains(
        stored.error(),
        (blocker / "uc" / runtimeCacheBuildStampSegment()).generic_string()))
        << stored.error();

    // (2) THE REASON THE WRITE FAILED — named, not merely implied.
    EXPECT_TRUE(
        contains(stored.error(), "could not create the artifact directory"))
        << stored.error();

    // (3) THE REMEDY, spelled as the variable a user can actually set.
    EXPECT_TRUE(contains(stored.error(), "DSS_RUNTIME_CACHE_DIR"))
        << stored.error();
    EXPECT_TRUE(contains(stored.error(), "REFUSAL")) << stored.error();

    // Nothing was created, replaced, or half-written on the way out.
    EXPECT_TRUE(fs::is_regular_file(blocker))
        << "the blocker was replaced — the store wrote through a regular file.";
    EXPECT_FALSE(lookupExpectingNoRefusal(*key).has_value());
}

TEST(RuntimeObjectCacheStore, NoWritableRootAtAllRefusesAndNamesEveryCandidate) {
    ScratchDir scratch{Location::Temp, "roc-no-root"};
    ASSERT_NO_FATAL_FAILURE(layDownConfigRoot(scratch.path(), kDescriptorV1));

    // The state an exotic host actually reaches: a service account or a
    // container with no HOME and no XDG variables.
    ScopedEnv noOverride{kCacheDirVar};
    ScopedEnv noLocalAppData{kLocalAppDataVar};
    ScopedEnv noXdg{kXdgCacheHomeVar};
    ScopedEnv noHome{kHomeVar};

    auto const key = computeRuntimeObjectKey(makeRequest(scratch.path()));

    // ★ THE KEY STILL COMPUTES, and that is the point of resolving the roots
    // late rather than refusing at key time: a build that HITS in a packaged
    // shipped root must still work on a host with nowhere to write.
    ASSERT_TRUE(key.has_value())
        << "computing a key REFUSED because no writable root exists — a "
           "read-only install with a packaged `dist/` could no longer build at "
           "all: "
        << key.error();
    EXPECT_TRUE(key->userArtifactPath.empty());
    EXPECT_FALSE(key->shippedArtifactPath.empty());

    // …and the shipped root is still READ on such a host.
    ASSERT_NO_FATAL_FAILURE(
        layDownEntry(key->shippedArtifactPath, "packaged\n", key->document));
    auto const hit = lookupExpectingNoRefusal(*key);
    ASSERT_TRUE(hit.has_value())
        << "a host with no writable root also lost the READ-ONLY shipped root.";
    EXPECT_EQ(hit->generic_string(), key->shippedArtifactPath.generic_string());

    // But a MISS that must be written REFUSES rather than degrading.
    ASSERT_TRUE(fs::remove(key->shippedArtifactPath));
    ASSERT_TRUE(fs::remove(runtimeKeyDocumentPath(key->shippedArtifactPath)));
    ASSERT_FALSE(lookupExpectingNoRefusal(*key).has_value());

    auto const stored = storeRuntimeObject(*key, kBytesA);
    ASSERT_FALSE(stored.has_value())
        << "a store with nowhere to write reported SUCCESS — the artifact was "
           "discarded and every later build would recompile it in silence.";

    // Every candidate by its own spelling, so a user can see which variable to
    // set rather than being told only that something was missing.
    for (char const* spelling : {"$DSS_RUNTIME_CACHE_DIR", "%LOCALAPPDATA%",
                                 "$XDG_CACHE_HOME", "$HOME"}) {
        EXPECT_TRUE(contains(stored.error(), spelling))
            << spelling << " missing from: " << stored.error();
    }
    EXPECT_TRUE(contains(
        stored.error(),
        (scratch.path() / "runtime" / "platform" / "dist").generic_string()))
        << stored.error();
    EXPECT_TRUE(contains(stored.error(), "NO per-user cache root"))
        << stored.error();
    EXPECT_TRUE(contains(stored.error(), "DSS_RUNTIME_CACHE_DIR"))
        << stored.error();
    EXPECT_TRUE(contains(stored.error(), "REFUSAL")) << stored.error();
}

// ═══ THE KEY DOCUMENT — WHAT MAKES THE 16-CHARACTER INDEX AN INDEX ══════════
//
// ★★★ THIS GROUP IS THE OTHER HALF OF THE PATH-BUDGET CHANGE, and without it
// the change is a WEAKENING rather than a fix: truncating the filename to 80
// bits turns "a stale artifact is UNREACHABLE" into "unreachable with
// probability 1−ε", and "unreachable" was the entire argument for compiling the
// runtime from source instead of shipping prebuilt objects. The key document
// beside the artifact is what turns the short name back into an index.
//
// ⚠ EVERY CASE HERE PLANTS ITS COLLISION BY HAND, and it has to: a real 80-bit
// collision needs ~2^40 entries in one directory. That is not a weakness of the
// test — the mechanism must be exercised at the SHAPE it defends against, and
// waiting for the shape to occur naturally is waiting forever. What the cases
// pin is that the cache compares the FULL key document and refuses on a
// difference, which is exactly what it would do for a real collision.

TEST(RuntimeObjectCacheKeyDocument, AMissingKeyDocumentBesideAnArtifactRefuses) {
    // ⛔ REFUSE, NOT MISS, and the docblock on `lookupRuntimeObject` carries the
    // argument: if an absent key document were a miss, deleting the sidecar
    // would silently restore the unverified 80-bit behaviour — the check would
    // be optional and therefore not a check.
    ScratchDir scratch{Location::Temp, "roc-kd-missing"};
    ASSERT_NO_FATAL_FAILURE(layDownConfigRoot(scratch.path(), kDescriptorV1));
    ScopedUserCacheRoot userRoot{scratch.path() / "uc"};

    auto const key = computeRuntimeObjectKey(makeRequest(scratch.path()));
    ASSERT_TRUE(key.has_value()) << key.error();
    ASSERT_TRUE(storeRuntimeObject(*key, kBytesA).has_value());

    fs::path const document = runtimeKeyDocumentPath(key->userArtifactPath);
    ASSERT_TRUE(fs::is_regular_file(document));
    // The CONTROL: it hits while the key document is there, so the refusal
    // below can only be caused by removing it.
    ASSERT_TRUE(lookupExpectingNoRefusal(*key).has_value());

    ASSERT_TRUE(fs::remove(document));
    ASSERT_TRUE(fs::is_regular_file(key->userArtifactPath))
        << "the artifact went with the key document, so this case would be "
           "asserting on an ordinary miss.";

    auto const found = lookupRuntimeObject(*key);
    ASSERT_FALSE(found.has_value())
        << "an artifact with NO key document was served on the strength of a "
           "16-character index alone.";
    EXPECT_TRUE(contains(found.error(), "CANNOT BE VERIFIED")) << found.error();
    EXPECT_TRUE(contains(found.error(), "key document is MISSING"))
        << found.error();
    // The message names both files and both forms of the digest, so a user can
    // act on it without reading the source.
    EXPECT_TRUE(contains(found.error(),
                         key->userArtifactPath.generic_string()))
        << found.error();
    EXPECT_TRUE(contains(found.error(), document.generic_string()))
        << found.error();
    EXPECT_TRUE(contains(found.error(), key->digest)) << found.error();
    EXPECT_TRUE(contains(found.error(), key->pathDigest)) << found.error();
}

TEST(RuntimeObjectCacheKeyDocument, ACollidingIndexWithADifferentKeyIsNotServed) {
    // ★★★ THE CASE THE WHOLE SIDECAR EXISTS FOR. A DIFFERENT key's entry is
    // planted at THIS key's path — i.e. the 80-bit index collided — and the
    // cache must neither serve it nor overwrite it.
    ScratchDir scratch{Location::Temp, "roc-kd-collide"};
    ASSERT_NO_FATAL_FAILURE(layDownConfigRoot(scratch.path(), kDescriptorV1));
    ScopedUserCacheRoot userRoot{scratch.path() / "uc"};

    auto const key = computeRuntimeObjectKey(makeRequest(scratch.path()));
    ASSERT_TRUE(key.has_value()) << key.error();

    // A REAL other key, not a made-up string: the release build of the same
    // unit. Its document genuinely differs, at the same shape and roughly the
    // same length, which is what a true collision would look like.
    auto const other =
        computeRuntimeObjectKey(makeRequest(scratch.path(), "release"));
    ASSERT_TRUE(other.has_value()) << other.error();
    ASSERT_NE(other->document, key->document);
    ASSERT_NE(other->digest, key->digest);

    constexpr std::string_view kForeignBytes = "!<arch>\nFOREIGN-CONTENT\n";
    ASSERT_NO_FATAL_FAILURE(layDownEntry(key->userArtifactPath, kForeignBytes,
                                         other->document));

    // ── (1) THE LOOKUP REFUSES, and does not hand back the foreign artifact ──
    auto const found = lookupRuntimeObject(*key);
    ASSERT_FALSE(found.has_value())
        << "a colliding entry was SERVED — the 16-character index was treated "
           "as the key.";
    EXPECT_TRUE(contains(found.error(), "CANNOT BE VERIFIED")) << found.error();
    EXPECT_TRUE(contains(found.error(), "DIFFERS")) << found.error();
    EXPECT_TRUE(contains(found.error(), "collided")) << found.error();

    // ── (2) THE STORE REFUSES TOO, and leaves the other entry INTACT ────────
    // This is the arm a "treat it as a miss" design cannot get right: a miss
    // returns to a store whose rule is `already exists ⇒ same bytes`, so the
    // foreign artifact would come straight back as a success.
    auto const stored = storeRuntimeObject(*key, kBytesA);
    ASSERT_FALSE(stored.has_value())
        << "the store wrote over a DIFFERENT key's entry, or accepted it as "
           "its own: "
        << stored->generic_string();
    EXPECT_TRUE(contains(stored.error(), "CANNOT BE VERIFIED"))
        << stored.error();

    EXPECT_EQ(readFile(key->userArtifactPath), kForeignBytes)
        << "the other key's artifact was overwritten.";
    EXPECT_EQ(readFile(runtimeKeyDocumentPath(key->userArtifactPath)),
              other->document)
        << "the other key's key document was overwritten.";
}

TEST(RuntimeObjectCacheKeyDocument, AShippedCollisionRefusesRatherThanFallingThrough) {
    // ⚠ THE FALL-THROUGH THAT LOOKS HELPFUL AND IS NOT. A corrupt or foreign
    // entry in the PACKAGED tree could be routed around by simply reading the
    // per-user root instead — and that is precisely how a broken package stays
    // invisible. The two roots hold the SAME bytes for one key by construction,
    // so a shipped mismatch is an anomaly, never a routine miss.
    ScratchDir scratch{Location::Temp, "roc-kd-shipped"};
    ASSERT_NO_FATAL_FAILURE(layDownConfigRoot(scratch.path(), kDescriptorV1));
    ScopedUserCacheRoot userRoot{scratch.path() / "uc"};

    auto const key = computeRuntimeObjectKey(makeRequest(scratch.path()));
    ASSERT_TRUE(key.has_value()) << key.error();
    auto const other =
        computeRuntimeObjectKey(makeRequest(scratch.path(), "release"));
    ASSERT_TRUE(other.has_value()) << other.error();

    // A PERFECTLY GOOD per-user entry — the copy a fall-through would return.
    ASSERT_TRUE(storeRuntimeObject(*key, kBytesA).has_value());
    ASSERT_TRUE(lookupExpectingNoRefusal(*key).has_value());

    ASSERT_NO_FATAL_FAILURE(layDownEntry(key->shippedArtifactPath,
                                         "PACKAGED-BUT-FOREIGN\n",
                                         other->document));

    auto const found = lookupRuntimeObject(*key);
    ASSERT_FALSE(found.has_value())
        << "the shipped root's unverifiable entry was skipped and the per-user "
           "copy returned instead: "
        << (found.has_value() && found->has_value() ? (*found)->generic_string()
                                                    : std::string{});
    EXPECT_TRUE(contains(found.error(),
                         key->shippedArtifactPath.generic_string()))
        << found.error();
}

TEST(RuntimeObjectCacheKeyDocument, AnInterruptedStoreLeavesAKeyDocumentAndNotAnArtifact) {
    // ★★ THE WRITE ORDER, ASSERTED AS THE PROPERTY IT BUYS. The store lands the
    // key document FIRST so that a run killed between the two renames leaves a
    // recoverable MISS rather than an artifact nobody can verify — which would
    // REFUSE for every later build, i.e. one interrupted build would brick the
    // cache entry until a human deleted it.
    ScratchDir scratch{Location::Temp, "roc-kd-partial"};
    ASSERT_NO_FATAL_FAILURE(layDownConfigRoot(scratch.path(), kDescriptorV1));
    ScopedUserCacheRoot userRoot{scratch.path() / "uc"};

    auto const key = computeRuntimeObjectKey(makeRequest(scratch.path()));
    ASSERT_TRUE(key.has_value()) << key.error();

    // The state a kill between the renames leaves behind.
    ASSERT_NO_FATAL_FAILURE(writeFile(
        runtimeKeyDocumentPath(key->userArtifactPath), key->document));
    ASSERT_FALSE(fs::exists(key->userArtifactPath));

    // It is an ORDINARY MISS — not a refusal.
    auto const found = lookupRuntimeObject(*key);
    ASSERT_TRUE(found.has_value())
        << "a key document with no artifact REFUSED; an interrupted store "
           "would have bricked this entry: "
        << found.error();
    EXPECT_FALSE(found->has_value());

    // …and the next store completes it.
    auto const stored = storeRuntimeObject(*key, kBytesA);
    ASSERT_TRUE(stored.has_value()) << stored.error();
    auto const hit = lookupExpectingNoRefusal(*key);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->generic_string(), key->userArtifactPath.generic_string());
}

TEST(RuntimeObjectCacheKeyDocument, TheArtifactIsNotWrittenWhenTheKeyDocumentCannotBe) {
    // ★★ THE WRITE ORDER, PINNED BY ITS OBSERVABLE CONSEQUENCE rather than by
    // reading the source. A DIRECTORY is planted at the key document's exact
    // path, so that step cannot complete; if the artifact appears anyway, the
    // store wrote it FIRST (or wrote it regardless), and an interrupted run
    // would be free to leave an artifact no later lookup can verify — a bricked
    // entry, which is the state the order exists to make unreachable.
    //
    // ⓘ A DIRECTORY and not a permission bit: mode bits are ignored for root
    // (CI containers run as root) and Windows needs an ACL edit a test process
    // may not be entitled to make — the same portability argument the
    // unwritable-root case is built on. A directory is not a file on any host.
    ScratchDir scratch{Location::Temp, "roc-kd-order"};
    ASSERT_NO_FATAL_FAILURE(layDownConfigRoot(scratch.path(), kDescriptorV1));
    ScopedUserCacheRoot userRoot{scratch.path() / "uc"};

    auto const key = computeRuntimeObjectKey(makeRequest(scratch.path()));
    ASSERT_TRUE(key.has_value()) << key.error();

    fs::path const document = runtimeKeyDocumentPath(key->userArtifactPath);
    std::error_code ec;
    fs::create_directories(document, ec);
    ASSERT_FALSE(ec) << ec.message();
    ASSERT_TRUE(fs::is_directory(document))
        << "the blocker is the SUBJECT of this case; without it the store "
           "would simply succeed.";

    auto const stored = storeRuntimeObject(*key, kBytesA);
    ASSERT_FALSE(stored.has_value())
        << "the store reported success though its key document could not be "
           "written: "
        << stored->generic_string();
    EXPECT_TRUE(contains(stored.error(), "CANNOT BE VERIFIED"))
        << stored.error();

    EXPECT_FALSE(fs::exists(key->userArtifactPath))
        << "THE ARTIFACT WAS WRITTEN ANYWAY — it would be present with no "
           "readable key document beside it, which REFUSES for every later "
           "build.";
    EXPECT_EQ(countEntries(key->userArtifactPath.parent_path()), 1u)
        << "something besides the planted blocker is in the artifact "
           "directory.";
}

// ═══ THE PATH BUDGET ════════════════════════════════════════════════════════

TEST(RuntimeObjectCachePathBudget, TheLongestComposedNameFitsWindowsMaxPath) {
    // ★★★ THE ARITHMETIC IN THIS FILE'S DOCBLOCK, AS AN ASSERTION. A docblock
    // full of measured numbers goes stale the first time someone lengthens a
    // component; this case makes that a RED instead of a comment nobody
    // re-checks. It is deliberately pure string arithmetic — no filesystem, no
    // platform branch — so it holds the same number on every host.
    ScratchDir scratch{Location::Temp, "roc-budget"};
    ASSERT_NO_FATAL_FAILURE(layDownConfigRoot(scratch.path(), kDescriptorV1));
    ScopedUserCacheRoot userRoot{scratch.path() / "uc"};

    auto const key = computeRuntimeObjectKey(
        makeRequest(scratch.path(), "release", "arm64:elf64-aarch64-linux-exec"));
    ASSERT_TRUE(key.has_value()) << key.error();

    // The REAL names, taken from the real key rather than re-typed.
    std::string const artifactName = key->userArtifactPath.filename().string();
    std::string const documentName =
        runtimeKeyDocumentPath(key->userArtifactPath).filename().string();
    EXPECT_EQ(artifactName, "unit-" + key->pathDigest + ".a");
    EXPECT_EQ(artifactName.size(), 23u);
    EXPECT_EQ(documentName.size(), 25u);

    // The LONGEST name the store ever composes is a temp: `.` + the
    // destination's own name + `.tmp-<pid>-<n>`. Modelled with a 5-digit pid
    // and a 1-digit counter, which is what this host produces.
    std::size_t const longestName = 1u + documentName.size() + 5u + 5u + 1u + 1u;
    EXPECT_EQ(longestName, 38u)
        << "the longest composed NAME moved; the installed-headroom figure in "
           "this file's docblock is now wrong.";

    // The INSTALLED worst case, spelled out: the default %LOCALAPPDATA% root,
    // a 5-character username, the 41-character dirty build stamp, `release`,
    // and the 30-character arm64 elf slug.
    std::string const stamp = runtimeCacheBuildStampSegment();
    std::size_t const installed =
        std::string_view{"C:/Users/"}.size() + 5u
        + std::string_view{"/AppData/Local"}.size()
        + std::string_view{"/dsscp/runtime-cache"}.size()
        + 1u + 41u          // the build-stamp segment
        + 1u + std::string_view{"release"}.size()
        + 1u + std::string_view{"arm64_elf64-aarch64-linux-exec"}.size()
        + 1u + longestName;
    EXPECT_EQ(installed, 168u);
    EXPECT_EQ(260u - installed, 92u)
        << "the Windows MAX_PATH headroom for an installed compiler changed; "
           "re-measure and update this file's docblock.";

    // The stamp segment this host actually renders must not exceed the 41 the
    // figure above assumes — a longer one silently eats the headroom.
    EXPECT_LE(stamp.size(), 41u)
        << "the build-stamp segment is longer than the arithmetic assumes: "
        << stamp;

    // And the real per-user artifact path is comfortably inside the limit here,
    // which is the claim the whole suite depends on for its own store cases.
    EXPECT_LT(key->userArtifactPath.generic_string().size(), 260u)
        << key->userArtifactPath.generic_string();
}

TEST(RuntimeObjectCachePathBudget, AnOverlongNameRefusesNamingThePathAndItsLength) {
    // ⚠ THE FAILURE THIS ROW WAS OPENED FOR, MADE SELF-DIAGNOSING. The original
    // symptom was a bare "could not open the temporary file" with no hint that
    // LENGTH was the cause — `create_directories` had succeeded, so nothing
    // pointed at it. The refusal must now name the composed path, its length,
    // and a MEASURED verdict on whether the length was to blame.
    //
    // ⓘ THE ONE VARIABLE IS THE ARTIFACT FILENAME'S LENGTH, and the key is a
    // REAL one — computed from a real config root, with a real document, a real
    // root trail and a real parent directory — with only that filename
    // lengthened. Two reasons it is done this way rather than by handing the
    // request a 400-character unit stem:
    //   * a 400-character SOURCE FILE cannot be created on ANY of the three
    //     hosts (NTFS, ext4 and APFS all cap a single NAME at 255), so the
    //     fixture could not be laid down at all;
    //   * every OTHER length that fails on Windows' 260-character MAX_PATH
    //     SUCCEEDS on Linux's 4096-character PATH_MAX — so a "realistic" long
    //     path would quietly assert nothing on two of the three hosts, which is
    //     the silent-skip shape this repo keeps paying for.
    // A 400-character NAME is refused by all three, so this case runs the SAME
    // experiment everywhere.
    ScratchDir scratch{Location::Temp, "roc-budget-long"};
    ASSERT_NO_FATAL_FAILURE(layDownConfigRoot(scratch.path(), kDescriptorV1));
    ScopedUserCacheRoot userRoot{scratch.path() / "uc"};

    auto const real = computeRuntimeObjectKey(makeRequest(scratch.path()));
    ASSERT_TRUE(real.has_value()) << real.error();
    ASSERT_FALSE(real->userArtifactPath.empty());

    std::string const longStem(400u, 'z');
    RuntimeObjectKey  key = *real;
    key.userArtifactPath  = real->userArtifactPath.parent_path()
                         / (longStem + "-" + key.pathDigest + ".a");

    auto const stored = storeRuntimeObject(key, kBytesA);
    ASSERT_FALSE(stored.has_value())
        << "a 400-character filename component was written; this host does not "
           "enforce the name limit the case assumes: "
        << stored->generic_string();

    // (1) THE COMPOSED PATH, VERBATIM — the thing a user has to look at.
    EXPECT_TRUE(contains(stored.error(), "COMPOSED PATH")) << stored.error();
    EXPECT_TRUE(contains(stored.error(), longStem)) << stored.error();
    // (2) ITS LENGTH, as a number.
    EXPECT_TRUE(contains(stored.error(), "characters")) << stored.error();
    // (3) THE MEASURED VERDICT — the short-name control, run by the refusal
    // itself, which is what turns "could not open" into a diagnosis. The
    // directory was created successfully, so the control must succeed and the
    // verdict must be LENGTH.
    EXPECT_TRUE(contains(stored.error(), "✔MEASURED just now")) << stored.error();
    EXPECT_TRUE(contains(stored.error(), "the LENGTH of the path above is what "
                                         "failed"))
        << stored.error();
    // (4) THE REMEDY.
    EXPECT_TRUE(contains(stored.error(), "DSS_RUNTIME_CACHE_DIR"))
        << stored.error();
    EXPECT_TRUE(contains(stored.error(), "REFUSAL")) << stored.error();

    // Nothing half-written survives: the short-name probe cleans up after
    // itself, and the guard removes the temp.
    std::error_code ec;
    ASSERT_TRUE(fs::is_directory(key.userArtifactPath.parent_path(), ec) && !ec)
        << "the artifact directory was never created, so the refusal came from "
           "the directory arm and this case never reached the name-length one.";
    EXPECT_EQ(countEntries(key.userArtifactPath.parent_path()), 0u)
        << "the refusal left files behind in the artifact directory.";
}
