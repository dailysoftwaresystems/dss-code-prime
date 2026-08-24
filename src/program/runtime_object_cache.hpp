#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"

// Includes are EXPLICIT — no reliance on transitive headers (house rule; the
// same note stands at the top of `core/crypto/sha256.hpp`). `<optional>` and
// `<cstdint>` are load-bearing here and happened to arrive transitively through
// the schema headers above: it compiled, which is exactly how this class of
// fragility survives review until an unrelated include is pruned.
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// ═══ THE SHIPPED RUNTIME'S CONTENT-ADDRESSED OBJECT CACHE ════════════════════
// D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF (operator ruling 2026-08-17).
//
// DSS ships the IMPLEMENTATION half of a toolchain, not only the DECLARATION
// half: a shipped-header descriptor may declare, per object format, that a
// symbol's body is PROVIDED by a shipped source unit rather than IMPORTED from
// a platform image. Those units are COMPILED FOR EVERY BUILD of a format that
// realizes them (never on-demand — see `allShippedSourcesForFormat`), so the
// runtime object set is a pure function of (target, config) and can therefore
// be cached, packaged and shipped. This file is that cache.
//
// ⚠ COMPILE-ALWAYS IS NOT LINK-ALWAYS. This cache answers "what has been
// COMPILED"; what reaches the image stays demand-driven, because the artifact
// is a single-member STATIC ARCHIVE and the existing archive path pulls a
// member only when the client module references it (`isArArchiveFile` →
// `pullStaticArchiveMembers`). The lazy reference-driven pull IS the
// compile/link split — it is reused, not reimplemented. A `hello.c` on pe64
// therefore carries no directory-walking code it never calls.
//
// ── WHY AN ARCHIVE AND NOT A LOOSE `.o` ─────────────────────────────────────
// ✔MEASURED 2026-08-17: a loose relocatable cannot be linked. The driver
// partitions `--resolve-library` inputs by MAGIC BYTES: `ar` archives go to the
// static-archive merge, everything else to the DYNAMIC reader, which needs a
// `.dynsym`/`SHT_DYNAMIC` and fails on a relocatable. `readRelocatableObject`
// exists in all three readers but its only production caller is the
// archive-member chokepoint. So a loose `.o` would have required a NEW driver
// input surface plus a reimplementation of demand-driven pull; the archive
// needs neither.
//
// ═══ THE KEY IS THE PATH, AND THAT IS THE WHOLE POINT ════════════════════════
// A stale artifact must be UNREACHABLE, not merely detectable. Selection is by
// construction: inputs that differ name a different file, and the previous file
// is simply not at the path anyone looks up.
//
// ═══ THE IDENTITY IS 64 HEX. THE PATH CARRIES A 16-CHARACTER INDEX. ══════════
//
// ★★★ READ THIS BEFORE TOUCHING EITHER FIELD. `RuntimeObjectKey::digest` — the
// full 64-character lowercase-hex SHA-256 of the key document — IS THE KEY'S
// IDENTITY, and it is what the key document beside the artifact is compared on.
// `RuntimeObjectKey::pathDigest` — 16 lowercase base32 characters, the first 80
// BITS of that same digest — IS AN INDEX INTO A DIRECTORY AND NOTHING MORE. A
// reader who mistakes the short form for the key will eventually "simplify" the
// verification away, which is precisely the mistake this section exists to stop.
//
// ── WHY THE PATH IS NOT THE FULL DIGEST: A MEASURED PATH-LENGTH WALL ────────
// ✔MEASURED 2026-08-17: with the full 64-hex digest in the filename, a store
// path reached 265 and then 273 characters and EVERY store failed at once on
// Windows `MAX_PATH` (260). It surfaced as *"could not open the temporary
// file"* even though `create_directories` had succeeded, because the directory
// prefix fit and the full path did not. The binding component is not even the
// artifact: the temp file is the artifact's own name plus `.tmp-<pid>-<n>`,
// thirteen characters longer. ⚠ `LongPathsEnabled` does not rescue this — a
// MinGW-produced binary carries no `longPathAware` manifest, so the registry
// setting does not apply to it.
//
// ── WHY BASE32 AND NOT BASE64, AND IT IS A CORRECTNESS CONSTRAINT ───────────
// Windows and macOS filenames are CASE-INSENSITIVE, so a mixed-case alphabet
// collapses under folding and two distinct keys could name ONE file — which is
// the content ambiguity this whole design exists to rule out. Lowercase base32
// is the densest case-insensitive-safe encoding. See the ⛔ on
// `dss::crypto::toBase32Lower`.
//
// ★★★ AND THE CONDITION THAT KEEPS TRUNCATION FROM BEING A WEAKENING — IT IS
// NOT OPTIONAL, IT IS THE OTHER HALF OF THE SAME DECISION. The FULL key
// document is written beside the artifact as `<stem>-<index>.key`, and it is
// RE-READ AND COMPARED BYTE FOR BYTE on every hit. Without that, truncation
// silently downgrades *"a stale artifact is UNREACHABLE"* to *"unreachable with
// probability 1−ε"* — and "unreachable" was the entire argument for compiling
// from source over shipping prebuilt objects. With it, the short name is an
// index, a collision is DETECTED rather than served, and the property the
// design was sold on is intact.
//
// ⛔ A MISMATCH — OR A MISSING KEY DOCUMENT BESIDE A PRESENT ARTIFACT — IS A
// REFUSAL, NOT A MISS, and the reason is that "miss" has nowhere correct to go:
//   * the store treats "destination already exists" as SUCCESS on the rule
//     *same key ⇒ same bytes*, which is the very inference a collision breaks —
//     so a miss would hand back the colliding artifact and report success;
//   * overwriting instead would destroy a live entry, and is not even available
//     when the collision is in the READ-ONLY shipped root;
//   * on a genuine collision the two entries are EQUALLY entitled to the name,
//     so choosing one would be a second owner of "which bytes are right" —
//     exactly what `resolveArchiveSiblingFormat`'s ambiguity arm refuses to be.
// At 80 bits a real collision needs ~2^40 entries in one directory before it is
// even likely; the shape this will actually catch is a TRUNCATED or CORRUPTED
// cache entry, which is equally something a user must be told about rather than
// silently recompiled around.
//
// ⚠ AND THE MISSING-SIDECAR CASE MUST REFUSE FOR THE VERIFICATION TO MEAN
// ANYTHING: if an absent key document were a miss, deleting the sidecar would
// restore the un-verified 80-bit behaviour, i.e. the check would be optional and
// therefore not a check.
//
// ★ A COROLLARY WORTH STATING, because it removes a whole class of locking: two
// concurrent builds that compute the SAME key are by definition compiling the
// SAME inputs, so they write the SAME bytes. A race cannot produce a wrong
// artifact — whoever wins, the content is right. Stores are therefore
// write-temp-then-rename with "target already exists" treated as success, never
// as a conflict.
//
// ── WHAT THE KEY COVERS, AND WHY EACH TERM IS THERE ─────────────────────────
//   compiler   — a build-time stamp compiled INTO the binary (see
//                `dss_build_stamp.hpp`). ⚠ NOT the compiler image's content
//                hash: ✔MEASURED 2026-08-17 the debug shared library is
//                419,277,885 bytes, so hashing it per invocation is infeasible
//                (`integrated_tests` spawns the CLI ~450 times). ⚠ And NOT
//                (path,size,mtime) either: that is install-UNSTABLE, so a
//                shipped `dist/release/` would miss on every user's machine and
//                the shipped cache would be worthless. ⚠ And `DSS_PROJECT_VERSION`
//                alone is insufficient — it does not move when codegen does.
//   target     — the `--target` spec verbatim, opaque to this file.
//   format     — the build's object format, and the ARCHIVE SIBLING resolved
//                from it (see `resolveArchiveSiblingFormat`). The sibling is a
//                real input: it is the writer that produces the bytes.
//   config     — debug / release.
//   unit       — the source path AND its content digest.
//   descriptor — ★★★ THE DESCRIPTOR THAT NAMES THE UNIT, BY CONTENT. This is
//                the term the whole design exists for. The unit `#include`s the
//                descriptor's declarations, so the descriptor IS part of the
//                unit's source text. Edit `struct dirent`'s layout and the key
//                must change, or a build links an archive compiled against the
//                OLD layout — which links clean and returns silently wrong
//                bytes. That is the `environ` copy-relocation class, the worst
//                failure mode this project has.
//   config     — the digest of every config document the build ALREADY LOADED.
//   documents    ⚠ NOT a re-walk of `src/dss-config/`: ✔MEASURED 2026-08-17
//                that costs ~165 ms per invocation (86 files, 2,078,133 bytes)
//                and the cost is I/O-dominated (walk+read 152-160 ms, hash only
//                9-13 ms), so a faster hash buys nothing and only not-re-reading
//                helps. Each loader now retains a digest of the bytes it loaded
//                (`GrammarSchema::contentDigest()` and friends), so this term
//                costs the hash alone.
//                ★★ AND THE SET MUST BE ASKED OF THE LOADERS, NEVER HAND-LISTED.
//                ✔MEASURED: the first hand-written list was already WRONG — it
//                named `c.lang.json` and stopped, but that document's
//                `languageReferences` block (line 18) pulls in `asm.lang.json`
//                and its dialects, which are therefore real inputs to compiling
//                a `.c`. A hand-enumerated set is a second owner of the loaders'
//                resolution logic and it drifted on the first attempt.
//
// ⚠ THE ASYMMETRY THAT DECIDES EVERY JUDGEMENT CALL HERE: over-invalidation
// costs one small recompile; under-invalidation ships wrong bytes. When in
// doubt, include the term.
//
// ⓘ What the key deliberately does NOT cover: the OTHER object-format documents.
// Adding or editing one can only move `resolveArchiveSiblingFormat` from UNIQUE
// to AMBIGUOUS — the previously-matching document still matches — and ambiguity
// fails loud. So the only transitions that change the selected sibling's CONTENT
// are edits to that sibling, which IS in the key. Fail-loud covers the rest.
//
// ═══ TWO ROOTS, READ THROUGH ═════════════════════════════════════════════════
// The artifact was originally rooted ONLY at the shipped config root
// (`<configRoot>/runtime/platform/dist/`). For an INSTALLED compiler that root
// is READ-ONLY — it is `<prefix>/share/dss-code-prime/<ver>/dss-config/`, owned
// by the package manager — so the first cache MISS after install could not be
// written and the build could not proceed at all. The cache therefore resolves
// through TWO roots:
//
//   1. the SHIPPED root, `<configRoot>/runtime/platform/dist/` — unchanged,
//      consulted FIRST on lookup, and NEVER written, renamed into, or pruned by
//      this file. That is what makes a packaged `dist/` a legitimate read-only
//      artifact set rather than a directory the compiler happens not to have
//      touched yet.
//   2. a PER-USER WRITABLE root — consulted SECOND on lookup, and THE ONLY ROOT
//      ANY STORE EVER TOUCHES. Its resolution is documented on
//      `resolveRuntimeCacheRoots` below.
//
// ★★★ THE PROPERTY THAT MAKES TWO ROOTS SAFE — and it is the key-as-path rule
// above doing its work a second time. THE KEY IS THE PATH AND THE KEY COVERS
// EVERY INPUT, SO THE SAME KEY NAMES THE SAME FILE IN EITHER ROOT: identical
// inputs produce one relative path, and the two roots differ only in where that
// relative path is anchored. Two roots therefore introduce LOCATION ambiguity
// and NEVER CONTENT ambiguity — a hit in either root is the same artifact, byte
// for byte, so "shipped first" is a preference about which COPY to read and not
// a rule about which copy is CORRECT.
//
// Without key-as-path, read-through would need a precedence rule that actually
// DECIDED something — newest mtime? shipped wins? user wins? — and every such
// rule is a place where a stale artifact becomes reachable again, because each
// one is a second owner of "which bytes are right" that can disagree with the
// inputs. With key-as-path there is nothing to decide: a disagreement between
// the roots is not a case that can arise, so no rule is needed to resolve it.
//
// ⓘ COROLLARY, and it is why the root is deliberately NOT a term in the key
// document: the same key must find the same artifact REGARDLESS of which root
// holds it. Hashing the root in would give the shipped copy and the per-user
// copy different keys, which is precisely the mechanism that would make a
// shipped `dist/` unfindable by any build and the whole packaged-cache story
// worthless.

namespace dss::runtime {

// ── The archive-sibling lookup ──────────────────────────────────────────────
//
// Given the build's object format and its target, find the format document that
// writes the STATIC ARCHIVE for the same machine — the format a runtime unit is
// compiled against.
//
// ★★ IT HAS TWO CALLERS AND THEY ASK FOR DIFFERENT REASONS, WHICH IS WHY THE
// REFUSAL TEXT IS NOT WRITTEN HERE. This started as the runtime object cache's
// private lookup and its messages said so; the archive-MEMBER reader
// ([[D-LK-ARCHIVE-MEMBER-READ-USES-THE-IMAGE-FORMAT-NOT-THE-OBJECT-FORMAT]])
// needs the same answer for the opposite direction — the format that WROTE the
// members it is about to READ. One derivation, two purposes: a refusal saying
// "runtime object cache" to somebody static-linking a `.a` would name a
// component that had nothing to do with the build, so the caller supplies its
// own label and its own anchor rather than inheriting the first caller's.
struct ArchiveSiblingRequester {
    // How the refusal introduces itself, e.g. "runtime object cache" or
    // "static-link". Rendered as the message's leading `<requester>: ` prefix.
    std::string_view label;
    // The anchor a reader who lands on the refusal should go and read. NOT a
    // shared constant: the two callers are documented by different rows.
    std::string_view anchor;
};

// This module's own identity, so neither its tests nor a future production
// caller hand-spells the pair and drifts from it.
inline constexpr ArchiveSiblingRequester kRuntimeCacheSiblingRequester{
    "runtime object cache", "D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF"};
//
// ★★★ SCAN EVERY CANDIDATE; FAIL LOUD ON 0 OR >1. NEVER FIRST-MATCH.
// `directory_iterator` order is SORTED on NTFS and HASH-ORDERED on ext4, so a
// first-match rule decides the answer by filesystem — the exact NTFS-green /
// ext4-red defect this repo already paid for once (see the `asm.lang.json`
// note on a test that loaded "the first discovered dialect"). Proving
// uniqueness requires looking at all of them, so the scan is total by
// construction rather than by discipline.
//
// ★★★ AND `pe` RESOLVING UNIQUELY IS THE DANGEROUS RESULT, NOT THE REASSURING
// ONE. ✔MEASURED 2026-08-17 over all 24 shipped formats: matching on
// (kind, dataModel, artifactProfiles ∋ "staticlib") leaves 6 of 7 exec formats
// AMBIGUOUS — both elf arms collide, both macho arms collide — and only
// `pe64-x86_64-windows-exec` resolves, purely because Windows has a SINGLE arch
// in the tree today. The first arm64-Windows format silently makes it ambiguous
// too. A green built on that would be green by accident of the corpus, which is
// this project's standing lesson: coverage is as wide as the SHAPES the tests
// name, never as the count that passes.
//
// ★★ HOW THE MACHINE IS COMPARED WITHOUT BRANCHING ON THE FORMAT'S IDENTITY —
// and this is the part that would otherwise reproduce the defect it is working
// around. Architecture is NOT a uniform declared property of a format document
// ([[D-CONFIG-FORMAT-DECLARES-NO-UNIFORM-ARCHITECTURE]]): it is reachable only
// through the per-kind `elf.machine` / `pe.machine` / `macho.cputype` fields, so
// reading it HERE would mean `if (kind == Elf) … else if (kind == Pe) …`, which
// is precisely the identity branch the bar vetoes in shared substrate. ⇒ this
// lookup NEVER reads a machine field. It asks the component that already owns
// the target↔format machine relation — `crossValidateTargetFormat` — whether a
// candidate agrees with the build's target, and filters on the two properties a
// format DOES declare uniformly: `kind()` and `container()`. Reuse of an
// existing verb, not a second spelling of it.
// ⓘ This also closes the spirv/wasm case without a special arm: those kinds
// declare no machine and have no archive sibling, so their candidate set is
// EMPTY and the 0-candidate refusal fires with a real diagnostic — never a
// silent skip that would report success having compiled nothing.
[[nodiscard]] DSS_EXPORT std::expected<std::string, std::string>
resolveArchiveSiblingFormat(ObjectFormatSchema const&      buildFormat,
                            TargetSchema const&            target,
                            std::filesystem::path const&   objectFormatsDir,
                            ArchiveSiblingRequester const& requester);

// ── The two roots ───────────────────────────────────────────────────────────

struct DSS_EXPORT RuntimeCacheRoots {
    // Read-only. ALWAYS present — it is a pure function of the config root, so
    // there is no arm in which it fails to resolve.
    std::filesystem::path shipped;

    // The only writable root. EMPTY iff no candidate resolved at all — which is
    // not an error HERE, because a build that HITS in the shipped root never
    // needs to write anything. It becomes an error at the first store, where a
    // write is actually required; see `storeRuntimeObject`.
    std::filesystem::path perUser;

    // Every candidate in precedence order — set or unset, with the composed
    // path and a marker on the one selected. ALWAYS populated, including on the
    // success path, so the refusal below never has to re-derive it (a second
    // derivation could disagree with the one that actually ran).
    std::string trail;
};

// Resolve both roots.
//
// ── THE PER-USER ROOT'S PRECEDENCE, AND WHAT EACH ARM IS FOR ────────────────
//   1. `DSS_RUNTIME_CACHE_DIR`, if set and non-empty — WINS OUTRIGHT over every
//      platform default. This is the explicit override for CI and hermetic
//      builds, and it takes the directory VERBATIM: the caller already named
//      the location, so no vendor tail is appended under it.
//   2. `%LOCALAPPDATA%` + `dss-code-prime/runtime-cache` — the Windows per-user
//      non-roaming cache location.
//   3. `$XDG_CACHE_HOME` + `dss-code-prime/runtime-cache` — the XDG base
//      directory spec's cache root, when the user has set one.
//   4. `$HOME` + `.cache/dss-code-prime/runtime-cache` — the XDG spec's own
//      documented default when `XDG_CACHE_HOME` is unset.
//   ⇒ NONE resolves: `perUser` is EMPTY, lookup still consults the shipped
//      root (so an installed compiler with a packaged `dist/` keeps working),
//      and the first store REFUSES, naming all four candidates and the
//      override. See `storeRuntimeObject`.
//
// ★★ THERE IS NO `#ifdef` IN THIS RESOLUTION, AND THAT IS THE DESIGN RATHER
// THAN AN ACCIDENT. The platform arm is selected by WHICH VARIABLE THE PLATFORM
// DEFINES — `LOCALAPPDATA` simply does not exist on Linux or macOS, and the
// candidate is skipped there for the same reason an unset variable is skipped
// anywhere. A compile-time branch would make the Linux arms unreachable from a
// Windows build and therefore untestable from it, which is exactly the shape
// this repo's cross-host rule exists to prevent. One list, walked identically
// everywhere, and a Git-Bash-on-Windows session that exports `HOME` but no
// `LOCALAPPDATA` lands on arm 4 without a special case.
//
// ★ A CANDIDATE "RESOLVES" WHEN ITS VARIABLE IS SET AND NON-EMPTY — existence
// of the directory is NOT required and writability is NOT probed. Requiring
// existence would break the first run after install, which is the case this
// mechanism was built for. Probing writability would be a second owner of the
// write attempt: it can race, it can disagree with the real `open`, and an
// answer it got wrong would silently demote a perfectly good root. The STORE is
// the writer, so the store is what reports what actually happened.
//
// ⚠ NOT CACHED IN A FUNCTION-LOCAL STATIC. The environment IS the input here,
// so a first-call cache would freeze whichever value the first caller happened
// to see and make the override untestable — a test that sets
// `DSS_RUNTIME_CACHE_DIR` after any earlier call would silently exercise the
// previous answer and pass while asserting nothing.
[[nodiscard]] DSS_EXPORT RuntimeCacheRoots
resolveRuntimeCacheRoots(std::filesystem::path const& configRoot);

// A build stamp rendered as ONE filesystem-safe path component.
//
// ★★ COMPILER IDENTITY IS IN THE PER-USER ROOT PATH, NOT ONLY IN THE KEY
// DOCUMENT, AND THE TWO ARE NOT REDUNDANT OWNERS OF ONE FACT. The stamp is
// already hashed into the key, and the key is the FILENAME, so two compiler
// builds cannot collide on an artifact no matter where it lives — collision is
// impossible before this segment exists. The segment buys OPERABILITY, which
// the digest cannot: two installed versions get separate directory trees, so a
// user can delete ONE version's cache with one `rm -rf`, and a person browsing
// the cache can tell whose it is. A 64-hex filename answers neither question.
//
// ⓘ The SHIPPED root carries no such segment and needs none: it lives inside
// the installed `<ver>/dss-config/` tree, so it is already scoped to exactly
// one compiler installation. The per-user root is the one that is SHARED across
// installations, which is what makes the segment load-bearing there.
//
// ⓘ Exported so the segment's shape is assertable from a test that cannot see
// `DSS_BUILD_STAMP` (the test binary deliberately does not opt into the stamp —
// see `tests/program/CMakeLists.txt`), and so two DIFFERENT stamps can be shown
// to render two different segments without rebuilding the compiler.
[[nodiscard]] DSS_EXPORT std::string
buildStampPathSegment(std::string_view buildStamp);

// The segment for THE RUNNING COMPILER — `buildStampPathSegment(kBuildStamp)`.
// The stamp macro is reachable from exactly one translation unit (see the
// review-stop note in `dss_build_stamp.hpp`), so this accessor is how anyone
// else observes the value that ends up in the path.
[[nodiscard]] DSS_EXPORT std::string runtimeCacheBuildStampSegment();

// ── The key ─────────────────────────────────────────────────────────────────

// One config document that participated in a build, as reported BY ITS LOADER.
// `label` disambiguates roles so two documents cannot silently swap places in
// the digest; `digest` is the loader's own `contentDigest()`.
struct DSS_EXPORT LoadedConfigDocument {
    std::string label;    // "language" / "target" / "format" / "descriptor" / …
    std::string path;     // config-root-relative, generic separators
    std::string digest;   // 64 lowercase hex, from the loader
};

struct DSS_EXPORT RuntimeObjectRequest {
    std::filesystem::path     configRoot;      // absolute .../src/dss-config
    std::string               descriptorPath;  // config-root-relative
    std::string               sourcePath;      // config-root-relative
    std::string               targetSpec;      // the --target string, verbatim
    std::string               buildFormatName;
    std::string               siblingFormatName;
    std::string               configName;      // "debug" / "release"
    std::vector<LoadedConfigDocument> loadedDocuments;
};

struct DSS_EXPORT RuntimeObjectKey {
    // The exact bytes hashed — diagnosable, testable, AND WRITTEN TO DISK: this
    // is the content of the `.key` document beside the artifact, and the thing
    // a hit is verified against.
    std::string document;

    // ★★★ THE IDENTITY. 64 lowercase hex — the full SHA-256 of `document`.
    // Every equality question about "is this the same cache entry?" is answered
    // on this field (or on `document` itself), NEVER on `pathDigest`.
    std::string digest;

    // ★★★ THE INDEX, AND IT IS NOT THE KEY. 16 lowercase base32 characters
    // carrying the first 80 BITS of `digest`'s bytes — enough to make a
    // collision an event nobody will observe, short enough to fit a Windows
    // path, and NEVER enough to stand in for the identity. It exists to name a
    // file. The verification that makes truncation safe is the `.key` document
    // (see the ★★★ section at the top of this header).
    std::string pathDigest;

    // ★ THE SAME RELATIVE PATH IN EITHER ROOT — `<config>/<target-slug>/
    // <unit-stem>-<pathDigest>.a`. This is the field the two-roots property is
    // ABOUT: both absolute paths below are this one relative path anchored at a
    // different root, which is why a hit in either root is the same artifact.
    std::filesystem::path relativePath;

    // `roots.shipped / relativePath`. Always populated; read-only.
    std::filesystem::path shippedArtifactPath;

    // `roots.perUser / relativePath`, or EMPTY when no per-user root resolved.
    // ⚠ EMPTY IS NOT "no artifact" — it is "nowhere to write one", and it is
    // the state `storeRuntimeObject` refuses on.
    std::filesystem::path userArtifactPath;

    // `RuntimeCacheRoots::trail`, carried so a store refusal can name every
    // root that was considered without re-resolving them.
    std::string rootTrail;
};

// ⚠ An EMPTY `digest` on any loaded document is a REFUSAL, not a skip: it means
// a document reached the build through a path that did not record what it
// loaded, and a key computed over an unknown input is exactly the "cache serves
// a key it cannot verify" defect. Empty is detectable; wrong is not.
[[nodiscard]] DSS_EXPORT std::expected<RuntimeObjectKey, std::string>
computeRuntimeObjectKey(RuntimeObjectRequest const& request);

// ── The key document beside an artifact ─────────────────────────────────────

// `<…>/<stem>-<pathDigest>.a` → `<…>/<stem>-<pathDigest>.key`.
//
// ⓘ ONE SPELLING, derived rather than composed a second time, so the artifact
// and its key document cannot drift apart in the two files that build them.
// The extension is REPLACED (never appended): `.a.key` would make the longest
// name in the tree two characters longer again, and the path budget this whole
// change is about is measured on the LONGEST name.
[[nodiscard]] DSS_EXPORT std::filesystem::path
runtimeKeyDocumentPath(std::filesystem::path const& artifactPath);

// ── Store / lookup ──────────────────────────────────────────────────────────

// READ THROUGH: the SHIPPED root first, the PER-USER root second. The optional
// is engaged iff a VERIFIED artifact for exactly this key exists in one of
// them, and the returned path says WHICH — callers that report or link the
// artifact need the real location, not a boolean.
//
// A MISS IS NORMAL and is never an error. An UNVERIFIABLE HIT is — that is the
// `std::expected` error arm, and it is the mechanism that keeps the 16-character
// path index from being a weakening (see the ★★★ section at the top of this
// header). Three shapes reach it, all treated alike because all three mean *the
// entry at this path cannot be shown to be this key's*:
//   * the artifact is present and its `.key` document is ABSENT;
//   * the `.key` document cannot be read;
//   * the `.key` document differs from `key.document`.
//
// ⓘ An EMPTY `userArtifactPath` costs this function nothing, and the reason is
// worth stating because it looks like a silent skip and is not: nothing can
// ever have been written to a root that never resolved, because the store on
// that path REFUSES. So there is no artifact being passed over — the second
// probe would be a probe of a location no writer could have reached.
//
// ⚠ A REFUSAL IN THE SHIPPED ROOT DOES NOT FALL THROUGH TO THE PER-USER ROOT.
// Falling through would silently tolerate a corrupt or foreign entry in the
// packaged tree — and the two roots hold the SAME bytes for the same key by
// construction, so a shipped mismatch is a real anomaly, never a routine miss.
[[nodiscard]] DSS_EXPORT
std::expected<std::optional<std::filesystem::path>, std::string>
lookupRuntimeObject(RuntimeObjectKey const& key);

// Write-temp-then-rename INTO THE PER-USER ROOT — never into the shipped root,
// which this file treats as read-only in every arm including pruning. TWO files
// land: the `.key` document FIRST, then the artifact.
//
// ★★ THE ORDER IS LOAD-BEARING AND IT IS THE ONLY ORDER THAT IS SAFE. A crash
// between the two renames must never leave an artifact with no key document,
// because that state is a REFUSAL for every later lookup — a build bricked by
// an interrupted predecessor. Sidecar-first makes the surviving intermediate
// state "a key document with no artifact", which is a plain MISS: the next
// store re-writes both and nothing is lost.
//
// "Already exists" is SUCCESS (same key ⇒ same bytes) — and that inference is
// now BACKED rather than assumed, because the sidecar write verifies an
// existing `.key` document byte for byte first and REFUSES on a mismatch.
//
// ⛔ A MISS THAT CANNOT BE WRITTEN IS A REFUSAL. There is deliberately NO
// degradation to compile-and-discard and NO silent uncached path: either would
// report success while every subsequent build recompiled the same unit, so the
// cost would be invisible and permanent, and a mechanism that silently does
// nothing is worse than one that fails. The diagnostic names EVERY root
// considered, the reason the write failed, and `DSS_RUNTIME_CACHE_DIR` as the
// remedy — the three things a user needs to fix it without reading this file.
//
// ⓘ It does NOT re-check the shipped root before writing. `lookupRuntimeObject`
// owns the read-through precedence, and a caller reaching this function has
// already missed in both roots; re-deriving that answer here would be a second
// owner of the precedence rule, free to drift from the first.
//
// ⓘ Pruning of superseded siblings is best-effort and its failure is NOT an
// error — an open artifact cannot be unlinked on Windows, and the cache's
// correctness never depends on old entries being gone, only on them being
// unreachable, which the key-as-path already guarantees.
[[nodiscard]] DSS_EXPORT std::expected<std::filesystem::path, std::string>
storeRuntimeObject(RuntimeObjectKey const&        key,
                   std::span<std::uint8_t const>  bytes);

} // namespace dss::runtime
