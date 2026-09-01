#include "program/runtime_object_cache.hpp"

#include "core/crypto/sha256.hpp"
#include "core/substrate/path_identity.hpp"  // genericSpelling
#include "program/cross_validate_target_format.hpp"
// D-PROGRAM-RUNTIME-CACHE-TEMP-CLAIM-ESCAPES-THROUGH-A-DANGLING-SYMLINK:
// `detail::createExclusiveBinary` is the EXCLUSIVE-CREATE primitive the
// linker's staging-temp claim already uses. Reused rather than re-derived —
// its host split (`_wfopen "wbxN"` / `open(O_CREAT|O_EXCL|O_CLOEXEC)`) and
// the reasoning behind both halves are measured at its definition site, and
// a second hand-rolled exclusive create here would be a second chance to get
// the Windows half wrong. No new link edge is needed: `lsp`, `program` and
// `link` are all aggregated into one `dsscp-lib`.
#include "link/writer.hpp"

// ── THE COMPILER STAMP TERM ─────────────────────────────────────────────────
//
// ★ THIS IS THE ONLY TRANSLATION UNIT IN THE LIBRARY THAT MAY REACH THE BUILD
// STAMP, and that is a build-level contract rather than a style note: the
// generated header is rewritten on every dirty edit, so a second includer
// doubles the recompile-and-relink cost of every keystroke. See the
// review-stop note on `dss_use_build_stamp(program)` in this directory's
// CMakeLists and in `dss_build_stamp.hpp` itself.
//
// ★★★ THE INCLUDE IS UNCONDITIONAL AND THERE IS NO FALLBACK ARM. BOTH HALVES OF
// THAT WERE DECIDED AGAINST AN EARLIER DRAFT OF THIS FILE, and the draft is
// described here so the reasoning is not re-run and re-lost.
//
// The draft gated the include on `__has_include` and, when the macro was
// missing, fell back to `DSS_PROJECT_VERSION` — on the argument that backing out
// the `dss_use_build_stamp(program)` CMake edge should yield a WEAKER build
// rather than a broken one. Both halves were wrong, in the same direction:
//
//   ⛔ THE GATE DISABLED THE GUARD IT WAS SUPPOSED TO PROTECT. `__has_include`
//      tests for `program/dss_build_stamp.hpp`, which is a CHECKED-IN file and
//      therefore always found; what the CMake edge actually supplies is the
//      GENERATED half on the include path. So the gate could never distinguish
//      the case it existed for, and a backed-out edge would surface one tier
//      DOWN — as a missing generated header, naming a file instead of naming the
//      fact. That is the same shape as the `NoLibraryForFormat` row already open
//      against this tier: a real condition reported below where it is known.
//   ⛔ A SILENTLY WEAKER STAMP IS THE WORST AVAILABLE OUTCOME HERE, not the
//      safe one. `DSS_PROJECT_VERSION` does not move when codegen moves, so two
//      compilers built from different source at one VERSION share a cache entry
//      and the older one's archive is served to the newer — a stale object
//      reachable, which is the ONE failure this whole mechanism exists to make
//      impossible. Compiler identity is the component measured as
//      non-negotiable (the image is 419 MB, so it cannot be hashed per run, and
//      a path/size/mtime identity is install-unstable). Degrading it quietly is
//      exactly how it would be lost with nobody noticing.
//
// ⇒ A backed-out edge must be a HARD `#error` NAMING THE EDGE, and it already
// is: the checked-in `dss_build_stamp.hpp` owns that check and `#error`s with
// `dss_use_build_stamp(<target>)` in the message. A bare include reaches it; the
// gate did not. No second guard here — that would be a second owner of one rule,
// and it could never fire first.
#include "program/dss_build_stamp.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <cstdio>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#ifdef _WIN32
#    include <process.h>
#else
#    include <unistd.h>
#endif

namespace dss::runtime {

namespace {

namespace fs = std::filesystem;

// The key document's first line. A VERSION is part of it on purpose: the day a
// term is added or reordered, every previously-written artifact must become
// unreachable rather than merely stale, and bumping this line is what does it.
// ⓘ `/2` since 2026-08-25: the single `descriptor=`/`descriptor-sha256=` pair
// became a SORTED RUN of pairs, one per declaring descriptor. A `/1` document
// and a `/2` document over a one-descriptor unit would otherwise be byte-
// identical, so every `/1` artifact would stay addressable under a scheme whose
// term list had changed — the exact reachability this line exists to end.
constexpr std::string_view kKeyDocumentHeader = "dss-runtime-object-cache-key/2";

// The DEPENDENCY ARTIFACT subject class's own header line, and it is a separate
// line rather than a shared one for the reason the version exists at all: the
// two documents carry DIFFERENT TERM SETS, so a reader (and the byte-for-byte
// sidecar comparison) must be able to tell a runtime-object key from a
// dependency-artifact key without parsing the rest. `/1` because this is the
// first shape of it; the same bump rule applies — add or reorder a term and
// every previously-written entry must become UNREACHABLE, not merely stale.
constexpr std::string_view kDependencyKeyDocumentHeader =
    "dss-dependency-artifact-cache-key/1";

// The anchor for the dependency artifact cache's own refusals. A message
// naming the shipped-runtime ruling to somebody whose dependency failed to
// cache would send them to a row that has nothing to do with their build — the
// same reason `ArchiveSiblingRequester` exists.
constexpr std::string_view kDependencyAnchor =
    "D-DEPS-NO-ARTIFACT-SHARING-ACROSS-BUILDS-AT-ONE-CONFIGURATION";

// ★★★ THE DEPENDENCY ENTRIES' OWN SUBTREE, AND IT IS A CORRECTNESS COMPONENT
// RATHER THAN TIDINESS. `pruneSupersededSiblings` deletes by
// `<stem>-<16 base32><suffix>` WITHIN ONE DIRECTORY, so two subject classes
// sharing a directory would prune each other: a dependency named `unistd`
// building to `.a` would delete the shipped runtime object for `unistd.c`, and
// the next build of an unrelated program would recompile it. One component
// keeps the two families disjoint by construction.
//
// ⓘ ONLY THE DEPENDENCY PATHS GAIN IT. The runtime object shape is unchanged,
// so every artifact already written — including a packaged read-only `dist/` a
// vendor shipped — stays exactly where it is and stays addressable.
constexpr std::string_view kDependencyPathComponent = "deps";

// Cited by every refusal below so a user landing on one message can find the
// ruling that produced this whole mechanism.
constexpr std::string_view kAnchor =
    "D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF";

// `kBuildStamp` rather than the raw `DSS_BUILD_STAMP` macro: the stamp header
// exposes the value precisely so callers do not re-wrap the macro, and it is
// already `dss::runtime`-scoped, which is this namespace.
// Unconditional: there is exactly one source for this value (see the decision
// recorded at the include above). A `#if defined(DSS_BUILD_STAMP)` here would be
// dead — the header cannot be included without defining it — and a dead arm on
// the compiler-identity term is the one place a silent weakening would hide.
constexpr std::string_view kCompilerStamp = kBuildStamp;

// ⚠ NOT a second owner of `program/test_build_stamp`'s shape assertions — that
// test judges the STRING, this judges the one thing THIS file's PATH depends on.
// An empty stamp renders an empty path segment, and `base / "" / rel` collapses
// to `base / rel`, silently deleting the per-version scoping of the per-user
// root: two compiler versions would share one tree and `rm -rf`-ing one would
// take the other. Compile-time, so there is no dead runtime arm to hide in.
static_assert(!kCompilerStamp.empty(),
              "DSS_BUILD_STAMP expanded to an EMPTY string. The per-user cache "
              "root embeds the stamp as a path component, and an empty "
              "component silently un-scopes the cache across compiler "
              "versions. Fix cmake/DssBuildStamp.cmake.");

// A digest is well-formed iff it is EXACTLY 64 lowercase hex characters — the
// shape `dss::crypto::toHexLower` produces for a SHA-256. Defined as a
// predicate over the whole string rather than "contains only hex", so a
// truncated, uppercase, or whitespace-padded value is rejected too: those are
// the shapes a hand-assembled or half-migrated loader actually produces.
[[nodiscard]] bool isSha256HexLower(std::string_view text) noexcept {
    if (text.size() != 64) return false;
    for (char const c : text) {
        bool const isDigit = c >= '0' && c <= '9';
        bool const isLower = c >= 'a' && c <= 'f';
        if (!isDigit && !isLower) return false;
    }
    return true;
}

// ── THE PATH INDEX'S SHAPE, DECIDED ONCE ────────────────────────────────────
//
// ⚠ THESE FOUR CONSTANTS ARE THE ONLY PLACE THE FILENAME SHAPE IS SPELLED.
// `computeRuntimeObjectKey` composes with them and `pruneSupersededSiblings`
// DELETES FILES with them; a second spelling that drifted would either strand
// artifacts or delete somebody else's.
//
// 10 bytes = 80 bits = EXACTLY 16 base32 characters with no partial group, so
// the render is a clean prefix of the digest rather than a prefix plus a
// zero-extended tail that looks like it carries bits it does not.
constexpr std::size_t      kPathDigestBytes = 10u;
constexpr std::size_t      kPathDigestChars = 16u;
constexpr std::string_view kArtifactSuffix  = ".a";
constexpr std::string_view kKeyDocumentSuffix = ".key";

static_assert(kPathDigestBytes * 8u % 5u == 0u,
              "the path index must be a whole number of base32 characters — a "
              "partial trailing group would render bits the digest prefix does "
              "not actually contain.");
static_assert(kPathDigestChars == kPathDigestBytes * 8u / 5u);

// A path index is well-formed iff it is EXACTLY 16 characters of RFC 4648's
// LOWERCASE base32 alphabet. Same shape of predicate as `isSha256HexLower`
// above and for the same reason: the complement is defined, so an uppercase,
// padded (`=`), truncated or hex-looking name is rejected rather than
// half-accepted.
//
// ⛔ THE ALPHABET EXCLUDES `0`, `1` AND `8`, AND THAT IS NOT AN OVERSIGHT — RFC
// 4648's base32 alphabet is `a-z` then `2-7`. A check that accepted `[a-z0-9]`
// would match names this encoder can never produce, and this predicate gates a
// `remove()`.
[[nodiscard]] bool isPathDigestBase32Lower(std::string_view text) noexcept {
    if (text.size() != kPathDigestChars) return false;
    for (char const c : text) {
        bool const isLetter = c >= 'a' && c <= 'z';
        bool const isDigit  = c >= '2' && c <= '7';
        if (!isLetter && !isDigit) return false;
    }
    return true;
}

// Read a whole file as bytes. Binary mode on every host: the digest must be
// identical whether the cache is populated on Windows or Linux, and text-mode
// CR translation would make it host-dependent.
[[nodiscard]] std::expected<std::string, std::string>
readWholeFile(fs::path const& path, std::string_view role) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec) {
        return std::unexpected(std::format(
            "runtime object cache: the {} '{}' does not exist or is not a "
            "regular file, so its content digest cannot enter the cache key. "
            "Anchored: {}.",
            role, core::genericSpelling(path), kAnchor));
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected(std::format(
            "runtime object cache: could not open the {} '{}' for reading. "
            "Anchored: {}.",
            role, core::genericSpelling(path), kAnchor));
    }
    std::string contents{std::istreambuf_iterator<char>(in),
                         std::istreambuf_iterator<char>()};
    if (in.bad()) {
        return std::unexpected(std::format(
            "runtime object cache: I/O error while reading the {} '{}'. A "
            "PARTIAL read would hash to a digest that names bytes nobody "
            "compiled, so this is a refusal rather than a best-effort. "
            "Anchored: {}.",
            role, core::genericSpelling(path), kAnchor));
    }
    return contents;
}

// Arbitrary text rendered as ONE filesystem-safe path component. Two callers:
// the TARGET SLUG (`arm64:elf64-…` → `arm64_elf64-…`) and the BUILD STAMP
// SEGMENT (`0.0.2+g4095c13b.dirty…` → `0.0.2_g4095c13b.dirty…`).
//
// ⓘ IT IS A HUMAN LABEL AND NOTHING ELSE, in BOTH uses — it carries NO
// selection authority, which is what makes one shared spelling correct rather
// than a conflation. The slug exists so a person browsing the cache can tell
// the arm64 tree from the x86_64 one and one compiler version from another; a
// COLLISION in either is harmless, because the colliding inputs still produce
// different key documents (`target=` and `compiler=` are the VERBATIM values,
// never the sanitized ones), hence different digests, hence different FILENAMES
// inside the shared directory. The digest is what disambiguates — this only has
// to be stable and legible.
//
// ★ ONE SPELLING, NOT TWO. A second sanitizer with a slightly different keep-set
// would put the same character class in two places and let them drift; the
// keep-set is `[A-Za-z0-9._-]`, which is the intersection every filesystem this
// project targets accepts unquoted, and it is decided here once.
[[nodiscard]] std::string pathComponentSafe(std::string_view text) {
    std::string component;
    component.reserve(text.size());
    for (char const c : text) {
        bool const keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                       || (c >= '0' && c <= '9') || c == '.' || c == '_'
                       || c == '-';
        component.push_back(keep ? c : '_');
    }
    return component;
}

// ── THE PER-USER WRITABLE ROOT'S CANDIDATES ─────────────────────────────────
//
// ★ THE `.gitignore` RULE AT LINES 96-101 IS NOT AN OBJECTION TO THIS, AND THE
// NEXT READER WILL THINK IT IS. That rule says the `.dss-deps/` checkout cache
// belongs to "the project being compiled, NOT to this compiler" and must live
// beside the consuming project's manifest — *"never in the compiler's tree or in
// a shared/global location"*. Read as a rule about LOCATION it forbids a
// per-user cache root. It is not a rule about location; it is a rule about
// OWNERSHIP, and its own closing sentence gives that away: *"a build of someone
// else's project must never write inside this repo."* The concern is an artifact
// owned by the PROJECT escaping into the compiler's tree.
//
// The runtime object cache has the INVERSE ownership. Its key is (compiler build
// stamp, target, config, shipped unit, shipped descriptor, loaded config
// documents) — every term belongs to the COMPILER, and NOTHING in it is derived
// from the project being compiled. Two unrelated projects targeting the same
// platform with the same compiler legitimately want the identical object, and
// they get it. So the same principle, applied to this artifact, puts it with the
// compiler and its user. Writing it into every consuming project's output tree
// is the option actually in tension with the rule: it would scatter one
// compiler-owned artifact across every user repo that ever built a `.c`.
//
// ⓘ Precedence and the no-`#ifdef` argument live on `resolveRuntimeCacheRoots`
// in the header; this table is only its data.
struct RootCandidate {
    char const* variable;  // the environment variable that supplies the base
    char const* tail;      // appended under it; EMPTY for the override
    char const* spelling;  // how the trail prints it, in the host's own idiom
};

// The vendor tail. `dsscp` (never a bare `dss`) so the directory is
// attributable in a cache root shared with every other tool on the machine.
constexpr char const* kVendorTail = "dsscp/runtime-cache";

// The PLATFORM DEFAULTS — arms 2..4. Arm 1, the override, is composed at call
// time from the variable the CALLER names, so it is not in this table: the
// runtime object cache supplies its compiled-in
// `kRuntimeCacheOverrideVariable`, a dependency artifact cache supplies the
// name its project manifest declared. The defaults are shared because they
// describe THE MACHINE, not the mechanism — two caches disagreeing about where
// a user's cache directory is would be two answers to one question.
constexpr RootCandidate kPlatformRootCandidates[] = {
    {"LOCALAPPDATA", kVendorTail, "%LOCALAPPDATA%"},
    {"XDG_CACHE_HOME", kVendorTail, "$XDG_CACHE_HOME"},
    // The XDG spec's OWN documented default when `XDG_CACHE_HOME` is unset —
    // not an invention of this file, which is why it is `.cache` and not, say,
    // `.dss-cache`.
    {"HOME", ".cache/dsscp/runtime-cache", "$HOME"},
};

// Join a candidate-name list for a diagnostic. Names are quoted so a message
// naming both `x` and `x-clone` cannot be read as naming only the longer one.
[[nodiscard]] std::string quotedList(std::vector<std::string> const& names) {
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i != 0) out += ", ";
        out += '\'';
        out += names[i];
        out += '\'';
    }
    return out;
}

// ── Superseded-sibling pruning ──────────────────────────────────────────────
//
// ⓘ BEST-EFFORT, AND ITS FAILURE IS NOT AN ERROR — this is a deliberate
// asymmetry, not a swallowed exception. Correctness here never depended on the
// old entry being GONE; it depends only on it being UNREACHABLE, and the
// key-as-path already guarantees that: nothing computes the superseded key, so
// nothing looks the superseded file up. Pruning is a disk-space courtesy.
// Meanwhile the failure it will actually hit is routine and unfixable from
// here: Windows refuses to unlink a file another process still has open, and a
// build that turned "someone else is reading the old archive" into a hard
// error would be less correct, not more.
//
// The match is EXACT rather than a prefix glob: `<stem>-<16 lowercase base32>`
// plus ONE of the two suffixes this cache writes, with a length check, a
// separator check and an alphabet check on the middle. A loose `"<stem>-*.a"`
// would let a unit named `foo` delete a DIFFERENT unit named `foo-bar`.
//
// ⚠ `keepIndex` IS THE PATH INDEX, NOT THE KEY. It is compared here only to
// spare the entry this store is about to write; the entry's IDENTITY is settled
// by the `.key` document, never by this 16-character name. Nothing about
// deletion depends on the index being unique — a name that is NOT the one being
// kept is superseded whichever key produced it, and its own key document goes
// with it.
[[nodiscard]] bool matchesEntryName(std::string_view name,
                                    std::string_view sourceStem,
                                    std::string_view suffix,
                                    std::string_view keepIndex) {
    if (name.size() != sourceStem.size() + 1u + kPathDigestChars + suffix.size())
        return false;
    if (!name.starts_with(sourceStem)) return false;
    if (name[sourceStem.size()] != '-') return false;
    if (!name.ends_with(suffix)) return false;
    std::string_view const index{name.data() + sourceStem.size() + 1u,
                                 kPathDigestChars};
    if (!isPathDigestBase32Lower(index)) return false;
    return index != keepIndex;
}

// ⚠ `artifactSuffix` IS A PARAMETER AND NOT `kArtifactSuffix`. It was the
// constant while a shipped runtime archive (`.a`) was the only subject class;
// a dependency artifact takes its OBJECT FORMAT'S OWN extension, so a hardcoded
// `.a` would silently match NOTHING for a `.lib` and the eviction policy would
// be a no-op nobody could see. The KEY DOCUMENT suffix stays the constant —
// that one is this file's own and is the same for every subject.
void pruneSupersededSiblings(fs::path const&  directory,
                             std::string_view sourceStem,
                             std::string_view artifactSuffix,
                             std::string_view keepIndex) {
    std::error_code ec;
    fs::directory_iterator it(directory, ec);
    if (ec) return;

    // ⓘ COLLECT FIRST, DELETE AFTER. Unlinking the entry a `directory_iterator`
    // is currently positioned on is a question every platform answers slightly
    // differently, and this loop has no reason to ask it: the match set is
    // small and the whole operation is best-effort anyway.
    //
    // ★★ TWO LISTS, AND THE SPLIT IS THE SAME SAFETY PROPERTY THE STORE'S WRITE
    // ORDER HAS, RUN BACKWARDS. A partial prune must never leave an ARTIFACT
    // whose key document is gone: that state REFUSES for anyone who later
    // computes exactly this key (a reverted edit does), so a best-effort
    // courtesy would have bricked a build. Artifacts are unlinked FIRST and key
    // documents SECOND, so the surviving intermediate state is a stray `.key`
    // — invisible to every lookup, and swept on the next prune.
    std::vector<fs::path> supersededArtifacts;
    std::vector<fs::path> supersededKeyDocuments;
    for (fs::directory_iterator const end{}; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code typeEc;
        if (!it->is_regular_file(typeEc) || typeEc) continue;
        std::string const name = it->path().filename().string();
        if (matchesEntryName(name, sourceStem, artifactSuffix, keepIndex)) {
            supersededArtifacts.push_back(it->path());
        } else if (matchesEntryName(name, sourceStem, kKeyDocumentSuffix,
                                    keepIndex)) {
            supersededKeyDocuments.push_back(it->path());
        }
    }
    for (std::vector<fs::path> const* list :
         {&supersededArtifacts, &supersededKeyDocuments}) {
        for (fs::path const& stale : *list) {
            std::error_code removeEc;
            fs::remove(stale, removeEc);  // failure is explicitly NOT an error
        }
    }
}

// RAII: the temp file is removed on EVERY exit path unless explicitly
// released. Written as a guard rather than as cleanup at each `return` because
// this function has seven of them and the one that gets forgotten is the one
// that leaks.
class TempFileGuard {
public:
    explicit TempFileGuard(fs::path path) : path_(std::move(path)) {}
    ~TempFileGuard() {
        if (!armed_) return;
        std::error_code ec;
        fs::remove(path_, ec);
    }
    TempFileGuard(TempFileGuard const&)            = delete;
    TempFileGuard& operator=(TempFileGuard const&) = delete;
    TempFileGuard(TempFileGuard&&)                 = delete;
    TempFileGuard& operator=(TempFileGuard&&)      = delete;

    void release() noexcept { armed_ = false; }

private:
    fs::path path_;
    bool     armed_ = true;
};

// ── THE ONE REFUSAL EVERY UNWRITABLE-MISS ARM GOES THROUGH ──────────────────
//
// ⛔ SINGLE-OWNER ON PURPOSE. `storeRuntimeObject` has five distinct ways to
// fail to write (no root resolved, the directory cannot be created, the temp
// cannot be opened, the bytes cannot be flushed, the rename loses to nothing),
// and the header promises the SAME three things in all of them: every root
// considered, the reason, and the override as the remedy. Formatting that at
// five sites is five chances to drop one — and the one that gets dropped is the
// arm nobody tested. Composed here once, so the property holds by construction
// and a test asserting it on any arm asserts it on all of them.
//
// ⓘ `rootTrail` is carried on the key rather than re-resolved here: re-reading
// the environment at refusal time could report a DIFFERENT set of roots than the
// one the failed write actually used, which is the worst possible content for a
// diagnostic whose whole job is to say where the compiler looked.
// ── THE PATH-LENGTH DIAGNOSIS IS AN EXPERIMENT, NEVER A PLATFORM CONSTANT ───
//
// ✔MEASURED 2026-08-17, and it is why this function exists: a store failed
// because the composed path exceeded Windows' `MAX_PATH` (260), and the message
// said only *"could not open the temporary file"*. `create_directories` had
// SUCCEEDED at 188 characters, so nothing in the refusal pointed at length and
// the real cause took a separate investigation to find. The path index this
// file now uses buys ~46 characters of headroom, but headroom is not a proof:
// a long username, a long unit stem or a deep `DSS_RUNTIME_CACHE_DIR` can still
// walk into the wall, and when it does the message must say so ITSELF.
//
// ⛔ NO `#ifdef _WIN32` AND NO 260 CONSTANT. A compile-time platform branch
// would make the other host's arm unreachable and therefore untestable from
// here, which is the shape `resolveRuntimeCacheRoots`'s no-`#ifdef` note
// forbids for the same reason. And a hard-coded limit would be wrong on every
// filesystem that disagrees with it (NTFS caps a single COMPONENT at 255
// regardless of `MAX_PATH`; ext4 caps a component at 255 with no path cap;
// long-path-aware Windows processes have no 260 at all).
//
// ⇒ SO IT RUNS A CONTROL INSTEAD. Open a deliberately SHORT name in the SAME
// directory. If the short name opens, the directory is demonstrably writable
// and what failed was the NAME — a MEASURED fact, identical logic on every
// host, with no platform identity anywhere in the code. If the short name also
// fails, length is NOT the cause and the message says that too, so the verdict
// is informative in both directions rather than a hedge.
[[nodiscard]] std::string composedPathNote(fs::path const& directory,
                                           fs::path const& composed) {
    // ★ `core::genericSpelling`, and here the LOSS WOULD FALSIFY THE NUMBER,
    // not just the name: this note's whole job is to answer "is the composed
    // path too long?", and it answers by measuring `rendered.size()`. The
    // generic-format collapse deletes a separator, so on a path that names an
    // authority the verdict is computed on a string one character shorter than
    // the path that actually failed — a length report about a different string.
    std::string const rendered = core::genericSpelling(composed);
    std::string       note     = std::format(
        " COMPOSED PATH: '{}' — {} characters", rendered, rendered.size());

    // The probe name is deliberately tiny and dot-prefixed: it must be shorter
    // than anything this cache composes, and it must not look like an artifact
    // to a concurrent prune.
    static std::atomic<std::uint64_t> probeCounter{0};
    fs::path const probe =
        directory / ("." + std::to_string(probeCounter.fetch_add(1u)));
    bool shortNameOpens = false;
    {
        std::ofstream out(probe, std::ios::binary | std::ios::trunc);
        shortNameOpens = static_cast<bool>(out);
    }
    std::error_code removeEc;
    fs::remove(probe, removeEc);

    if (shortNameOpens) {
        note +=
            ". ✔MEASURED just now by this refusal: a SHORT name in the SAME "
            "directory opened successfully, so the directory IS writable and "
            "the LENGTH of the path above is what failed. Point "
            "DSS_RUNTIME_CACHE_DIR at a SHORT directory (Windows rejects paths "
            "over 260 characters unless the process is long-path aware, and "
            "NTFS/ext4 reject any single name over 255)";
    } else {
        note +=
            ". ✔MEASURED just now by this refusal: a SHORT name in the SAME "
            "directory ALSO failed to open, so the path's LENGTH is not the "
            "cause — the directory itself cannot be written";
    }
    return note;
}

[[nodiscard]] std::string unwritableMissRefusal(RuntimeObjectKey const& key,
                                                std::string_view reason) {
    // ⚠ THE WORDING SAYS WHAT **THE STORE** COULD NOT DO, NOT WHAT THE BUILD
    // WILL DO — it used to say *"so this build cannot proceed"*, written when
    // this function had no production caller and the author could assume the
    // caller would treat it as fatal. It now has one, and it does NOT stop the
    // build: the archive is already compiled and correct, and the driver links
    // it out of its staging area, so a message claiming the build has stopped
    // would be a diagnostic contradicted by the very run that printed it.
    //
    // ★ THE "REFUSAL, NOT A FALLBACK" ARGUMENT IS UNCHANGED AND IS SATISFIED,
    // which is why it stays. What it forbids is a SILENT degradation — success
    // reported while every later build recompiles the same unit, the cost
    // permanent and invisible. This function is the loud half of that bargain;
    // the caller's obligation is to SAY SO, and `reportRuntimeCacheNote` in
    // `program.cpp` prints this text verbatim on every affected build.
    return std::format(
        "runtime object cache: a cache MISS could not be WRITTEN, so the "
        "compiled runtime archive was NOT added to the cache and every later "
        "build will compile it again. {}. Roots considered, in precedence "
        "order: {}. This is a REFUSAL by the store and never a silent fallback: "
        "a mechanism that silently does nothing is worse than one that fails, "
        "so it is reported on every affected build until it is fixed. REMEDY: "
        "set {} to a writable directory; it is the explicit override, it wins "
        "over every platform default, and it is what CI and hermetic builds are "
        "expected to use. Anchored: {}.",
        // ⚠ THE KEY'S OWN OVERRIDE NAME, never a compiled-in constant. This
        // refusal serves two subject classes now, and a dependency artifact's
        // override is the variable its PROJECT MANIFEST named — telling that
        // user to set `DSS_RUNTIME_CACHE_DIR` would send them to a variable
        // this key never consults. Same for the anchor.
        reason, key.rootTrail, key.overrideVariable, key.anchor);
}

// ── THE COLLISION CHECK ─────────────────────────────────────────────────────
//
// ★★★ THIS IS THE HALF THAT KEEPS THE 16-CHARACTER PATH INDEX FROM BEING A
// WEAKENING, and the header's ★★★ section carries the argument. In one
// sentence: the short name is an INDEX, so the entry it names must still be
// shown to be THIS key's, and the full key document beside it is what shows it.
//
// ⓘ Returns the document's bytes when present, `nullopt` when the file simply
// is not there, and an error only for a real I/O failure. The three are kept
// distinct here because the CALLERS need different things from them — the store
// writes on `nullopt`, the lookup refuses on it — and collapsing them at this
// tier would force a caller to re-derive the distinction from the filesystem,
// which is a second answer that can disagree with this one.
[[nodiscard]] std::expected<std::optional<std::string>, std::string>
readKeyDocumentBeside(fs::path const& artifactPath) {
    fs::path const   documentPath = runtimeKeyDocumentPath(artifactPath);
    std::error_code  ec;
    if (!fs::exists(documentPath, ec) || ec) return std::optional<std::string>{};

    // ⚠ NOT-A-REGULAR-FILE IS A REFUSAL, AND THE GUARD IS LOAD-BEARING RATHER
    // THAN DEFENSIVE — ✔MEASURED on the WSL x86_64 leg, invisible on Windows.
    // `std::ifstream` on a DIRECTORY opens successfully on libstdc++ and then
    // throws `std::ios_base::failure` out of `underflow()` ("Is a directory")
    // during the istreambuf read, so the exception escapes this function and
    // every caller's refusal path — the diagnostic never runs. On Windows the
    // open simply fails and `!in` catches it, which is exactly why a one-leg
    // gate would have shipped this. The sibling reader above already guards
    // with `is_regular_file`; this one did not, and that asymmetry WAS the bug.
    if (!fs::is_regular_file(documentPath, ec) || ec) {
        return std::unexpected(std::format(
            "the key document '{}' exists but is NOT A REGULAR FILE, so the "
            "stored key cannot be read and a hit cannot be verified. This is a "
            "refusal rather than a miss: something else owns that name, and "
            "treating it as absent would let the cache write over it",
            core::genericSpelling(documentPath)));
    }

    std::ifstream in(documentPath, std::ios::binary);
    if (!in) {
        return std::unexpected(std::format(
            "the key document '{}' exists but could not be opened for reading",
            core::genericSpelling(documentPath)));
    }
    std::string contents{std::istreambuf_iterator<char>(in),
                         std::istreambuf_iterator<char>()};
    if (in.bad()) {
        return std::unexpected(std::format(
            "an I/O error interrupted the read of key document '{}'. A PARTIAL "
            "read would compare unequal and be reported as a collision, so this "
            "is a refusal rather than a verdict",
            core::genericSpelling(documentPath)));
    }
    return std::optional<std::string>{std::move(contents)};
}

// The single spelling of "this entry cannot be shown to be this key's". Both
// the lookup and the store compose their refusal here, so a test asserting the
// message on one arm asserts it on the other — the same single-owner argument
// `unwritableMissRefusal` above is written for.
[[nodiscard]] std::string unverifiableEntryRefusal(RuntimeObjectKey const& key,
                                                   fs::path const& artifactPath,
                                                   std::string_view reason) {
    return std::format(
        "runtime object cache: the entry at '{}' CANNOT BE VERIFIED as this "
        "build's, so it is neither served nor overwritten. {}. The filename "
        "carries a 16-character INDEX (the first 80 bits of this key's SHA-256, "
        "'{}'), NOT the key itself; the key's identity is the full digest '{}' "
        "and the exact bytes of the key document '{}'. That is why a mismatch "
        "here is a REFUSAL and not a miss: a miss would return to a store whose "
        "rule is `destination already exists ⇒ same key ⇒ same bytes`, which is "
        "the very inference this mismatch has broken — so the colliding "
        "artifact would be handed back as a success. REMEDY: delete the entry "
        "named above (both the artifact and its '.key' sibling) and rebuild; if "
        "it lives in a packaged read-only tree, the package is corrupt. "
        "Anchored: {}.",
        core::genericSpelling(artifactPath), reason, key.pathDigest, key.digest,
        core::genericSpelling(runtimeKeyDocumentPath(artifactPath)),
        key.anchor);
}

// Present + verified ⇒ empty. Otherwise the refusal text, already composed.
//
// ⚠ THE COMPARISON IS ON THE WHOLE DOCUMENT, NEVER ON A DIGEST OF IT. Hashing
// the stored document and comparing hashes would put a SECOND truncation-shaped
// question in the one place that exists to answer the first one; comparing the
// bytes is cheaper (the documents are a few hundred bytes) and admits no
// probability at all.
[[nodiscard]] std::optional<std::string>
verifyKeyDocumentBeside(RuntimeObjectKey const& key,
                        fs::path const&         artifactPath) {
    auto const stored = readKeyDocumentBeside(artifactPath);
    if (!stored.has_value()) {
        return unverifiableEntryRefusal(key, artifactPath, stored.error());
    }
    if (!stored->has_value()) {
        return unverifiableEntryRefusal(
            key, artifactPath,
            "the artifact is present but its key document is MISSING, so "
            "nothing records which inputs produced it. An absent key document "
            "is refused rather than ignored on purpose: were it a miss, "
            "deleting the sidecar would silently restore the unverified "
            "80-bit behaviour and the check would be optional, hence not a "
            "check");
    }
    if (**stored != key.document) {
        return unverifiableEntryRefusal(
            key, artifactPath,
            std::format("the stored key document ({} bytes) DIFFERS from this "
                        "build's ({} bytes) — two distinct keys have collided "
                        "on one 16-character index, or the entry is corrupt",
                        (*stored)->size(), key.document.size()));
    }
    return std::nullopt;
}

// ── ONE WRITER FOR BOTH FILES ───────────────────────────────────────────────
//
// Write `bytes` to `destination` through a unique temp in the SAME directory,
// then rename. "Destination already present" is SUCCESS.
//
// ★ SINGLE-OWNER FOR THE SAME REASON `unwritableMissRefusal` IS. The store now
// lands TWO files, and the flush-check, the guard, the pid-unique temp name and
// the lost-race arm are all subtle enough that a second hand-written copy would
// eventually differ from this one in exactly the arm nobody exercises. The
// caller supplies the bytes and the destination; everything else is decided
// here, once.
//
// ⚠ IT DOES NOT VERIFY. Whether an existing file at `destination` is legitimate
// is a question about the KEY, not about writing, and it is answered by
// `verifyKeyDocumentBeside` at the one place that owns it.
[[nodiscard]] std::expected<void, std::string>
writeThroughTemp(RuntimeObjectKey const&       key,
                 fs::path const&               destination,
                 std::span<std::uint8_t const> bytes) {
    fs::path const directory = destination.parent_path();

    // ── A unique temp IN THE SAME DIRECTORY ─────────────────────────────────
    //
    // Same directory so the rename below is a same-filesystem operation (a
    // cross-device rename is a copy, and a copy is not atomic). The name is
    // dot-prefixed and carries a `.tmp-<pid>-<n>` tail, so it ends in neither
    // `.a` nor `.key` and therefore cannot be mistaken for an entry by
    // `pruneSupersededSiblings` — including by a CONCURRENT process's prune,
    // which would otherwise be free to delete a temp being written right now.
    //
    // ⚠ IT IS ALSO THE LONGEST NAME THIS CACHE EVER COMPOSES — the destination's
    // own name plus thirteen characters. That is why the path budget is measured
    // on the temp and never on the artifact.
    //
    // ── D-PROGRAM-RUNTIME-CACHE-TEMP-CLAIM-ESCAPES-THROUGH-A-DANGLING-SYMLINK ──
    //
    // ★★★ THIS USED TO PROBE WITH `fs::exists` AND THEN OPEN WITH
    // `std::ofstream(..., trunc)`, AND THE TWO DISAGREE ABOUT WHAT A NAME IS.
    // `fs::exists` FOLLOWS symlinks, so a DANGLING symlink sitting at a
    // candidate name answers FALSE — the loop reads that as "free", claims the
    // name, and the truncating open then CREATES THE LINK'S TARGET. ✔MEASURED
    // 2026-08-27 on WSL with the exact primitives: `stat` says the name does not
    // exist, `open(O_WRONLY|O_CREAT|O_TRUNC)` SUCCEEDS, and the byte written
    // lands OUTSIDE the cache directory at wherever the link pointed. The
    // `rename` below then moves that link into place as a cache entry.
    //
    // ⚠ IT IS STRICTLY WORSE THAN THE SAME BLINDNESS IN `link::writeBytes`
    // ([[D-LINK-WRITER-DANGLING-SYMLINK-CLAIM-MISROUTE]]), and the difference is
    // the OPEN, not the probe: there the actual open is exclusive, so the wrong
    // branch was taken but NOTHING WAS WRITTEN and the run failed loud. A
    // truncating open destroys before it can fail.
    //
    // ⭐ THE FIX IS TO STOP ASKING AND START CLAIMING. `createExclusiveBinary`
    // IS the probe and the open in one indivisible step, so there is no window
    // and no second opinion: it refuses any existing directory ENTRY — a
    // dangling symlink included — and leaves it byte-intact, which is exactly
    // the question this loop was trying to ask.
    //
    // ⓘ THE COMMENT THAT USED TO STAND HERE ARGUED THE RACE AWAY, AND THAT
    // ARGUMENT WAS AND REMAINS CORRECT — it is preserved because it is still
    // load-bearing: two processes racing on this path computed the SAME key and
    // are writing the SAME bytes, and two LIVE processes cannot share a pid, so
    // the only way to draw an occupied name is a temp left by a killed run.
    // ★ What it did not cover is the case above: a dangling symlink is not a
    // temp left by a killed run, and the loop never even reached the
    // "overwriting that wholesale is exactly right" reasoning — it believed the
    // name was free. An argument that is sound about the case it considered is
    // still silent about the case it did not.
    static std::atomic<std::uint64_t> tempCounter{0};
#ifdef _WIN32
    auto const pid = static_cast<std::uint64_t>(_getpid());
#else
    auto const pid = static_cast<std::uint64_t>(getpid());
#endif
    fs::path   temporary;
    std::FILE* claimed = nullptr;
    for (std::uint32_t attempt = 0;; ++attempt) {
        if (attempt > 10000u) {
            return std::unexpected(unwritableMissRefusal(
                key, std::format("could not claim a unique temporary file in "
                                 "'{}' after 10000 attempts",
                                 core::genericSpelling(directory))));
        }
        fs::path candidate =
            directory
            / ("." + destination.filename().string() + ".tmp-"
               + std::to_string(pid) + "-"
               + std::to_string(tempCounter.fetch_add(1u)));
        claimed = linker::detail::createExclusiveBinary(candidate);
        if (claimed != nullptr) {
            temporary = std::move(candidate);
            break;
        }
        // ── WHY THE CLAIM WAS REFUSED, ASKED AT THE ENTRY AND NOT AT ITS
        //    TARGET ────────────────────────────────────────────────────────
        //
        // Two OPPOSITE responses are possible here — "the slot is TAKEN, try
        // the next name" and "this is a REAL error, stop now" — and the probe
        // that chooses between them must ask the SAME question the claim asked.
        // `createExclusiveBinary` refuses an existing directory ENTRY, so the
        // probe looks at the ENTRY: `symlink_status` does NOT follow, so a
        // dangling symlink reads as OCCUPIED, exactly as `O_EXCL` saw it.
        //
        // ⚠ `fs::exists(candidate)` HERE WOULD REINTRODUCE THE DEFECT IN ITS
        // OTHER FORM — it follows, so a dangling symlink would read as absent,
        // the loop would call the refusal a hard error, and an operator would
        // be handed a path-budget explanation for a name that is merely
        // occupied. That is [[D-LINK-WRITER-DANGLING-SYMLINK-CLAIM-MISROUTE]]
        // verbatim, and its own preferred fix is this `symlink_status` probe.
        //
        // ★ AND THE HARD-ERROR ARM MUST STAY, rather than letting a real
        // failure exhaust 10000 slots: ✔MEASURED as a red in
        // `RuntimeObjectCachePathBudget.AnOverlongNameRefusesNamingThePathAndItsLength`
        // when this loop retried instead. A name too long fails on EVERY slot,
        // and the composed-path note is the only thing that tells the operator
        // WHY — a "could not claim a unique temporary file after 10000
        // attempts" message names the directory and explains nothing.
        std::error_code    entryEc;
        auto const         entry = fs::symlink_status(candidate, entryEc);
        if (!fs::exists(entry)) {
            return std::unexpected(unwritableMissRefusal(
                key,
                std::format("could not open the temporary file '{}' for "
                            "writing.{}",
                            core::genericSpelling(candidate),
                            composedPathNote(directory, candidate))));
        }
    }
    TempFileGuard guard{temporary};

    {
        // The claimed handle, closed on every path out — including one where
        // building a diagnostic string throws.
        struct FileCloser {
            void operator()(std::FILE* f) const noexcept { std::fclose(f); }
        };
        std::unique_ptr<std::FILE, FileCloser> out{claimed};
        if (!bytes.empty()
            && std::fwrite(bytes.data(), 1u, bytes.size(), out.get())
                   != bytes.size()) {
            return std::unexpected(unwritableMissRefusal(
                key, std::format("failed writing {} byte(s) to the temporary "
                                 "file '{}'",
                                 bytes.size(), core::genericSpelling(temporary))));
        }
        // ⚠ CLOSED AND CHECKED EXPLICITLY, not left to the deleter. A
        // close-time flush failure (a full disk, a quota) is DISCARDED by a
        // destructor, and the next statement would rename a TRUNCATED file into
        // place under a key that promises the full bytes — a silently wrong
        // artifact, which is the one outcome this whole mechanism exists to
        // make impossible.
        if (std::fclose(out.release()) != 0) {
            return std::unexpected(unwritableMissRefusal(
                key, std::format("failed writing {} byte(s) to the temporary "
                                 "file '{}'",
                                 bytes.size(), core::genericSpelling(temporary))));
        }
    }

    std::error_code ec;
    if (fs::exists(destination, ec) && !ec) return {};  // the temp is discarded
    ec.clear();

    fs::rename(temporary, destination, ec);
    if (ec) {
        // A concurrent winner landed between the check and the rename. Same
        // key, same bytes — success, and the guard discards our temp.
        std::error_code probeEc;
        if (fs::exists(destination, probeEc) && !probeEc) return {};
        return std::unexpected(unwritableMissRefusal(
            key,
            std::format("could not rename '{}' into place as '{}': {}.{}",
                        core::genericSpelling(temporary),
                        core::genericSpelling(destination), ec.message(),
                        composedPathNote(directory, destination))));
    }
    guard.release();  // the temp no longer exists under that name
    return {};
}

// ── THE THREE PIECES BOTH KEY BUILDERS SHARE ────────────────────────────────
//
// ★★★ FACTORED RATHER THAN COPIED, AND THE REASON IS THIS FILE'S OWN STANDING
// ARGUMENT APPLIED ONE MORE TIME. Every one of these encodes a property the
// header sells the design on — an unknown input is a REFUSAL, the document is a
// function of the SET, the key IS the path and the SAME relative path is
// anchored at BOTH roots. A second subject class that re-spelled any of them
// would be free to drift in exactly the arm nobody exercises, and the drift
// would not be a crash: it would be a cache that serves.

// Both builders' first check. `label` prefixes the message so the user learns
// WHICH cache refused, and `anchor` sends them to the row that owns it.
[[nodiscard]] std::optional<std::string> refuseUnverifiableDocuments(
    std::vector<LoadedConfigDocument> const& documents,
    std::string_view label, std::string_view anchor) {
    for (LoadedConfigDocument const& doc : documents) {
        if (doc.digest.empty()) {
            return std::format(
                "{}: loaded config document '{}' at '{}' reports an EMPTY "
                "content digest. It reached the build through a path that did "
                "not record what it loaded, so the cache key would cover an "
                "unknown input — a cache serving a key it cannot verify. "
                "Refusing. Anchored: {}.",
                label, doc.label, doc.path, anchor);
        }
        if (!isSha256HexLower(doc.digest)) {
            return std::format(
                "{}: loaded config document '{}' at '{}' reports a MALFORMED "
                "content digest '{}' — expected exactly 64 lowercase hex "
                "characters (the shape of a SHA-256). Refusing: an unrecognized "
                "digest shape means the loader and this key disagree about what "
                "a digest IS, and the key would silently cover something other "
                "than the document's bytes. Anchored: {}.",
                label, doc.label, doc.path, doc.digest, anchor);
        }
    }
    return std::nullopt;
}

// ★ SORTED BY (label, path). The loaders report their documents in whatever
// order resolution happened to reach them — which is a function of
// `languageReferences` traversal and, for a directory-sourced set, of the
// filesystem. Sorting makes the document a function of the SET, which is what
// the key is actually about.
//
// ⓘ Pointers into the CALLER's vector, which must outlive the result. Every
// caller is one statement away.
[[nodiscard]] std::vector<LoadedConfigDocument const*>
sortedDocuments(std::vector<LoadedConfigDocument> const& documents) {
    std::vector<LoadedConfigDocument const*> sorted;
    sorted.reserve(documents.size());
    for (LoadedConfigDocument const& doc : documents) sorted.push_back(&doc);
    std::sort(sorted.begin(), sorted.end(),
              [](LoadedConfigDocument const* a, LoadedConfigDocument const* b) {
                  return std::tie(a->label, a->path)
                       < std::tie(b->label, b->path);
              });
    return sorted;
}

// Hash the finished document once, render the two forms, compose the ONE
// relative path and anchor it at both roots.
//
// ★★★ ONE HASH, TWO RENDERINGS, AND THEY ARE NOT PEERS. `digest` is the
// IDENTITY — the full 64-hex SHA-256, carried in the key document and compared
// on every hit. `pathDigest` is an INDEX — the first 80 bits of THE SAME BYTES,
// rendered as 16 characters of lowercase base32 so it fits a Windows path.
// Hashed once and rendered twice rather than re-derived, because decoding the
// hex back into bytes would be a second decoder free to disagree with the
// encoder about what the digest even is.
//
// ★ The relative path is computed ONCE and anchored twice. That is not a
// tidiness choice: it is the mechanical form of the header's two-roots
// property. Composing the shipped and per-user paths separately would put the
// `<dir>/<stem>-<index><suffix>` shape in two places, and the day they drifted
// the same key would name DIFFERENT files in the two roots — which is exactly
// the content ambiguity key-as-path exists to rule out.
[[nodiscard]] RuntimeObjectKey
finishKey(std::string document, fs::path relativeDirectory,
          std::string_view stem, std::string_view suffix,
          fs::path const& cacheAnchorRoot, std::string_view overrideVariable,
          std::string_view anchor) {
    RuntimeObjectKey key;
    std::array<std::uint8_t, 32> const raw = dss::crypto::sha256OfText(document);
    key.digest     = dss::crypto::toHexLower(raw);
    key.pathDigest = dss::crypto::toBase32Lower(
        std::span<std::uint8_t const>{raw.data(), kPathDigestBytes});
    key.document   = std::move(document);

    key.relativePath = std::move(relativeDirectory)
                     / (std::string{stem} + "-" + key.pathDigest
                        + std::string{suffix});

    // ⓘ Resolved HERE and not at store time, so the key is a complete
    // description of where its artifact may live and both probe sites read the
    // same answer. ⚠ And resolution failure is NOT propagated as an error:
    // a build that HITS in the shipped root never needs a writable root at all,
    // and refusing here would break exactly the packaged read-only install this
    // mechanism was built to serve. The failure surfaces at the first write,
    // which is the first moment it is real.
    RuntimeCacheRoots roots =
        resolveArtifactCacheRoots(cacheAnchorRoot, overrideVariable);
    key.shippedArtifactPath = roots.shipped / key.relativePath;
    key.userArtifactPath    = roots.perUser.empty()
                                ? fs::path{}
                                : roots.perUser / key.relativePath;
    key.rootTrail           = std::move(roots.trail);
    key.overrideVariable    = std::string{overrideVariable};
    key.anchor              = std::string{anchor};
    return key;
}

} // namespace

// ═══ THE ARCHIVE-SIBLING LOOKUP ══════════════════════════════════════════════

std::expected<std::string, std::string>
resolveArchiveSiblingFormat(ObjectFormatSchema const&      buildFormat,
                            TargetSchema const&            target,
                            fs::path const&                objectFormatsDir,
                            ArchiveSiblingRequester const& requester) {
    std::error_code ec;
    if (!fs::is_directory(objectFormatsDir, ec) || ec) {
        return std::unexpected(std::format(
            "{}: the object-format directory '{}' does not "
            "exist or is not a directory, so the archive-writing sibling of "
            "build format '{}' cannot be resolved. Anchored: {}.",
            requester.label, core::genericSpelling(objectFormatsDir),
            buildFormat.name(), requester.anchor));
    }

    // ── STEP 1: enumerate, then SORT BY FILENAME ────────────────────────────
    //
    // ★ The sort is for DETERMINISM OF THE MESSAGE, not of the answer — the
    // answer cannot depend on order, because the scan below never stops early.
    // But an ambiguity diagnostic that lists its candidates in
    // `directory_iterator` order would read differently on NTFS (sorted) and
    // ext4 (hash-ordered), and a diagnostic whose text depends on the host is
    // a diagnostic nobody can pin in a test.
    std::vector<fs::path> documents;
    {
        fs::directory_iterator it(objectFormatsDir, ec);
        if (ec) {
            return std::unexpected(std::format(
                "{}: could not open the object-format "
                "directory '{}': {}. Anchored: {}.",
                requester.label, core::genericSpelling(objectFormatsDir),
                ec.message(), requester.anchor));
        }
        for (fs::directory_iterator const end{}; it != end; it.increment(ec)) {
            if (ec) {
                return std::unexpected(std::format(
                    "{}: the scan of object-format directory "
                    "'{}' was interrupted after PARTIAL enumeration: {}. A "
                    "partial scan cannot prove the archive-writing sibling is "
                    "unique, so this is a refusal. Anchored: {}.",
                    requester.label, core::genericSpelling(objectFormatsDir),
                    ec.message(), requester.anchor));
            }
            // A dedicated error_code: `ec` carries the ITERATION's status and
            // clobbering it here would let a probe failure masquerade as a
            // scan failure on the next loop test.
            std::error_code typeEc;
            if (!it->is_regular_file(typeEc) || typeEc) continue;
            if (!it->path().filename().string().ends_with(".format.json")) {
                continue;
            }
            documents.push_back(it->path());
        }
    }
    std::sort(documents.begin(), documents.end(),
              [](fs::path const& a, fs::path const& b) {
                  return a.filename().string() < b.filename().string();
              });

    // ── STEP 2: the TOTAL scan ──────────────────────────────────────────────
    //
    // ★★★ NO EARLY EXIT ANYWHERE IN THIS LOOP. Proving UNIQUENESS is the whole
    // job; a `break` on the first hit would answer with whichever document the
    // filesystem happened to yield first, which is green on NTFS and red on
    // ext4 (or the reverse) for reasons no test output would ever name.
    std::vector<std::string> matches;
    for (fs::path const& document : documents) {
        auto loaded = ObjectFormatSchema::loadFromFile(document);
        if (!loaded.has_value()) {
            // ⚠ A DOCUMENT THAT CANNOT BE READ IS A REFUSAL, NEVER A SKIP.
            // Skipping it can only move the candidate set from AMBIGUOUS to
            // apparently-UNIQUE — i.e. it manufactures a confident wrong
            // answer out of a broken input, which is the exact direction the
            // header's asymmetry note forbids.
            std::string detail;
            for (auto const& diag : loaded.error()) {
                if (!detail.empty()) detail += "; ";
                detail += diag.message;
            }
            return std::unexpected(std::format(
                "{}: object-format document '{}' failed to "
                "load while scanning for the archive-writing sibling of build "
                "format '{}': {}. The scan must be TOTAL — a document that "
                "cannot be read could have been a second candidate, so "
                "skipping it would turn an AMBIGUOUS set into a silently "
                "UNIQUE one. Anchored: {}.",
                requester.label, core::genericSpelling(document),
                buildFormat.name(),
                detail, requester.anchor));
        }
        ObjectFormatSchema const& candidate = **loaded;

        // The two properties an object-format document declares UNIFORMLY.
        if (candidate.kind() != buildFormat.kind()) continue;
        if (candidate.container() != ObjectFormatContainer::Archive) continue;

        // ★★ THE MACHINE COMPARISON IS DELEGATED, NEVER RE-SPELLED HERE.
        // Architecture is reachable only through the per-kind `elf.machine` /
        // `pe.machine` / `macho.cputype` fields, so reading it here would be a
        // format-identity branch in shared substrate. `crossValidateTargetFormat`
        // already owns the target↔format machine relation; this asks it.
        //
        // ⚠ THE REPORTER IS LOCAL AND DELIBERATELY THROWN AWAY. A NON-matching
        // candidate is the ORDINARY case (24 shipped documents, one of them
        // the answer), so its mismatch diagnostic is not a user-facing event.
        // Routing it to the build's reporter would print ~20 "machine mismatch"
        // errors on every successful resolve. Only the final refusal below is
        // reported, and it is reported by the CALLER, from this function's
        // return value.
        DiagnosticReporter scratch;
        if (!crossValidateTargetFormat(target, candidate, scratch)) continue;

        matches.emplace_back(candidate.name());
    }
    std::sort(matches.begin(), matches.end());

    if (matches.size() == 1u) return matches.front();

    if (matches.empty()) {
        return std::unexpected(std::format(
            "{}: build format '{}' (kind '{}') has NO "
            "archive-writing sibling object format that agrees with target "
            "'{}'. A relocatable object — a static archive's MEMBER — is "
            "described by the `container: \"archive\"` document for its "
            "machine, so a format whose kind ships none for this target's "
            "machine has no object vocabulary to write members with, and none "
            "to read them back through. Scanned {} document(s) in '{}'. "
            "Anchored: {}.",
            requester.label, buildFormat.name(),
            objectFormatKindName(buildFormat.kind()), target.name(),
            documents.size(), core::genericSpelling(objectFormatsDir),
            requester.anchor));
    }

    return std::unexpected(std::format(
        "{}: build format '{}' (kind '{}') has {} AMBIGUOUS "
        "archive-writing siblings for target '{}': {}. Refusing to pick one — "
        "a first-match rule would let filesystem enumeration order decide the "
        "answer (sorted on NTFS, hash-ordered on ext4), so the same tree would "
        "resolve a different object format on different hosts. "
        "Make exactly one archive-writing format agree with this target's "
        "machine. Scanned {} document(s) in '{}'. Anchored: {}.",
        requester.label, buildFormat.name(),
        objectFormatKindName(buildFormat.kind()), matches.size(),
        target.name(), quotedList(matches), documents.size(),
        core::genericSpelling(objectFormatsDir), requester.anchor));
}

// ═══ THE TWO ROOTS ═══════════════════════════════════════════════════════════

std::string buildStampPathSegment(std::string_view buildStamp) {
    return pathComponentSafe(buildStamp);
}

std::string runtimeCacheBuildStampSegment() {
    return buildStampPathSegment(kCompilerStamp);
}

RuntimeCacheRoots resolveRuntimeCacheRoots(fs::path const& configRoot) {
    // ONE walker. See `resolveArtifactCacheRoots` in the header for why this is
    // a delegation rather than a second copy of the precedence list.
    return resolveArtifactCacheRoots(configRoot, kRuntimeCacheOverrideVariable);
}

RuntimeCacheRoots resolveArtifactCacheRoots(fs::path const&  configRoot,
                                            std::string_view overrideVariable) {
    RuntimeCacheRoots roots;
    roots.shipped = configRoot / "runtime" / "platform" / "dist";

    std::string const segment = runtimeCacheBuildStampSegment();

    auto const note = [&roots](std::string_view text) {
        if (!roots.trail.empty()) roots.trail += "; ";
        roots.trail += text;
    };
    // The shipped root leads the trail even though it is never a WRITE
    // candidate. A user reading the refusal is asking "where did it look?", and
    // an answer that omitted the root the lookup consulted FIRST would send
    // them hunting for a cache the compiler had already read.
    note(std::format("shipped (read-only, never written) -> '{}'",
                     core::genericSpelling(roots.shipped)));

    // ★ EVERY CANDIDATE IS WALKED, INCLUDING THE ONES AFTER THE WINNER — the
    // selection stops at the first that resolves, the ENUMERATION does not. The
    // trail is a diagnostic, and a diagnostic that listed only the candidates up
    // to the winner would answer "where did it look?" with a prefix, hiding the
    // very fallbacks a user whose build just refused needs to know exist. The
    // walk is four `getenv` calls; the completeness is worth more.
    //
    // ⓘ ARM 1 IS COMPOSED HERE FROM THE CALLER'S NAME rather than sitting in
    // the table, and it takes the directory VERBATIM — no vendor tail. The
    // caller already named the location, and silently appending two components
    // under it would put the cache somewhere a hermetic build script did not
    // choose and cannot clean up by the path it set.
    std::string const overrideSpelling = "$" + std::string{overrideVariable};
    std::string const overrideName{overrideVariable};
    RootCandidate const overrideCandidate{overrideName.c_str(), "",
                                          overrideSpelling.c_str()};

    auto const walk = [&](RootCandidate const& candidate) {
        char const* const raw = std::getenv(candidate.variable);
        // Set-but-EMPTY is treated as unset, and that is the only reading that
        // matches how a shell behaves: `DSS_RUNTIME_CACHE_DIR=` in a CI file is
        // how a user turns an override OFF, and honouring it would resolve the
        // root to the process's CURRENT DIRECTORY — writing the cache into
        // whatever tree the build happened to start in.
        if (raw == nullptr || raw[0] == '\0') {
            note(std::format("{}: unset or empty", candidate.spelling));
            return;
        }

        fs::path resolved{raw};
        if (candidate.tail[0] != '\0') resolved /= fs::path{candidate.tail};
        resolved /= segment;

        bool const selected = roots.perUser.empty();
        if (selected) roots.perUser = resolved;
        note(std::format("{} -> '{}'{}", candidate.spelling,
                         core::genericSpelling(resolved),
                         selected ? " [SELECTED]" : ""));
    };

    // ⚠ AN EMPTY OVERRIDE NAME SKIPS ARM 1 ENTIRELY RATHER THAN PROBING `""`.
    // `getenv("")` is unset on every host, so probing would print a trail line
    // reading `$: unset or empty` — a diagnostic naming no variable at all,
    // which is how a caller that failed to declare one would read "the override
    // was considered". The key builders REFUSE an empty name outright; this is
    // the arm that keeps a direct caller from being told a comforting lie.
    if (!overrideVariable.empty()) walk(overrideCandidate);
    for (RootCandidate const& candidate : kPlatformRootCandidates) {
        walk(candidate);
    }
    return roots;
}

// ═══ THE KEY ═════════════════════════════════════════════════════════════════

std::expected<RuntimeObjectKey, std::string>
computeRuntimeObjectKey(RuntimeObjectRequest const& request) {
    // ── STEP 1: every loaded document must report a VERIFIABLE digest ───────
    //
    // ⚠ Checked BEFORE anything is read from disk, because this is the failure
    // that says the INPUT SET is untrustworthy — and a key computed over an
    // untrustworthy input set is worse than no cache at all. Empty means a
    // document reached the build through a path that never recorded what it
    // loaded; empty is DETECTABLE, and the wrong bytes it would otherwise
    // stand in for are not.
    if (auto refusal = refuseUnverifiableDocuments(
            request.loadedDocuments, "runtime object cache", kAnchor);
        refusal.has_value()) {
        return std::unexpected(std::move(*refusal));
    }

    // ── STEP 2: the two content terms this file digests itself ──────────────
    //
    // These two are read and hashed HERE rather than reported by a loader,
    // and the reason is different for each. The UNIT is C source: no config
    // loader ever sees it, so there is nobody else to ask. The DESCRIPTOR has
    // a loader, but that loader retains no digest — ✔MEASURED 2026-08-17,
    // `contentDigest()` exists on exactly three types (`GrammarSchema`,
    // `TargetSchema`, `ObjectFormatSchema`) and on nothing under `src/ffi/`.
    //
    // ⓘ The day the descriptor loader grows one, this term MOVES into
    // `loadedDocuments` and these four lines shrink to one. That is a strict
    // improvement — the header's rule is that the SET must be asked of the
    // loaders and never hand-listed — but it is not this file's to make, and
    // reading the file here produces the same digest meanwhile.
    auto const unitContents =
        readWholeFile(request.configRoot / request.sourcePath,
                      "shipped runtime source unit");
    if (!unitContents.has_value()) {
        return std::unexpected(unitContents.error());
    }
    // ★★★ AN EMPTY DESCRIPTOR SET IS A REFUSAL AND NOT A KEY WITH ONE FEWER
    // TERM. A realized unit exists only because a descriptor's
    // `realization.<fmt>.source` named it, so "no declaring descriptor" is the
    // caller's attribution disagreeing with the corpus reader that produced the
    // unit — two owners of one fact, drifted. Keying anyway would drop the term
    // the whole mechanism exists for, and drop it SILENTLY.
    if (request.descriptorPaths.empty()) {
        return std::unexpected(std::format(
            "runtime object cache: the request for shipped runtime unit '{}' "
            "names NO declaring shipped-header descriptor. A realized unit is "
            "named by at least one descriptor's 'realization.<format>.source', "
            "so an empty set means the caller's attribution and the corpus "
            "reader disagree — and a key computed without the descriptor term "
            "would serve an artifact compiled against declarations that have "
            "since moved. Refusing. Anchored: {}.",
            request.sourcePath, kAnchor));
    }
    // ★ SORTED AND DEDUPLICATED, so the key is a function of the SET. The
    // caller's order is whatever its corpus walk happened to produce (sorted on
    // NTFS, hash-ordered on ext4 — the same filesystem-decides-the-answer trap
    // `resolveArchiveSiblingFormat` refuses), and one descriptor listed twice is
    // ONE input.
    std::vector<std::string> descriptorPaths = request.descriptorPaths;
    std::sort(descriptorPaths.begin(), descriptorPaths.end());
    descriptorPaths.erase(
        std::unique(descriptorPaths.begin(), descriptorPaths.end()),
        descriptorPaths.end());
    std::vector<std::string> descriptorDigests;
    descriptorDigests.reserve(descriptorPaths.size());
    for (std::string const& descriptorPath : descriptorPaths) {
        auto const contents = readWholeFile(request.configRoot / descriptorPath,
                                            "shipped-header descriptor");
        if (!contents.has_value()) {
            return std::unexpected(contents.error());
        }
        descriptorDigests.push_back(dss::crypto::sha256Hex(*contents));
    }

    // ── STEP 3: the document ────────────────────────────────────────────────
    //
    // Exact bytes, LF-terminated, UTF-8 (every term is either ASCII or already
    // UTF-8 as it came off disk). It is retained on the key so a mismatch is
    // DIAGNOSABLE — "these two builds disagree" is answerable by diffing two
    // documents, and unanswerable from two digests.
    std::string document;
    auto const line = [&document](std::string_view text) {
        document += text;
        document += '\n';
    };
    auto const field = [&line](std::string_view key, std::string_view value) {
        std::string row;
        row.reserve(key.size() + value.size());
        row += key;
        row += value;
        line(row);
    };

    line(kKeyDocumentHeader);
    field("compiler=", kCompilerStamp);
    field("target=", request.targetSpec);
    field("format=", request.buildFormatName);
    field("sibling=", request.siblingFormatName);
    field("config=", request.configName);
    field("unit=", request.sourcePath);
    field("unit-sha256=", dss::crypto::sha256Hex(*unitContents));
    // ⓘ PATH AND DIGEST STAY ADJACENT, one pair per descriptor, in the sorted
    // order above. Emitting all the paths and then all the digests would let a
    // reader diffing two documents pair a path with the wrong digest, and would
    // let two descriptors that swapped both fields produce the same document.
    for (std::size_t i = 0; i < descriptorPaths.size(); ++i) {
        field("descriptor=", descriptorPaths[i]);
        field("descriptor-sha256=", descriptorDigests[i]);
    }

    // ★ SORTED BY (label, path) — see `sortedDocuments`.
    for (LoadedConfigDocument const* doc :
         sortedDocuments(request.loadedDocuments)) {
        line(std::format("doc={}:{}:{}", doc->label, doc->path, doc->digest));
    }

    // STEPS 3b + 4 are the SHARED TAIL — see `finishKey`.
    return finishKey(std::move(document),
                     fs::path{request.configName}
                         / pathComponentSafe(request.targetSpec),
                     fs::path{request.sourcePath}.stem().string(),
                     kArtifactSuffix, request.configRoot,
                     kRuntimeCacheOverrideVariable, kAnchor);
}

std::expected<RuntimeObjectKey, std::string>
computeDependencyArtifactKey(DependencyArtifactRequest const& request) {
    // ── STEP 1: the four refusals this subject class owns ───────────────────
    //
    // ⚠ EVERY ONE OF THEM IS "THE INPUT SET IS UNKNOWN OR THE ENTRY CANNOT BE
    // NAMED", and none of them is a degradation to an uncached build DECIDED
    // HERE. This function's contract is the same as its sibling's: it returns a
    // key or it says why it cannot. What the DRIVER does with a refusal — print
    // it and compile normally — is the driver's decision, taken where the
    // difference between "an optimization is unavailable" and "the bytes might
    // be wrong" is visible.
    if (request.overrideVariable.empty()) {
        return std::unexpected(std::format(
            "dependency artifact cache: the request names NO cache-location "
            "override variable. The location override is NAMED IN CONFIGURATION "
            "('dependencyArtifactCache.rootOverrideVariable' on the root "
            "manifest) precisely so it is not sniffed from a compiled-in name, "
            "so an empty name means the policy reached this point through a "
            "path that did not carry it. Refusing. Anchored: {}.",
            kDependencyAnchor));
    }
    if (request.inputClosureDigest.empty()) {
        return std::unexpected(std::format(
            "dependency artifact cache: the request for artifact '{}' carries "
            "an EMPTY input-closure digest. `CompilationUnit::inputDigest()` "
            "reports empty for a unit built through a path that never computed "
            "one, so the set of files this artifact was compiled from is "
            "UNKNOWN — and a key over an unknown input set is exactly the entry "
            "that would be served after a header it read had changed. Refusing. "
            "Anchored: {}.",
            request.artifactStem, kDependencyAnchor));
    }
    if (!isSha256HexLower(request.inputClosureDigest)) {
        return std::unexpected(std::format(
            "dependency artifact cache: the request for artifact '{}' carries a "
            "MALFORMED input-closure digest '{}' — expected exactly 64 "
            "lowercase hex characters (the shape of a SHA-256). Refusing: an "
            "unrecognized shape means the caller and this key disagree about "
            "what a digest IS. Anchored: {}.",
            request.artifactStem, request.inputClosureDigest,
            kDependencyAnchor));
    }
    // ⚠ THE STEM AND THE SUFFIX GATE A `remove()`, WHICH IS WHY THEY ARE
    // REFUSED RATHER THAN DEFAULTED. `pruneSupersededSiblings` matches
    // `<stem>-<16 base32><suffix>` EXACTLY; an empty stem would make every
    // entry in the directory whose name is `-<index><suffix>` one family, and
    // an empty suffix would make the artifact and its `.key` sidecar
    // indistinguishable to `replace_extension`.
    if (request.artifactStem.empty() || request.artifactSuffix.empty()) {
        return std::unexpected(std::format(
            "dependency artifact cache: the request names an artifact stem "
            "('{}') or suffix ('{}') that is EMPTY, so the cache entry cannot "
            "be given a name of the `<stem>-<index><suffix>` shape this store's "
            "superseded-sibling prune matches on. Refusing rather than "
            "composing a name whose prune would reach unrelated entries. "
            "Anchored: {}.",
            request.artifactStem, request.artifactSuffix, kDependencyAnchor));
    }

    if (request.ltoModeName.empty()) {
        return std::unexpected(std::format(
            "dependency artifact cache: the request for artifact '{}' names no "
            "link-time-optimization topology. It changes the emitted bytes, so "
            "an unnamed one is a term missing from the key rather than a "
            "default. Refusing. Anchored: {}.",
            request.artifactStem, kDependencyAnchor));
    }
    // ── STEP 2: every loaded document must report a VERIFIABLE digest ───────
    // The same check, the same reasoning and the same two shapes as
    // `computeRuntimeObjectKey`'s STEP 1 — see `refuseUnverifiableDocuments`.
    // Applied to BOTH digest-bearing lists: a link input that could not be read
    // reports an empty digest and is exactly as unverifiable as a config
    // document that could not.
    if (auto refusal = refuseUnverifiableDocuments(
            request.loadedDocuments, "dependency artifact cache",
            kDependencyAnchor);
        refusal.has_value()) {
        return std::unexpected(std::move(*refusal));
    }
    if (auto refusal = refuseUnverifiableDocuments(
            request.linkInputs, "dependency artifact cache",
            kDependencyAnchor);
        refusal.has_value()) {
        return std::unexpected(std::move(*refusal));
    }

    // ── STEP 3: the document ────────────────────────────────────────────────
    //
    // ⓘ IT READS NOTHING FROM DISK, and that is the whole structural difference
    // from the sibling builder. There, the unit and the descriptors are FILES
    // this module opens and hashes. Here every content term arrived already
    // digested — by the front end (`inputDigest()`) and by the config loaders —
    // so the key is complete BY CONSTRUCTION rather than verified against a
    // record, which is what removes the depfile failure surface entirely.
    std::string document;
    auto const line = [&document](std::string_view text) {
        document += text;
        document += '\n';
    };
    auto const field = [&line](std::string_view key, std::string_view value) {
        std::string row;
        row.reserve(key.size() + value.size());
        row += key;
        row += value;
        line(row);
    };

    line(kDependencyKeyDocumentHeader);
    field("compiler=", kCompilerStamp);
    field("target=", request.targetSpec);
    field("format=", request.buildFormatName);
    field("sibling=", request.siblingFormatName);
    field("config=", request.configName);
    field("lto=", request.ltoModeName);
    // ⓘ THE SAME VALUE THE ENTRY IS NAMED AFTER, hashed as a term because it
    // reaches the emitted bytes. One field, both jobs — see `artifactStem`.
    field("artifact-name=", request.artifactStem);
    // ⓘ TWO DISTINCT LINES rather than a sentinel number. "The format's
    // declared default stands" and "the manifest asked for N" are different
    // requests even when N is today's default, because the default lives in a
    // `.format.json` that can move under a fixed manifest — and that document
    // is a `doc=` term below, so the two states stay distinguishable.
    if (request.stackReserveBytes.has_value()) {
        field("stack-reserve=", std::to_string(*request.stackReserveBytes));
    } else {
        line("stack-reserve=<format-default>");
    }
    field("inputs-sha256=", request.inputClosureDigest);

    // ⓘ IN LINK ORDER, path and digest adjacent — see `linkInputs`. The COUNT
    // leads so a document cannot be a prefix of a longer one: without it a
    // build linking [a] and one linking [a, b] would differ only by trailing
    // lines, which is fine for a hash but leaves the two indistinguishable to
    // anyone reading a truncated document.
    field("link-input-count=", std::to_string(request.linkInputs.size()));
    for (LoadedConfigDocument const& input : request.linkInputs) {
        field("link-input=", input.label + ":" + input.path);
        field("link-input-sha256=", input.digest);
    }

    for (LoadedConfigDocument const* doc :
         sortedDocuments(request.loadedDocuments)) {
        line(std::format("doc={}:{}:{}", doc->label, doc->path, doc->digest));
    }

    // ★★★ THE ENTRY LIVES UNDER ITS OWN `deps/` COMPONENT — see
    // `kDependencyPathComponent`. Two subject classes in one directory would
    // prune each other's artifacts, because the prune matches on a stem.
    return finishKey(std::move(document),
                     fs::path{kDependencyPathComponent}
                         / fs::path{request.configName}
                         / pathComponentSafe(request.targetSpec),
                     request.artifactStem, request.artifactSuffix,
                     request.configRoot, request.overrideVariable,
                     kDependencyAnchor);
}

// ═══ STORE / LOOKUP ══════════════════════════════════════════════════════════

fs::path runtimeKeyDocumentPath(fs::path const& artifactPath) {
    // ⓘ `replace_extension` and NOT string surgery: it replaces the LAST
    // extension, which is exactly right for a stem that legitimately contains
    // dots (`foo.bar-<index>.a` → `foo.bar-<index>.key`), and it is one call
    // rather than a second parse of the filename shape.
    return fs::path{artifactPath}.replace_extension(
        fs::path{kKeyDocumentSuffix});
}

std::expected<std::optional<fs::path>, std::string>
lookupRuntimeObject(RuntimeObjectKey const& key) {
    // ⓘ A MISS IS NORMAL and is never a diagnostic — it is the ordinary state
    // of a cache the first time a (target, config) pair is built. `error_code`
    // rather than the throwing overload for the same reason: a permission
    // error on the dist tree reads as "not present", and the store path that
    // follows will report it properly if it also cannot write.
    std::error_code ec;

    // ── SHIPPED FIRST ───────────────────────────────────────────────────────
    // ⓘ "First" is a preference, never a correctness rule — the header's
    // two-roots note is the reason a preference is all it has to be: the same
    // key names the same bytes in either root, so this ordering only decides
    // which COPY is read. It is shipped-first because a packaged `dist/` is the
    // set the vendor tested and shipped, and preferring it means a user's stale
    // per-user tree can never shadow it.
    //
    // ⚠ AND A PRESENT-BUT-UNVERIFIABLE SHIPPED ENTRY REFUSES RATHER THAN
    // FALLING THROUGH. Falling through would silently tolerate a corrupt or
    // foreign file in the packaged tree, and "silently tolerate" on a
    // content-addressed store is how a wrong object gets linked.
    if (fs::is_regular_file(key.shippedArtifactPath, ec) && !ec) {
        if (auto refusal = verifyKeyDocumentBeside(key, key.shippedArtifactPath);
            refusal.has_value()) {
            return std::unexpected(std::move(*refusal));
        }
        return std::optional<fs::path>{key.shippedArtifactPath};
    }
    ec.clear();

    // ── THE PER-USER ROOT SECOND ────────────────────────────────────────────
    // ⓘ An empty `userArtifactPath` is skipped, and that is not a silent skip:
    // no root resolved means no store could ever have succeeded there (the
    // store REFUSES on exactly this state), so there is no artifact to pass
    // over — only a location no writer could have reached.
    if (!key.userArtifactPath.empty()
        && fs::is_regular_file(key.userArtifactPath, ec) && !ec) {
        if (auto refusal = verifyKeyDocumentBeside(key, key.userArtifactPath);
            refusal.has_value()) {
            return std::unexpected(std::move(*refusal));
        }
        return std::optional<fs::path>{key.userArtifactPath};
    }
    return std::optional<fs::path>{};
}

std::expected<fs::path, std::string>
storeRuntimeObject(RuntimeObjectKey const&      key,
                   std::span<std::uint8_t const> bytes,
                   CacheEviction                 eviction) {
    // ── THE WRITABLE ROOT, OR A REFUSAL ─────────────────────────────────────
    //
    // ⛔ THE FIRST AND MOST IMPORTANT ARM: no per-user root resolved at all, so
    // there is nowhere on this machine the compiler may write. There is
    // deliberately NO third option here. The tempting one — compile the unit,
    // hand the bytes back, skip the cache — is the shape this refusal exists to
    // forbid: it returns SUCCESS, so the build goes green, and every later build
    // silently recompiles the same unit forever with nothing in any log to say
    // why. A mechanism that silently does nothing is worse than one that fails,
    // and this is the exact case that proves it.
    if (key.userArtifactPath.empty()) {
        return std::unexpected(unwritableMissRefusal(
            key,
            "NO per-user cache root could be resolved: every candidate "
            "environment variable was unset or empty, so there is no writable "
            "location for the artifact (the shipped root is read-only by "
            "design and is never written)"));
    }

    fs::path const& destination  = key.userArtifactPath;
    fs::path const  directory    = destination.parent_path();
    fs::path const  documentPath = runtimeKeyDocumentPath(destination);

    // The prune target is derived ONCE, up front, and held in real strings —
    // a `string_view` over `destination.filename().string()` would view a
    // temporary that dies at the end of the statement.
    //
    // ★ SPLIT ON THE INDEX, NOT ON A KNOWN SUFFIX. This used to test
    // `ends_with("-" + pathDigest + ".a")`, which silently produced an EMPTY
    // stem — and therefore pruned nothing — for any entry whose extension is
    // not `.a`. The index is the one component this file always composes, so
    // splitting on it derives BOTH halves for every subject class instead of
    // asserting one of them. `rfind` because a stem may legitimately contain a
    // `-` run that happens to look like an index.
    std::string const filename = destination.filename().string();
    std::string const marker   = "-" + key.pathDigest;
    std::size_t const markerAt = filename.rfind(marker);
    bool const wellFormed = markerAt != std::string::npos && markerAt > 0u
                         && markerAt + marker.size() < filename.size();
    std::string const sourceStem =
        wellFormed ? filename.substr(0u, markerAt) : std::string{};
    std::string const artifactSuffix =
        wellFormed ? filename.substr(markerAt + marker.size()) : std::string{};
    // ⓘ Called on BOTH success paths (fresh rename AND destination-already-
    // present): a build that lost the race still wants the superseded sibling
    // gone, and the winner may have been a process that never had a chance to
    // prune. Guarded on a non-empty stem so a hand-built key whose
    // `userArtifactPath` does not follow `<stem>-<index>.a` prunes NOTHING
    // rather than guessing at a stem — deleting the wrong file is not
    // best-effort.
    //
    // ⓘ IT PRUNES THE PER-USER ROOT AND ONLY THAT ROOT, and it needs no check
    // to guarantee it: `directory` is derived from `userArtifactPath`, so the
    // shipped root is unreachable from here by construction rather than by a
    // guard someone could weaken. A shipped superseded sibling is left exactly
    // where the package manager put it — it is already unreachable (nothing
    // computes its key) and deleting another owner's files is not this file's
    // business.
    //
    // ⓘ AND UNDER `CacheEviction::Retain` IT DOES NOTHING AT ALL — the whole
    // policy, expressed as the one call it gates. Retaining costs disk and
    // nothing else: the key-as-path rule already makes a superseded entry
    // UNREACHABLE, so keeping it can never cause one to be served.
    auto const pruneNow = [&] {
        if (eviction == CacheEviction::Retain) return;
        if (sourceStem.empty() || artifactSuffix.empty()) return;
        pruneSupersededSiblings(directory, sourceStem, artifactSuffix,
                                key.pathDigest);
    };

    std::error_code ec;
    if (!directory.empty()) {
        fs::create_directories(directory, ec);
        if (ec && !fs::is_directory(directory)) {
            // The arm an INSTALLED compiler hits when the resolved root is not
            // writable — a read-only volume, a quota, a path whose parent is
            // not a directory. It carries the OS reason verbatim, because
            // "permission denied" and "not a directory" send a user to two
            // completely different fixes. The composed path and its LENGTH go
            // with it: a directory chain can be refused for being too long too,
            // and there is no directory yet to run the short-name control in.
            // Same reason as `composedPathNote`: the length rides on this
            // string, so a collapsed separator is a wrong MEASUREMENT.
            std::string const rendered = core::genericSpelling(directory);
            return std::unexpected(unwritableMissRefusal(
                key,
                std::format("could not create the artifact directory '{}' ({} "
                            "characters): {}",
                            rendered, rendered.size(), ec.message())));
        }
        ec.clear();
    }

    // ── THE KEY DOCUMENT FIRST, THEN VERIFIED, THEN THE ARTIFACT ────────────
    //
    // ★★ THE ORDER IS THE SAFETY PROPERTY, not a sequencing detail. An artifact
    // with no key document beside it REFUSES for every later lookup, so a run
    // killed between the two renames must not be able to leave that state. It
    // cannot: the sidecar lands first, and the intermediate state it leaves —
    // a key document with no artifact — is an ordinary MISS that the next store
    // simply completes.
    if (auto const written = writeThroughTemp(
            key, documentPath,
            std::span<std::uint8_t const>{
                reinterpret_cast<std::uint8_t const*>(key.document.data()),
                key.document.size()});
        !written.has_value()) {
        return std::unexpected(written.error());
    }

    // ★★★ VERIFIED UNCONDITIONALLY, AFTER THE WRITE SETTLES, AND THAT IS WHY IT
    // IS HERE RATHER THAN BEFORE IT. Checking first and writing second would
    // leave a window in which a COLLIDING key (a different key document sharing
    // this 16-character index) lands between the probe and the rename — and
    // `writeThroughTemp`'s "already present ⇒ success" rule would then accept
    // the other key's sidecar in silence. Reading back what is actually on disk
    // afterwards has no such window, costs one read of a few hundred bytes, and
    // routes through the SAME chokepoint the lookup uses, so the two can never
    // disagree about what "verified" means.
    if (auto refusal = verifyKeyDocumentBeside(key, destination);
        refusal.has_value()) {
        return std::unexpected(std::move(*refusal));
    }

    // ── "Already there" is SUCCESS, not a conflict ──────────────────────────
    // Same key ⇒ same inputs ⇒ same bytes. That inference is what the check
    // above has just established for this path rather than assumed.
    if (auto const written = writeThroughTemp(key, destination, bytes);
        !written.has_value()) {
        return std::unexpected(written.error());
    }

    pruneNow();  // best-effort; see `pruneSupersededSiblings`
    return destination;
}

} // namespace dss::runtime
