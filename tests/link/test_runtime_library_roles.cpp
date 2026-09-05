// UCRT-P4 (D-FFI-PE-CRT-UCRT-MIGRATION): the `runtimeLibraries` ROLE TABLE's
// red-on-disable levers, and the mechanical EXIT CRITERION of the pe CRT migration.
//
// ★★★ WHAT MAKES THESE LEVERS REAL. The C4 precedent: change CONFIG, recompile NO
// C++, and observe DIFFERENT EMITTED BYTES. Both levers here do that — one changes
// a value and asserts the emitted PE IMPORT TABLE moves with it; the other removes
// a row and asserts the format is REFUSED AT LOAD, naming the file. Anything weaker
// (asserting the config value equals itself, or asserting only an exit code) would
// pass while the engine ignored the table entirely, which is exactly the state
// `cSymbolDecoration` was in before step C4 and the state
// `MirMerge.PeUcrtbaseAndMsvcrtRowsOfOneNameStayTwoImports` is in today.
//
// The mutation technique: load the SHIPPED `.format.json` as TEXT, edit ONE value
// in memory, and load the mutated text. Precedent: `tests/test_support/
// mutate_target_schema.hpp` does exactly this for target schemas, and for the same
// reason — a hand-authored "broken" JSON file would rot against the shipped one and
// couple the test to one particular shape, whereas mutating the shipped text pins
// the SUBSTRATE'S handling of the mutation.
//
// ⚠ NO MUTATION IS EVER NAMED INSIDE THE MUTATED TEXT — not even in a comment.
// A `# MUTANT: …` marker once kept a pin green over a REMOVED guard because the
// comment carried the token the pin searched for. Every mutation is described in
// THIS file's prose and in the failure messages, never in the bytes handed to the
// loader.

#include "core/types/config_path_walk.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "image_dependency_table.hpp"
#include "link/entry_trampoline.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "repo_root.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace dss;

namespace {

// ── THE PER-FLAVOUR COVERAGE TABLE ────────────────────────────────────
//
// ⚠⚠ WHY THIS IS A TABLE WITH A VERDICT PER FLAVOUR AND NOT A LIST PLUS A COUNT.
// The first version of the exit-criterion test below swept a flat list of the four
// pe names and fail-closed with `EXPECT_GE(tablesSeen, 1u)`. That guard blocks
// "EVERY pe format dropped its table" and nothing else: with three of the four
// carrying no table it passed having actually inspected ONE, and reported the
// migration's exit criterion as met. An aggregate count over a population where
// most members are absent is the "guard that asserts nothing" shape this project
// has anchored twice (D-TEST-PE64-CONFOUND-PIN-WEAKENED-BY-ITS-OWN-SUBJECT). The
// fix is not a bigger number — it is a per-member verdict that must be RESTATED
// when the member changes.
//
// Each verdict is the answer for that flavour, and each `why` is the argument for
// it. An excluded flavour is NEVER silently skipped: it is asserted to still be
// excludable, so the day it gains what it was excluded from this test goes red and
// the exclusion has to be re-argued instead of quietly widening.
//
// ⚠⚠ P54 SPLIT ONE BOOLEAN INTO TWO, AND THE SPLIT IS THE POINT RATHER THAN A
// TIDY-UP. Until D-CSUBSET-PACKED-ATOMIC-MEMBER's pe64 arm landed there was only
// `mustDeclare`, meaning "declares a runtimeLibraries table" — and it was serving
// as a proxy for a question it does not actually ask, namely "declares the SEH /
// unwind spine". The two coincided for as long as `unwindPersonality` was the only
// role any pe flavour could name. `atomicsRuntime` ended that: the .lib and .obj
// flavours now declare a table for a reason that has NOTHING to do with unwind, and
// the old single boolean could only have been resolved by either reddening a
// correct config or widening an exclusion that must stay narrow. That is the same
// shape the target key hit one tier over — one boolean fusing two facts whose
// REMEDIES differ — so it is fixed the same way, by asking the two questions
// separately and restating both per member.
struct PeFlavour {
    std::string_view name;
    // Does this flavour declare `sehPersonality` + an `unwindPersonality` row?
    // Governed by whether its writer arm actually EMITS .pdata/.xdata.
    bool             mustDeclareUnwind;
    // Does it declare an `atomicsRuntime` block + row? Governed by whether an
    // under-aligned `_Atomic` access can reach an extern call on this flavour.
    bool             mustDeclareAtomics;
    char const*      why;
    char const*      whyAtomics;
};

// MEASURED 2026-08-10 (build-dbg, `--compile` of a `__try` TU at each target, then
// reading the emitted artifact's section table) — the verdicts below are that
// measurement, not a reading of the writers.
constexpr PeFlavour kPeFlavours[] = {
    {"pe64-x86_64-windows-exec", true, true,
     "the .exe image: encodeExec emits .pdata + .xdata, and the spine's "
     "processExit/processArgs/librarySynthesis name cLibrary + systemPrimitives "
     "on top of the unwindPersonality row",
     "the .exe is the artifact form the execution witness was taken on: a DSS "
     "pe64 image whose packed `_Atomic` member calls __atomic_load/__atomic_store "
     "and RUNS, while its naturally-aligned control keeps the native xchg/mov. "
     "P54 lane `la` moved WHO answers those calls — from mingw-w64's "
     "libatomic-1.dll (not in-box on Windows) to DSS's own compiled body — and "
     "the witness moved with it: the image now imports ucrtbase.dll ALONE"},
    {"pe64-x86_64-windows-dll", true, true,
     "the .dll image routes through the SAME encodeExec substrate as the .exe "
     "(pe.type = dll), so it genuinely carries .pdata + .xdata. MEASURED by "
     "RUNNING: a real Windows process loaded a DSS-built .dll, took a hardware "
     "divide-by-zero inside a __try and resumed in the __except body at both "
     "--config=debug and --config=release. A DLL containing __try is how much of "
     "Windows is written, so a missing block here is a wrong-reject a USER finds",
     "a DLL can contain an under-aligned `_Atomic` access for exactly the reason "
     "it can contain a __try — much of Windows is written as DLLs — and a "
     "format-level omission would silently keep the non-atomic native pair there"},
    {"pe64-x86_64-windows-staticlib", false, true,
     "the .lib archive goes through pe::encode's Obj arm, which attaches no "
     "FrameUnwindInfo and emits NO .pdata/.xdata. MEASURED by declaring the keys "
     "anyway: rc=0 and the emitted seh.lib carried `.text` ONLY, so the filter "
     "funclets and the scope table are SILENTLY DROPPED and __except can never "
     "run. Declaring here would trade a loud, correct refusal for a silent "
     "miscompile; the rationale lives in the file's "
     "$sehPersonalityOmittedComment",
     "⚠ THE ATOMICS ANSWER IS THE OPPOSITE OF THE UNWIND ONE ON THIS SAME FILE, "
     "which is why they cannot share a boolean. There is no silent-drop hazard "
     "for a CALL: `externCallDispatch: direct-plt` is already declared and an "
     "extern call is what this arm emits every day, so the member objects carry "
     "an undefined external the final link resolves. Omitting it would split the "
     "flavours on MEANING — the same source taking the runtime as an .exe and "
     "silently keeping the non-atomic pair as a .lib. elf64-x86_64-linux-staticlib "
     "declares libatomic.so.1 on identical reasoning"},
    {"pe64-x86_64-windows", false, true,
     "the relocatable .obj, same Obj arm and the same MEASURED silent drop "
     "(`.text` only). Worse here than for the archive, because the object exists "
     "to be linked LATER and the final linker cannot notice that unwind data was "
     "never emitted for these functions",
     "same split verdict as the archive, and the contrast is what makes it safe: "
     "the unwind omission exists because a dropped scope table is INVISIBLE to the "
     "later linker, whereas an atomics entry that never resolves is an UNRESOLVED "
     "SYMBOL at that same later link — loud, at the tier that can see it, and "
     "strictly better than shipping a silent loss of atomicity"},
};

// The root key an excluded flavour must carry so the absence reads as a decision
// rather than an oversight, at the site a future editor is already looking.
// ⚠ NOT `$absentKeyRationale`: that key belongs to the CROSS-ARCH mechanism in
// tests/link/test_object_format_family_symmetry.cpp, whose families are keyed on
// `<container><bits>-<os>[-<tier>]` with the ARCH token stripped. x86_64 is the
// only pe arch shipped, so every pe flavour is a ONE-MEMBER family with an empty
// absent-vs-siblings set, and `NoRationaleOutlivesTheAbsenceItExplains` would
// reject any entry placed there as explaining an asymmetry that does not exist.
constexpr std::string_view kOmissionRationaleKey = "$sehPersonalityOmittedComment";

// A flavour carries a `runtimeLibraries` TABLE iff at least one of its two
// verdicts is positive — the table is the union of the roles its blocks name, so
// it is derived from the verdicts rather than being a third independent one.
[[nodiscard]] constexpr bool declaresATable(PeFlavour const& f) {
    return f.mustDeclareUnwind || f.mustDeclareAtomics;
}

[[nodiscard]] std::size_t tableCount() {
    std::size_t n = 0;
    for (auto const& f : kPeFlavours) if (declaresATable(f)) ++n;
    return n;
}

[[nodiscard]] std::string readShippedFormatText(std::string_view name) {
    ShippedConfigLocator loc;
    loc.name            = name;
    loc.subdir          = "object-formats";
    loc.suffix          = ".format.json";
    loc.kindLabel       = "object format";
    loc.invalidNameCode = DiagnosticCode::C_InvalidFormatName;
    auto const p = findShippedConfig(loc);
    if (!p.has_value()) return {};
    std::ifstream in(*p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Every pe `*.format.json` stem ON DISK. Enumerated from the tree, never from a
// hard-coded list, so a FIFTH pe flavour added tomorrow cannot slip past the
// coverage table unexamined — it will not be in `kPeFlavours` and the
// set-equality assertion below names it. This is the enumeration-level twin of
// the per-flavour verdict: a table of four is only meaningful if four is also
// what is shipped.
[[nodiscard]] std::set<std::string> peFormatStemsOnDisk() {
    std::set<std::string> out;
    auto const root = dss::test::findConfigRoot();
    if (!root) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return out;
    }
    auto const dir = *root / "object-formats";
    constexpr std::string_view kSuffix = ".format.json";
    std::error_code ec;
    for (auto const& entry : fs::directory_iterator{dir, ec}) {
        auto const leaf = entry.path().filename().string();
        if (leaf.size() <= kSuffix.size()) continue;
        if (leaf.compare(leaf.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
            continue;
        }
        auto const stem = leaf.substr(0, leaf.size() - kSuffix.size());
        if (stem.rfind("pe64", 0) == 0) out.insert(stem);
    }
    if (ec) ADD_FAILURE() << "cannot enumerate " << dir.string() << ": " << ec.message();
    return out;
}

// The RAW document, for the `$`-prefixed rationale key — `ObjectFormatSchema`
// drops documentation keys on load, so the parsed schema cannot answer whether the
// justification is present.
[[nodiscard]] nlohmann::json formatDoc(std::string_view name) {
    auto const text = readShippedFormatText(name);
    if (text.empty()) return {};
    return nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
}

// Replace the FIRST occurrence of `from` with `to`; returns false if absent, so a
// mutation whose anchor has drifted FAILS the test instead of silently applying
// nothing (a no-op mutation is how a red-on-disable pin goes green forever).
[[nodiscard]] bool substituteOnce(std::string& text, std::string_view from,
                                  std::string_view to) {
    auto const at = text.find(from);
    if (at == std::string::npos) return false;
    text.replace(at, from.size(), to);
    return true;
}

// A one-function module returning 42, linked as a pe64 image. Mirrors the shape
// tests/link/test_lk10_entry_slice_c.cpp uses for its runnable smoke; kept local so
// this file's assertions do not depend on another test's fixture staying put.
[[nodiscard]] AssembledModule makeReturn42Module() {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};   // mov eax, 42 ; ret
    mod.functions.push_back(std::move(fn));
    return mod;
}

// Link `format` around the return-42 module and hand back the emitted image bytes.
// Empty on any failure, with the diagnostics rendered into `why` — an empty result
// must never read as "the assertion below is satisfied".
[[nodiscard]] std::vector<std::uint8_t>
emitImage(ObjectFormatSchema const& format, std::string& why) {
    auto target = TargetSchema::loadShipped("x86_64");
    if (!target.has_value()) { why = "x86_64.target.json did not load"; return {}; }
    auto mod = makeReturn42Module();
    DiagnosticReporter rep;
    auto image = linker::link(mod, **target, format, rep);
    if (rep.errorCount() != 0 || !image.ok() || image.bytes.empty()) {
        std::string text;
        for (auto const& d : rep.all()) { text += d.actual; text += "\n"; }
        why = "link failed: " + text;
        return {};
    }
    return image.bytes;
}

}  // namespace

// ── THE EXIT CRITERION ────────────────────────────────────────────────
//
// "No pe format file's `runtimeLibraries` table has ANY value naming msvcrt.dll",
// across all four pe flavours. STRUCTURED, so it is not a text matcher over the
// file: it reads the parsed table, which means it also catches a future edit that
// repoints ONE role at the wrong image — the thing a single-CRT-string criterion is
// blind to.
//
// ★★ AND IT CANNOT CERTIFY GREEN VACUOUSLY, which is the whole reason it reads the
// way it does. Three independent things must hold, each one of which the previous
// `EXPECT_GE(tablesSeen, 1u)` version would have passed straight through:
//   (1) the pe flavours ON DISK are exactly the ones this file has a verdict for —
//       a fifth flavour cannot be swept while unclassified;
//   (2) every flavour the verdict says MUST declare a table is present BY NAME with
//       a non-empty table that actually declares `unwindPersonality` — so "three of
//       four quietly stopped declaring" is a failure, not a pass;
//   (3) every EXCLUDED flavour is asserted to still be excluded AND to still carry
//       its written justification — so an exclusion can neither be silently
//       widened (a table appearing there reds this) nor silently stripped of the
//       argument that earned it.
// Only then is the msvcrt sweep run, over ALL FOUR.
TEST(RuntimeLibraryRoles, NoPeFormatNamesTheLegacyCrtInItsRoleTable) {
    // (1) ENUMERATION — the verdict table and the shipped tree must agree, both
    // directions. A pe flavour with no verdict is an unreviewed flavour.
    {
        std::set<std::string> onDisk = peFormatStemsOnDisk();
        ASSERT_FALSE(onDisk.empty())
            << "no pe *.format.json found — every assertion below would be vacuous";
        std::set<std::string> classified;
        for (auto const& f : kPeFlavours) classified.emplace(f.name);
        EXPECT_EQ(onDisk, classified)
            << "the pe flavours SHIPPED and the pe flavours this test carries a "
               "verdict for have diverged. A new pe flavour must be classified in "
               "`kPeFlavours` (does it declare a runtimeLibraries table, and why?) "
               "before the migration's exit criterion can claim to cover it — a "
               "sweep over a list that is missing a member reports the criterion "
               "met for a file it never opened.";
    }

    std::size_t tablesSeen = 0;
    for (auto const& flavour : kPeFlavours) {
        auto r = ObjectFormatSchema::loadShipped(flavour.name);
        ASSERT_TRUE(r.has_value()) << flavour.name << " must load";
        auto const& table = (*r)->runtimeLibraries();
        if (!table.empty()) ++tablesSeen;

        // (2a) THE ATOMICS VERDICT, asked separately from the unwind one on every
        // flavour — P54. Both directions, because the negative arm is the one that
        // ratchets: a flavour that gains or loses the block without its verdict
        // moving reds here rather than passing quietly.
        {
            auto const& ar = (*r)->atomicsRuntime();
            EXPECT_EQ(ar.has_value(), flavour.mustDeclareAtomics)
                << flavour.name << ": the `atomicsRuntime` verdict and the shipped "
                   "file disagree. Verdict: " << flavour.whyAtomics;
            if (ar.has_value() && flavour.mustDeclareAtomics) {
                EXPECT_NE(table.rowForRole(RuntimeLibraryRole::AtomicsRuntime),
                          nullptr)
                    << flavour.name
                    << " declares an `atomicsRuntime` block with no matching row; "
                       "the loader resolves the block's role against the table and "
                       "a missing row is fail-loud by design.";
                // ★★ P54 lane `la` (D-C-ATOMICS-RUNTIME-IS-OURS-ON-PE64): THIS
                // ROLE IS REALIZED, NOT IMPORTED, AND BOTH HALVES ARE PINNED.
                // The previous text pinned `libraryPath == "libatomic-1.dll"`,
                // and the measurement behind it stands — ucrtbase, vcruntime140,
                // kernel32, msvcrt and ntdll export ZERO `__atomic_*` symbols
                // between them, MSVC's bundled compiler-rt defines none, and
                // mingw-w64's libatomic-1.dll exports both generic entries. What
                // changed is the RULING: depending on an image that is not
                // in-box on Windows was a workaround, so DSS ships the body.
                // ⚠ THE EMPTY `libraryPath` IS THE LOAD-BEARING HALF. It is not
                // "unset": it is the `noLibraryBinding` shape, and it is what
                // makes the minted extern resolve out of the shipped runtime
                // archive instead of becoming an import. A regression that put
                // an image back here would silently return the external
                // dependency AND leave the archive member unpulled.
                EXPECT_EQ(ar->libraryPath, "") << flavour.name;
                auto const src =
                    table.sourceForRole(RuntimeLibraryRole::AtomicsRuntime);
                ASSERT_TRUE(src.has_value())
                    << flavour.name
                    << ": the atomicsRuntime role must be REALIZED from a shipped "
                       "source on every pe flavour — an `image` here is the "
                       "external-DLL dependency P54 removed.";
                EXPECT_EQ(*src, "runtime/platform/src/atomic.c") << flavour.name;
                // Undecorated: this format's `cSymbolDecoration` is `none`.
                EXPECT_EQ(ar->loadMangledName, "__atomic_load") << flavour.name;
                EXPECT_EQ(ar->storeMangledName, "__atomic_store") << flavour.name;
            }
        }

        if (flavour.mustDeclareUnwind) {
            // (2) PRESENT, BY NAME. Not "some flavour has a table".
            EXPECT_FALSE(table.empty())
                << flavour.name
                << " MUST declare a `runtimeLibraries` table and does not. "
                   "Verdict: " << flavour.why
                << ". Without the table this format's exit-criterion row is "
                   "unevaluable, and the sweep below would silently skip it.";
            EXPECT_TRUE(table.imageForRole(RuntimeLibraryRole::UnwindPersonality)
                            .has_value())
                << flavour.name
                << " declares a table with no `unwindPersonality` row, so a `__try` "
                   "in a translation unit built for it is REFUSED at "
                   "synthesizeSehFunclets. That is the wrong-reject this coverage "
                   "requirement exists to close. Verdict: " << flavour.why;
            EXPECT_TRUE((*r)->sehPersonality().has_value())
                << flavour.name
                << " must declare `sehPersonality`; the table row alone names an "
                   "image with nothing pointing at it.";
        } else {
            // (3) EXCLUDED — asserted, justified, and RATCHETED. This is the arm
            // that must never be a silent skip: it fails the moment the flavour
            // gains what it was excluded from having.
            // ⚠⚠ P54 NARROWED THIS FROM `table.empty()` TO THE UNWIND ROLE, AND
            // THE NARROWING IS NOT A WEAKENING — it is the same correction the
            // struct's two booleans are. `table.empty()` asserted the ABSENCE OF
            // ANY TABLE as a stand-in for the absence of the UNWIND SPINE, which
            // held only while unwind was the sole role a pe flavour could name.
            // The .lib and .obj now name `atomicsRuntime` for a reason that has
            // nothing to do with .pdata/.xdata, so the old form would have
            // reddened a correct config and the only ways out would have been to
            // widen an exclusion that must stay narrow or to delete the ratchet.
            // Asserting the ROLE keeps every bit of the original guard: the day
            // this flavour gains an unwindPersonality row, this still reds.
            EXPECT_FALSE(table.imageForRole(RuntimeLibraryRole::UnwindPersonality)
                             .has_value())
                << flavour.name
                << " is on the EXPLICIT exclusion list but now names an "
                   "`unwindPersonality` role. This test is deliberately red so the "
                   "exclusion gets re-argued rather than widened by accident. The "
                   "recorded reason it was excluded: " << flavour.why
                << ". If the underlying limitation is genuinely fixed (the pe Obj "
                   "arm now emits .pdata/.xdata), move this flavour to "
                   "mustDeclareUnwind = true and re-measure the emitted sections — "
                   "do not delete this assertion.";
            EXPECT_FALSE((*r)->sehPersonality().has_value())
                << flavour.name
                << " is excluded but declares `sehPersonality`. MEASURED: the Obj "
                   "arm accepts the declaration, returns rc=0, and emits `.text` "
                   "only — the SEH funclets and scope table are dropped without a "
                   "diagnostic, so __except can never run. " << flavour.why;
            auto const doc = formatDoc(flavour.name);
            ASSERT_TRUE(doc.is_object())
                << flavour.name << ".format.json must parse as a JSON object";
            auto const key = std::string{kOmissionRationaleKey};
            ASSERT_TRUE(doc.contains(key))
                << flavour.name << " is excluded from the runtimeLibraries "
                << "coverage requirement but carries no `" << kOmissionRationaleKey
                << "`. An exclusion with no written reason at the site is "
                   "indistinguishable from an oversight, and the next editor "
                   "reading the `synthesizeSehFunclets` diagnostic will follow its "
                   "'Add the block' advice straight into the silent drop.";
            EXPECT_TRUE(doc.at(key).is_string() && !doc.at(key).get<std::string>().empty())
                << flavour.name << "'s `" << kOmissionRationaleKey
                << "` must be a non-empty string.";
        }

        // The criterion itself, over ALL FOUR flavours — the excluded ones have
        // empty tables, so this loop is trivially satisfied for them, and that is
        // exactly why it could not be the only assertion in this test.
        for (auto const& b : table.bindings) {
            EXPECT_NE(b.image, "msvcrt.dll")
                << flavour.name << " points role '"
                << runtimeLibraryRoleName(b.role)
                << "' at the LEGACY CRT. D-FFI-PE-CRT-UCRT-MIGRATION's exit "
                   "criterion is stated against this table precisely so ONE "
                   "mis-pointed role is visible.";
        }
    }

    // FAIL-CLOSED on the aggregate too — but as an EXACT equality against the
    // verdicts, never a floor. `>= 1` is what let three absent tables read as
    // success; `== mustDeclareCount()` cannot, because it moves whenever a verdict
    // moves and disagrees the instant a table appears or disappears anywhere.
    EXPECT_EQ(tablesSeen, tableCount())
        << "the number of pe formats actually declaring a `runtimeLibraries` table "
           "must equal the number this file's verdicts say should — a count that "
           "merely has a FLOOR is satisfied by one file out of four, which is the "
           "state this assertion replaced. Since P54 the expected count is DERIVED "
           "from the two per-flavour verdicts (`declaresATable`) rather than being "
           "a third independent number that could drift from both.";
}

// Every role a spine block NAMES must resolve, on every shipped format, and the
// resolved copy each block carries must EQUAL the table's value. The whole
// population, not a sample: this is the multi-site class where a green subset hides
// the miss.
TEST(RuntimeLibraryRoles, EveryShippedFormatsRolesResolveConsistently) {
    // The same 24-name population `FormatRootKeyVocabulary` sweeps.
    constexpr std::string_view kAll[] = {
        "elf64-aarch64-linux-dyn",      "elf64-aarch64-linux-exec",
        "elf64-aarch64-linux-pie",      "elf64-aarch64-linux-staticlib",
        "elf64-aarch64-linux",          "elf64-x86_64-linux-dyn",
        "elf64-x86_64-linux-exec",      "elf64-x86_64-linux-pie",
        "elf64-x86_64-linux-staticlib", "elf64-x86_64-linux",
        "macho64-arm64-darwin-dylib",   "macho64-arm64-darwin-exec",
        "macho64-arm64-darwin-staticlib", "macho64-arm64-darwin",
        "macho64-x86_64-darwin-dylib",  "macho64-x86_64-darwin-exec",
        "macho64-x86_64-darwin-staticlib", "macho64-x86_64-darwin",
        "pe64-x86_64-windows-dll",      "pe64-x86_64-windows-exec",
        "pe64-x86_64-windows-staticlib", "pe64-x86_64-windows",
        "spirv-1.6",                    "wasm32-v1"};
    static_assert(std::size(kAll) == 24,
                  "all 24 shipped .format.json files must be listed");
    std::size_t rolesChecked = 0;
    for (auto const name : kAll) {
        auto r = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(r.has_value()) << name << " must load clean";
        auto const& table = (*r)->runtimeLibraries();
        auto check = [&](RuntimeLibraryRole role, std::string const& resolved,
                         char const* where) {
            if (role == RuntimeLibraryRole::None) return;
            ++rolesChecked;
            auto const image = table.imageForRole(role);
            ASSERT_TRUE(image.has_value())
                << name << where << " names role '"
                << runtimeLibraryRoleName(role) << "' with no table row";
            EXPECT_EQ(std::string{*image}, resolved)
                << name << where << ": the block's resolved image must EQUAL the "
                   "role table's — two copies of one fact that disagree is the "
                   "defect the table removes";
        };
        if (auto const& pe = (*r)->processExit(); pe.has_value()) {
            check(pe->role, pe->importLibraryPath, " /processExit");
        }
        if (auto const& pa = (*r)->processArgs(); pa.has_value()) {
            check(pa->role, pa->crtLibraryPath, " /processArgs");
        }
        if (auto const& sp = (*r)->sehPersonality(); sp.has_value()) {
            check(sp->role, sp->libraryPath, " /sehPersonality");
        }
        if (auto const& ls = (*r)->librarySynthesis(); ls.has_value()) {
            check(ls->role, ls->libraryPath, " /librarySynthesis");
        }
    }
    // ★ THE FLOOR IS THE MEASURED LIVE COUNT, NOT A TOKEN VALUE, and the
    // difference matters: this stood at 8 against a live 16, so HALF the role
    // claims in the tree could have vanished with the sweep still reporting
    // green — a vacuity guard that tolerates 50% loss is barely a guard.
    // ✔MEASURED 2026-08-10 across the 24 shipped formats: **12** role claims —
    // the four elf exec/pie contribute processExit ONLY (4), macho64-arm64
    // -darwin-exec processExit+librarySynthesis (2), macho64-x86_64-darwin-exec
    // processExit alone (1), pe dll sehPersonality (1), pe exec all four (4).
    // ★ IT IS 12 AND NOT 16, AND THE DIFFERENCE IS THE WHOLE POINT OF THIS
    // COUNTER: what is counted is a role CLAIM (`role != None`), never a BLOCK
    // that happens to be present. The four elf `processArgs` blocks DO exist and
    // deliberately name NO role, because elf obtains argv from the process
    // STACK VECTOR — no library image is involved, so `None` there is a real
    // ANSWER rather than an omission. A floor of 16 was briefly pinned here by
    // counting present blocks off a capability matrix; it reddened this test
    // against a correct tree. Derive this number by running the sweep, never by
    // counting keys in the format files.
    // ★★ PINNING IT AT THE LIVE VALUE IS SAFE *AND* STRICTER, because this
    // population only ever GROWS: a format gaining a role-naming block raises
    // the count, so an addition can never false-red this. The realization arc
    // will raise it (macho64-x86_64-darwin-exec gains librarySynthesis ⇒ 17).
    // ⇒ RAISE this when a vehicle lands; NEVER lower it to green a break. A
    // removed role claim is precisely the regression this exists to catch.
    EXPECT_GE(rolesChecked, 12u)
        << "only " << rolesChecked << " role claims were checked, against 12 "
           "measured live — a block stopped naming a role, so the two-copies-"
           "of-one-fact agreement it used to force is no longer being checked "
           "anywhere. Find the block that lost its role; do not lower this.";
}

// ── LEVER (a): DELETE a role row ⇒ the format is REFUSED AT LOAD ──────
TEST(RuntimeLibraryRoles, DeletingARoleRowRefusesTheFormatAtLoad) {
    std::string text = readShippedFormatText("pe64-x86_64-windows-exec");
    ASSERT_FALSE(text.empty()) << "the shipped pe exec format must be readable";

    // WITNESS: the unmutated text loads clean. Without this the refusal below could
    // be caused by the READ, the parse, or anything else.
    {
        auto ok = ObjectFormatSchema::loadFromText(text);
        ASSERT_TRUE(ok.has_value())
            << "the UNMUTATED shipped text must load clean, or the mutant proves "
               "nothing";
    }

    // The witness token, and it must be UNIQUE in the subject: if the row appeared
    // twice, removing one occurrence would leave the fact intact and the pin would
    // be green over a mutation that changed nothing.
    constexpr std::string_view kRow =
        R"({ "role": "unwindPersonality", "image": "ucrtbase.dll" },)";
    std::size_t occurrences = 0;
    for (std::size_t at = text.find(kRow); at != std::string::npos;
         at = text.find(kRow, at + 1)) {
        ++occurrences;
    }
    ASSERT_EQ(occurrences, 1u)
        << "the row under test must appear EXACTLY once in the subject, or removing "
           "one occurrence leaves the fact in place";

    std::string mutant = text;
    ASSERT_TRUE(substituteOnce(mutant, kRow, ""))
        << "the mutation anchor must still exist — a no-op mutation is how a "
           "red-on-disable pin goes green forever";
    // Fail-closed: the mutant DIFFERS from the subject, and the witness is ABSENT
    // from it BY THE SAME MATCHER the assertion above used.
    ASSERT_NE(mutant, text) << "the mutant must differ byte-wise from the subject";
    ASSERT_EQ(mutant.find(kRow), std::string::npos)
        << "the witness must be ABSENT from the mutant, by the same matcher";

    auto bad = ObjectFormatSchema::loadFromText(mutant, "<mutant>");
    ASSERT_FALSE(bad.has_value())
        << "removing a role row that `sehPersonality` NAMES must REFUSE the format "
           "at LOAD — otherwise the personality would resolve to nothing and the "
           "emitted unwind info would name a handler from no image";
    bool namedTheBlock = false;
    for (auto const& d : bad.error()) {
        if (d.path.find("sehPersonality") != std::string::npos) namedTheBlock = true;
    }
    EXPECT_TRUE(namedTheBlock)
        << "the refusal must point at the BLOCK that named the missing role, so the "
           "reader knows which declaration to fix";
}

// ── LEVER (a), THE NEWLY-COVERED FILE: the .dll arm ───────────────────
//
// The exec arm above is not a substitute for this one. UCRT-P4's operator
// requirement was that the table reach every pe flavour that can realize SEH, and
// the .dll is the flavour that requirement ADDED — so the .dll's own table needs
// its own red-on-disable, on its own file, or "the exec file's row is load-bearing"
// is the only thing anyone ever measured.
//
// This file's table has exactly ONE row, which makes the lever unusually sharp:
// deleting it empties the table entirely, and `sehPersonality` — the only
// role-naming block on this format — then resolves against nothing.
// ⚠ RENAMED BY P54 FROM `DeletingTheDllsOnlyRoleRowRefusesTheFormatAtLoad`, AND
// THE RENAME IS THE HONEST HALF OF THE CHANGE. The subject is unchanged — remove
// the row that `sehPersonality` NAMES and the format must be refused at load,
// naming that block. What expired is the word ONLY: D-CSUBSET-PACKED-ATOMIC-MEMBER
// added an `atomicsRuntime` row to this format, so `unwindPersonality` is no longer
// the sole row and "empty the table" is no longer the same mutation as "delete this
// row". ★ THE TEST CAUGHT ITS OWN STALENESS RATHER THAN PASSING THROUGH IT: the
// old matcher removed the row without its trailing comma, FAIL-CLOSED CHECK 4
// parsed the result, found malformed JSON and refused to score the arm — which is
// exactly what that check exists for, since a syntax error would otherwise have
// produced a refusal "wearing the role table's clothes" and read as green.
TEST(RuntimeLibraryRoles, DeletingTheRowSehPersonalityNamesRefusesTheDllFormatAtLoad) {
    std::string const text = readShippedFormatText("pe64-x86_64-windows-dll");
    ASSERT_FALSE(text.empty()) << "the shipped pe dll format must be readable";

    // WITNESS: unmutated, it loads clean. Without this the refusal below could come
    // from the read, the parse, or anything but the mutation.
    {
        auto ok = ObjectFormatSchema::loadFromText(text);
        ASSERT_TRUE(ok.has_value())
            << "the UNMUTATED shipped text must load clean, or the mutant proves "
               "nothing";
    }

    // FAIL-CLOSED CHECK 1 — the witness is UNIQUE in the subject. If the row
    // appeared twice, removing one occurrence would leave the fact in place and this
    // pin would be green over a mutation that changed nothing.
    // ⚠ THE TRAILING COMMA IS PART OF THE WITNESS, DELIBERATELY. The table now
    // carries a second row (`atomicsRuntime`), so removing this row's text alone
    // would leave a dangling comma and hand the loader malformed JSON — a refusal
    // for a reason that has nothing to do with the role table. Matching the comma
    // keeps the mutant a well-formed one-element array, which is what makes the
    // refusal below attributable to the missing ROLE.
    constexpr std::string_view kRow =
        R"({ "role": "unwindPersonality", "image": "ucrtbase.dll" },)";
    std::size_t occurrences = 0;
    for (std::size_t at = text.find(kRow); at != std::string::npos;
         at = text.find(kRow, at + 1)) {
        ++occurrences;
    }
    ASSERT_EQ(occurrences, 1u)
        << "the row under test must appear EXACTLY once in this file";

    std::string mutant = text;
    ASSERT_TRUE(substituteOnce(mutant, kRow, ""))
        << "the mutation anchor must still exist — a no-op mutation is how a "
           "red-on-disable pin goes green forever";

    // FAIL-CLOSED CHECK 2 — the mutant DIFFERS from the subject, asserted on a HASH
    // of the whole text. Never on a line count: the mutation removes no line (the
    // row's line survives as whitespace inside the now-empty array), so a
    // line-count comparison would report "unchanged" and pass this test while
    // proving nothing.
    ASSERT_NE(std::hash<std::string>{}(mutant), std::hash<std::string>{}(text))
        << "the mutant must differ from the subject";

    // FAIL-CLOSED CHECK 3 — the witness is ABSENT from the mutant, BY THE SAME
    // MATCHER that asserted its uniqueness above.
    ASSERT_EQ(mutant.find(kRow), std::string::npos)
        << "the witness must be ABSENT from the mutant, by the same matcher";

    // FAIL-CLOSED CHECK 4 — the mutant is still WELL-FORMED JSON. This is what
    // separates "the schema rule rejected a coherent document" from "we handed the
    // loader garbage": a mutation that merely broke the syntax would refuse the
    // format for a reason that has nothing to do with the role table.
    {
        auto const doc = nlohmann::json::parse(mutant, nullptr,
                                              /*allow_exceptions=*/false);
        ASSERT_FALSE(doc.is_discarded())
            << "the mutant must remain well-formed JSON, or the refusal below is a "
               "parse error wearing the role table's clothes";
        // P54: the state under test is "the table no longer names the role
        // `sehPersonality` asks for", NOT "the table is empty". Those coincided
        // while unwindPersonality was the only row; asserting emptiness now would
        // pin an accident of how many roles this format happens to declare.
        ASSERT_TRUE(doc.contains("runtimeLibraries")
                    && doc.at("runtimeLibraries").is_array())
            << "the mutation must leave a well-formed runtimeLibraries array";
        for (auto const& row : doc.at("runtimeLibraries")) {
            ASSERT_TRUE(row.contains("role") && row.at("role").is_string());
            EXPECT_NE(row.at("role").get<std::string>(), "unwindPersonality")
                << "the mutant must NOT still name the role under test — if it "
                   "does, the refusal below cannot be attributed to its absence";
        }
        ASSERT_FALSE(doc.at("runtimeLibraries").empty())
            << "and the surviving `atomicsRuntime` row must still be there, or "
               "this became the emptied-table mutation instead of the one named";
    }

    auto bad = ObjectFormatSchema::loadFromText(mutant, "<mutant>");
    ASSERT_FALSE(bad.has_value())
        << "emptying the table that `sehPersonality` NAMES must REFUSE the .dll "
           "format at LOAD. If it loaded, the personality would resolve to no "
           "image and the emitted .xdata would name a handler imported from "
           "nowhere.";
    bool namedTheBlock = false;
    for (auto const& d : bad.error()) {
        if (d.path.find("sehPersonality") != std::string::npos) namedTheBlock = true;
    }
    EXPECT_TRUE(namedTheBlock)
        << "the refusal must point at the BLOCK that named the missing role, so the "
           "reader knows which declaration to fix";
}

// ── LEVER (b): REPOINT a role ⇒ the EMITTED IMPORT TABLE changes ──────
//
// The strongest form of the C4 lever: no C++ recompiles between the two halves,
// only a config VALUE differs, and the observable is the bytes of a linked PE image.
TEST(RuntimeLibraryRoles, RepointingCLibraryChangesTheEmittedImportTable) {
    std::string const text = readShippedFormatText("pe64-x86_64-windows-exec");
    ASSERT_FALSE(text.empty());

    // WITNESS half — the shipped table, emitted.
    std::string whyA;
    auto witnessFmt = ObjectFormatSchema::loadFromText(text);
    ASSERT_TRUE(witnessFmt.has_value());
    auto const witnessBytes = emitImage(**witnessFmt, whyA);
    ASSERT_FALSE(witnessBytes.empty()) << whyA;
    auto const witnessLibs = dss::test_support::peImportedLibraries(witnessBytes);
    ASSERT_FALSE(witnessLibs.empty())
        << "the extractor must find an import table in the witness, or every "
           "assertion below is vacuous";
    EXPECT_GE(dss::test_support::dependencyOccurrences(witnessLibs,
                                                      "ucrtbase.dll"), 1u)
        << "witness libraries: "
        << dss::test_support::joinDependencies(witnessLibs);
    // The mutant's image must be ABSENT from the witness — the other half of
    // fail-closed, by the SAME matcher the mutant assertion uses.
    EXPECT_EQ(dss::test_support::dependencyOccurrences(witnessLibs,
                                                      "vcruntime140.dll"), 0u)
        << "witness libraries: "
        << dss::test_support::joinDependencies(witnessLibs);

    // MUTANT half — ONE value repointed. `vcruntime140.dll` is chosen because it is
    // a REAL image that MEASURED-exports the routine the spine asks for
    // (`__C_specific_handler` at ord 8) and Microsoft genuinely names it separately
    // from the C library in BOTH /MD and /MT configurations. So the mutant is a
    // COHERENT alternative configuration, not a nonsense value the loader might
    // reject for an unrelated reason.
    std::string mutant = text;
    ASSERT_TRUE(substituteOnce(
        mutant, R"({ "role": "cLibrary",          "image": "ucrtbase.dll" },)",
                R"({ "role": "cLibrary",          "image": "vcruntime140.dll" },)"))
        << "the mutation anchor must still exist";
    ASSERT_NE(mutant, text) << "the mutant must differ byte-wise from the subject";

    auto mutantFmt = ObjectFormatSchema::loadFromText(mutant, "<mutant>");
    ASSERT_TRUE(mutantFmt.has_value())
        << "the mutant must still PARSE — a mutation that merely breaks the file "
           "proves nothing about what the engine reads";
    std::string whyB;
    auto const mutantBytes = emitImage(**mutantFmt, whyB);
    ASSERT_FALSE(mutantBytes.empty()) << whyB;

    // THE OBSERVABLE: different emitted bytes, and specifically a different import
    // table. Asserted on NAMES, never on a byte count or a hash alone — though the
    // whole-image difference is asserted too, because a table that changed while the
    // image did not would mean the extractor is reading something the loader will
    // not.
    EXPECT_NE(witnessBytes, mutantBytes)
        << "changing one role's image must change the EMITTED IMAGE";
    auto const mutantLibs = dss::test_support::peImportedLibraries(mutantBytes);
    std::string const mutantText = dss::test_support::joinDependencies(mutantLibs);
    EXPECT_GE(dss::test_support::dependencyOccurrences(mutantLibs,
                                                      "vcruntime140.dll"), 1u)
        << "the repointed image must appear in the mutant's import table; got: "
        << mutantText;
    EXPECT_EQ(dss::test_support::dependencyOccurrences(mutantLibs,
                                                      "ucrtbase.dll"), 0u)
        << "and the ORIGINAL image must be gone — this format points only cLibrary "
           "and unwindPersonality at it, and the trampoline's exit import rides "
           "cLibrary; got: " << mutantText;
}

// ── D-CSUBSET-PACKED-ATOMIC-MEMBER: the FIFTH role, both directions ────────
//
// `atomicsRuntime` joins `cLibrary` / `unwindPersonality` / `systemPrimitives`
// as a runtime-library ROLE, and `atomicsRuntime` joins `processExit` /
// `processArgs` / `sehPersonality` / `librarySynthesis` as a role-NAMING spine
// block. The biconditional the table rests on therefore has two new halves, and
// both are pinned here because either one alone would let the pair rot: a row
// with no block is inert config (the stale-image shape), and a block with no row
// resolves its import to NO IMAGE.
//
// ⚠ WHY THIS MATTERS MORE FOR THIS ROLE THAN FOR THE OTHERS: the consumer
// MINTS an extern import bound to whatever this table says. A wrong or absent
// image is an UNRESOLVED SYMBOL AT LINK for every program that touches an
// under-aligned `_Atomic` — which is precisely the outcome two prior P53 lanes
// refused to risk by shipping the config ahead of its consumer.

TEST(RuntimeLibraryRoles, DeletingTheAtomicsRuntimeRowRefusesTheFormatAtLoad) {
    std::string text = readShippedFormatText("elf64-aarch64-linux-exec");
    ASSERT_FALSE(text.empty()) << "the shipped elf-aarch64 exec format must be readable";
    {
        auto ok = ObjectFormatSchema::loadFromText(text);
        ASSERT_TRUE(ok.has_value())
            << "the UNMUTATED shipped text must load clean, or the mutant proves "
               "nothing";
    }
    constexpr std::string_view kRow =
        R"({ "role": "atomicsRuntime", "image": "libatomic.so.1" })";
    std::size_t occurrences = 0;
    for (std::size_t at = text.find(kRow); at != std::string::npos;
         at = text.find(kRow, at + 1)) {
        ++occurrences;
    }
    ASSERT_EQ(occurrences, 1u)
        << "the row under test must appear EXACTLY once in the subject, or "
           "removing one occurrence leaves the fact in place";

    std::string mutant = text;
    ASSERT_TRUE(substituteOnce(mutant, kRow, R"({ "role": "cLibrary", "image": "libc.so.6" })"))
        << "the mutation anchor must still exist — a no-op mutation is how a "
           "red-on-disable pin goes green forever";
    ASSERT_NE(mutant, text) << "the mutant must differ byte-wise from the subject";
    ASSERT_EQ(mutant.find(kRow), std::string::npos)
        << "the witness must be ABSENT from the mutant, by the same matcher";

    auto bad = ObjectFormatSchema::loadFromText(mutant, "<mutant>");
    ASSERT_FALSE(bad.has_value())
        << "removing the row that `atomicsRuntime` NAMES must REFUSE the format "
           "at LOAD — otherwise the block's minted `__atomic_load` import would "
           "be bound to no image, i.e. an unresolved symbol at link for every "
           "program with an under-aligned _Atomic access";
    bool namedTheBlock = false;
    for (auto const& d : bad.error()) {
        if (d.path.find("atomicsRuntime") != std::string::npos) namedTheBlock = true;
    }
    EXPECT_TRUE(namedTheBlock)
        << "the refusal must NAME the block whose role stopped resolving, so the "
           "reader knows which `.format.json` key to add";
}

TEST(RuntimeLibraryRoles, DeletingTheAtomicsRuntimeBlockLeavesTheRowInertAndRefuses) {
    // The OTHER direction. Delete the BLOCK and the row it named becomes inert
    // config — nothing reads it, so nothing contradicts it, which is exactly how
    // a stale image survives a migration. The loader refuses that too.
    std::string text = readShippedFormatText("elf64-aarch64-linux-exec");
    ASSERT_FALSE(text.empty());
    {
        auto ok = ObjectFormatSchema::loadFromText(text);
        ASSERT_TRUE(ok.has_value());
    }
    std::size_t const blockAt = text.find(R"("atomicsRuntime": {)");
    ASSERT_NE(blockAt, std::string::npos)
        << "the shipped format must declare an `atomicsRuntime` BLOCK";
    std::size_t const closeAt = text.find("\n  },\n", blockAt);
    ASSERT_NE(closeAt, std::string::npos);
    std::string mutant = text;
    mutant.erase(blockAt, (closeAt + 6) - blockAt);
    ASSERT_NE(mutant, text);
    ASSERT_EQ(mutant.find(R"("atomicsRuntime": {)"), std::string::npos)
        << "the block must be ABSENT from the mutant, by the same matcher";
    ASSERT_NE(mutant.find(R"({ "role": "atomicsRuntime", "image": "libatomic.so.1" })"),
              std::string::npos)
        << "the ROW must SURVIVE — this mutant is about the row going unnamed, "
           "and removing both would test nothing";

    auto bad = ObjectFormatSchema::loadFromText(mutant, "<mutant>");
    ASSERT_FALSE(bad.has_value())
        << "a runtimeLibraries row that NO block names is inert config and must "
           "be REFUSED at load";
    bool namedTheTable = false;
    for (auto const& d : bad.error()) {
        if (d.path.find("runtimeLibraries") != std::string::npos) namedTheTable = true;
    }
    EXPECT_TRUE(namedTheTable)
        << "the refusal must name the TABLE, so the reader knows which row lost "
           "its consumer";
}

TEST(RuntimeLibraryRoles, EveryFormatThatDeclaresAnAtomicsBlockDeclaresBothEntryNames) {
    // A block missing ONE entry name would lower half of `_Atomic` correctly and
    // leave the other direction on the faulting native instruction — the
    // partial-fix-reads-as-a-complete-one shape, at load time. The loader
    // refuses it; this walks every SHIPPED format to prove none of them ships
    // the half-declared form, and that the names carry the format's own
    // C-symbol decoration rather than one spelling for all of them.
    constexpr std::string_view kElf[] = {
        "elf64-aarch64-linux-exec", "elf64-aarch64-linux-pie",
        "elf64-aarch64-linux-dyn",  "elf64-aarch64-linux-staticlib",
        "elf64-x86_64-linux-exec",  "elf64-x86_64-linux-pie",
        "elf64-x86_64-linux-dyn",   "elf64-x86_64-linux-staticlib",
    };
    constexpr std::string_view kMacho[] = {
        "macho64-arm64-darwin-exec",  "macho64-arm64-darwin-dylib",
        "macho64-x86_64-darwin-exec", "macho64-x86_64-darwin-dylib",
    };
    for (auto const name : kElf) {
        auto loaded = ObjectFormatSchema::loadShipped(std::string{name});
        ASSERT_TRUE(loaded.has_value()) << name;
        auto const& ar = (*loaded)->atomicsRuntime();
        ASSERT_TRUE(ar.has_value())
            << name << " must declare an `atomicsRuntime` block — without one an "
                       "under-aligned _Atomic access has no runtime to call";
        EXPECT_EQ(ar->loadMangledName, "__atomic_load") << name;
        EXPECT_EQ(ar->storeMangledName, "__atomic_store") << name;
        EXPECT_EQ(ar->libraryPath, "libatomic.so.1")
            << name << ": MEASURED — libc and libgcc_s export ZERO __atomic_* "
                       "symbols on both Linux legs; this is a distinct image";
    }
    for (auto const name : kMacho) {
        auto loaded = ObjectFormatSchema::loadShipped(std::string{name});
        ASSERT_TRUE(loaded.has_value()) << name;
        auto const& ar = (*loaded)->atomicsRuntime();
        ASSERT_TRUE(ar.has_value()) << name;
        // ★ THE DECORATION IS THE FORMAT'S OWN AND THAT IS WHY THE NAME IS
        // DECLARED PER FORMAT: the C symbol `__atomic_load` is carried by a
        // Mach-O image as `___atomic_load`. A lowerer re-deriving this rule
        // would be the hardcoded-`msvcrt.dll` defect one tier over.
        EXPECT_EQ(ar->loadMangledName, "___atomic_load") << name;
        EXPECT_EQ(ar->storeMangledName, "___atomic_store") << name;
        EXPECT_EQ(ar->libraryPath, "/usr/lib/libSystem.B.dylib")
            << name << ": MEASURED — Apple clang links the packed case with "
                       "libSystem ALONE; the role points at the image cLibrary "
                       "already names, which the table permits";
    }
    // ⚠⚠ THE pe64 ARM, AND IT IS A P54 REVERSAL OF WHAT STOOD HERE. This loop
    // used to assert pe64 declared NO atomics runtime, on the reasoning that
    // "UCRT exports no `__atomic_*` symbol and there is no Windows atomics image
    // to name". The FIRST clause is still true and is re-measured below; the
    // SECOND was never measured, only inferred from it — a true answer to the
    // narrower question (does MICROSOFT ship one) read as an answer to the wider
    // one (does ANY image on this platform), and it failed toward the quiet
    // outcome ([[feedback-an-instrument-that-answers-an-adjacent-question]]).
    //
    // ✔MEASURED 2026-09-02 (P54, `objdump -p`, every probe asserting rc=0 AND a
    // non-trivial line count so a path-translation failure could not read as a
    // zero): mingw-w64's `libatomic-1.dll` — shipped beside the gcc that IS this
    // platform's C reference — exports `__atomic_load` (ord 55) and
    // `__atomic_store` (ord 71). What is absent is a MICROSOFT image: ucrtbase
    // (46125 export lines), vcruntime140 (3840), kernel32 (26109), msvcrt (18383)
    // and ntdll (56304) export ZERO between them, and MSVC's own bundled
    // compiler-rt (`clang_rt.builtins-x86_64.lib`, 2902 nm lines) defines none.
    //
    // ★ WHY THE LIBCALL IS REQUIRED HERE AND NOT MERELY AVAILABLE — and it is not
    // a tiebreak between references. `DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO C` is a
    // union over what WORKS, not over what is merely ACCEPTED, and no vertex is
    // privileged. A reference that accepts a construct and then emits code which
    // faults, tears, or silently drops its meaning is not a working reference for
    // it. gcc is that reference here: its `xchgl` store carries x86's implicit
    // LOCK and IS atomic misaligned, but the paired plain `movl` LOAD is not
    // architecturally atomic across a cache line. clang works. So the union
    // requires clang's behaviour, with no adjudication involved.
    for (auto const name : {"pe64-x86_64-windows-exec", "pe64-x86_64-windows-dll",
                            "pe64-x86_64-windows-staticlib", "pe64-x86_64-windows"}) {
        auto loaded = ObjectFormatSchema::loadShipped(std::string{name});
        ASSERT_TRUE(loaded.has_value()) << name;
        auto const& ar = (*loaded)->atomicsRuntime();
        ASSERT_TRUE(ar.has_value())
            << name << " must declare an `atomicsRuntime` block — without one an "
                       "under-aligned _Atomic access silently keeps a load that is "
                       "not atomic across a cache line";
        // Undecorated, because this format's `cSymbolDecoration` is `none` — the
        // same key that makes the Mach-O siblings' names carry a leading
        // underscore. Three spellings, one per format, all declared not derived.
        EXPECT_EQ(ar->loadMangledName, "__atomic_load") << name;
        EXPECT_EQ(ar->storeMangledName, "__atomic_store") << name;
        // ★★ P54 lane `la` (D-C-ATOMICS-RUNTIME-IS-OURS-ON-PE64) — THE PIN
        // FLIPPED FROM AN IMAGE TO A REALIZATION, AND THE OLD MEASUREMENT IS NOT
        // REFUTED, ONLY SUPERSEDED. It remains true that no in-box Windows DLL
        // and no MSVC-bundled compiler-rt exports the generic entries and that
        // mingw-w64's `libatomic-1.dll` does; depending on that non-in-box image
        // was ruled a workaround, so DSS ships the body instead. The EMPTY
        // library path is the `noLibraryBinding` shape and is what makes the
        // minted extern resolve out of the shipped runtime archive rather than
        // become an import — putting an image back here would silently restore
        // the external dependency AND leave the archive member unpulled.
        EXPECT_EQ(ar->libraryPath, "")
            << name << ": the atomicsRuntime role is REALIZED here, so its "
                       "resolved image must be EMPTY.";
        auto const src = (*loaded)->runtimeLibraries().sourceForRole(
            RuntimeLibraryRole::AtomicsRuntime);
        ASSERT_TRUE(src.has_value()) << name;
        EXPECT_EQ(*src, "runtime/platform/src/atomic.c") << name;
    }
}

// ── D-C-ATOMICS-RUNTIME-IS-OURS-ON-PE64: the REALIZED-ROLE mechanism ───────
//
// ★★ THE LOADER AND THE RAW KEY MUST AGREE, ACROSS EVERY SHIPPED FORMAT. The
// shipped-source SWEEP in `tests/ffi/test_shipped_source_realization.cpp` reads
// `runtimeLibraries[].source` as raw JSON, because `ffi` cannot depend on the
// object-format schema without inverting the layering. That makes it a SECOND
// READER of a config key, which is how a sweep and a loader come to disagree
// about what a document says — so the two are pinned against each other here,
// where both are reachable. A format whose loader dropped, renamed or rewrote
// the key would keep the sweep green and red HERE.
TEST(RuntimeLibraryRoles, EveryRealizedRoleSourceMatchesTheRawKey) {
    auto const root = dss::test::findConfigRoot();
    ASSERT_TRUE(root.has_value()) << dss::test::configRootDiagnostic();
    auto const dir = *root / "object-formats";
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::is_directory(dir, ec)) << dir.generic_string();

    std::size_t compared = 0;
    for (std::filesystem::directory_iterator it{dir, ec}, end; it != end;
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string const stem = it->path().filename().generic_string();
        auto const        dot  = stem.find(".format.json");
        if (dot == std::string::npos) continue;
        std::string const name = stem.substr(0, dot);

        std::ifstream in{it->path()};
        ASSERT_TRUE(in.good()) << stem;
        nlohmann::json doc;
        in >> doc;

        std::vector<std::string> raw;
        if (doc.contains("runtimeLibraries")
            && doc.at("runtimeLibraries").is_array()) {
            for (auto const& row : doc.at("runtimeLibraries")) {
                if (!row.is_object() || !row.contains("source")) continue;
                if (!row.at("source").is_string()) continue;
                raw.push_back(row.at("source").get<std::string>());
            }
        }

        auto loaded = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(loaded.has_value()) << name << " must load";
        auto const seen = (*loaded)->runtimeLibraries().realizedSources();
        EXPECT_EQ(seen, raw)
            << name
            << ": the loader's realized-source list and the raw "
               "`runtimeLibraries[].source` key disagree. The shipped-source "
               "sweep in tests/ffi reads the RAW key, so a disagreement here "
               "means that sweep is guarding a document the compiler does not "
               "see.";
        ++compared;
    }
    // A directory walk that matched nothing exits green; assert the COUNT.
    EXPECT_GE(compared, 20u)
        << "the shipped object-format corpus must have been walked; only "
        << compared << " document(s) were compared";
}

// ══ THE FAMILY-AGREEMENT GUARD (P54 lane `ar`) ═══════════════════════════════
//
// **Every flavour document of one format KIND must name the SAME provider for
// every role two of them both declare.**
//
// ★★★ WHY THIS EXISTS, AND IT IS THE ANSWER TO AN OPERATOR OBSERVATION RATHER
// THAN A SPECULATIVE INVARIANT. The observation was that
// `runtime/platform/src/atomic.c` is written FOUR times — once in each pe64
// flavour document — while a shipped-lib descriptor declares its realization
// ONCE for the whole format family, and that the family-level fact therefore
// belongs at a family-level tier.
//
// ✔MEASURED over the 26 shipped documents, and the measurement REFRAMES the
// observation rather than confirming it: the repetition is a property of the
// format-document TIER, not of the atomics row.
//   * `runtime/platform/src/atomic.c` (pe, atomicsRuntime)   appears in  4 docs
//   * `libatomic.so.1`                (elf, atomicsRuntime)  appears in 10 docs
//   * `/usr/lib/libSystem.B.dylib`    (macho, atomicsRuntime) appears in 8 docs
//   * `libc.so.6`                     (elf, cLibrary)        appears in  4 docs
// and the whole `atomicsRuntime` SPINE BLOCK is byte-identical across every
// flavour of all five families — as are 11 of 23 top-level keys on pe64 and 14
// of 23 on elf64. The atomics source path is the LEAST-repeated provider fact in
// the corpus. There is no inheritance or family document in this tier, so the
// only way to write a family fact once would be to invent one — which is above
// a lane and would move ELF and Mach-O.
//
// ⇒ WHAT IS ACTUALLY AVAILABLE, AND IT IS STRICTLY BETTER THAN A DEDUPLICATION:
// the four copies cannot be made ONE, but they CAN be made provably ONE FACT.
// Deduplication removes the chance to disagree; this guard removes the ABILITY
// to disagree while leaving the documents readable on their own. And it covers
// the ten `libatomic.so.1` copies and the eight libSystem copies at the same
// time, which a hand-move of one row would not have touched.
//
// ⚠ ABSENCE IS NOT DISAGREEMENT, DELIBERATELY. `-dll` declares no `cLibrary`
// row and `-exec` does; a flavour that does not need a role must not be forced
// to name one, and the ONLY thing forbidden is two flavours naming DIFFERENT
// providers. That is why the rule quantifies over roles two documents BOTH
// declare, not over the cross product.
//
// ⚠ AND THE GROUPING IS THE FORMAT'S OWN DECLARED KIND, read off the loaded
// schema — never a hardcoded {pe, elf, macho} list, which would silently stop
// covering the next kind the corpus grows.
TEST(RuntimeLibraryRoles, EveryFlavourOfAFormatKindNamesOneProviderPerRole) {
    auto const root = dss::test::findConfigRoot();
    ASSERT_TRUE(root.has_value()) << dss::test::configRootDiagnostic();
    auto const dir = *root / "object-formats";
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::is_directory(dir, ec)) << dir.generic_string();

    // (kind, role) -> provider spelling -> the documents naming it.
    std::map<std::pair<std::string, std::string>,
             std::map<std::string, std::vector<std::string>>> byFamily;
    std::size_t documents = 0;

    for (std::filesystem::directory_iterator it{dir, ec}, end; it != end;
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string const file = it->path().filename().generic_string();
        auto const        dot  = file.find(".format.json");
        if (dot == std::string::npos) continue;
        std::string const name = file.substr(0, dot);

        auto loaded = ObjectFormatSchema::loadShipped(name);
        ASSERT_TRUE(loaded.has_value()) << name << " must load";
        ++documents;
        std::string const kind{objectFormatKindName((*loaded)->kind())};
        for (auto const& b : (*loaded)->runtimeLibraries().bindings) {
            // The provider spelling carries its KIND as well as its value, so a
            // format naming an IMAGE called exactly what a sibling names as a
            // SOURCE cannot collapse into agreement.
            std::string const provider =
                b.source.empty() ? ("image " + b.image)
                                 : ("shipped source " + b.source);
            byFamily[{kind, std::string{runtimeLibraryRoleName(b.role)}}]
                    [provider].push_back(name);
        }
    }

    std::size_t multiDocumentGroups = 0;
    std::size_t peAtomicsDocuments  = 0;
    for (auto const& [key, providers] : byFamily) {
        auto const& [kind, role] = key;
        std::size_t docs = 0;
        for (auto const& [provider, names] : providers) docs += names.size();
        if (docs >= 2) ++multiDocumentGroups;
        if (kind == "pe" && role == "atomicsRuntime") peAtomicsDocuments = docs;
        if (providers.size() <= 1) continue;

        std::string detail;
        for (auto const& [provider, names] : providers) {
            detail += "\n    " + provider + "  <-  ";
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (i != 0) detail += ", ";
                detail += names[i];
            }
        }
        ADD_FAILURE()
            << "format kind '" << kind << "' declares role '" << role
            << "' with " << providers.size()
            << " DIFFERENT providers across its flavour documents:" << detail
            << "\n  Who plays a runtime role is a property of the format FAMILY "
               "— a shipped-lib descriptor declares its `library`/`realization` "
               "per family for exactly this reason. This tier has no family "
               "document, so the fact is written once per flavour; two spellings "
               "means one of them is a typo or a half-finished migration, and "
               "which flavour a program happens to build decides which runtime "
               "it gets.";
    }

    // ⚠ A SWEEP THAT COMPARED NOTHING PASSES. Three counts, each closing a
    // different vacuity: the corpus was walked at all; at least some groups had
    // TWO documents to disagree (a corpus of one flavour per kind would satisfy
    // the rule trivially); and the specific group this guard was written for —
    // the four pe64 atomics rows — is one of them.
    EXPECT_GE(documents, 20u)
        << "the shipped object-format corpus must have been walked; only "
        << documents << " document(s) loaded";
    EXPECT_GE(multiDocumentGroups, 4u)
        << "only " << multiDocumentGroups
        << " (kind, role) group(s) had two or more documents, so this guard "
           "compared almost nothing — it passes vacuously on a corpus where "
           "every kind has a single flavour";
    EXPECT_GE(peAtomicsDocuments, 4u)
        << "the pe `atomicsRuntime` role was declared by " << peAtomicsDocuments
        << " document(s); this guard exists because it is declared by FOUR, and "
           "a drop means the rows this rule was written for stopped being "
           "compared";
}

// ★★ EXACTLY ONE PROVIDER PER ROW, REFUSED AT LOAD IN BOTH DIRECTIONS. Neither
// is an unfilled role; both are two owners for one body — the refusal
// `shippedLibs`' own `library`/`realization` pair has made since
// D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF, now at the tier where ROLES live.
TEST(RuntimeLibraryRoles, ARuntimeLibraryRowDeclaringBothAProviderKindIsRefused) {
    std::string const text = readShippedFormatText("pe64-x86_64-windows-exec");
    ASSERT_FALSE(text.empty());
    {
        auto ok = ObjectFormatSchema::loadFromText(text);
        ASSERT_TRUE(ok.has_value())
            << "the UNMUTATED shipped text must load clean, or the mutants prove "
               "nothing";
    }
    constexpr std::string_view kRow =
        R"({ "role": "atomicsRuntime",    "source": "runtime/platform/src/atomic.c" })";
    ASSERT_NE(text.find(kRow), std::string::npos)
        << "the mutation anchor must still exist — a no-op mutation is how a "
           "red-on-disable pin goes green forever";

    // (a) BOTH.
    {
        std::string mutant = text;
        ASSERT_TRUE(substituteOnce(
            mutant, kRow,
            R"({ "role": "atomicsRuntime", "image": "libatomic-1.dll", "source": "runtime/platform/src/atomic.c" })"));
        auto bad = ObjectFormatSchema::loadFromText(mutant, "<both>");
        EXPECT_FALSE(bad.has_value())
            << "a row naming BOTH an image and a shipped source is two owners for "
               "one body and must be refused at load";
    }
    // (b) NEITHER.
    {
        std::string mutant = text;
        ASSERT_TRUE(substituteOnce(mutant, kRow,
                                   R"({ "role": "atomicsRuntime" })"));
        auto bad = ObjectFormatSchema::loadFromText(mutant, "<neither>");
        EXPECT_FALSE(bad.has_value())
            << "a row naming NEITHER provider is an unfilled role and must be "
               "refused at load";
    }
    // (c) AN ESCAPING SOURCE PATH. The containment check is shared with the
    // shipped-lib descriptor's `realization.<format>.source`, and it is a
    // SECURITY predicate: a rooted spelling would let a config document reach an
    // arbitrary file on the host.
    {
        std::string mutant = text;
        ASSERT_TRUE(substituteOnce(
            mutant, kRow,
            R"({ "role": "atomicsRuntime", "source": "//host/share/evil.c" })"));
        auto bad = ObjectFormatSchema::loadFromText(mutant, "<escape>");
        EXPECT_FALSE(bad.has_value())
            << "a rooted / escaping `source` spelling must be refused at load — "
               "`operator/` with a rooted right operand REPLACES rather than "
               "appends, so the document would name a file outside the config "
               "root";
    }
}
