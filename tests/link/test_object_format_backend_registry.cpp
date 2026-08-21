// ─────────────────────────────────────────────────────────────────────────
// The object-format BACKEND SEAM: fail-closed resolution, and a whole-family
// mutation probe that proves the identity rules are still RUNNING.
// D-LINK-OBJECT-FORMAT-SCHEMA-RETAINS-KIND-IDENTITY-BRANCHES (TF-C125).
// ─────────────────────────────────────────────────────────────────────────
//
// ★★★ WHY THIS FILE EXISTS, stated as the failure it is built to catch rather
// than as the feature it covers.
//
// TF-C125 moved ~1,830 lines of per-format identity rules out of the schema
// triple and into five backends reached through an abstract interface. The
// schema tier now asks a resolved `ObjectFormatBackend` to validate a document
// instead of running `if (kind == ObjectFormatKind::Elf) { … }` itself.
//
// That refactor has exactly one catastrophic failure mode, and it is SILENT:
// if the registry lookup ever yields null and the null case is treated as
// "no per-format rules apply", then **all 24 shipped formats validate CLEAN
// while validating NOTHING**. Every positive test still passes — they assert
// a format LOADS, and it would. And the negative tests would keep passing too,
// because they assert only that SOMETHING rejected, and their fixtures are
// malformed in more than one way at once. There were 38 `loadShipped` calls in
// this tree before this file and every one of them was POSITIVE. Nothing
// anywhere asked "is the ELF machine rule still capable of firing?"
//
// So this file asks it, for every rule, on every shipped format, by BREAKING
// the subject. For each of the 24 shipped `.format.json` files it: loads the
// real file and asserts it is clean; then removes each REQUIRED identity field
// in turn and demands a rejection AT THAT EXACT JSON POINTER. A registry that
// silently stopped resolving does not fail one assertion here — it fails a
// hundred, and it names which rule went quiet.
//
// ⚠ AND THE FIXTURES ARE THE SHIPPED FILES THEMSELVES, mutated in memory —
// never hand-written copies. A hand-written "ELF-like" fixture proves the rule
// fires on the fixture; only the real file proves it fires on what we ship.

#include "link/object_format_backend.hpp"
#include "link/object_format_schema.hpp"

#include "format_reject_support.hpp"   // countAtPath / rejectSummary
#include "repo_root.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>   // std::pair — reached transitively under GCC, NOT guaranteed under MSVC
#include <vector>

namespace fs = std::filesystem;
using namespace dss;
using json = nlohmann::json;
using dss::link_format::test::countAtPath;
using dss::link_format::test::countWithMessage;
using dss::link_format::test::rejectSummary;

namespace {

[[nodiscard]] fs::path objectFormatsDir() {
    auto const root = dss::test::findRepoRoot();
    if (!root) {
        ADD_FAILURE() << dss::test::repoRootDiagnostic();
        return {};
    }
    return *root / "src" / "dss-config" / "object-formats";
}

struct ShippedFormat {
    std::string name;   // file stem
    std::string text;   // the raw file bytes
    json        doc;
    std::string kind;   // its declared `format.kind`
};

// Enumerated FROM DISK, never from a hard-coded list: a format added tomorrow
// is probed the day it lands. A hard-coded list is how a new format ships
// without anybody noticing its rules were never exercised.
[[nodiscard]] std::vector<ShippedFormat> shippedFormats() {
    std::vector<ShippedFormat> out;
    auto const dir = objectFormatsDir();
    if (dir.empty()) return out;

    std::vector<fs::path> paths;
    std::error_code ec;
    for (auto const& entry : fs::directory_iterator{dir, ec}) {
        auto const p = entry.path();
        if (p.filename().string().find(".format.json") == std::string::npos) continue;
        paths.push_back(p);
    }
    std::sort(paths.begin(), paths.end());

    for (auto const& p : paths) {
        std::ifstream in{p, std::ios::binary};
        if (!in) { ADD_FAILURE() << "cannot open " << p.string(); continue; }
        ShippedFormat f;
        f.text.assign(std::istreambuf_iterator<char>{in},
                      std::istreambuf_iterator<char>{});
        f.doc = json::parse(f.text, nullptr, /*allow_exceptions=*/false);
        if (f.doc.is_discarded() || !f.doc.is_object()) {
            ADD_FAILURE() << p.filename().string() << " is not a JSON object";
            continue;
        }
        auto const filename = p.filename().string();
        f.name = filename.substr(
            0, filename.size() - std::string_view{".format.json"}.size());
        if (f.doc.contains("format") && f.doc.at("format").contains("kind")) {
            f.kind = f.doc.at("format").at("kind").get<std::string>();
        }
        out.push_back(std::move(f));
    }
    return out;
}

// One mutation: erase `<block>/<field>` and expect a rejection at `<pointer>`.
struct IdentityMutation {
    char const* block;    // root block holding the field ("elf" / "pe" / …)
    char const* field;    // the key to erase
    char const* pointer;  // the JSON pointer the rejection MUST carry
};

// ★ The required identity fields, per declared kind, WITH the pointer each
// rule is pinned at. These pointers are the contract this refactor had to
// preserve: the rules moved TU, the pointers did not move at all.
//
// MEASURED alongside this file, method stated so the number is checkable:
// extracting every `"/…"` JSON-pointer string literal from the tier (the two
// schema TUs before, plus the five backend TUs after) gives 112 distinct
// pointers before and 112 after — ZERO lost, ZERO gained. An independent audit
// re-measured with a different extraction and got 107/107; the counts differ
// with the extraction, the load-bearing half (nothing lost, nothing gained)
// was reproduced by both.
[[nodiscard]] std::vector<IdentityMutation> mutationsFor(std::string const& kind) {
    if (kind == "elf") {
        return {
            {"elf", "class",   "/elf/class"},
            {"elf", "data",    "/elf/data"},
            {"elf", "machine", "/elf/machine"},
        };
    }
    if (kind == "pe") {
        return {{"pe", "machine", "/pe/machine"}};
    }
    if (kind == "macho") {
        return {{"macho", "cputype", "/macho/cputype"}};
    }
    // wasm / spirv declare no identity block at all — their rules are
    // REJECTIONS of substrate keys, probed by `RejectedRootFieldStillFires`.
    return {};
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════
// 1. THE REGISTRY FAILS CLOSED. Tested first, deliberately — everything else
//    in this file is meaningless if a null backend can mean "skip the rules".
// ═════════════════════════════════════════════════════════════════════════

TEST(ObjectFormatBackendRegistry, UnknownSpellingResolvesToNullNeverToADefault) {
    using dss::link::objectFormatBackendByConfigName;

    // The reserved sentinel is claimed by NOBODY. This is the single most
    // important assertion in the file: if some backend answered to "unknown",
    // or if the resolver fell back to its first entry, a schema declaring the
    // sentinel would acquire that backend's identity rules and its walker.
    EXPECT_EQ(objectFormatBackendByConfigName("unknown"), nullptr)
        << "the invalid sentinel must resolve to NO backend";
    EXPECT_EQ(objectFormatBackendByConfigName(""), nullptr);
    EXPECT_EQ(objectFormatBackendByConfigName("elff"), nullptr)
        << "a near-miss spelling must not fuzzy-match";
    EXPECT_EQ(objectFormatBackendByConfigName("ELF"), nullptr)
        << "resolution is exact — a case-folded match would let a config file "
           "pick a backend by accident";
    EXPECT_EQ(objectFormatBackendByConfigName("notaformat"), nullptr);

    // …and the real spellings DO resolve, so the nulls above are not simply a
    // resolver that never returns anything.
    for (char const* name : {"elf", "pe", "macho", "wasm", "spirv"}) {
        EXPECT_NE(objectFormatBackendByConfigName(name), nullptr)
            << "shipped config spelling '" << name << "' must resolve";
    }
}

TEST(ObjectFormatBackendRegistry, TableIsWellFormedAndSelfConsistent) {
    auto const table = dss::link::objectFormatBackendTable();
    ASSERT_FALSE(table.empty()) << "an empty table would make EVERY format "
                                  "unresolvable — the catastrophic state";

    std::set<std::string> names;
    for (auto const* b : table) {
        ASSERT_NE(b, nullptr) << "a null row would crash resolution";
        EXPECT_FALSE(b->configName().empty())
            << "a nameless backend is unreachable by config, so it would be "
               "silently dead rather than loudly missing";
        EXPECT_TRUE(names.insert(std::string{b->configName()}).second)
            << "duplicate config name '" << b->configName()
            << "' — the FIRST match wins the linear scan, so the second row "
               "would be dead code that a maintainer believes is live";
        // Round-trip: every table entry must be reachable through the same
        // resolver the loader uses. A backend in the table that the resolver
        // cannot find is the MSVC-discards-the-TU failure wearing a disguise.
        EXPECT_EQ(dss::link::objectFormatBackendByConfigName(b->configName()), b)
            << "table entry '" << b->configName()
            << "' is not reachable through the resolver";
    }
}

TEST(ObjectFormatBackendRegistry, NullBackendSchemaFailsClosedOnEveryPredicate) {
    // `ObjectFormatSchema{ObjectFormatData}` is a PUBLIC constructor that runs
    // no validation at all, so a struct with no backend can be handed straight
    // to the engine. Every predicate must answer the STRICT direction.
    //
    // ★ `allowsUndefinedImports()` is the one that matters and the one that is
    // easy to get backwards: `true` is the PERMISSIVE answer there, and the
    // relocatable short-circuit (`!isImageFlavor()`) is true on a null backend
    // — so the obvious implementation makes an unresolvable schema the most
    // permissive object in the tree.
    dss::detail::ObjectFormatData data;
    data.name = "synthetic-no-backend";
    ASSERT_EQ(data.backend, nullptr) << "the DEFAULT must be null — the field "
                                        "this replaced defaulted to ELF";
    ObjectFormatSchema schema{std::move(data)};

    EXPECT_EQ(schema.kind(), ObjectFormatKind::Unknown)
        << "a backend-less schema must report the sentinel, never a format";
    EXPECT_EQ(schema.backend(), nullptr);
    EXPECT_FALSE(schema.isImageFlavor());
    EXPECT_FALSE(schema.isExecFlavor());
    EXPECT_FALSE(schema.allowsUndefinedImports())
        << "a schema with no resolvable format must NOT be granted the most "
           "permissive undefined-import policy in the tree";
}

TEST(ObjectFormatBackendRegistry, NullBackendIsRejectedByValidate) {
    dss::detail::ObjectFormatData data;
    data.name               = "synthetic-no-backend";
    data.dataModel          = DataModel::Lp64;
    data.headerNameMatching = HeaderNameMatching::CaseSensitive;
    data.cSymbolDecoration.scheme = CSymbolDecorationScheme::None;

    auto const problems = data.validate();
    bool sawKind = false;
    for (auto const& p : problems) {
        if (p.path == "/format/kind") sawKind = true;
    }
    EXPECT_TRUE(sawKind)
        << "a hand-built ObjectFormatData with no backend must be rejected at "
           "/format/kind — this is the arm that replaced the `kind == Unknown` "
           "sentinel test, and it is strictly stronger: the old field DEFAULTED "
           "to ObjectFormatKind::Elf, so a default-constructed struct sailed "
           "past claiming an ELF identity with elf.machine == 0 (EM_NONE)";
}

// ═════════════════════════════════════════════════════════════════════════
// 2. THE WHOLE-FAMILY MUTATION PROBE.
// ═════════════════════════════════════════════════════════════════════════

TEST(ObjectFormatMutationProbe, EveryShippedFormatLoadsClean) {
    auto const formats = shippedFormats();
    ASSERT_GE(formats.size(), 24u)
        << "expected at least the 24 shipped formats — a shrinking corpus "
           "would quietly reduce this file's coverage to nothing";
    for (auto const& f : formats) {
        SCOPED_TRACE(f.name);
        auto const r = ObjectFormatSchema::loadFromText(f.text, f.name);
        EXPECT_TRUE(r.has_value())
            << "shipped format failed to load: " << rejectSummary(r);
    }
}

// ★★ THE CENTRAL TEST. Break each required identity field of each shipped
// format and demand a rejection at that exact pointer.
TEST(ObjectFormatMutationProbe, RemovingARequiredIdentityFieldRejectsAtItsPointer) {
    auto const formats = shippedFormats();
    ASSERT_FALSE(formats.empty());

    std::size_t mutationsRun = 0;
    std::map<std::string, std::size_t> perKind;
    for (auto const& f : formats) {
        for (auto const& m : mutationsFor(f.kind)) {
            SCOPED_TRACE(f.name + " :: erase " + m.block + "." + m.field);
            ASSERT_TRUE(f.doc.contains(m.block))
                << "the shipped file does not declare the '" << m.block
                << "' block at all — either the mutation table is stale or a "
                   "shipped format lost its identity block";
            json mutated = f.doc;
            ASSERT_EQ(mutated.at(m.block).erase(m.field), 1u)
                << "field '" << m.field << "' was not present to erase — a "
                   "mutation that removes nothing PROVES NOTHING, and a probe "
                   "that silently no-ops is the exact vacuity this file exists "
                   "to prevent";

            auto const r = ObjectFormatSchema::loadFromText(mutated.dump(), f.name);
            ASSERT_FALSE(r.has_value())
                << "removing a REQUIRED identity field left the format still "
                   "loading — the per-format rules are not running";
            EXPECT_EQ(countAtPath(r, m.pointer), 1u)
                << "expected exactly one rejection at " << m.pointer << "\n"
                << rejectSummary(r);
            ++mutationsRun;
            ++perKind[f.kind];
        }
    }
    // ★ EXACT, NOT `>=`. An independent audit caught the first version pinning
    // `>= 24` against an actual 42: `mutationsFor("pe")` and
    // `mutationsFor("macho")` could BOTH have silently returned {} — 30
    // mutations gone — and the pin would still have passed. Those two
    // single-entry families are exactly the ones a `format.kind` spelling
    // change would drop, which is the failure the old comment NAMED and then
    // pinned a number too weak to detect.
    //
    // 10 elf x 3 fields + 4 pe x 1 + 8 macho x 1 = 42.
    EXPECT_EQ(mutationsRun, 42u)
        << "the mutation count moved. If a format was ADDED, update this "
           "number. If it DROPPED, a `format.kind` spelling no longer matches "
           "`mutationsFor` and a whole family stopped being probed.";
    EXPECT_EQ(perKind["elf"], 30u);
    EXPECT_EQ(perKind["pe"], 4u)
        << "the PE family stopped being probed entirely";
    EXPECT_EQ(perKind["macho"], 8u)
        << "the Mach-O family stopped being probed entirely";
}

TEST(ObjectFormatMutationProbe, ForeignIdentityBlockRejectsAtThatBlock) {
    // The cross-block ownership rule — what `kCrossKindRules[]` used to be.
    // A stray block of the wrong kind must be rejected AT THE BLOCK, because
    // a silently-dropped block is a format whose declared bytes never ship.
    //
    // ★ DRIVEN FROM THE REGISTRY, NOT FROM A HAND-WRITTEN PAIR. The first
    // version injected only `elf` and `macho` — 2 of the 5 rows the old table
    // carried — leaving `pe`, `optionalHeader` and `image` ownership UNPROBED
    // anywhere in the suite. An independent audit measured that: dropping
    // `"optionalHeader"` from `kPeBlocks` or `"image"` from `kMachOBlocks`
    // would silently accept a stray block on a foreign-kind document and the
    // whole suite would stay green. The refactor made that easier to lose, not
    // harder, by moving the pairing from one table into five per-backend
    // arrays — so the probe now enumerates EVERY block EVERY backend declares
    // and injects each into every document that does not own it.
    auto const formats = shippedFormats();
    ASSERT_FALSE(formats.empty());

    // Every (owner, blockName) pair the registry declares.
    std::vector<std::pair<std::string, std::string>> owned;
    for (auto const* b : dss::link::objectFormatBackendTable()) {
        for (char const* blk : b->identityBlockNames()) {
            owned.emplace_back(std::string{b->configName()}, std::string{blk});
        }
    }
    ASSERT_EQ(owned.size(), 5u)
        << "expected the 5 declared identity blocks (elf; pe + optionalHeader; "
           "macho + image). If a backend stopped declaring one, the ownership "
           "rule for it stopped being enforced AND stopped being probed — the "
           "pair of failures that hides a silent drop.";

    std::map<std::string, std::size_t> perBlock;
    for (auto const& f : formats) {
        for (auto const& [owner, blk] : owned) {
            if (owner == f.kind) continue;      // this document owns it
            if (f.doc.contains(blk)) continue;  // present legitimately
            SCOPED_TRACE(f.name + " :: inject stray '" + blk + "' block");
            json mutated = f.doc;
            mutated[blk]  = json::object();
            auto const r = ObjectFormatSchema::loadFromText(mutated.dump(), f.name);
            ASSERT_FALSE(r.has_value())
                << "a stray identity block owned by another backend must "
                   "reject — a silently dropped block is a format whose "
                   "declared bytes never ship";
            EXPECT_EQ(countAtPath(r, std::string{"/"} + blk), 1u) << rejectSummary(r);
            ++perBlock[blk];
        }
    }
    // Every declared block must have been probed on a foreign document.
    for (auto const& [owner, blk] : owned) {
        EXPECT_GT(perBlock[blk], 0u)
            << "block '" << blk << "' (owned by '" << owner
            << "') was never injected into a foreign document — its ownership "
               "rule is unprobed";
    }
}

TEST(ObjectFormatMutationProbe, RejectedRootFieldStillFires) {
    // WASM and SPIR-V own no identity block; their rules are REJECTIONS of
    // substrate keys, now declared by the backend rather than enumerated by
    // the loader behind `if (data.kind == Wasm || … Spirv)`.
    // ★ EVERY DECLARED KEY, READ FROM THE BACKEND — not a hand-written triple.
    // The first version probed 3 of the 14 rejected fields, so the other 11
    // (`processExit`, `tlsAccess`, `stackReserveControl`, …) could be dropped
    // from either backend's array with nothing going red. An independent audit
    // measured that. The probe now asks the backend what it claims to reject
    // and checks that it actually does — so the list and its enforcement
    // cannot drift apart.
    auto const formats = shippedFormats();
    ASSERT_FALSE(formats.empty());

    std::map<std::string, std::size_t> perFormat;
    for (auto const& f : formats) {
        auto const* backend = dss::link::objectFormatBackendByConfigName(f.kind);
        ASSERT_NE(backend, nullptr) << f.name;
        auto const declared = backend->rejectedRootFields();
        if (declared.empty()) continue;   // elf / pe / macho reject nothing

        for (char const* key : declared) {
            SCOPED_TRACE(f.name + " :: inject " + key);
            ASSERT_FALSE(f.doc.contains(key))
                << "the shipped file already declares '" << key
                << "' — this mutation would not be a mutation, and a probe "
                   "that no-ops proves nothing";
            json mutated = f.doc;
            mutated[key] = json::object();
            auto const r = ObjectFormatSchema::loadFromText(mutated.dump(), f.name);
            ASSERT_FALSE(r.has_value())
                << "a root key this format DECLARES it cannot express must "
                   "reject, not be silently ignored by the walker";
            // ★ ASSERT THE RULE'S OWN MESSAGE, NOT JUST THE POINTER — and this
            // is a correction the strengthened probe earned on its first run.
            // `json::object()` is the wrong SHAPE for several of these keys, so
            // some also draw a type diagnostic at the same pointer and a
            // `countAtPath == 1` reds for a reason unrelated to the rule. The
            // rule under test is PRESENCE, so pin the presence message exactly
            // once and let the pointer count be >= 1.
            EXPECT_GE(countAtPath(r, std::string{"/"} + key), 1u) << rejectSummary(r);
            EXPECT_EQ(countWithMessage(
                          r, std::string{"must not declare a top-level '"} + key),
                      1u)
                << "the dead-key rejection itself did not fire for '" << key
                << "'\n" << rejectSummary(r);
            ++perFormat[f.name];
        }
    }
    // Both bytecode formats must have been probed across their WHOLE list.
    ASSERT_EQ(perFormat.size(), 2u) << "expected exactly wasm + spirv";
    for (auto const& [name, n] : perFormat) {
        // ⚠ WRITTEN OUT ON PURPOSE — do NOT replace it with
        // `backend->rejectedRootFields().size()`. Deriving the expectation from
        // the very list under test makes the assertion self-satisfying: a key
        // deleted from the array would take the expected count down with it and
        // nothing would go red. The constant is the teeth. Bump it in the SAME
        // change that adds or removes an entry, and only then.
        // 14 → 15 with `weakDefinition`
        // (D-CONFIG-WEAK-DEFINITION-DIALECT-NOT-DECLARED).
        EXPECT_EQ(n, 15u)
            << name << " probed " << n << " rejected root fields; the declared "
               "list is 15 long. A shrinking list means keys stopped being "
               "rejected AND stopped being probed at the same time.";
    }
}

TEST(ObjectFormatMutationProbe, SentinelAndUnknownKindAreBothRejectedAtFormatKind) {
    auto const formats = shippedFormats();
    ASSERT_FALSE(formats.empty());
    auto const& f = formats.front();

    for (char const* bad : {"unknown", "elff", "", "ELF"}) {
        SCOPED_TRACE(std::string{"kind = "} + bad);
        json mutated = f.doc;
        mutated["format"]["kind"] = bad;
        auto const r = ObjectFormatSchema::loadFromText(mutated.dump(), f.name);
        ASSERT_FALSE(r.has_value())
            << "an unresolvable format.kind must reject — resolution FAILS "
               "CLOSED, it does not fall through to a default backend";
        EXPECT_EQ(countAtPath(r, "/format/kind"), 1u)
            << "and it must be the ONLY diagnostic: continuing the parse under "
               "an unresolved format would emit one spurious 'identity block X "
               "is only meaningful when …' per declared block\n"
            << rejectSummary(r);
    }
}

// ═════════════════════════════════════════════════════════════════════════
// 3. CAPABILITY, NEVER IDENTITY — the interface rule, pinned on real files.
// ═════════════════════════════════════════════════════════════════════════

// ═════════════════════════════════════════════════════════════════════════
// 2b. THE SECOND NET — the identity comparisons the compile pin CANNOT see.
// ═════════════════════════════════════════════════════════════════════════

TEST(ObjectFormatBackendRegistry, SchemaTierDoesNotCompareFormatIdentity) {
    // ★ WHY A SOURCE SCAN EXISTS AT ALL, WHEN THE POINT OF THE PIN WAS TO
    // AVOID ONE. The `#define` pin in both schema TUs makes the TYPE's name
    // unspellable, in every form. It cannot see an identity comparison that
    // never names the type — and an independent audit measured four such
    // spellings compiling straight through:
    //     backend->configName() == "elf"
    //     objectFormatKindFromName("elf") == schema.kind()
    //     objectFormatKindName(k) == "pe"
    // The first is the likeliest re-introduction of all, because the string is
    // right there on the interface the loader already holds, and NEITHER the
    // pin NOR the agnosticism grep in dss-audit's SKILL.md (which looks for
    // `== …Kind::`) would see it.
    //
    // ⚠ THIS IS WEAKER THAN THE PIN AND IS LABELLED AS SUCH. A grep-shaped
    // test is vacuous until somebody violates it, which is exactly the
    // objection this cycle raised against using one for the type itself. It is
    // shipped as a SECOND net over the residue, not as the primary mechanism —
    // and it is demonstrated red-on-disable rather than assumed to work.
    auto const root = dss::test::findRepoRoot();
    ASSERT_TRUE(root.has_value()) << dss::test::repoRootDiagnostic();

    // ⚠ COMMENTS ARE STRIPPED FIRST, and that is not tidiness. This very file
    // documents the forbidden spellings IN PROSE, a few lines above; a scanner
    // that read comments would fail on its own explanation. The mirror-image
    // mistake — a scanner that does NOT strip, over a subject whose comment
    // carries the token — produced a false GREEN elsewhere in this project on
    // 2026-08-06. Strip, then scan.
    auto stripComments = [](std::string const& src) {
        std::string out;
        out.reserve(src.size());
        for (std::size_t i = 0; i < src.size(); ++i) {
            if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/') {
                while (i < src.size() && src[i] != '\n') ++i;
                out.push_back('\n');
            } else if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*') {
                i += 2;
                while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) ++i;
                ++i;
            } else {
                out.push_back(src[i]);
            }
        }
        return out;
    };

    char const* const kForbidden[] = {
        "configName() ==",   "configName()==",
        "== backend->configName", "objectFormatKindFromName",
        "objectFormatKindName",
    };

    std::size_t scanned = 0;
    for (char const* rel : {"src/link/object_format_schema.cpp",
                            "src/link/object_format_schema_json.cpp"}) {
        SCOPED_TRACE(rel);
        std::ifstream in{*root / rel, std::ios::binary};
        ASSERT_TRUE(in.good()) << "cannot open " << rel;
        std::string const raw{std::istreambuf_iterator<char>{in},
                              std::istreambuf_iterator<char>{}};
        ASSERT_FALSE(raw.empty());
        std::string const code = stripComments(raw);
        // Fail-closed: if stripping ever silently emptied the file, the scan
        // below would pass over nothing at all.
        ASSERT_GT(code.size(), raw.size() / 4)
            << "comment-stripping removed almost everything — the scan would "
               "be looking at nothing";
        for (char const* needle : kForbidden) {
            EXPECT_EQ(code.find(needle), std::string::npos)
                << rel << " compares format IDENTITY through '" << needle
                << "'. The compile pin cannot see this spelling. Ask the "
                   "backend a CAPABILITY question instead — see "
                   "ObjectFormatBackend.";
        }
        ++scanned;
    }
    ASSERT_EQ(scanned, 2u) << "both schema TUs must be scanned";
}

TEST(ObjectFormatBackendRegistry, CapabilityIsNotUniformWithinOneBackend) {
    // ★ THE ARGUMENT FOR THE WHOLE INTERFACE SHAPE, made on shipped config.
    // `pe64-x86_64-windows-exec` and `pe64-x86_64-windows-dll` resolve to the
    // SAME backend and disagree about the stack-reserve capability, because
    // the Windows loader reads SizeOfStackReserve on an .exe and ignores it on
    // a .dll. Any interface offering an `isPe()`-shaped question would answer
    // identically for both and be wrong about one of them. This is why every
    // predicate on `ObjectFormatBackend` asks what a format CAN DO.
    auto const exe = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    auto const dll = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-dll");
    ASSERT_TRUE(exe.has_value()) << rejectSummary(exe);
    ASSERT_TRUE(dll.has_value()) << rejectSummary(dll);

    EXPECT_EQ((*exe)->backend(), (*dll)->backend())
        << "both are PE — same backend";
    EXPECT_TRUE((*exe)->stackReserveControl().has_value());
    EXPECT_FALSE((*dll)->stackReserveControl().has_value())
        << "a .dll must NOT claim the capability — the loader ignores its "
           "SizeOfStackReserve, so writing one would be a knob that lies";

    // Same backend, opposite exec-flavor answers, for the same reason.
    EXPECT_TRUE((*exe)->isExecFlavor());
    EXPECT_FALSE((*dll)->isExecFlavor());
    // …and both are images.
    EXPECT_TRUE((*exe)->isImageFlavor());
    EXPECT_TRUE((*dll)->isImageFlavor());
}

TEST(ObjectFormatBackendRegistry, ExecFlavorStillReDerivesTheWholePieCluster) {
    // ★ THE ANTI-REGRESSION FOR THE ONE THING THAT COULD HAVE COLLAPSED IN THE
    // MOVE. `isExecFlavor()` deliberately re-derives ALL FOUR members of the
    // ELF ET_DYN PIE cluster so a hand-built, validate-bypassing struct cannot
    // fake a PIE by setting one field. Had the axis become a declared
    // `"execFlavor": true` key during TF-C125, the single-field fake would be
    // handed straight back. Prove the derivation, not the declaration.
    auto const pie = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-pie");
    ASSERT_TRUE(pie.has_value()) << rejectSummary(pie);
    EXPECT_TRUE((*pie)->isExecFlavor()) << "a shipped PIE IS an executable";

    // Now build the single-field fake by hand: ET_DYN + processExit only.
    dss::detail::ObjectFormatData fake;
    fake.name               = "synthetic-fake-pie";
    fake.backend            = dss::link::objectFormatBackendByConfigName("elf");
    fake.dataModel          = DataModel::Lp64;
    fake.headerNameMatching = HeaderNameMatching::CaseSensitive;
    fake.cSymbolDecoration.scheme = CSymbolDecorationScheme::None;
    fake.elf.fileClass    = 2;
    fake.elf.dataEncoding = 1;
    fake.elf.machine      = 62;
    fake.elf.objectType   = ElfObjectType::Dyn;
    fake.processExit      = (*pie)->processExit();   // ONE cluster member
    ASSERT_TRUE(fake.processExit.has_value());
    ASSERT_TRUE(fake.elf.interpreter.empty());
    ASSERT_TRUE(fake.entryCallingConvention.empty());
    ASSERT_FALSE(fake.processArgs.has_value());

    ObjectFormatSchema fakeSchema{std::move(fake)};
    EXPECT_FALSE(fakeSchema.isExecFlavor())
        << "ONE cluster member must not be enough to present as a PIE "
           "executable — the four-member re-derivation is the whole point, and "
           "this path bypasses validate() entirely so nothing else defends it";
    EXPECT_TRUE(fakeSchema.isImageFlavor()) << "an ET_DYN is still an image";
    EXPECT_FALSE(fakeSchema.allowsUndefinedImports())
        << "a dyn carrying processExit is treated as an executable here — the "
           "cluster-member spelling stays honest about what it read";
}
