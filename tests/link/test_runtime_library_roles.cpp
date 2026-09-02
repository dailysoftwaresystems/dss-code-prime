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
// `mustDeclare` is the UCRT-P4 answer for that flavour, and each `why` is the
// argument for it. An excluded flavour is NEVER silently skipped: it is asserted
// to still be excludable, so the day it gains a table this test goes red and the
// exclusion has to be re-argued instead of quietly widening.
struct PeFlavour {
    std::string_view name;
    bool             mustDeclare;
    char const*      why;
};

// MEASURED 2026-08-10 (build-dbg, `--compile` of a `__try` TU at each target, then
// reading the emitted artifact's section table) — the verdicts below are that
// measurement, not a reading of the writers.
constexpr PeFlavour kPeFlavours[] = {
    {"pe64-x86_64-windows-exec", true,
     "the .exe image: encodeExec emits .pdata + .xdata, and the spine's "
     "processExit/processArgs/librarySynthesis name cLibrary + systemPrimitives "
     "on top of the unwindPersonality row"},
    {"pe64-x86_64-windows-dll", true,
     "the .dll image routes through the SAME encodeExec substrate as the .exe "
     "(pe.type = dll), so it genuinely carries .pdata + .xdata. MEASURED by "
     "RUNNING: a real Windows process loaded a DSS-built .dll, took a hardware "
     "divide-by-zero inside a __try and resumed in the __except body at both "
     "--config=debug and --config=release. A DLL containing __try is how much of "
     "Windows is written, so a missing block here is a wrong-reject a USER finds"},
    {"pe64-x86_64-windows-staticlib", false,
     "the .lib archive goes through pe::encode's Obj arm, which attaches no "
     "FrameUnwindInfo and emits NO .pdata/.xdata. MEASURED by declaring the keys "
     "anyway: rc=0 and the emitted seh.lib carried `.text` ONLY, so the filter "
     "funclets and the scope table are SILENTLY DROPPED and __except can never "
     "run. Declaring here would trade a loud, correct refusal for a silent "
     "miscompile; the rationale lives in the file's "
     "$sehPersonalityOmittedComment"},
    {"pe64-x86_64-windows", false,
     "the relocatable .obj, same Obj arm and the same MEASURED silent drop "
     "(`.text` only). Worse here than for the archive, because the object exists "
     "to be linked LATER and the final linker cannot notice that unwind data was "
     "never emitted for these functions"},
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

[[nodiscard]] std::size_t mustDeclareCount() {
    std::size_t n = 0;
    for (auto const& f : kPeFlavours) if (f.mustDeclare) ++n;
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

        if (flavour.mustDeclare) {
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
            EXPECT_TRUE(table.empty())
                << flavour.name
                << " is on the EXPLICIT exclusion list but now DECLARES a "
                   "`runtimeLibraries` table. This test is deliberately red so the "
                   "exclusion gets re-argued rather than widened by accident. The "
                   "recorded reason it was excluded: " << flavour.why
                << ". If the underlying limitation is genuinely fixed (the pe Obj "
                   "arm now emits .pdata/.xdata), move this flavour to "
                   "mustDeclare = true and re-measure the emitted sections — do "
                   "not delete this assertion.";
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
    EXPECT_EQ(tablesSeen, mustDeclareCount())
        << "the number of pe formats actually declaring a `runtimeLibraries` table "
           "must equal the number this file's verdicts say should — a count that "
           "merely has a FLOOR is satisfied by one file out of four, which is the "
           "state this assertion replaced.";
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
TEST(RuntimeLibraryRoles, DeletingTheDllsOnlyRoleRowRefusesTheFormatAtLoad) {
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
    constexpr std::string_view kRow =
        R"({ "role": "unwindPersonality", "image": "ucrtbase.dll" })";
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
        ASSERT_TRUE(doc.contains("runtimeLibraries")
                    && doc.at("runtimeLibraries").is_array()
                    && doc.at("runtimeLibraries").empty())
            << "the mutation must leave an EMPTY runtimeLibraries array — that is "
               "the state under test (a table that declares no role at all)";
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
    // ⚠ AND THE NEGATIVE, WHICH IS THE LOAD-BEARING HALF: pe64 declares NONE.
    // UCRT exports no `__atomic_*` symbol and there is no Windows atomics image
    // to name. That is exactly why the target key is a three-way enum — under
    // `losesAtomicity` the native form STANDS here rather than the build being
    // refused, which keeps DSS inside the reference union on Windows.
    for (auto const name : {"pe64-x86_64-windows-exec", "pe64-x86_64-windows-dll"}) {
        auto loaded = ObjectFormatSchema::loadShipped(std::string{name});
        ASSERT_TRUE(loaded.has_value()) << name;
        EXPECT_FALSE((*loaded)->atomicsRuntime().has_value())
            << name << " must declare NO atomics runtime — naming an image that "
                       "does not export the entries would turn a working build "
                       "into an unresolved symbol at link";
    }
}
