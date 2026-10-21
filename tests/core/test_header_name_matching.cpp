// D-PP-HEADER-CASE-INSENSITIVE-PE — header-name case matching is decided by the
// TARGET's declared convention, never by the build HOST's filesystem.
//
// ★ WHY EVERY PIN BELOW COMES IN A PAIR. This defect had TWO directions, and
// each is only OBSERVABLE on one kind of host:
//
//   * WRONG REJECT  — `<Windows.h>` for a pe target fails on a case-SENSITIVE
//     host (ext4). Invisible on Windows, where NTFS folded it by accident.
//   * SILENT ACCEPT — `<Stdio.h>` for an elf target COMPILES on a
//     case-INSENSITIVE host (NTFS/APFS) and emits a real object, which a
//     conforming POSIX toolchain must reject. Invisible on Linux.
//
// A test that pinned only one direction would therefore be GREEN on the host
// where the other direction is broken. Each pin below asserts BOTH policies
// against the SAME on-disk file, so whichever host runs it, at least one arm is
// contradicting that host's own convention and would go red if DSS delegated
// the case decision to `fs::exists` again.
//
// MEASURED baseline before the fix (one Linux host, one binary, one commit,
// only the filesystem varied): config tree on ext4 -> `<windows.h>` rc=0,
// `<Windows.h>` rc=1 error[F001A]; the SAME tree via /mnt/c DrvFs -> BOTH rc=0.

#include "core/substrate/path_identity.hpp"
#include "core/substrate/process_spawn.hpp"   // the LIVE case asks git itself
#include "core/types/ascii_case.hpp"
#include "core/types/header_name_matching.hpp"
#include "core/types/header_case_diagnostic.hpp"
#include "core/types/include_path_resolve.hpp"
#include "core/types/unsuppressable_codes.hpp"
#include "program/git_acquire.hpp"           // gitRepositoryLocalVariables: the LIVE case's git sees no caller repository
#include "os_link.hpp"                       // links made by the OS: MinGW's library makes none
#include "repo_root.hpp"
#include "scratch_dir.hpp"
#include "tree_ignores.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>                 // the one case table (tests/test_support/tree_ignores_cases.json)

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace dss;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

void writeFile(fs::path const& p, std::string_view text = "{}\n") {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << text;
}

[[nodiscard]] std::string u8Bytes(std::u8string_view s) {
    return std::string(reinterpret_cast<char const*>(s.data()), s.size());
}

// ⚠⚠ NARROW WITHOUT THROWING — `path::string()` AND `generic_string()` NARROW THROUGH THE ANSI
// CODE PAGE ON MS STL AND THROW ON A NAME THEY CANNOT MAP.
// That is [[D-PP-HEADER-CASE-NON-ASCII-NAME-NARROWING-THROW]], a P0 CLOSED for the production
// sites — and MISSED in this file, which owns the one scan that walks ARBITRARY trees.
// ✔MEASURED 2026-09-02 on a pt-BR host (CP1252) under MSVC Release: one directory whose name held
// U+F03A (how Windows stores a `:` written by a POSIX tool that treated `C:/…` as relative) made
// `ShippedConfigTreeHasNoCaseCollidingPaths` die with `C++ exception with description "Não existe
// mapeamento para o caractere Unicode…"` — a localized message naming NO PATH, thrown by a guard
// whose entire job is to name paths. gcc and clang narrow as UTF-8 and never saw it.
// ★ THE FALLBACK IS THE CALLER'S BY DESIGN, WHICH IS WHY IT LIVES HERE AND NOT IN THE OWNER.
// `genericSpellingU8` throws exactly where `u8string()` does, deliberately: a native name can be
// text no encoding accepts (NTFS permits lone surrogates), and swallowing that inside the owner
// would hand back a name that is not the file's. So the owner is reused for the spelling, and the
// decision about an unrenderable entry is made by each caller — here, an EMPTY string meaning
// "cannot classify", counted rather than silently skipped.
[[nodiscard]] std::string narrowUtf8OrEmpty(fs::path const& p) {
    try {
        return u8Bytes(dss::core::genericSpellingU8(p));
    } catch (...) {
        return {};
    }
}

// Can THIS filesystem hold two entries whose names differ only by ASCII case?
// Answered by EXPERIMENT in the very directory under test, never by an
// `#ifdef _WIN32` — the same discipline the resolver itself follows. Used only
// to decide whether an ambiguity fixture is CONSTRUCTIBLE here; it never
// changes what the resolver is expected to answer.
// Best-effort: ask the platform to make THIS directory case-sensitive, so the
// collision fixture below is constructible even on a folding host. Windows
// 10+ supports per-directory case sensitivity (the flag WSL uses) and the
// directory must be EMPTY when it is set. Non-fatal by design: if the command
// is absent, refused, or the feature is off, `hostCanHoldCaseCollidingPair`
// simply keeps answering false and the caller falls back. Nothing here decides
// what the RESOLVER should answer — it only decides whether the fixture can be
// built, and the EXPERIMENT below, never an `#ifdef`, is what reports that.
void tryMakeDirCaseSensitive(fs::path const& dir) {
#ifdef _WIN32
    std::string cmd = "fsutil.exe file setCaseSensitiveInfo \"";
    cmd += dir.string();
    cmd += "\" enable >nul 2>&1";
    (void)std::system(cmd.c_str());
#else
    (void)dir;   // POSIX filesystems used by this project already qualify
#endif
}

bool hostCanHoldCaseCollidingPair(fs::path const& dir) {
    fs::path const lower = dir / "dss_case_probe.tmp";
    fs::path const upper = dir / "DSS_CASE_PROBE.tmp";
    writeFile(lower, "a");
    writeFile(upper, "b");
    std::error_code ec;
    std::size_t entries = 0;
    for (fs::directory_iterator it{dir, ec}, end; !ec && it != end; it.increment(ec)) {
        // Same narrowing hazard as the sweep below, and NOT hypothetical here either: this
        // directory lives under TEMP, whose path carries the account name — on a host whose
        // user name the code page cannot map, `.string()` would throw and take the host-capability
        // probe with it, turning "can this filesystem hold a colliding pair?" into a crash.
        std::string const n = narrowUtf8OrEmpty(it->path().filename());
        if (asciiToLower(n) == "dss_case_probe.tmp") ++entries;
    }
    fs::remove(lower, ec);
    fs::remove(upper, ec);
    return entries == 2;
}

// ── non-ASCII fixtures (D-PP-HEADER-CASE-NON-ASCII-NAME-NARROWING-THROW) ──
//
// A `u8"..."` literal is UTF-8 BY DEFINITION regardless of this file's own
// encoding or `-fexec-charset`, and every non-ASCII name below is spelled with
// universal-character escapes, so the fixture bytes are identical on every
// host and every toolchain. Nothing exotic is ever COMMITTED — the names are
// created at runtime in a scratch dir and removed with it.
constexpr std::u8string_view kNaive = u8"na\u00EFve.json";       // Latin-1 range
constexpr std::u8string_view kZhong = u8"\u4E2D\u6587.json";    // CJK, distinct...
constexpr std::u8string_view kNihon = u8"\u65E5\u672C.json";    // ...from this one

// A filename the HOST FILESYSTEM will hold but the host's NARROW conversion
// cannot represent — the shape that turns `path::string()` into a THROW rather
// than a diagnostic (MEASURED: libstdc++ 13.2 raises `filesystem_error`
// "Cannot convert character sequence"; MS STL raises `system_error`).
//
// Decided by a question about the TYPE plus an EXPERIMENT, never by an
// `#ifdef`. U+D800 is a lone surrogate on a UTF-16 native character type and is
// not even representable in a byte-sized one, so a host whose
// `path::value_type` is `char` correctly answers "I have no such name" and the
// caller says so out loud instead of pretending to have tested it.
[[nodiscard]] std::optional<fs::path> makeUnnarrowableEntry(fs::path const& dir) {
    using NativeChar   = fs::path::value_type;
    using NativeString = fs::path::string_type;
    constexpr unsigned long long kLoneSurrogate = 0xD800ull;
    if constexpr (static_cast<unsigned long long>(
                      (std::numeric_limits<NativeChar>::max)()) < kLoneSurrogate) {
        (void)dir;
        return std::nullopt;
    } else {
        NativeString name;
        for (char c : std::string_view{"lone"}) name += static_cast<NativeChar>(c);
        name += static_cast<NativeChar>(kLoneSurrogate);
        for (char c : std::string_view{".json"}) name += static_cast<NativeChar>(c);
        fs::path const p = dir / fs::path{name};
        writeFile(p);
        std::error_code ec;
        if (!fs::exists(p, ec)) return std::nullopt;   // host refused it
        return p;
    }
}

// ── the repo-hygiene checker, factored out so the guard test and its
//    red-on-disable control run the IDENTICAL code over different trees ──

// WHAT THE SWEEP LEAVES OUT: WHAT GIT IGNORES IN THE TREE, and `.git`. The question
// the repo-wide sweep answers is "can this repo be CLONED onto a case-insensitive
// host", and a clone materializes nothing git ignores: build output, a session's
// scratch, and the copies and workers DssHarness keeps in a tree (`.worktrees/`,
// `.orchestrators/`, `.legs/`, `.mutations/` — each a whole tree of its own, whose
// files a sweep or a lane may delete while this walk reads them, and a walk error is
// a red here). It is asked of the ONE statement of that, the tree's own `.gitignore`,
// through `tests/test_support/tree_ignores.hpp` — the C++ reader of the grammar
// `owning-tree.py`'s `TreeIgnores` reads for the repository's programs. Recursion is
// pruned AT a directory left out, so its contents are never even enumerated.
// ⓘ Until 2026-10-15 this file kept its own list of bare names (`.git`,
// `test-scratch`, `node_modules`, `.claude`, `target`, every name starting `build`)
// plus one generated subtree by path: a second statement that walked the copies the
// tool keeps — ✔MEASURED from run records: 11 to 56 s for this test on the main
// checkout's Windows legs, against 1 to 7 s in lane trees that hold none — and that
// pruned the TRACKED `.claude/` skills as well. An anchored rule still prunes only
// its one path (`/src/dss-config/runtime/platform/dist/`), as git reads it.
using dss::test_support::TreeIgnores;

// Returns every group of >= 2 paths under `root` whose relative paths are equal
// after ASCII lowercasing. Such a group cannot be checked out at all on NTFS or
// on a default APFS/HFS+ volume (git will either collide the two onto one file
// or leave the tree permanently dirty), so a repo that contains one only works
// on ext4 — a non-starter for a project whose whole point is any-host.
// ⚠⚠ NARROW WITHOUT THROWING — `path::string()` AND `generic_string()` NARROW THROUGH THE ANSI
// CODE PAGE ON MS STL AND THROW ON A NAME THEY CANNOT MAP.
// That is [[D-PP-HEADER-CASE-NON-ASCII-NAME-NARROWING-THROW]], a P0 CLOSED for the production
// sites — and MISSED here, in the one scan that walks ARBITRARY trees. ✔MEASURED 2026-09-02 on a
// pt-BR host (CP1252) under MSVC Release: a single directory whose name held U+F03A (how Windows
// stores a `:` written by a POSIX tool) made `ShippedConfigTreeHasNoCaseCollidingPaths` die with
// `C++ exception with description "Não existe mapeamento para o caractere Unicode…"` — a localized
// message naming NO PATH, from a guard whose whole job is to name paths.
// ★ THE FALLBACK IS THE CALLER'S BY DESIGN. `genericSpellingU8` throws exactly where `u8string()`
// does, deliberately, because a native name can be text no encoding accepts (NTFS permits lone
// surrogates) and swallowing that inside the owner would hand back a name that is not the file's.
// So the owner is reused and the fallback lives HERE: an unrenderable entry yields an EMPTY
// spelling, which the callers treat as "cannot classify" and COUNT, rather than aborting a sweep
// over a whole repository because of one path it only needed to compare.
struct CaseCollisionScan {
    std::vector<std::vector<std::string>> groups;   // each >= 2 fold-equal paths
    // ★ THE SCAN MUST REPORT ITS OWN DENOMINATOR. `groups` is empty both when
    // the tree is clean and when NOTHING WAS LOOKED AT, and the two are
    // indistinguishable to a caller that only checks emptiness — a guard that
    // passes vacuously is exactly the "instrument reports a pass over work it
    // did not do" class. Two live vectors: the iterator can fail at
    // CONSTRUCTION (bad root), and `increment(ec)` can fail MID-WALK, which
    // silently ENDS the loop over a partial tree. The second is not theoretical
    // in this repo — concurrent ctest runs produce MEASURED vanished-file races
    // in this very tree.
    std::size_t     filesScanned = 0;
    std::error_code walkError;   // non-empty ⇒ TRUNCATED walk, never a pass
    // ★ THE SAME DENOMINATOR DISCIPLINE, ONE STEP FURTHER: an entry whose name cannot be
    // rendered is SKIPPED, and a silent skip is the vacuous-pass class this struct already
    // guards against. Counting it makes the limit legible — the sweep can then say "clean over
    // N files, and M I could not spell" instead of implying it looked at everything.
    std::size_t     unrenderable = 0;
    // ★ AND WHAT IT LEFT OUT IS SAID, NEVER SILENT: a `.gitignore` the one reader cannot
    // read as git does refuses the whole sweep, and so does a swept root the tree's rules
    // leave out — a sweep of nothing.
    std::string              refusal;
    std::string              leftOutRoot;
    std::vector<std::string> paths;   // every file swept, relative to the swept root, in walk order
};

// `tree` is the checkout `ignores` was read from (`.gitignore` at its root), `root` the
// directory swept — the tree itself or one under it. What `ignores` leaves out is never
// enumerated: a path is judged RELATIVE TO THE TREE, the frame its rules are written in,
// and reported relative to `root`.
CaseCollisionScan caseCollisionsUnder(fs::path const& tree, fs::path const& root, TreeIgnores const& ignores) {
    CaseCollisionScan scan;
    std::map<std::string, std::vector<std::string>> byFolded;
    if (!ignores.refusal.empty()) {
        scan.refusal = ignores.refusal;
        return scan;
    }

    // ⚠ NORMALIZE, AND `.` IS EXACTLY WHY. The repo-wide guard walks `*root / "."` as
    // one of its four roots, and `.` is a REAL element of `fs::path` iteration — not a
    // spelling the library folds away — so every entry that walk yields reads
    // `<root>/./src/...`. Judged as written, nothing under it would ever match an
    // anchored rule, and the whole-repo root — the only one that reaches every
    // directory the tree's rules leave out — would silently walk them all.
    // `lexically_normal()` settles `.` and `/` vs `\`; a trailing separator it leaves
    // is taken off, so the frame is the TREE and never how it was spelled.
    auto const frame = [](fs::path p) {
        p = p.lexically_normal();
        return p.has_filename() ? p : p.parent_path();
    };
    fs::path const treeFrame = frame(tree);
    auto const     treeRel = [&](fs::path const& p) { return narrowUtf8OrEmpty(frame(p).lexically_relative(treeFrame)); };
    // ★ A PATH THE ONE READER CANNOT ANSWER AS GIT DOES REFUSES THE WHOLE SWEEP, naming it (a name
    // holding a backslash, a rule whose match core.ignorecase would change): the sweep's verdict
    // would otherwise be one host's answer.
    try {
        if (std::string const rootRel = treeRel(root); !rootRel.empty() && rootRel != ".") {
            scan.leftOutRoot = ignores.leavesOut(rootRel, true);
            if (!scan.leftOutRoot.empty()) return scan;
        }

        using dss::core::EntryKind;
        using dss::core::WalkStep;
        // The substrate's walk (`dss::core::walkTree`): an explicit work stack, and it never descends
        // a link, on every build.
        auto const walk = dss::core::walkTree(root, [&](dss::core::TreeEntry const& entry) {
            std::string const inTree = treeRel(entry.path);
            auto const        names = dss::test_support::tree_ignores_detail::splitNames(inTree);
            // ★ A SYMBOLIC LINK IS NO DIRECTORY AND IS NEVER DESCENDED: git records a link as a file,
            // so a `build/` rule does not take one (✔MEASURED, git 2.55), and a clone holds the link
            // itself — a path that can collide like any file. The ENTRY's own kind decides, never what
            // it points at — asked of the substrate, which asks the OS on Windows: libstdc++ there
            // reads a link as what it reaches (✔MEASURED, the substrate's header), so a scan that asked
            // `symlink_status` DESCENDED a directory link when built with MinGW.
            if (entry.kind == EntryKind::Directory) {
                // An unrenderable directory name yields "" — which no rule can judge, so the walk
                // DESCENDS rather than pruning. Erring toward scanning more is the safe direction
                // for a collision guard.
                if (!inTree.empty() && !ignores.decide(names, true).empty()) return WalkStep::Prune;
                return WalkStep::Continue;
            }
            if (entry.kind == EntryKind::Absent) return WalkStep::Continue;   // gone since it was listed
            if (!inTree.empty() && !ignores.decide(names, false).empty()) return WalkStep::Continue;   // never cloned
            // LEXICALLY relative to the swept root, never through `fs::relative`, which resolves a
            // symbolic link to what it points at and would report the target, not the link.
            fs::path const    relPath = frame(entry.path).lexically_relative(frame(root));
            std::string const rel     = narrowUtf8OrEmpty(relPath);
            if (rel.empty()) {
                // Distinguish the two reasons rather than folding them: no relative path is a
                // walk problem, an unrenderable spelling is a text problem, and only the second is
                // this scan's own blind spot.
                if (!relPath.empty()) ++scan.unrenderable;
                return WalkStep::Continue;
            }
            ++scan.filesScanned;
            scan.paths.push_back(rel);
            byFolded[asciiToLower(rel)].push_back(rel);
            return WalkStep::Continue;
        });
        if (walk.error) {   // truncated — NOT a pass
            scan.walkError = walk.error;
            return scan;
        }
    } catch (dss::test_support::TreeIgnoreRefusal const& refused) {
        scan.refusal = refused.what();
        return scan;
    }
    for (auto& [folded, group] : byFolded) {
        if (group.size() < 2) continue;
        std::sort(group.begin(), group.end());
        scan.groups.push_back(group);
    }
    return scan;
}

} // namespace

// ── the closed vocabulary ────────────────────────────────────────────────

TEST(HeaderNameMatching, VocabularyRoundTripsAndRejectsUnknown) {
    EXPECT_EQ(headerNameMatchingName(HeaderNameMatching::CaseSensitive),
              "case-sensitive");
    EXPECT_EQ(headerNameMatchingName(HeaderNameMatching::CaseInsensitive),
              "case-insensitive");
    EXPECT_EQ(*headerNameMatchingFromName("case-sensitive"),
              HeaderNameMatching::CaseSensitive);
    EXPECT_EQ(*headerNameMatchingFromName("case-insensitive"),
              HeaderNameMatching::CaseInsensitive);
    // The INVALID sentinel has no spelling: a spellable "invalid" would let a
    // typo look deliberate (the LongDoubleFormat::None precedent).
    EXPECT_TRUE(headerNameMatchingName(HeaderNameMatching::Invalid).empty());
    EXPECT_FALSE(headerNameMatchingFromName("invalid").has_value());
    EXPECT_FALSE(headerNameMatchingFromName("caseinsensitive").has_value());
    EXPECT_FALSE(headerNameMatchingFromName("CASE-SENSITIVE").has_value());
    // The no-active-format default is the CONSERVATIVE, POSIX-conforming one:
    // guessing it can only ever reject loudly, never accept silently.
    EXPECT_EQ(kDefaultHeaderNameMatching, HeaderNameMatching::CaseSensitive);
}

// ── the two directions, on one on-disk file, on any host ─────────────────

TEST(HeaderNameMatching, PolicyDecidesCaseNotTheHostFilesystem) {
    HeaderSearchCache cache;   // one compile's view of the tree
    ScratchDir scratch{Location::Temp, "header_case"};
    fs::path const dir = scratch.path();
    writeFile(dir / "stdio.json");

    // Byte-exact request resolves under BOTH policies — the control that keeps
    // the two rejections below attributable to CASE and nothing else.
    EXPECT_EQ(resolveInDir(dir, "stdio.json", HeaderNameMatching::CaseSensitive, cache).status,
              HeaderSearchStatus::Found);
    EXPECT_EQ(resolveInDir(dir, "stdio.json", HeaderNameMatching::CaseInsensitive, cache).status,
              HeaderSearchStatus::Found);

    // ★ THE SILENT-ACCEPT PIN (the one that matters most). A case-SENSITIVE
    // format must NOT resolve `Stdio.json` to `stdio.json`. On a case-
    // INSENSITIVE host `fs::exists(dir/"Stdio.json")` returns TRUE, so this
    // goes red the moment DSS delegates the decision back to the host.
    EXPECT_EQ(resolveInDir(dir, "Stdio.json", HeaderNameMatching::CaseSensitive, cache).status,
              HeaderSearchStatus::NotFound)
        << "a POSIX/elf target must reject a case-mismatched header name even "
           "when the BUILD HOST's filesystem would happily fold it";

    // ★ THE WRONG-REJECT PIN. A case-INSENSITIVE format MUST resolve it. On a
    // case-SENSITIVE host `fs::exists` returns FALSE, so this goes red if DSS
    // stops folding for itself.
    HeaderSearchResult const ci =
        resolveInDir(dir, "Stdio.json", HeaderNameMatching::CaseInsensitive, cache);
    ASSERT_EQ(ci.status, HeaderSearchStatus::Found)
        << "a pe/macho target must resolve `<Stdio.h>` to the shipped "
           "`stdio.json` on ANY build host";
    EXPECT_EQ(ci.path.filename().string(), "stdio.json")
        << "the resolved path must be the REAL on-disk spelling, not the "
           "requested one — downstream readers open this path";

    // Mixed case, and a name that differs from the on-disk one by more than
    // case (never a match under either policy).
    EXPECT_EQ(resolveInDir(dir, "StDiO.json", HeaderNameMatching::CaseInsensitive, cache).status,
              HeaderSearchStatus::Found);
    EXPECT_EQ(resolveInDir(dir, "stdioo.json", HeaderNameMatching::CaseInsensitive, cache).status,
              HeaderSearchStatus::NotFound);
}

TEST(HeaderNameMatching, PolicyAppliesToEveryPathComponent) {
    HeaderSearchCache cache;   // one compile's view of the tree
    ScratchDir scratch{Location::Temp, "header_case_subdir"};
    fs::path const dir = scratch.path();
    writeFile(dir / "sys" / "types.json");

    // The DIRECTORY component must fold too — `<SYS/TYPES.h>` is a legal
    // Windows spelling of the POSIX subdir header, and a resolver that folded
    // only the leaf would still reject it.
    EXPECT_EQ(resolveInDir(dir, "SYS/TYPES.json", HeaderNameMatching::CaseInsensitive, cache)
                  .status,
              HeaderSearchStatus::Found);
    EXPECT_EQ(resolveInDir(dir, "Sys/types.json", HeaderNameMatching::CaseInsensitive, cache)
                  .status,
              HeaderSearchStatus::Found);
    EXPECT_EQ(resolveInDir(dir, "SYS/TYPES.json", HeaderNameMatching::CaseSensitive, cache)
                  .status,
              HeaderSearchStatus::NotFound);
    EXPECT_EQ(resolveInDir(dir, "SYS/types.json", HeaderNameMatching::CaseSensitive, cache)
                  .status,
              HeaderSearchStatus::NotFound)
        << "a mismatch in the DIRECTORY component alone must still reject";
    EXPECT_EQ(resolveInDir(dir, "sys/types.json", HeaderNameMatching::CaseSensitive, cache)
                  .status,
              HeaderSearchStatus::Found);
}

// The `<stem>.json` rewrite is CASE-PRESERVING byte slicing; the policy then
// answers the case question. This is the exact shape of the sqlite CLI blocker:
// `sqlite3.c` writes `#include <Windows.h>` and DSS ships `windows.json`.
TEST(HeaderNameMatching, SystemDescriptorStemRewriteHonoursPolicy) {
    HeaderSearchCache cache;   // one compile's view of the tree
    ScratchDir scratch{Location::Temp, "header_case_desc"};
    fs::path const dir = scratch.path();
    writeFile(dir / "windows.json");
    std::vector<fs::path> const sysDirs{dir};

    EXPECT_EQ(resolveSystemDescriptor("Windows.h", sysDirs,
                                      HeaderNameMatching::CaseInsensitive, cache).status,
              HeaderSearchStatus::Found)
        << "the pe/macho convention must reach windows.json from <Windows.h>";
    EXPECT_EQ(resolveSystemDescriptor("WiNdOwS.h", sysDirs,
                                      HeaderNameMatching::CaseInsensitive, cache).status,
              HeaderSearchStatus::Found);
    EXPECT_EQ(resolveSystemDescriptor("Windows.h", sysDirs,
                                      HeaderNameMatching::CaseSensitive, cache).status,
              HeaderSearchStatus::NotFound);
    EXPECT_EQ(resolveSystemDescriptor("windows.h", sysDirs,
                                      HeaderNameMatching::CaseSensitive, cache).status,
              HeaderSearchStatus::Found);
}

// The angle funnel (`#include <h>` AND `__has_include(<h>)` both call it) must
// carry the policy into BOTH of its arms — the descriptor arm and the
// source-header fallback. A policy applied to only one arm would make an angle
// include of a real `-I` header disagree with an angle include of a descriptor.
TEST(HeaderNameMatching, AngleFunnelHonoursPolicyInBothArms) {
    HeaderSearchCache cache;   // one compile's view of the tree
    ScratchDir scratch{Location::Temp, "header_case_angle"};
    fs::path const sysDir = scratch.path() / "sys";
    fs::path const incDir = scratch.path() / "inc";
    writeFile(sysDir / "windows.json");
    writeFile(incDir / "sqlite3ext.h", "int x;\n");
    std::vector<fs::path> const sysDirs{sysDir};
    std::vector<fs::path> const incDirs{incDir};

    EXPECT_EQ(resolveAngleInclude("Windows.h", sysDirs, incDirs,
                                  HeaderNameMatching::CaseInsensitive, cache).kind,
              AngleIncludeKind::Descriptor);
    EXPECT_EQ(resolveAngleInclude("Windows.h", sysDirs, incDirs,
                                  HeaderNameMatching::CaseSensitive, cache).kind,
              AngleIncludeKind::NotFound);
    EXPECT_EQ(resolveAngleInclude("SQLite3Ext.h", sysDirs, incDirs,
                                  HeaderNameMatching::CaseInsensitive, cache).kind,
              AngleIncludeKind::Source)
        << "the -I source fallback arm must fold too, or an angle include of a "
           "real header disagrees with an angle include of a descriptor";
    EXPECT_EQ(resolveAngleInclude("SQLite3Ext.h", sysDirs, incDirs,
                                  HeaderNameMatching::CaseSensitive, cache).kind,
              AngleIncludeKind::NotFound);
}

TEST(HeaderNameMatching, QuoteAndDirListSearchesHonourPolicy) {
    HeaderSearchCache cache;   // one compile's view of the tree
    ScratchDir scratch{Location::Temp, "header_case_quote"};
    fs::path const selfDir = scratch.path() / "self";
    fs::path const incDir  = scratch.path() / "inc";
    writeFile(selfDir / "local.h", "int a;\n");
    writeFile(incDir / "shared.h", "int b;\n");
    std::vector<fs::path> const incDirs{incDir};

    // Self-dir arm (quote-only, C 6.10.2p3).
    EXPECT_EQ(resolveIncludePath("Local.h", selfDir, incDirs,
                                 HeaderNameMatching::CaseInsensitive, cache).status,
              HeaderSearchStatus::Found);
    EXPECT_EQ(resolveIncludePath("Local.h", selfDir, incDirs,
                                 HeaderNameMatching::CaseSensitive, cache).status,
              HeaderSearchStatus::NotFound);
    // -I arm.
    EXPECT_EQ(resolveIncludePath("Shared.h", selfDir, incDirs,
                                 HeaderNameMatching::CaseInsensitive, cache).status,
              HeaderSearchStatus::Found);
    EXPECT_EQ(resolveIncludePath("Shared.h", selfDir, incDirs,
                                 HeaderNameMatching::CaseSensitive, cache).status,
              HeaderSearchStatus::NotFound);
    // `findInDirs` (the shared dir-list search) directly.
    EXPECT_EQ(findInDirs("Shared.h", incDirs, HeaderNameMatching::CaseInsensitive, cache).status,
              HeaderSearchStatus::Found);
    EXPECT_EQ(findInDirs("Shared.h", incDirs, HeaderNameMatching::CaseSensitive, cache).status,
              HeaderSearchStatus::NotFound);
}

// ── [[D-PP-HEADER-CASE-DANGLING-SYMLINK-ASYMMETRY]] ─────────────────────────
//
// A DANGLING LINK IS NO HEADER, UNDER EITHER POLICY. ✔MEASURED 2026-10-20 on this very
// fixture, built by each reference: a dangling link `x.h` in the first search directory and
// a real `x.h` in a later one — gcc 13.3.0, clang 18.1.3, Apple clang 21.0.0, MinGW-w64 gcc
// 13.2.0 and cl 19.51, `#include "x.h"` and `#include <x.h>` alike, each went ON to the later
// directory. The two policy arms used to disagree on it (the case-sensitive arm's probe said
// "absent", the case-insensitive arm took whatever its listing held), and the MinGW build took
// the link under both. One case per policy arm; the link is made by the OS, which MinGW's
// library cannot do.
namespace {

// Which `x.h` a search took, spelled `<its directory's name>/x.h`, or what it took instead.
[[nodiscard]] std::string tookWhat(HeaderSearchResult const& r) {
    switch (r.status) {
    case HeaderSearchStatus::Found:
        return u8Bytes(r.path.parent_path().filename().u8string()) + "/" +
               u8Bytes(r.path.filename().u8string());
    case HeaderSearchStatus::NotFound: return "nothing";
    case HeaderSearchStatus::AmbiguousCase: return "an ambiguity";
    }
    return "an unknown status";
}

void expectADanglingLinkIsNoHeader(HeaderNameMatching matching, char const* scratchName) {
    ScratchDir scratch{Location::Temp, scratchName};
    fs::path const first = scratch.path() / "first";
    fs::path const later = scratch.path() / "later";
    fs::create_directories(first);
    writeFile(later / "x.h", "#define FROM_LATER 1\n");
    ASSERT_FALSE(dss::test_support::makeSymbolicLink(first / "no-such.h", first / "x.h",
                                                     dss::test_support::LinkTo::File))
        << "the OS made no link here, so this case cannot ask its question";

    HeaderSearchCache cache;   // one compile's view of the tree
    std::vector<fs::path> const dirs{first, later};
    std::string const took = tookWhat(findInDirs("x.h", dirs, matching, cache));
    EXPECT_EQ(took, "later/x.h")
        << "the search took " << took << " -- a dangling link was taken for a header, where "
           "every reference goes on to the later directory";
    std::string const atFirst = tookWhat(resolveInDir(first, "x.h", matching, cache));
    EXPECT_EQ(atFirst, "nothing") << "the first directory alone answered " << atFirst;
}

} // namespace

TEST(HeaderNameMatching, ADanglingLinkIsNoHeaderUnderTheCaseSensitivePolicy) {
    expectADanglingLinkIsNoHeader(HeaderNameMatching::CaseSensitive, "header_dangling_cs");
}

TEST(HeaderNameMatching, ADanglingLinkIsNoHeaderUnderTheCaseInsensitivePolicy) {
    expectADanglingLinkIsNoHeader(HeaderNameMatching::CaseInsensitive, "header_dangling_ci");
}

// ── [[D-PP-INCLUDE-RESOLVER-RELISTS-EVERY-DIRECTORY-PER-RESOLUTION]] ────────
//
// A COMPILE reads each directory ONCE, and only for itself. Every search above used to
// list its directory afresh (✔MEASURED on the sqlite build: 11,204 listings of 13
// directories, the shipped-descriptor directory 6,477 times); a `HeaderSearchCache` is
// the compile's one view of the tree. COUNTED, from a tally that sees listings made
// through any cache: both policies, hits and misses, answer from ONE listing and one
// probe per candidate. And the view is the compile's alone — a header written after one
// compile looked is seen by the next, which lists again: a cache that outlived its
// compile would answer a later build from a tree that is gone.
TEST(HeaderSearchCache, EachCompileListsADirectoryOnceAndOnlyForItself) {
    ScratchDir scratch{Location::Temp, "header_cache_compile"};
    fs::path const dir = scratch.path();
    writeFile(dir / "early.h", "int e;\n");
    std::vector<fs::path> const dirs{dir};
    HeaderSearchTally const tally;

    HeaderSearchCache first;
    for (auto m : {HeaderNameMatching::CaseSensitive, HeaderNameMatching::CaseInsensitive}) {
        SCOPED_TRACE(headerNameMatchingName(m));
        for (int i = 0; i < 3; ++i) {
            EXPECT_EQ(findInDirs("early.h", dirs, m, first).status, HeaderSearchStatus::Found);
            EXPECT_EQ(findInDirs("late.h", dirs, m, first).status, HeaderSearchStatus::NotFound);
        }
    }
    EXPECT_EQ(first.listings(), 1u)
        << "one directory, twelve searches under two policies: ONE listing per compile";
    EXPECT_EQ(first.probes(), 2u) << "one `exists` probe per candidate per compile";

    // A header written AFTER the first compile listed its directory.
    writeFile(dir / "late.h", "int l;\n");
    // The first compile keeps the view it took — a per-compile snapshot, as gcc's
    // `file_hash` and clang's `SeenFileEntries` keep negative answers too...
    EXPECT_EQ(findInDirs("late.h", dirs, HeaderNameMatching::CaseInsensitive, first).status,
              HeaderSearchStatus::NotFound);
    // ...and the NEXT compile reads the tree again, and sees it.
    HeaderSearchCache second;
    EXPECT_EQ(findInDirs("late.h", dirs, HeaderNameMatching::CaseInsensitive, second).status,
              HeaderSearchStatus::Found)
        << "a later compile answered from an earlier compile's listing — the cache "
           "outlived the compile it belongs to";
    EXPECT_EQ(findInDirs("late.h", dirs, HeaderNameMatching::CaseSensitive, second).status,
              HeaderSearchStatus::Found);

    auto const listed = tally.listingsPerDirectory();
    ASSERT_EQ(listed.size(), 1u) << "exactly the one fixture directory was listed";
    EXPECT_EQ(listed.front().second, 2u)
        << "the directory must be listed once PER COMPILE: twice for two compiles, "
           "never once per search and never once for both";
}

// ── the AMBIGUITY path: fail loud, never pick ────────────────────────────
//
// Under a case-INSENSITIVE policy on a case-SENSITIVE host, a fold can match
// two DISTINCT files. Picking either one (including "prefer the exact match")
// would make the answer depend on the build host, because a case-insensitive
// host cannot hold the pair at all — that is the original defect, one layer
// down. So: fail loud, naming every candidate.
//
// The colliding directory is CONSTRUCTED AT RUNTIME and never committed: a
// case-only pair in the repo would break checkout on exactly the hosts this
// whole change exists to serve.
TEST(HeaderNameMatching, FoldCollisionFailsLoudAndNamesEveryCandidate) {
    HeaderSearchCache cache;   // one compile's view of the tree
    ScratchDir scratch{Location::Temp, "header_case_ambig"};
    fs::path const dir = scratch.path();
    tryMakeDirCaseSensitive(dir);
    if (!hostCanHoldCaseCollidingPair(dir)) {
        // A definite verdict, not silence: on this filesystem the fixture is
        // UNREPRESENTABLE, so the ambiguity branch is unreachable here by
        // construction. Assert the reachable half — the single surviving file
        // still resolves — and let the case-sensitive leg (WSL/Linux ext4)
        // cover the collision itself.
        writeFile(dir / "foo.json");
        EXPECT_EQ(resolveInDir(dir, "Foo.json", HeaderNameMatching::CaseInsensitive, cache)
                      .status,
                  HeaderSearchStatus::Found);
        GTEST_SKIP() << "this filesystem folds case, so a `foo.json`/`Foo.json` "
                        "pair cannot exist on it — the ambiguity branch is "
                        "unreachable here and is pinned on the case-sensitive leg";
    }
    writeFile(dir / "foo.json");
    writeFile(dir / "Foo.json");

    HeaderSearchResult const r =
        resolveInDir(dir, "Foo.json", HeaderNameMatching::CaseInsensitive, cache);
    ASSERT_EQ(r.status, HeaderSearchStatus::AmbiguousCase)
        << "an exact match must NOT win the tie — that answer differs between "
           "ext4 (both files) and NTFS (only one can exist), which is the bug";
    ASSERT_EQ(r.ambiguousCandidates.size(), 2u);
    std::string const msg = headerCaseAmbiguityMessage("Foo.h", r.ambiguousCandidates);
    EXPECT_NE(msg.find("Foo.h"), std::string::npos);
    EXPECT_NE(msg.find("foo.json"), std::string::npos)
        << "the diagnostic must NAME every colliding file: " << msg;
    EXPECT_NE(msg.find("Foo.json"), std::string::npos)
        << "the diagnostic must NAME every colliding file: " << msg;

    // A case-SENSITIVE policy has no ambiguity to report — it wants the bytes
    // it asked for, and exactly one file can carry them.
    EXPECT_EQ(resolveInDir(dir, "Foo.json", HeaderNameMatching::CaseSensitive, cache).status,
              HeaderSearchStatus::Found);
    EXPECT_EQ(resolveInDir(dir, "foo.json", HeaderNameMatching::CaseSensitive, cache).status,
              HeaderSearchStatus::Found);

    // The collision must also stop the ANGLE funnel and the dir-list search,
    // not just the single-dir atom — those are what the compiler actually calls.
    std::vector<fs::path> const dirs{dir};
    EXPECT_EQ(findInDirs("Foo.json", dirs, HeaderNameMatching::CaseInsensitive, cache).status,
              HeaderSearchStatus::AmbiguousCase);
    // The DESCRIPTOR half (systemDirs) — distinct from the source half,
    // because a different tier owns each report.
    EXPECT_EQ(resolveAngleInclude("Foo.h", dirs, {},
                                  HeaderNameMatching::CaseInsensitive, cache).kind,
              AngleIncludeKind::AmbiguousDescriptor);
    // The SOURCE half (-I dirs), reached only when no descriptor matches. It
    // must report a DIFFERENT kind, because the import resolver re-resolves the
    // descriptor half and owns its diagnostic while NOTHING re-resolves this
    // one — collapsing the two is what made a `-I` collision surface as
    // `F_ShippedHeaderNotFound`, naming the wrong defect.
    writeFile(dir / "bar.h", "int b;");
    writeFile(dir / "Bar.h", "int B;");
    // New files are a new compile: the listing above is this test's first
    // compile's snapshot, which by design does not see files written after it.
    HeaderSearchCache afterWrites;
    EXPECT_EQ(resolveAngleInclude("Bar.h", dirs, dirs,
                                  HeaderNameMatching::CaseInsensitive, afterWrites).kind,
              AngleIncludeKind::AmbiguousSource)
        << "no `bar.json` exists, so the descriptor half misses and the search "
           "reaches the -I source half, where the collision lives";
    // CONTROL: a name matching neither half is a plain miss, so the two
    // Ambiguous* verdicts above are attributable to the collisions.
    EXPECT_EQ(resolveAngleInclude("nothing_here.h", dirs, dirs,
                                  HeaderNameMatching::CaseInsensitive, afterWrites).kind,
              AngleIncludeKind::NotFound);
}

// ── repo hygiene: ONE descriptor file must serve EVERY case spelling ─────
//
// The folding resolver makes alias files (`Windows.json` beside `windows.json`)
// unnecessary; this guard makes them impossible to add by accident. It is not a
// style rule: a case-only pair CANNOT be checked out on NTFS or on a default
// APFS/HFS+ volume, so committing one would make the repo work on ext4 and
// nowhere else — and it would be INVISIBLE to a contributor on the host most of
// them use, because their own git client would have silently collapsed it.
TEST(HeaderNameMatching, ShippedConfigTreeHasNoCaseCollidingPaths) {
    // The REAL shipped tree, resolved by the ONE test-side resolver
    // (`repo_root.hpp`) instead of the private cwd walk this file used to carry
    // — that walk found nothing in an OUT-OF-TREE build, whose cwd has no
    // `src/dss-config` in its ancestry. Non-throwing `findRepoRoot()` keeps this
    // guard's existing contract (a miss is an ASSERT here, never an `abort()`
    // that would cost every sibling test in this binary its verdict), and the
    // message now carries all three candidates the resolver actually tried —
    // "walking up from cwd" was only one of them and would send the reader to
    // the wrong half of the system.
    auto const root = dss::test::findRepoRoot();
    ASSERT_TRUE(root.has_value()) << dss::test::repoRootDiagnostic();
    // What a clone never materializes is what git ignores in the tree — build
    // output, a session's scratch, the generated runtime objects under
    // `src/dss-config/runtime/platform/dist/`, the copies DssHarness keeps in a tree —
    // read ONCE from the tree's own `.gitignore` for every root below.
    TreeIgnores const ignores = dss::test_support::readTreeIgnores(*root);
    ASSERT_TRUE(ignores.refusal.empty()) << ignores.refusal;

    // `src/dss-config/**` first (the descriptors this axis resolves), then the
    // WHOLE checked-out tree — the invariant is about CLONING the repo, not
    // just about headers, so the sweep must cover everything a clone
    // materializes, and nothing git ignores (`.git` included).
    for (char const* sub : {"src/dss-config/shippedLibs",
                            "src/dss-config/object-formats",
                            "src/dss-config",
                            "."}) {
        fs::path const dir = *root / sub;
        ASSERT_TRUE(fs::is_directory(dir))
            << sub << " must exist — a guard that silently skips its own "
                      "subject is the vacuous-pass class";
        auto const scan = caseCollisionsUnder(*root, dir, ignores);
        EXPECT_TRUE(scan.refusal.empty())
            << sub << ": the sweep was refused, so it says nothing of the tree: " << scan.refusal;
        EXPECT_TRUE(scan.leftOutRoot.empty())
            << sub << ": the tree's .gitignore leaves the swept root out (" << scan.leftOutRoot << ")";
        // Prove the instrument did work before believing its verdict.
        EXPECT_FALSE(scan.walkError)
            << sub << ": the walk was TRUNCATED (" << scan.walkError.message()
            << ") — a partial tree is not a clean tree";
        EXPECT_GT(scan.filesScanned, 0u)
            << sub << ": scanned ZERO files, so an empty result says nothing";
        // NOT a failure: an entry this host cannot spell is a real limit of the sweep, not a
        // collision. It is REPORTED so a clean verdict cannot quietly mean "clean over the part
        // I could read" — the same reason `filesScanned` is asserted at all.
        if (scan.unrenderable > 0)
            std::cerr << "[ note ] " << sub << ": " << scan.unrenderable
                      << " entr" << (scan.unrenderable == 1 ? "y" : "ies")
                      << " could not be rendered on this host's code page and were not "
                         "compared\n";
        for (auto const& g : scan.groups) {
            std::string joined;
            for (auto const& p : g) { joined += "\n  "; joined += p; }
            ADD_FAILURE() << "case-only path collision under " << sub
                          << " — this tree cannot be checked out on Windows or "
                             "default macOS:" << joined;
        }
        EXPECT_TRUE(scan.groups.empty());
    }
}

// RED-ON-DISABLE control for the guard above, over a scratch tree the test
// builds itself (a committed fixture would break the very hosts the guard
// protects). Skipped where the host cannot represent the collision — the same
// honest verdict the ambiguity test gives.
TEST(HeaderNameMatching, CaseCollisionGuardActuallyDetectsACollision) {
    ScratchDir scratch{Location::Temp, "header_case_guard"};
    fs::path const dir = scratch.path();
    tryMakeDirCaseSensitive(dir);
    if (!hostCanHoldCaseCollidingPair(dir)) {
        GTEST_SKIP() << "this filesystem folds case — a colliding pair cannot be "
                        "constructed here; the control runs on the "
                        "case-sensitive leg";
    }
    writeFile(dir / "nested" / "windows.json");
    TreeIgnores const none;   // the scratch tree has no `.gitignore`: nothing but `.git` is left out
    auto const clean = caseCollisionsUnder(dir, dir, none);
    EXPECT_TRUE(clean.groups.empty())
        << "no collision yet — the guard must not fire on a clean tree";
    EXPECT_EQ(clean.filesScanned, 1u) << "and it must have actually LOOKED";
    EXPECT_FALSE(clean.walkError);
    writeFile(dir / "nested" / "Windows.json");
    auto const scan = caseCollisionsUnder(dir, dir, none);
    EXPECT_FALSE(scan.walkError);
    EXPECT_EQ(scan.filesScanned, 2u);
    ASSERT_EQ(scan.groups.size(), 1u)
        << "the guard must SEE the pair it exists to ban";
    EXPECT_EQ(scan.groups[0].size(), 2u);
    EXPECT_EQ(scan.groups[0][0], "nested/Windows.json");
    EXPECT_EQ(scan.groups[0][1], "nested/windows.json");
}

// CONTROL for an ANCHORED rule of the tree's `.gitignore`, the shape the repo-wide
// guard needs for `src/dss-config/runtime/platform/dist/`.
//
// ★ WHY THIS CANNOT BE LEFT TO THE REAL TREE. That directory is GENERATED, so it is
// ABSENT from a fresh clone and absent on any host that has not built the runtime —
// which means a rule that silently matched nothing would look EXACTLY like a rule
// that worked, in every log this project keeps. That is the vacuous-fix class, one
// step removed from the vacuous-pass class `CaseCollisionScan` already carries a
// denominator for. So the mechanism is exercised over a tree this test builds, in
// BOTH directions, on EXACT counts — emptiness cannot tell "left out" apart from
// "scanned nothing".
//
// It needs no case-colliding pair and therefore no host capability: the claim is
// about WHAT WAS ENUMERATED, which every filesystem can answer.
TEST(HeaderNameMatching, CaseCollisionScanLeavesOutOnlyTheAnchoredPathItsRuleNames) {
    ScratchDir scratch{Location::Temp, "header_case_prune"};
    fs::path const dir = scratch.path();
    writeFile(dir / ".gitignore", "/generated/\n");
    writeFile(dir / "kept.json");
    writeFile(dir / "kept-nested" / "also-kept.json");
    writeFile(dir / "generated" / "one.o");
    writeFile(dir / "generated" / "deeper" / "two.o");
    // ★ A SECOND DIRECTORY OF THE SAME BARE NAME, at a different path — the whole
    // reason the rule is anchored. Read as a bare name it would take this one out
    // too, deleting coverage of a tree that IS cloned, and the guard would still
    // report a clean pass.
    writeFile(dir / "kept-nested" / "generated" / "three.o");

    auto const all = caseCollisionsUnder(dir, dir, TreeIgnores{});
    EXPECT_FALSE(all.walkError);
    EXPECT_EQ(all.filesScanned, 6u)
        << "UNPRUNED direction: the walk must see all six files, including the "
           "two under `<dir>/generated/`. Without this the count below could be "
           "explained by the files never existing";
    EXPECT_TRUE(all.groups.empty());

    TreeIgnores const ignores = dss::test_support::readTreeIgnores(dir);
    ASSERT_TRUE(ignores.refusal.empty()) << ignores.refusal;
    auto const pruned = caseCollisionsUnder(dir, dir, ignores);
    EXPECT_FALSE(pruned.walkError);
    EXPECT_EQ(pruned.filesScanned, 4u)
        << "LEFT-OUT direction: exactly `<dir>/generated/one.o` and "
           "`<dir>/generated/deeper/two.o` drop out. `.gitignore`, `kept.json`, "
           "`kept-nested/also-kept.json` and `kept-nested/generated/three.o` "
           "remain — the last one is the anchoring proof";
    EXPECT_TRUE(pruned.groups.empty());
}

// ...and the rule must hold through the root spelling the repo-wide guard actually
// uses. `*root / "."` is one of that guard's four roots, and `.` is a REAL element
// of `fs::path` iteration, so every entry it yields carries a `./`. Judged as
// written, it fails HERE AND NOWHERE ELSE: the three narrow roots would keep their
// rules while the whole-repo root — the only one that reaches every directory the
// rules leave out — silently walked them. The scan with no rules is the control that
// attributes the count to the rule rather than to the dot-spelled root breaking the walk.
TEST(HeaderNameMatching, CaseCollisionScanLeavesOutThroughADotSpelledRoot) {
    ScratchDir scratch{Location::Temp, "header_case_prune_dot"};
    fs::path const dir = scratch.path();
    writeFile(dir / ".gitignore", "/generated/\n");
    writeFile(dir / "kept.json");
    writeFile(dir / "generated" / "one.o");

    auto const all = caseCollisionsUnder(dir, dir / ".", TreeIgnores{});
    EXPECT_FALSE(all.walkError);
    EXPECT_EQ(all.filesScanned, 3u)
        << "a `.` component must not disturb the walk itself";

    auto const pruned = caseCollisionsUnder(dir, dir / ".", dss::test_support::readTreeIgnores(dir));
    EXPECT_FALSE(pruned.walkError);
    EXPECT_EQ(pruned.filesScanned, 2u)
        << "with an as-written compare this reads 3, and the repo-wide guard "
           "walks the generated tree its rule names";
}

// THE COPIES DSSHARNESS KEEPS IN A TREE, A SESSION'S SCRATCH, AND A BUILD, beside the
// real files: the sweep enumerates exactly what a clone holds. The tree's `.gitignore`
// is written in the shapes the repository's carries — the tool's managed block (each
// directory's CONTENTS left out, its placeholder kept), a bare scratch name, a build
// directory — and the files in each would be swept twice, or vanish under the walk,
// were they read.
TEST(HeaderNameMatching, CaseCollisionScanLeavesOutTheCopiesTheToolKeepsAScratchAndABuild) {
    ScratchDir scratch{Location::Temp, "header_case_harness_dirs"};
    fs::path const dir = scratch.path();
    writeFile(dir / ".gitignore",
              ".temp/\nbuild/\n/.worktrees/*\n!/.worktrees/.gitkeep\n/.orchestrators/*\n!/.orchestrators/.gitkeep\n"
              "/.mutations/*\n!/.mutations/.gitkeep\n/.legs/*\n!/.legs/.gitkeep\n");
    for (char const* real : {"examples/c/x/expected.json", "src/a.json", ".legs/.gitkeep", ".mutations/.gitkeep"})
        writeFile(dir / real);
    for (char const* copy : {".legs/x/examples/c/x/expected.json", ".mutations/aw-1/src/a.json",
                             ".temp/x/examples/c/x/expected.json", ".worktrees/lane/src/a.json",
                             ".orchestrators/o/work/a/src/a.json", "build/v1/bin/dss/runner.json",
                             "tests/.temp/y.json"})
        writeFile(dir / copy);

    auto const all = caseCollisionsUnder(dir, dir, TreeIgnores{});
    EXPECT_EQ(all.filesScanned, 12u) << "the control: with no rule every file is swept";

    auto const scan = caseCollisionsUnder(dir, dir, dss::test_support::readTreeIgnores(dir));
    EXPECT_TRUE(scan.refusal.empty()) << scan.refusal;
    EXPECT_FALSE(scan.walkError);
    std::vector<std::string> swept = scan.paths;
    std::sort(swept.begin(), swept.end());
    EXPECT_EQ(swept, (std::vector<std::string>{".gitignore", ".legs/.gitkeep", ".mutations/.gitkeep",
                                               "examples/c/x/expected.json", "src/a.json"}))
        << "a leg copy, a mutation worker, a lane's checkout, a work copy, a session's scratch "
           "(at the root and below) and a build are what git ignores here: none is swept";
}

// THE SWEEP NEVER DESCENDS A SYMBOLIC LINK AND COUNTS IT AS THE FILE A CLONE HOLDS. git records a
// link as a file, so a `build/` rule does not take a link named `build` (✔MEASURED, git 2.55), and
// a clone materializes the link itself — a path that collides like any other. A walk through it
// would read another tree, or this one twice.
TEST(HeaderNameMatching, CaseCollisionScanNeverDescendsASymbolicLinkAndCountsIt) {
    ScratchDir scratch{Location::Temp, "header_case_links"};
    fs::path const dir = scratch.path();
    writeFile(dir / ".gitignore", "build/\n");
    writeFile(dir / "real" / "x.json");
    // Made by the OS (`os_link.hpp`): MinGW's library cannot make a link, and that build is the one
    // whose library cannot see one either.
    using dss::test_support::LinkTo;
    std::error_code ec = dss::test_support::makeSymbolicLink(dir / "real", dir / "build", LinkTo::Directory);
    if (!ec) ec = dss::test_support::makeSymbolicLink(dir / "real", dir / "elsewhere", LinkTo::Directory);
    ASSERT_FALSE(ec) << "a symbolic link cannot be made on this host (" << ec.message()
                     << "): the case did not run";
    auto const scan = caseCollisionsUnder(dir, dir, dss::test_support::readTreeIgnores(dir));
    EXPECT_TRUE(scan.refusal.empty()) << scan.refusal;
    EXPECT_FALSE(scan.walkError);
    std::vector<std::string> swept = scan.paths;
    std::sort(swept.begin(), swept.end());
    EXPECT_EQ(swept, (std::vector<std::string>{".gitignore", "build", "elsewhere", "real/x.json"}))
        << "the scan swept " << swept.size()
        << " of the 4 files: each link is swept as the file it is, and nothing behind it is read";
}

// ── the C++ reader of the one statement (`tests/test_support/tree_ignores.hpp`) ──
// THE ONE CASE TABLE, `tests/test_support/tree_ignores_cases.json`, states every scenario: its
// rules and each path's kind and answer, or the line a reader refuses. A case below asks one
// scenario of THIS reader, on paths alone, so every host answers alike; owning-tree.py's self-test
// asks the same scenario of the Python reader, and each suite's LIVE case asks git itself.

namespace {

using dss::test_support::TreeIgnoreRefusal;

// Every scenario a case below asks: the table's names, compared with the table by
// `EveryScenarioOfTheCaseTableIsAskedByOneCase` (a scenario added there and asked here by no case is red).
constexpr char const* kScenarios[] = {
    "the copies the tool keeps in a tree, a session's scratch and a build",
    "a later rule decides, and a negation re-includes",
    "a rule with no slash matches that name at any depth",
    "a rule with a slash is anchored to the tree's root",
    "a rule ending in a slash matches a directory only",
    "a rule ending in a slash never matches a symbolic link",
    "a double-star name is any number of directories",
    "a star, a question mark and a class match the name's bytes",
    "the twelve POSIX classes read as git reads them",
    "a dash, a bracket first, a range and a lone [: inside a class read as git reads them",
    "nothing under a left-out directory is re-included",
    "git's own directory is always left out, as a directory or a worktree's file",
    "a path a rule matches under one setting of core.ignorecase and not the other is refused",
    "a name that is git's own only when case is folded is refused",
    "a name holding a backslash is refused",
    "a byte order mark, a CR LF ending and trailing spaces are no part of a rule, a trailing tab is",
    "a rule holding a backslash is refused, naming its line",
    "a class opened and never closed is refused, naming its line",
    "a POSIX class git does not name is refused, naming its line",
    "a rule naming nothing between two separators, or nothing at all, is refused",
};

[[nodiscard]] nlohmann::json const& caseTable() {
    static nlohmann::json const table = [] {
        auto const root = dss::test::findRepoRoot();
        if (!root) throw std::runtime_error("no repository root, so no case table: " + dss::test::repoRootDiagnostic());
        std::ifstream in(*root / "tests" / "test_support" / "tree_ignores_cases.json", std::ios::binary);
        if (!in) throw std::runtime_error("tests/test_support/tree_ignores_cases.json cannot be read");
        return nlohmann::json::parse(in);
    }();
    return table;
}

[[nodiscard]] nlohmann::json const& scenario(std::string const& name) {
    for (auto const& s : caseTable().at("scenarios"))
        if (s.at("name").get<std::string>() == name) return s;
    throw std::runtime_error("the case table holds no scenario named '" + name + "'");
}

[[nodiscard]] std::string rulesText(nlohmann::json const& sc) {
    std::string const nl = sc.contains("newline") ? sc.at("newline").get<std::string>() : std::string("\n");
    std::string       text;
    for (auto const& r : sc.at("rules")) text += r.get<std::string>() + nl;
    return text;
}

// This reader's answer for one path, in the table's words.
struct Answer {
    bool        refused = false;
    std::string text;   // the rule that leaves it out ("" kept), or the refusal
};

[[nodiscard]] Answer ask(TreeIgnores const& t, std::string const& path, bool isDir) {
    try {
        return {false, t.leavesOut(path, isDir)};
    } catch (TreeIgnoreRefusal const& refused) {
        return {true, refused.what()};
    }
}

[[nodiscard]] std::string render(Answer const& a) {
    if (a.refused) return "refused (" + a.text.substr(0, 140) + ")";
    return a.text.empty() ? std::string("kept") : "left out by " + a.text;
}

[[nodiscard]] std::string render(nlohmann::json const& answer) {
    if (answer.is_null()) return "kept";
    if (answer.is_object()) return "refused (" + answer.at("refused").get<std::string>() + ")";
    return "left out by " + answer.get<std::string>();
}

[[nodiscard]] bool agrees(nlohmann::json const& answer, Answer const& got) {
    if (answer.is_object()) return got.refused && got.text.find(answer.at("refused").get<std::string>()) != std::string::npos;
    return !got.refused && got.text == (answer.is_null() ? std::string{} : answer.get<std::string>());
}

// Every path of one scenario asked of this reader.
void expectAnswers(std::string const& name) {
    auto const&       sc = scenario(name);
    TreeIgnores const t = dss::test_support::readTreeIgnoreRules(rulesText(sc), "fixture-ignore");
    ASSERT_TRUE(t.refusal.empty()) << t.refusal;
    for (auto const& p : sc.at("paths")) {
        std::string const path = p.at(0).get<std::string>();
        std::string const kind = p.at(1).get<std::string>();
        Answer const      got = ask(t, path, kind == "dir");
        EXPECT_TRUE(agrees(p.at(2), got))
            << path << " (" << kind << "): answered " << render(got) << ", the table says " << render(p.at(2));
    }
}

// Every rule set of one scenario read by this reader: refused at the line the table names, saying
// what it says, or read whole where the table names no line.
void expectRefusals(std::string const& name) {
    for (auto const& r : scenario(name).at("refusals")) {
        std::string text, shown;
        for (auto const& line : r.at("rules")) {
            text += line.get<std::string>() + "\n";
            shown += (shown.empty() ? "" : " ; ") + line.get<std::string>();
        }
        std::string const refusal = dss::test_support::readTreeIgnoreRules(text, "fixture-ignore").refusal;
        if (r.at("line").is_null()) {
            EXPECT_TRUE(refusal.empty()) << "[" << shown << "] was refused: " << refusal;
        } else {
            int const         line = r.at("line").get<int>();
            std::string const says = r.at("says").get<std::string>();
            EXPECT_TRUE(refusal.find("fixture-ignore line " + std::to_string(line) + ":") != std::string::npos
                        && refusal.find(says) != std::string::npos)
                << "[" << shown << "] was " << (refusal.empty() ? std::string("read whole") : "refused: " + refusal)
                << ", the table says refused at line " << line << " for " << says;
        }
    }
}

} // namespace

TEST(TreeIgnoresGrammar, TheCopiesTheToolKeepsAScratchAndABuildAreLeftOut) { expectAnswers(kScenarios[0]); }
TEST(TreeIgnoresGrammar, ALaterRuleDecidesAndANegationReincludes) { expectAnswers(kScenarios[1]); }
TEST(TreeIgnoresGrammar, ARuleWithNoSlashMatchesThatNameAtAnyDepth) { expectAnswers(kScenarios[2]); }
TEST(TreeIgnoresGrammar, ARuleWithASlashIsAnchoredToTheTreesRoot) { expectAnswers(kScenarios[3]); }
TEST(TreeIgnoresGrammar, ATrailingSlashMatchesADirectoryOnly) { expectAnswers(kScenarios[4]); }
TEST(TreeIgnoresGrammar, ATrailingSlashNeverMatchesASymbolicLink) { expectAnswers(kScenarios[5]); }
TEST(TreeIgnoresGrammar, ADoubleStarNameIsAnyNumberOfDirectories) { expectAnswers(kScenarios[6]); }
TEST(TreeIgnoresGrammar, AStarAQuestionMarkAndAClassMatchTheNamesBytes) { expectAnswers(kScenarios[7]); }
TEST(TreeIgnoresGrammar, TheTwelvePosixClassesReadAsGitReadsThem) { expectAnswers(kScenarios[8]); }
TEST(TreeIgnoresGrammar, ADashABracketFirstARangeAndALoneColonOpenerReadAsGitReadsThem) {
    expectAnswers(kScenarios[9]);
}
TEST(TreeIgnoresGrammar, NothingUnderALeftOutDirectoryIsReincluded) { expectAnswers(kScenarios[10]); }
TEST(TreeIgnoresGrammar, GitsOwnIsAlwaysLeftOutAsADirectoryOrAWorktreesFile) { expectAnswers(kScenarios[11]); }
TEST(TreeIgnoresGrammar, APathARuleMatchesUnderOneSettingOfIgnoreCaseAndNotTheOtherIsRefused) {
    expectAnswers(kScenarios[12]);
}
TEST(TreeIgnoresGrammar, ANameThatIsGitsOwnOnlyWhenCaseIsFoldedIsRefused) { expectAnswers(kScenarios[13]); }
TEST(TreeIgnoresGrammar, ANameHoldingABackslashIsRefused) { expectAnswers(kScenarios[14]); }
TEST(TreeIgnoresGrammar, AByteOrderMarkACrLfEndingAndTrailingSpacesAreNoPartOfARule) { expectAnswers(kScenarios[15]); }
TEST(TreeIgnoresGrammar, ARuleHoldingABackslashIsRefusedNamingItsLine) { expectRefusals(kScenarios[16]); }
TEST(TreeIgnoresGrammar, AClassNeverClosedIsRefusedNamingItsLine) { expectRefusals(kScenarios[17]); }
TEST(TreeIgnoresGrammar, APosixClassGitDoesNotNameIsRefusedNamingItsLine) { expectRefusals(kScenarios[18]); }
TEST(TreeIgnoresGrammar, ARuleNamingNothingIsRefusedNamingItsLine) { expectRefusals(kScenarios[19]); }

TEST(TreeIgnoresGrammar, EveryScenarioOfTheCaseTableIsAskedByOneCase) {
    std::vector<std::string> named, asked(std::begin(kScenarios), std::end(kScenarios));
    for (auto const& s : caseTable().at("scenarios")) named.push_back(s.at("name").get<std::string>());
    std::sort(named.begin(), named.end());
    std::sort(asked.begin(), asked.end());
    EXPECT_EQ(asked, named) << "a scenario of the table that no case asks, or a case asking none";
}

// A `.gitignore` that is a DIRECTORY is refused, as the Python reader refuses it: a reader that
// opened it and read nothing would leave out nothing, in silence.
TEST(TreeIgnoresGrammar, AGitignoreThatIsADirectoryIsRefused) {
    ScratchDir scratch{Location::Temp, "tree_ignores_dir_gitignore"};
    fs::create_directories(scratch.path() / ".gitignore");
    TreeIgnores const t = dss::test_support::readTreeIgnores(scratch.path());
    EXPECT_NE(t.refusal.find("it is a directory"), std::string::npos) << "read as: '" << t.refusal << "'";
}

// LIVE: git itself, over every scenario's paths. One repository, each scenario in its own directory
// whose `.gitignore` holds the scenario's rules (anchored to that directory exactly as a root rule
// is to the root); a `dir` leaf made a directory and a `link` leaf a link (git decides a path it
// cannot stat as no directory, so a `file` leaf needs no file). Asked under both settings of
// core.ignorecase, with no excludes file of the host's and none of the caller's git environment. A
// path the table refuses for case must be one git answers differently under the two; every other
// answered path must be this reader's answer under both. No git on the host is a failure, never a pass.
// git's C-style quoting of a path undone: a name holding a control byte, a double quote, a backslash
// or (under core.quotePath) a byte at or above 0x80 is written between double quotes, with \a \b \t
// \n \v \f \r \" \\ and three octal digits for every other byte it quotes; any other name is as is.
namespace {
[[nodiscard]] std::string gitUnquoted(std::string const& s) {
    if (s.size() < 2 || s.front() != '"' || s.back() != '"') return s;
    std::string out;
    for (std::size_t i = 1; i + 1 < s.size(); ++i) {
        if (s[i] != '\\' || i + 2 >= s.size()) {
            out += s[i];
            continue;
        }
        char const e = s[++i];
        switch (e) {
            case 'a': out += '\a'; break;
            case 'b': out += '\b'; break;
            case 't': out += '\t'; break;
            case 'n': out += '\n'; break;
            case 'v': out += '\v'; break;
            case 'f': out += '\f'; break;
            case 'r': out += '\r'; break;
            default:
                if (e >= '0' && e <= '3' && i + 3 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7' && s[i + 2] >= '0'
                    && s[i + 2] <= '7') {
                    out += static_cast<char>(((e - '0') << 6) | ((s[i + 1] - '0') << 3) | (s[i + 2] - '0'));
                    i += 2;
                } else {
                    out += e;   // `\"` and `\\`
                }
        }
    }
    return out;
}
} // namespace

TEST(TreeIgnoresGrammar, LiveEveryScenarioIsAnsweredAsGitCheckIgnoreAnswersIt) {
    auto const gitExe = dss::substrate::resolveExecutableOnPath("git");
    ASSERT_TRUE(gitExe.has_value()) << "no git on this host: the live comparison did not run, which is no pass";
    ScratchDir     scratch{Location::Temp, "tree_ignores_live_git"};
    fs::path const repo = scratch.path() / "repo";
    fs::path const target = scratch.path() / "link-target";
    fs::create_directories(repo);
    fs::create_directories(target);
    auto const& withheld = dss::gitRepositoryLocalVariables();
    {
        auto const init = dss::substrate::spawnAndWaitRedirectStdout({narrowUtf8OrEmpty(*gitExe), "init", "-q"}, repo,
                                                                     scratch.path() / "init.out", withheld);
        ASSERT_TRUE(init.spawned && init.exitCode == 0) << init.diagnostic;
    }
    struct Asked {
        std::string           scenario, path, kind, asAsked;
        nlohmann::json        answer;
        TreeIgnores const*    reader;
    };
    std::vector<Asked>       asked;
    std::vector<TreeIgnores> readers;
    auto const&              scenarios = caseTable().at("scenarios");
    readers.reserve(scenarios.size());
    for (std::size_t k = 0; k < scenarios.size(); ++k) {
        auto const& sc = scenarios[k];
        if (!sc.contains("paths") || (sc.contains("asked_of_git") && !sc.at("asked_of_git").get<bool>())) continue;
        std::string const top = "s" + std::to_string(k);
        fs::create_directories(repo / top);
        writeFile(repo / top / ".gitignore", rulesText(sc));
        readers.push_back(dss::test_support::readTreeIgnoreRules(rulesText(sc), "fixture-ignore"));
        for (auto const& p : sc.at("paths")) {
            std::string const path = p.at(0).get<std::string>();
            std::string const kind = p.at(1).get<std::string>();
            if (p.at(2).is_object() && p.at(2).at("refused").get<std::string>() != "core.ignorecase") continue;
            std::u8string const u8path(path.begin(), path.end());
            fs::path const      full = repo / top / fs::path(u8path);
            std::error_code     ec;
            if (kind == "dir") {
                fs::create_directories(full, ec);
            } else if (kind == "link") {
                fs::create_directories(full.parent_path(), ec);
                if (!ec) ec = dss::test_support::makeSymbolicLink(target, full, dss::test_support::LinkTo::Directory);
            }
            ASSERT_FALSE(ec) << "cannot make " << path << " (" << kind << ") on this host (" << ec.message()
                             << "): the live comparison did not run";
            asked.push_back({sc.at("name").get<std::string>(), path, kind, top + "/" + path, p.at(2), &readers.back()});
        }
    }
    ASSERT_FALSE(asked.empty());
    // The paths go on the command line: git takes `-z` only with `--stdin` (it refuses `-z` beside
    // named paths), and the spawner hands a child this process's own standard input. So each answer
    // is git's line form: `<source>:<line>:<pattern>` (`::` when no rule matched), a tab, and the
    // path -- written C-quoted whenever it holds a byte git quotes, which a quoted path's tab never
    // is; the pattern is written as it stands and may hold a tab, so the LAST tab ends it.
    std::map<std::string, std::map<std::string, std::string>> said;   // setting -> path -> pattern ("" none)
    for (std::string const fold : {"false", "true"}) {
        std::vector<std::string> argv{narrowUtf8OrEmpty(*gitExe), "-c", "core.ignorecase=" + fold, "-c",
                                      "core.excludesFile=" + narrowUtf8OrEmpty(scratch.path() / "no-excludes-file"),
                                      "-c", "core.quotePath=true", "check-ignore", "--no-index", "-v", "-n", "--"};
        for (auto const& a : asked) argv.push_back(a.asAsked);
        fs::path const out = scratch.path() / ("check-ignore-" + fold + ".out");
        auto const     r = dss::substrate::spawnAndWaitRedirectStdout(argv, repo, out, withheld);
        ASSERT_TRUE(r.spawned && (r.exitCode == 0 || r.exitCode == 1)) << r.diagnostic << " exit " << r.exitCode;
        std::ifstream     in(out, std::ios::binary);
        std::string const bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        std::size_t       at = 0;
        while (at < bytes.size()) {
            std::size_t const  eol = bytes.find('\n', at);
            std::string const  line = bytes.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
            at = eol == std::string::npos ? bytes.size() : eol + 1;
            std::size_t const  tab = line.rfind('\t');
            ASSERT_NE(tab, std::string::npos) << "a line of git check-ignore with no tab: " << line;
            std::string const  head = line.substr(0, tab);
            std::size_t const  c1 = head.find(':');
            std::size_t const  c2 = c1 == std::string::npos ? c1 : head.find(':', c1 + 1);
            ASSERT_NE(c2, std::string::npos) << "a line of git check-ignore with no source and line: " << line;
            std::string const  pattern = head.substr(c2 + 1);
            said[fold][gitUnquoted(line.substr(tab + 1))] =
                (pattern.empty() || pattern.front() == '!') ? std::string{} : pattern;
        }
    }
    for (auto const& a : asked) {
        auto const   gf = said["false"].count(a.asAsked) ? said["false"][a.asAsked] : std::string("<unanswered>");
        auto const   gt = said["true"].count(a.asAsked) ? said["true"][a.asAsked] : std::string("<unanswered>");
        Answer const mine = ask(*a.reader, a.path, a.kind == "dir");
        if (a.answer.is_object()) {
            EXPECT_TRUE(mine.refused && gf != gt)
                << a.path << " (" << a.kind << "): this reader " << render(mine) << ", git '" << gf
                << "' under false and '" << gt << "' under true";
        } else {
            EXPECT_TRUE(!mine.refused && mine.text == gf && gf == gt)
                << a.path << " (" << a.kind << "): this reader " << render(mine) << ", git '" << gf
                << "' under false and '" << gt << "' under true";
        }
    }
}

// LIVE: the repository's own `.gitignore` leaves out every directory DssHarness keeps
// in a tree and keeps each one's placeholder — the statement the sweep above relies on.
TEST(TreeIgnoresGrammar, ThisRepositorysGitignoreLeavesOutEveryDirectoryTheToolKeepsInATree) {
    auto const root = dss::test::findRepoRoot();
    ASSERT_TRUE(root.has_value()) << dss::test::repoRootDiagnostic();
    TreeIgnores const t = dss::test_support::readTreeIgnores(*root);
    ASSERT_TRUE(t.refusal.empty()) << t.refusal;
    for (char const* kept : {".worktrees", ".orchestrators", ".mutations", ".legs"}) {
        EXPECT_FALSE(t.leavesOut(std::string(kept) + "/x", true).empty()) << kept << " is walked";
        EXPECT_EQ(t.leavesOut(std::string(kept) + "/.gitkeep", false), "") << kept << "'s placeholder is left out";
    }
}

// `.` and `..` are NAVIGATION components, not filenames — no directory listing
// contains an entry called `..`, so a resolver that ran them through the
// name-matcher would turn every `#include "../shared.h"` into a miss. Caught by
// the post-implementation read of `descend`, NOT by the corpus: the existing
// tests happen not to use a `..` include, so this would have shipped green.
TEST(HeaderNameMatching, DotAndDotDotComponentsNavigateRatherThanMatch) {
    HeaderSearchCache cache;   // one compile's view of the tree
    ScratchDir scratch{Location::Temp, "header_case_dots"};
    fs::path const root = scratch.path();
    writeFile(root / "shared.h", "int s;\n");
    writeFile(root / "inc" / "here.h", "int h;\n");
    fs::path const inc = root / "inc";

    for (auto m : {HeaderNameMatching::CaseSensitive,
                   HeaderNameMatching::CaseInsensitive}) {
        EXPECT_EQ(resolveInDir(inc, "../shared.h", m, cache).status,
                  HeaderSearchStatus::Found)
            << "`..` must navigate up, not be matched as a filename";
        EXPECT_EQ(resolveInDir(inc, "./here.h", m, cache).status,
                  HeaderSearchStatus::Found)
            << "`.` must be a no-op component";
        EXPECT_EQ(resolveInDir(root, "inc/../shared.h", m, cache).status,
                  HeaderSearchStatus::Found);
        // ...and the case rule still governs the components that DO name files.
        EXPECT_EQ(resolveInDir(inc, "../Shared.h", m, cache).status,
                  m == HeaderNameMatching::CaseInsensitive
                      ? HeaderSearchStatus::Found
                      : HeaderSearchStatus::NotFound)
            << "navigating through `..` must not switch off the case policy";
    }
}

// ── D-PP-HEADER-CASE-NON-ASCII-NAME-NARROWING-THROW ──────────────────────
//
// The policy walker compares NAMES, and its first cut compared them after
// `fs::path::string()` — a wide->narrow conversion the pre-policy
// `fs::exists(dir / rel)` never performed, because it never had to LOOK at an
// on-disk name in order to answer. MEASURED, one Windows host, ACP 1252:
//   * MS STL 14.51 — `.string()` THROWS `std::system_error` ("no mapping for
//     the Unicode character in the target multi-byte code page") for ANY entry
//     the active code page cannot represent. A plain CJK filename does it.
//   * libstdc++ 13.2, the MinGW gate toolchain — THROWS `filesystem_error`
//     ("Cannot convert character sequence") for an entry NTFS holds happily but
//     UTF-16 cannot encode: a lone surrogate, which `std::ofstream` creates
//     without complaint (MEASURED, hence `makeUnnarrowableEntry` above).
//   * The two disagree on the RESULT for entries that DO convert (CP1252 `EF`
//     vs UTF-8 `C3 AF` for the same file), so a narrowed comparison makes
//     header resolution depend on which C++ compiler built DSS.
// `main()` installs no handler, so a throw is a process ABORT out of the
// PREPROCESSOR — neither a diagnostic nor fail-loud.
//
// ★ WHICH LEG PROVES WHAT. On a POSIX host `path::value_type` IS `char`, so
// `string()` is the identity and none of these pins can go red there: the Linux
// legs prove the fix is AGNOSTIC (it compiles and behaves identically where the
// conversion is a no-op), and the WINDOWS leg is the one that proves it WORKS.

TEST(HeaderNameMatching, NonAsciiHeaderNamesResolveAndStayDistinct) {
    HeaderSearchCache cache;   // one compile's view of the tree
    ScratchDir scratch{Location::Temp, "header_case_utf8"};
    fs::path const dir = scratch.path();

    // The fixtures are created FROM THE REQUEST BYTES, so the test asserts
    // nothing about how a given toolchain maps narrow source bytes onto native
    // filenames — the on-disk name and the requested name are the same object
    // whichever mapping is in force. What is under test is the resolver's
    // treatment of the ENTRY.
    fs::path const naive{u8Bytes(kNaive)};
    fs::path const zhong{u8Bytes(kZhong)};
    fs::path const nihon{u8Bytes(kNihon)};
    writeFile(dir / naive);
    writeFile(dir / zhong);
    writeFile(dir / nihon);
    writeFile(dir / "plain.json");

    for (auto m : {HeaderNameMatching::CaseSensitive,
                   HeaderNameMatching::CaseInsensitive}) {
        SCOPED_TRACE(headerNameMatchingName(m));

        HeaderSearchResult const r = resolveInDir(dir, u8Bytes(kZhong), m, cache);
        ASSERT_EQ(r.status, HeaderSearchStatus::Found)
            << "a non-ASCII header name must resolve, not abort the process";
        // ★ THE DISTINCTNESS PIN, and the reason this fix is justified even if
        // the throw above were somehow benign. A LOSSY narrowing collapses two
        // DIFFERENT on-disk names onto the same bytes, and the resolver then
        // hands back the WRONG FILE — or invents an ambiguity — SILENTLY.
        // Compared on `native()`, because narrowing them HERE is the very thing
        // under test.
        EXPECT_EQ(r.path.filename().native(), zhong.native());
        EXPECT_NE(r.path.filename().native(), nihon.native());

        HeaderSearchResult const other = resolveInDir(dir, u8Bytes(kNihon), m, cache);
        ASSERT_EQ(other.status, HeaderSearchStatus::Found);
        EXPECT_EQ(other.path.filename().native(), nihon.native())
            << "two distinct non-ASCII names must not resolve to one file";

        EXPECT_EQ(resolveInDir(dir, u8Bytes(kNaive), m, cache).status,
                  HeaderSearchStatus::Found);
        // CONTROL: a non-ASCII name that is NOT there is a plain miss — never a
        // crash, and never a fold onto its neighbour.
        EXPECT_EQ(resolveInDir(dir, "x" + u8Bytes(kNihon), m, cache).status,
                  HeaderSearchStatus::NotFound);
        // ...and the ASCII control, so the verdicts above are attributable to
        // the non-ASCII names rather than to the directory.
        EXPECT_EQ(resolveInDir(dir, "plain.json", m, cache).status,
                  HeaderSearchStatus::Found);
    }

    // The ASCII fold must still govern the ASCII PART of a non-ASCII name. This
    // is what makes generalising the ONE folding helper to the native character
    // type load-bearing rather than cosmetic: the helper now folds a string
    // whose other code units are not ASCII at all, and must leave them alone.
    std::string upperExt = u8Bytes(kZhong);
    upperExt.replace(upperExt.size() - 5, 5, ".JSON");
    EXPECT_EQ(resolveInDir(dir, upperExt, HeaderNameMatching::CaseInsensitive, cache).status,
              HeaderSearchStatus::Found);
    EXPECT_EQ(resolveInDir(dir, upperExt, HeaderNameMatching::CaseSensitive, cache).status,
              HeaderSearchStatus::NotFound);
}

// ★ THE SURPRISING SHAPE, and the one that proves the ENUMERATION path: the
// requested header is ordinary ASCII and the exotic file is an INNOCENT
// BYSTANDER sharing its directory. Under the case-insensitive policy every
// lookup enumerates the whole directory unconditionally, and the case-sensitive
// arm enumerates too in order to verify the real on-disk spelling — so ONE
// unrepresentable entry anywhere on the include path used to take down every
// lookup that passed through that directory, including lookups of headers that
// have nothing to do with it.
TEST(HeaderNameMatching, AnUnnarrowableSiblingDoesNotBreakAnAsciiLookup) {
    HeaderSearchCache cache;   // one compile's view of the tree
    ScratchDir scratch{Location::Temp, "header_case_sibling"};
    fs::path const dir = scratch.path();
    writeFile(dir / "plain.json");
    writeFile(dir / fs::path{u8Bytes(kZhong)});
    std::optional<fs::path> const unnarrowable = makeUnnarrowableEntry(dir);
    // A definite verdict about the fixture, never silence about it.
    SCOPED_TRACE(unnarrowable
                     ? "an UNNARROWABLE sibling is present (native char type "
                       "can hold a lone surrogate and the host accepted it)"
                     : "this host has no unnarrowable name — the well-formed "
                       "non-ASCII sibling alone covers the enumeration walk");
    std::vector<fs::path> const dirs{dir};

    for (auto m : {HeaderNameMatching::CaseSensitive,
                   HeaderNameMatching::CaseInsensitive}) {
        SCOPED_TRACE(headerNameMatchingName(m));
        EXPECT_EQ(resolveInDir(dir, "plain.json", m, cache).status,
                  HeaderSearchStatus::Found)
            << "an ASCII lookup must survive an exotic BYSTANDER in the same "
               "directory — the resolver has no business narrowing entries it "
               "is not being asked about";
        EXPECT_EQ(findInDirs("plain.json", dirs, m, cache).status,
                  HeaderSearchStatus::Found);
        // A MISS still has to enumerate the whole directory, so it hits the
        // bystander on every entry rather than stopping early at a hit.
        EXPECT_EQ(resolveInDir(dir, "absent.json", m, cache).status,
                  HeaderSearchStatus::NotFound);
    }
    EXPECT_EQ(resolveInDir(dir, "Plain.json", HeaderNameMatching::CaseInsensitive, cache)
                  .status,
              HeaderSearchStatus::Found);
    EXPECT_EQ(resolveInDir(dir, "Plain.json", HeaderNameMatching::CaseSensitive, cache)
                  .status,
              HeaderSearchStatus::NotFound);
    // And through the angle funnel, which is what the preprocessor calls.
    EXPECT_EQ(resolveAngleInclude("Plain.h", dirs, dirs,
                                  HeaderNameMatching::CaseInsensitive, cache).kind,
              AngleIncludeKind::Descriptor);
}

// The fail-loud path must not itself fail. A collision report exists to NAME
// the colliding files, so it is precisely the names that are hardest to render
// that it must render — and it must render them the same way on every
// toolchain, or the diagnostic text becomes another host-dependent answer.
TEST(HeaderNameMatching, AmbiguityDiagnosticRendersNonAsciiCandidates) {
    // Synthesized candidates, so this runs on EVERY host — including the ones
    // that cannot hold a colliding pair at all.
    std::vector<fs::path> const candidates{fs::path{std::u8string{kZhong}},
                                           fs::path{std::u8string{kNihon}}};
    std::string msg;
    ASSERT_NO_THROW(msg = headerCaseAmbiguityMessage("Foo.h", candidates));
    EXPECT_NE(msg.find(u8Bytes(kZhong)), std::string::npos)
        << "a non-ASCII candidate must be named, in UTF-8, on every toolchain: "
        << msg;
    EXPECT_NE(msg.find(u8Bytes(kNihon)), std::string::npos) << msg;

    // ...and a name that is not well-formed text in ANY encoding still has to
    // be IDENTIFIABLE and still has to stay DISTINCT from its neighbour, or the
    // report is a lie about which files collided.
    ScratchDir scratch{Location::Temp, "header_case_diag_utf8"};
    auto const odd = makeUnnarrowableEntry(scratch.path());
    SCOPED_TRACE(odd ? "unnarrowable candidate exercised"
                     : "host has no unnarrowable name; UTF-8 arm only");
    if (odd) {
        std::vector<fs::path> const two{*odd, scratch.path() / "plain.json"};
        std::string oddMsg;
        ASSERT_NO_THROW(oddMsg = headerCaseAmbiguityMessage("Foo.h", two));
        EXPECT_NE(oddMsg.find("lone"), std::string::npos)
            << "the unrenderable candidate must still be identifiable: " << oddMsg;
        EXPECT_NE(oddMsg.find("plain.json"), std::string::npos) << oddMsg;
    }
}

// ── H3: the DIAGNOSTIC itself, on EVERY host ─────────────────────────────
//
// A fold collision cannot be CONSTRUCTED on NTFS or a default APFS volume —
// the two names cannot coexist — so before this test every emit site was
// unreachable, and therefore untested, on the Windows gate leg. That is the
// "an instrument reports a pass over work it did not do" class.
//
// The remedy is to test the emit with a SYNTHESIZED verdict rather than an
// on-disk one. `makeHeaderCaseAmbiguityDiagnostic` is the single builder every
// one of the six sites goes through (`reportHeaderCaseAmbiguity` is a
// one-liner over it), so pinning it here pins the CODE, the SEVERITY and the
// names-every-candidate contract on hosts that can never reach the sites
// through the filesystem.
TEST(HeaderNameMatching, AmbiguityDiagnosticNamesEveryCandidateOnAnyHost) {
    std::vector<fs::path> const candidates{fs::path{"/inc/Foo.h"},
                                           fs::path{"/inc/fOO.h"},
                                           fs::path{"/inc/foo.h"}};
    ParseDiagnostic const d = makeHeaderCaseAmbiguityDiagnostic(
        BufferId{}, SourceSpan::empty(0), "Foo.h", candidates);

    EXPECT_EQ(d.code, DiagnosticCode::F_HeaderNameCaseAmbiguous);
    EXPECT_EQ(d.severity, DiagnosticSeverity::Error)
        << "a collision is never a warning — any pick would be host-dependent";
    EXPECT_NE(d.actual.find("Foo.h"), std::string::npos)
        << "the message must name the REQUESTED spelling: " << d.actual;
    // EVERY candidate, not just the first — the whole point of the code.
    for (fs::path const& c : candidates) {
        EXPECT_NE(d.actual.find(c.generic_string()), std::string::npos)
            << "missing candidate " << c.generic_string() << " in: " << d.actual;
    }
    EXPECT_NE(d.actual.find("Remedy"), std::string::npos)
        << "the message must say what to DO about it";

    // Unsuppressable: `--suppress`ing it would force the resolver to pick, and
    // the pick differs by build host — the defect one layer down.
    EXPECT_TRUE(isUnsuppressable(DiagnosticCode::F_HeaderNameCaseAmbiguous));

    // And the reporting wrapper actually reports THAT diagnostic.
    DiagnosticReporter rep;
    reportHeaderCaseAmbiguity(rep, BufferId{}, SourceSpan::empty(0), "Foo.h",
                              candidates);
    ASSERT_EQ(rep.all().size(), 1u);
    EXPECT_EQ(rep.all()[0].code, DiagnosticCode::F_HeaderNameCaseAmbiguous);
    EXPECT_EQ(rep.all()[0].actual, d.actual);
}
