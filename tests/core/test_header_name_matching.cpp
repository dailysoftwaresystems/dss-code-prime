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
#include "core/types/ascii_case.hpp"
#include "core/types/header_name_matching.hpp"
#include "core/types/header_case_diagnostic.hpp"
#include "core/types/include_path_resolve.hpp"
#include "core/types/unsuppressable_codes.hpp"
#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <string>
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

// Directory names that are BUILD OUTPUT or VCS internals rather than
// checked-out content. Skipped so the repo-wide sweep answers the question that
// actually matters — "can this repo be CLONED onto a case-insensitive host" —
// and does not spend minutes walking `build-dbg/`. Recursion is pruned AT the
// directory, so their contents are never even enumerated.
//
// ⚠ THIS LIST MATCHES A BARE NAME AT EVERY DEPTH, so only names nobody would
// give to CONTENT may join it. `.git`, `node_modules`, `test-scratch` qualify;
// an ordinary word like `dist` does not. This repo has already paid for that
// distinction once — an UNANCHORED rsync exclude `build*` also matched
// `src/program/build_scripts.cpp`, and a gate leg was configured against a tree
// missing a changed `.cpp`. Generated trees whose name is ordinary are pruned by
// ABSOLUTE PATH instead; see `prunedSubtrees` on `caseCollisionsUnder` below.
[[nodiscard]] bool isGeneratedDirName(std::string const& name) {
    return name == ".git" || name == "test-scratch" || name == "node_modules"
        || name == ".claude" || name == "target"
        || name.rfind("build", 0) == 0;   // build, build-dbg, build-wsl, …
}

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
};

// `prunedSubtrees` names ABSOLUTE subtrees to skip — the path-anchored half of
// the prune, for generated trees whose bare NAME is too ordinary to hand to
// `isGeneratedDirName`. Today that is
// `src/dss-config/runtime/platform/dist/`: compiled object artifacts living
// INSIDE the config tree (D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF). Nothing
// generated is ever CLONED, so enumerating it would make this guard answer a
// different question than the one it states. Default empty, so a caller that
// has no generated subtree writes nothing.
CaseCollisionScan caseCollisionsUnder(fs::path const&              root,
                                      std::vector<fs::path> const& prunedSubtrees = {}) {
    CaseCollisionScan scan;
    std::map<std::string, std::vector<std::string>> byFolded;

    // ⚠ NORMALIZE BOTH SIDES, AND `.` IS EXACTLY WHY. The repo-wide guard walks
    // `*root / "."` as one of its four roots, and `.` is a REAL element of
    // `fs::path` iteration — not a spelling the library folds away — so every
    // entry that walk yields reads `<root>/./src/...`. An as-written compare
    // against `<root>/src/...` therefore NEVER matches, and the prune would be
    // live for the `src/dss-config` root while silently absent for the
    // whole-repo one, which is the only root that reaches the generated tree at
    // all. `lexically_normal()` also settles `/` vs `\` on Windows, so the
    // compare is about the SUBTREE and never about how it was spelled.
    std::vector<fs::path> pruned;
    pruned.reserve(prunedSubtrees.size());
    for (auto const& p : prunedSubtrees) pruned.push_back(p.lexically_normal());

    std::error_code ec;
    fs::recursive_directory_iterator it{root, ec};
    if (ec) { scan.walkError = ec; return scan; }
    for (fs::recursive_directory_iterator const end; it != end; it.increment(ec)) {
        if (ec) { scan.walkError = ec; return scan; }   // truncated — NOT a pass
        std::error_code dirEc;
        if (it->is_directory(dirEc)) {
            // An unrenderable directory name yields "" — which is not a generated name, so the
            // walk DESCENDS rather than pruning. Erring toward scanning more is the safe
            // direction for a collision guard.
            bool const prunedHere =
                isGeneratedDirName(narrowUtf8OrEmpty(it->path().filename()))
                || std::find(pruned.begin(), pruned.end(),
                             it->path().lexically_normal()) != pruned.end();
            if (prunedHere) it.disable_recursion_pending();
            continue;
        }
        std::error_code relEc;
        fs::path const  relPath = fs::relative(it->path(), root, relEc);
        std::string const rel   = narrowUtf8OrEmpty(relPath);
        if (rel.empty()) {
            // Distinguish the two reasons rather than folding them: `relative` failing is a
            // walk problem, an unrenderable spelling is a text problem, and only the second is
            // this scan's own blind spot.
            if (!relPath.empty()) ++scan.unrenderable;
            continue;
        }
        ++scan.filesScanned;
        byFolded[asciiToLower(rel)].push_back(rel);
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
    ScratchDir scratch{Location::Temp, "header_case"};
    fs::path const dir = scratch.path();
    writeFile(dir / "stdio.json");

    // Byte-exact request resolves under BOTH policies — the control that keeps
    // the two rejections below attributable to CASE and nothing else.
    EXPECT_EQ(resolveInDir(dir, "stdio.json", HeaderNameMatching::CaseSensitive).status,
              HeaderSearchStatus::Found);
    EXPECT_EQ(resolveInDir(dir, "stdio.json", HeaderNameMatching::CaseInsensitive).status,
              HeaderSearchStatus::Found);

    // ★ THE SILENT-ACCEPT PIN (the one that matters most). A case-SENSITIVE
    // format must NOT resolve `Stdio.json` to `stdio.json`. On a case-
    // INSENSITIVE host `fs::exists(dir/"Stdio.json")` returns TRUE, so this
    // goes red the moment DSS delegates the decision back to the host.
    EXPECT_EQ(resolveInDir(dir, "Stdio.json", HeaderNameMatching::CaseSensitive).status,
              HeaderSearchStatus::NotFound)
        << "a POSIX/elf target must reject a case-mismatched header name even "
           "when the BUILD HOST's filesystem would happily fold it";

    // ★ THE WRONG-REJECT PIN. A case-INSENSITIVE format MUST resolve it. On a
    // case-SENSITIVE host `fs::exists` returns FALSE, so this goes red if DSS
    // stops folding for itself.
    HeaderSearchResult const ci =
        resolveInDir(dir, "Stdio.json", HeaderNameMatching::CaseInsensitive);
    ASSERT_EQ(ci.status, HeaderSearchStatus::Found)
        << "a pe/macho target must resolve `<Stdio.h>` to the shipped "
           "`stdio.json` on ANY build host";
    EXPECT_EQ(ci.path.filename().string(), "stdio.json")
        << "the resolved path must be the REAL on-disk spelling, not the "
           "requested one — downstream readers open this path";

    // Mixed case, and a name that differs from the on-disk one by more than
    // case (never a match under either policy).
    EXPECT_EQ(resolveInDir(dir, "StDiO.json", HeaderNameMatching::CaseInsensitive).status,
              HeaderSearchStatus::Found);
    EXPECT_EQ(resolveInDir(dir, "stdioo.json", HeaderNameMatching::CaseInsensitive).status,
              HeaderSearchStatus::NotFound);
}

TEST(HeaderNameMatching, PolicyAppliesToEveryPathComponent) {
    ScratchDir scratch{Location::Temp, "header_case_subdir"};
    fs::path const dir = scratch.path();
    writeFile(dir / "sys" / "types.json");

    // The DIRECTORY component must fold too — `<SYS/TYPES.h>` is a legal
    // Windows spelling of the POSIX subdir header, and a resolver that folded
    // only the leaf would still reject it.
    EXPECT_EQ(resolveInDir(dir, "SYS/TYPES.json", HeaderNameMatching::CaseInsensitive)
                  .status,
              HeaderSearchStatus::Found);
    EXPECT_EQ(resolveInDir(dir, "Sys/types.json", HeaderNameMatching::CaseInsensitive)
                  .status,
              HeaderSearchStatus::Found);
    EXPECT_EQ(resolveInDir(dir, "SYS/TYPES.json", HeaderNameMatching::CaseSensitive)
                  .status,
              HeaderSearchStatus::NotFound);
    EXPECT_EQ(resolveInDir(dir, "SYS/types.json", HeaderNameMatching::CaseSensitive)
                  .status,
              HeaderSearchStatus::NotFound)
        << "a mismatch in the DIRECTORY component alone must still reject";
    EXPECT_EQ(resolveInDir(dir, "sys/types.json", HeaderNameMatching::CaseSensitive)
                  .status,
              HeaderSearchStatus::Found);
}

// The `<stem>.json` rewrite is CASE-PRESERVING byte slicing; the policy then
// answers the case question. This is the exact shape of the sqlite CLI blocker:
// `sqlite3.c` writes `#include <Windows.h>` and DSS ships `windows.json`.
TEST(HeaderNameMatching, SystemDescriptorStemRewriteHonoursPolicy) {
    ScratchDir scratch{Location::Temp, "header_case_desc"};
    fs::path const dir = scratch.path();
    writeFile(dir / "windows.json");
    std::vector<fs::path> const sysDirs{dir};

    EXPECT_EQ(resolveSystemDescriptor("Windows.h", sysDirs,
                                      HeaderNameMatching::CaseInsensitive).status,
              HeaderSearchStatus::Found)
        << "the pe/macho convention must reach windows.json from <Windows.h>";
    EXPECT_EQ(resolveSystemDescriptor("WiNdOwS.h", sysDirs,
                                      HeaderNameMatching::CaseInsensitive).status,
              HeaderSearchStatus::Found);
    EXPECT_EQ(resolveSystemDescriptor("Windows.h", sysDirs,
                                      HeaderNameMatching::CaseSensitive).status,
              HeaderSearchStatus::NotFound);
    EXPECT_EQ(resolveSystemDescriptor("windows.h", sysDirs,
                                      HeaderNameMatching::CaseSensitive).status,
              HeaderSearchStatus::Found);
}

// The angle funnel (`#include <h>` AND `__has_include(<h>)` both call it) must
// carry the policy into BOTH of its arms — the descriptor arm and the
// source-header fallback. A policy applied to only one arm would make an angle
// include of a real `-I` header disagree with an angle include of a descriptor.
TEST(HeaderNameMatching, AngleFunnelHonoursPolicyInBothArms) {
    ScratchDir scratch{Location::Temp, "header_case_angle"};
    fs::path const sysDir = scratch.path() / "sys";
    fs::path const incDir = scratch.path() / "inc";
    writeFile(sysDir / "windows.json");
    writeFile(incDir / "sqlite3ext.h", "int x;\n");
    std::vector<fs::path> const sysDirs{sysDir};
    std::vector<fs::path> const incDirs{incDir};

    EXPECT_EQ(resolveAngleInclude("Windows.h", sysDirs, incDirs,
                                  HeaderNameMatching::CaseInsensitive).kind,
              AngleIncludeKind::Descriptor);
    EXPECT_EQ(resolveAngleInclude("Windows.h", sysDirs, incDirs,
                                  HeaderNameMatching::CaseSensitive).kind,
              AngleIncludeKind::NotFound);
    EXPECT_EQ(resolveAngleInclude("SQLite3Ext.h", sysDirs, incDirs,
                                  HeaderNameMatching::CaseInsensitive).kind,
              AngleIncludeKind::Source)
        << "the -I source fallback arm must fold too, or an angle include of a "
           "real header disagrees with an angle include of a descriptor";
    EXPECT_EQ(resolveAngleInclude("SQLite3Ext.h", sysDirs, incDirs,
                                  HeaderNameMatching::CaseSensitive).kind,
              AngleIncludeKind::NotFound);
}

TEST(HeaderNameMatching, QuoteAndDirListSearchesHonourPolicy) {
    ScratchDir scratch{Location::Temp, "header_case_quote"};
    fs::path const selfDir = scratch.path() / "self";
    fs::path const incDir  = scratch.path() / "inc";
    writeFile(selfDir / "local.h", "int a;\n");
    writeFile(incDir / "shared.h", "int b;\n");
    std::vector<fs::path> const incDirs{incDir};

    // Self-dir arm (quote-only, C 6.10.2p3).
    EXPECT_EQ(resolveIncludePath("Local.h", selfDir, incDirs,
                                 HeaderNameMatching::CaseInsensitive).status,
              HeaderSearchStatus::Found);
    EXPECT_EQ(resolveIncludePath("Local.h", selfDir, incDirs,
                                 HeaderNameMatching::CaseSensitive).status,
              HeaderSearchStatus::NotFound);
    // -I arm.
    EXPECT_EQ(resolveIncludePath("Shared.h", selfDir, incDirs,
                                 HeaderNameMatching::CaseInsensitive).status,
              HeaderSearchStatus::Found);
    EXPECT_EQ(resolveIncludePath("Shared.h", selfDir, incDirs,
                                 HeaderNameMatching::CaseSensitive).status,
              HeaderSearchStatus::NotFound);
    // `findInDirs` (the shared dir-list search) directly.
    EXPECT_EQ(findInDirs("Shared.h", incDirs, HeaderNameMatching::CaseInsensitive).status,
              HeaderSearchStatus::Found);
    EXPECT_EQ(findInDirs("Shared.h", incDirs, HeaderNameMatching::CaseSensitive).status,
              HeaderSearchStatus::NotFound);
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
        EXPECT_EQ(resolveInDir(dir, "Foo.json", HeaderNameMatching::CaseInsensitive)
                      .status,
                  HeaderSearchStatus::Found);
        GTEST_SKIP() << "this filesystem folds case, so a `foo.json`/`Foo.json` "
                        "pair cannot exist on it — the ambiguity branch is "
                        "unreachable here and is pinned on the case-sensitive leg";
    }
    writeFile(dir / "foo.json");
    writeFile(dir / "Foo.json");

    HeaderSearchResult const r =
        resolveInDir(dir, "Foo.json", HeaderNameMatching::CaseInsensitive);
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
    EXPECT_EQ(resolveInDir(dir, "Foo.json", HeaderNameMatching::CaseSensitive).status,
              HeaderSearchStatus::Found);
    EXPECT_EQ(resolveInDir(dir, "foo.json", HeaderNameMatching::CaseSensitive).status,
              HeaderSearchStatus::Found);

    // The collision must also stop the ANGLE funnel and the dir-list search,
    // not just the single-dir atom — those are what the compiler actually calls.
    std::vector<fs::path> const dirs{dir};
    EXPECT_EQ(findInDirs("Foo.json", dirs, HeaderNameMatching::CaseInsensitive).status,
              HeaderSearchStatus::AmbiguousCase);
    // The DESCRIPTOR half (systemDirs) — distinct from the source half,
    // because a different tier owns each report.
    EXPECT_EQ(resolveAngleInclude("Foo.h", dirs, {},
                                  HeaderNameMatching::CaseInsensitive).kind,
              AngleIncludeKind::AmbiguousDescriptor);
    // The SOURCE half (-I dirs), reached only when no descriptor matches. It
    // must report a DIFFERENT kind, because the import resolver re-resolves the
    // descriptor half and owns its diagnostic while NOTHING re-resolves this
    // one — collapsing the two is what made a `-I` collision surface as
    // `F_ShippedHeaderNotFound`, naming the wrong defect.
    writeFile(dir / "bar.h", "int b;");
    writeFile(dir / "Bar.h", "int B;");
    EXPECT_EQ(resolveAngleInclude("Bar.h", dirs, dirs,
                                  HeaderNameMatching::CaseInsensitive).kind,
              AngleIncludeKind::AmbiguousSource)
        << "no `bar.json` exists, so the descriptor half misses and the search "
           "reaches the -I source half, where the collision lives";
    // CONTROL: a name matching neither half is a plain miss, so the two
    // Ambiguous* verdicts above are attributable to the collisions.
    EXPECT_EQ(resolveAngleInclude("nothing_here.h", dirs, dirs,
                                  HeaderNameMatching::CaseInsensitive).kind,
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
    // The ONE generated tree that `isGeneratedDirName` cannot claim, because
    // `dist` is an ordinary word and a bare-name entry would prune EVERY
    // directory so named anywhere in the repo (see the note there). Named by
    // ABSOLUTE path, once, for every root below: it is simply unreachable from
    // the two narrow roots, so listing it there costs one path compare that
    // never fires and keeps a single source of truth for what is generated.
    std::vector<fs::path> const generatedSubtrees{
        *root / "src/dss-config/runtime/platform/dist"};

    // `src/dss-config/**` first (the descriptors this axis resolves), then the
    // WHOLE checked-out tree — the invariant is about CLONING the repo, not
    // just about headers, so the sweep must cover everything a clone
    // materializes. Build output and `.git` are pruned by `isGeneratedDirName`.
    for (char const* sub : {"src/dss-config/shippedLibs",
                            "src/dss-config/object-formats",
                            "src/dss-config",
                            "."}) {
        fs::path const dir = *root / sub;
        ASSERT_TRUE(fs::is_directory(dir))
            << sub << " must exist — a guard that silently skips its own "
                      "subject is the vacuous-pass class";
        auto const scan = caseCollisionsUnder(dir, generatedSubtrees);
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
    auto const clean = caseCollisionsUnder(dir);
    EXPECT_TRUE(clean.groups.empty())
        << "no collision yet — the guard must not fire on a clean tree";
    EXPECT_EQ(clean.filesScanned, 1u) << "and it must have actually LOOKED";
    EXPECT_FALSE(clean.walkError);
    writeFile(dir / "nested" / "Windows.json");
    auto const scan = caseCollisionsUnder(dir);
    EXPECT_FALSE(scan.walkError);
    EXPECT_EQ(scan.filesScanned, 2u);
    ASSERT_EQ(scan.groups.size(), 1u)
        << "the guard must SEE the pair it exists to ban";
    EXPECT_EQ(scan.groups[0].size(), 2u);
    EXPECT_EQ(scan.groups[0][0], "nested/Windows.json");
    EXPECT_EQ(scan.groups[0][1], "nested/windows.json");
}

// CONTROL for the PATH-ANCHORED prune the repo-wide guard passes.
//
// ★ WHY THIS CANNOT BE LEFT TO THE REAL TREE. `src/dss-config/runtime/platform/
// dist/` is GENERATED, so it is ABSENT from a fresh clone and absent on any host
// that has not built the runtime — which means a prune that silently matched
// nothing would look EXACTLY like a prune that worked, in every log this project
// keeps. That is the vacuous-fix class, one step removed from the vacuous-pass
// class `CaseCollisionScan` already carries a denominator for. So the mechanism
// is exercised over a tree this test builds, in BOTH directions, on EXACT
// counts — emptiness cannot tell "pruned" apart from "scanned nothing".
//
// It needs no case-colliding pair and therefore no host capability: the claim is
// about WHAT WAS ENUMERATED, which every filesystem can answer.
TEST(HeaderNameMatching, CaseCollisionScanPrunesOnlyTheSubtreesItIsGiven) {
    ScratchDir scratch{Location::Temp, "header_case_prune"};
    fs::path const dir = scratch.path();
    writeFile(dir / "kept.json");
    writeFile(dir / "kept-nested" / "also-kept.json");
    writeFile(dir / "generated" / "one.o");
    writeFile(dir / "generated" / "deeper" / "two.o");
    // ★ A SECOND DIRECTORY OF THE SAME BARE NAME, at a different path — the
    // whole reason this prune is anchored. A bare-name `dist`/`generated` entry
    // in `isGeneratedDirName` would take this one out too, deleting coverage of
    // a tree that IS cloned, and the guard would still report a clean pass.
    writeFile(dir / "kept-nested" / "generated" / "three.o");

    auto const all = caseCollisionsUnder(dir);
    EXPECT_FALSE(all.walkError);
    EXPECT_EQ(all.filesScanned, 5u)
        << "UNPRUNED direction: the walk must see all five files, including the "
           "two under `<dir>/generated/`. Without this the count below could be "
           "explained by the files never existing";
    EXPECT_TRUE(all.groups.empty());

    auto const pruned = caseCollisionsUnder(dir, {dir / "generated"});
    EXPECT_FALSE(pruned.walkError);
    EXPECT_EQ(pruned.filesScanned, 3u)
        << "PRUNED direction: exactly `<dir>/generated/one.o` and "
           "`<dir>/generated/deeper/two.o` drop out. `kept.json`, "
           "`kept-nested/also-kept.json` and `kept-nested/generated/three.o` "
           "remain — the last one is the anchoring proof";
    EXPECT_TRUE(pruned.groups.empty());
}

// ...and the prune must survive the root spelling the repo-wide guard actually
// uses. `*root / "."` is one of that guard's four roots, and `.` is a REAL
// element of `fs::path` iteration, so every entry it yields carries a `./` the
// pruned path does not. An as-written compare fails HERE AND NOWHERE ELSE: the
// three narrow roots would keep pruning correctly while the whole-repo root —
// the only one that reaches the generated tree — silently walked it. The
// unpruned scan is the control that attributes the count to the prune rather
// than to the dot-spelled root breaking the walk.
TEST(HeaderNameMatching, CaseCollisionScanPrunesThroughADotSpelledRoot) {
    ScratchDir scratch{Location::Temp, "header_case_prune_dot"};
    fs::path const dir = scratch.path();
    writeFile(dir / "kept.json");
    writeFile(dir / "generated" / "one.o");

    auto const all = caseCollisionsUnder(dir / ".");
    EXPECT_FALSE(all.walkError);
    EXPECT_EQ(all.filesScanned, 2u)
        << "a `.` component must not disturb the walk itself";

    auto const pruned = caseCollisionsUnder(dir / ".", {dir / "generated"});
    EXPECT_FALSE(pruned.walkError);
    EXPECT_EQ(pruned.filesScanned, 1u)
        << "with an as-written compare this reads 2, and the repo-wide guard "
           "walks the generated tree it was told to skip";
}

// `.` and `..` are NAVIGATION components, not filenames — no directory listing
// contains an entry called `..`, so a resolver that ran them through the
// name-matcher would turn every `#include "../shared.h"` into a miss. Caught by
// the post-implementation read of `descend`, NOT by the corpus: the existing
// tests happen not to use a `..` include, so this would have shipped green.
TEST(HeaderNameMatching, DotAndDotDotComponentsNavigateRatherThanMatch) {
    ScratchDir scratch{Location::Temp, "header_case_dots"};
    fs::path const root = scratch.path();
    writeFile(root / "shared.h", "int s;\n");
    writeFile(root / "inc" / "here.h", "int h;\n");
    fs::path const inc = root / "inc";

    for (auto m : {HeaderNameMatching::CaseSensitive,
                   HeaderNameMatching::CaseInsensitive}) {
        EXPECT_EQ(resolveInDir(inc, "../shared.h", m).status,
                  HeaderSearchStatus::Found)
            << "`..` must navigate up, not be matched as a filename";
        EXPECT_EQ(resolveInDir(inc, "./here.h", m).status,
                  HeaderSearchStatus::Found)
            << "`.` must be a no-op component";
        EXPECT_EQ(resolveInDir(root, "inc/../shared.h", m).status,
                  HeaderSearchStatus::Found);
        // ...and the case rule still governs the components that DO name files.
        EXPECT_EQ(resolveInDir(inc, "../Shared.h", m).status,
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

        HeaderSearchResult const r = resolveInDir(dir, u8Bytes(kZhong), m);
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

        HeaderSearchResult const other = resolveInDir(dir, u8Bytes(kNihon), m);
        ASSERT_EQ(other.status, HeaderSearchStatus::Found);
        EXPECT_EQ(other.path.filename().native(), nihon.native())
            << "two distinct non-ASCII names must not resolve to one file";

        EXPECT_EQ(resolveInDir(dir, u8Bytes(kNaive), m).status,
                  HeaderSearchStatus::Found);
        // CONTROL: a non-ASCII name that is NOT there is a plain miss — never a
        // crash, and never a fold onto its neighbour.
        EXPECT_EQ(resolveInDir(dir, "x" + u8Bytes(kNihon), m).status,
                  HeaderSearchStatus::NotFound);
        // ...and the ASCII control, so the verdicts above are attributable to
        // the non-ASCII names rather than to the directory.
        EXPECT_EQ(resolveInDir(dir, "plain.json", m).status,
                  HeaderSearchStatus::Found);
    }

    // The ASCII fold must still govern the ASCII PART of a non-ASCII name. This
    // is what makes generalising the ONE folding helper to the native character
    // type load-bearing rather than cosmetic: the helper now folds a string
    // whose other code units are not ASCII at all, and must leave them alone.
    std::string upperExt = u8Bytes(kZhong);
    upperExt.replace(upperExt.size() - 5, 5, ".JSON");
    EXPECT_EQ(resolveInDir(dir, upperExt, HeaderNameMatching::CaseInsensitive).status,
              HeaderSearchStatus::Found);
    EXPECT_EQ(resolveInDir(dir, upperExt, HeaderNameMatching::CaseSensitive).status,
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
        EXPECT_EQ(resolveInDir(dir, "plain.json", m).status,
                  HeaderSearchStatus::Found)
            << "an ASCII lookup must survive an exotic BYSTANDER in the same "
               "directory — the resolver has no business narrowing entries it "
               "is not being asked about";
        EXPECT_EQ(findInDirs("plain.json", dirs, m).status,
                  HeaderSearchStatus::Found);
        // A MISS still has to enumerate the whole directory, so it hits the
        // bystander on every entry rather than stopping early at a hit.
        EXPECT_EQ(resolveInDir(dir, "absent.json", m).status,
                  HeaderSearchStatus::NotFound);
    }
    EXPECT_EQ(resolveInDir(dir, "Plain.json", HeaderNameMatching::CaseInsensitive)
                  .status,
              HeaderSearchStatus::Found);
    EXPECT_EQ(resolveInDir(dir, "Plain.json", HeaderNameMatching::CaseSensitive)
                  .status,
              HeaderSearchStatus::NotFound);
    // And through the angle funnel, which is what the preprocessor calls.
    EXPECT_EQ(resolveAngleInclude("Plain.h", dirs, dirs,
                                  HeaderNameMatching::CaseInsensitive).kind,
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
