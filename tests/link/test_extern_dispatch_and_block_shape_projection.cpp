// ── D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET ──────────
//    The two halves of the class that meet in the LINK tier:
//      * a BLOCK-SHAPE sentence, which advertises a KEY set exactly as a value
//        refusal advertises a spelling set, and drifts identically; and
//      * `externCallDispatch`, a `.format.json` vocabulary whose accepted set
//        was retyped in THREE tiers — the format loader that owns it, the
//        entry-trampoline emitter, and the MIR→LIR lowerer — none of which can
//        see a rename in the others.
// ─────────────────────────────────────────────────────────────────────────
//
// THE CLASS. Acceptance is decided by a table; the diagnostic beside the check
// states the accepted set as a STRING LITERAL. Two owners of one fact. The rows
// keep working while the SENTENCE becomes a lie, and the sentence is the half a
// config author reads.
//
// ★★★ WHY THE CROSS-TIER HALF IS THE SHARPER ONE. A retyped set inside a loader
// at least sits beside the check it lies about, so a careful reader of that
// block can notice. The `externCallDispatch` spellings were typed into
// `src/link/entry_trampoline.cpp` and `src/lir/lowering/mir_to_lir.cpp` — two
// files that hold no vocabulary, resolve no spelling, and would not be opened
// by anyone renaming a row of `kExternCallDispatchTable`. Both messages END IN
// AN INSTRUCTION ("declare `externCallDispatch` (…) in the format JSON"), so a
// stale one does not merely mislead: it tells an author to write a value the
// loader will refuse.
//
// ⚠ AND THE COMMENT ABOVE ONE OF THEM HAD ALREADY GONE WRONG IN THE SAME WAY —
// it motivated the gate with a claim about PE that the shipped configs refute
// (✔MEASURED 2026-08-20: all 22 object-format documents declare `direct-plt`
// and not one declares `indirect-slot`). That was corrected earlier this cycle;
// the retyped SPELLINGS were the separate half, and are what these pins hold.
//
// ★★ WHAT EACH PIN ASSERTS:
//   (A) CORPUS EXERCISE — the shipped format documents really declare these
//       blocks and these spellings, so a table-row rename breaks something real
//       rather than nothing.
//   (B) COMPLETENESS — every name in the table appears in the refusal message.
//   (C) HONESTY — every vocabulary token the message quotes is one the check
//       accepts. For the cross-tier sites this is the load-bearing direction:
//       the FORMAT LOADER is the check those two messages instruct against, so
//       "honest" means every spelling they advertise really loads.
//
// ★★ THE EXPECTATION IS DRIVEN FROM THE TABLE, NEVER FROM TODAY'S SENTENCE. A
// pin spelling out the expected message is the same retyping one level up.
//
// ⚠ MUST run through ctest, never a bare `.exe`: the shipped-config resolver
// walks the cwd unless `DSS_CONFIG_ROOT` is set, and only `dss_add_test` sets
// it. A bare binary reads whichever config tree the shell happens to stand in.

#include "core/types/aggregate_layout.hpp"
#include "core/types/enum_name_table.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "link/entry_trampoline.hpp"
#include "link/object_format_schema.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"

#include "repo_root.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

using ::dss::allNames;
using ::dss::ObjectFormatSchema;

// A spelling no vocabulary in this tree claims. Deliberately ugly: a probe that
// collided with a real name would make every negative arm pass for the wrong
// reason.
constexpr char const* kBadSpelling = "zzNotAnyLinkVocabularySpelling";

// The two shipped documents these pins base their mutations on. Both are read
// FROM DISK, never re-typed here: a hand-written "format-like" fixture proves
// the rule fires on the fixture, and only the real file proves it fires on what
// we ship.
constexpr char const* kElfExecFormat = "elf64-x86_64-linux-exec";
constexpr char const* kPeExecFormat  = "pe64-x86_64-windows-exec";

[[nodiscard]] json shippedFormatDoc(std::string_view name) {
    auto const root = dss::test::findRepoRoot();
    if (!root) {
        ADD_FAILURE() << dss::test::repoRootDiagnostic();
        return json{};
    }
    auto const p = *root / "src" / "dss-config" / "object-formats" /
                   (std::string{name} + ".format.json");
    std::ifstream in{p, std::ios::binary};
    if (!in) {
        ADD_FAILURE() << "cannot open shipped format document " << p.string();
        return json{};
    }
    std::string text{std::istreambuf_iterator<char>{in},
                     std::istreambuf_iterator<char>{}};
    auto doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) {
        ADD_FAILURE() << p.string() << " is not a JSON object";
        return json{};
    }
    return doc;
}

[[nodiscard]] std::string summarize(auto const& diags) {
    std::string s;
    for (auto const& d : diags) s += "\n  " + d.path + ": " + d.message;
    return s.empty() ? std::string{"<no diagnostics>"} : s;
}

// Every `'…'`-quoted token in a message. The shared renderer quotes each
// spelling, so this is how a test reads back WHAT THE MESSAGE CLAIMS — as
// opposed to re-deriving it from the table, which would make the comparison
// circular and green by construction.
//
// ⚠ IT PAIRS APOSTROPHES, so a possessive `'` in the prose would scramble every
// token after it. That is a constraint on the MESSAGES, not a weakness here:
// a sentence advertising a quoted set is written without a possessive, and the
// two sites that had one were reworded when these pins landed.
[[nodiscard]] std::vector<std::string> quotedTokens(std::string const& msg) {
    std::vector<std::string> out;
    std::size_t              i = 0;
    while (true) {
        auto const open = msg.find('\'', i);
        if (open == std::string::npos) break;
        auto const close = msg.find('\'', open + 1);
        if (close == std::string::npos) break;
        out.push_back(msg.substr(open + 1, close - open - 1));
        i = close + 1;
    }
    return out;
}

[[nodiscard]] bool contains(std::span<std::string_view const> names,
                            std::string_view                  needle) {
    for (auto const& n : names) {
        if (n == needle) return true;
    }
    return false;
}

// (B) COMPLETENESS — SOME message in the batch names every spelling the table
// owns, and that message is the one the honesty arm then reads.
//
// ⚠ THE SEARCH IS THE ASSERTION. A refused load emits more than the vocabulary
// refusal (a rejected value is not stored, so downstream required/congruent
// rules fire on the zero default, each with its own quoted tokens). Running the
// honesty check over the UNION of every message reports those cascades as
// "advertised tokens", which is the pin lying rather than the subject.
template <typename Messages>
[[nodiscard]] std::string const*
findVocabularyMessage(Messages const&                   messages,
                      std::span<std::string_view const> names) {
    for (auto const& m : messages) {
        auto const quoted = quotedTokens(m);
        bool       all    = true;
        for (std::string_view const n : names) {
            if (std::find(quoted.begin(), quoted.end(), std::string{n})
                == quoted.end()) {
                all = false;
                break;
            }
        }
        if (all) return &m;
    }
    return nullptr;
}

template <typename Messages>
void expectNamesExactly(Messages const&                   messages,
                        std::span<std::string_view const> names,
                        std::span<char const* const>      ignoreQuoted,
                        std::string_view                  label) {
    std::string const* msg = findVocabularyMessage(messages, names);
    if (msg == nullptr) {
        std::string all;
        for (auto const& m : messages) all += "\n  " + m;
        ADD_FAILURE()
            << label
            << ": NO message names every spelling the vocabulary accepts. A "
               "message NARROWER than its check tells an author, by name, that "
               "a spelling the loader takes is not allowed. Render the list "
               "through `dss::detail::renderAllowedList` over the table's "
               "projection, never as a literal.\nmessages were:"
            << (all.empty() ? std::string{"\n  <none>"} : all);
        return;
    }
    for (auto const& q : quotedTokens(*msg)) {
        bool ok = contains(names, q);
        for (char const* const g : ignoreQuoted) {
            if (q == g) { ok = true; break; }
        }
        EXPECT_TRUE(ok)
            << label << ": the message quotes '" << q
            << "', which is neither a spelling the vocabulary accepts nor a "
               "declared non-vocabulary quote for this site. A message WIDER "
               "than its check sends an author to write a value that will then "
               "be refused.\nmessage was:\n"
            << *msg;
    }
}

// The diagnostic MESSAGES of a refused format load, as a plain vector — the
// shape the two helpers above consume, shared with the reporter-driven sites
// so both tiers are read by exactly one reader.
[[nodiscard]] std::vector<std::string>
refusalMessages(json const& doc, std::string_view label) {
    auto const r = ObjectFormatSchema::loadFromText(doc.dump(), label);
    std::vector<std::string> out;
    if (r.has_value()) return out;
    for (auto const& d : r.error()) {
        if (d.severity != dss::DiagnosticSeverity::Error) continue;
        out.push_back(d.message);
    }
    return out;
}

} // namespace

// ── (A) CORPUS EXERCISE ─────────────────────────────────────────────────

TEST(LinkVocabularyProjection, ShippedFormatsExerciseTheProbedVocabularies) {
    static constexpr auto kDispatchNames =
        allNames(dss::kExternCallDispatchTable);

    auto const elf = shippedFormatDoc(kElfExecFormat);
    auto const pe  = shippedFormatDoc(kPeExecFormat);
    ASSERT_TRUE(elf.is_object());
    ASSERT_TRUE(pe.is_object());

    for (auto const* doc : {&elf, &pe}) {
        ASSERT_TRUE(doc->contains("externCallDispatch"))
            << "the base document declares no `externCallDispatch`, so the "
               "cross-tier pins below would guard a path nothing takes";
        EXPECT_TRUE(contains(kDispatchNames,
                             doc->at("externCallDispatch")
                                 .get<std::string>()))
            << "a shipped format declares an `externCallDispatch` spelling the "
               "projection does not contain — the corpus and the vocabulary "
               "disagree";
        auto const r = ObjectFormatSchema::loadFromText(doc->dump(), "baseline");
        ASSERT_TRUE(r.has_value())
            << "the base document must load clean before any mutant means "
               "anything: " << summarize(r.error());
    }

    // The blocks whose SHAPE sentences are pinned below really exist in the
    // documents the mutations are aimed at — a probe whose block went missing
    // would write a brand-new subtree that nothing reads.
    EXPECT_TRUE(elf.contains("runtimeLibraries"));
    EXPECT_TRUE(pe.contains("tlsAccess"));
    EXPECT_TRUE(pe.contains("sehPersonality"));
    EXPECT_TRUE(pe.contains("librarySynthesis"));
}

// ★★ (C) HONESTY, THE CROSS-TIER DIRECTION. Every spelling the entry-trampoline
// and MIR→LIR messages instruct an author to write must be one the FORMAT
// LOADER really accepts — those sentences end in "declare `externCallDispatch`
// (…) in the format JSON", so a spelling they advertise and the loader refuses
// is a message that actively breaks the config it is trying to fix.
TEST(LinkVocabularyProjection, EveryExternCallDispatchSpellingReallyLoads) {
    static constexpr auto kDispatchNames =
        allNames(dss::kExternCallDispatchTable);
    ASSERT_GT(kDispatchNames.size(), 0u);
    for (std::string_view const spelling : kDispatchNames) {
        SCOPED_TRACE(std::string{spelling});
        auto doc = shippedFormatDoc(kPeExecFormat);
        ASSERT_TRUE(doc.is_object());
        doc["externCallDispatch"] = std::string{spelling};
        auto const r = ObjectFormatSchema::loadFromText(doc.dump(), "dispatch");
        EXPECT_TRUE(r.has_value())
            << "the format loader REFUSES '" << spelling
            << "', a spelling two lowering-tier messages tell config authors to "
               "declare. One of the three is wrong and none of them can see the "
               "other two.";
    }
}

// ── (B)+(C) THE FORMAT LOADER: bitFieldStrategy and its sentinel ────────

TEST(LinkVocabularyProjection,
     FormatBitFieldStrategyRefusalNamesOnlyTheSelectableSet) {
    // ★ THE SELECTABLE PROJECTION, NOT `allNames` — and that asymmetry with the
    // TARGET loader is the contract, not an oversight. A `.target.json` may
    // write `none`; a `.format.json` may not, because the format value falls
    // back to the target one when absent, which makes an explicit `none`
    // ambiguous. Each message states ITS OWN check.
    constexpr char const* kIgnore[] = {kBadSpelling, "bitFieldStrategy"};
    auto doc = shippedFormatDoc(kElfExecFormat);
    ASSERT_TRUE(doc.is_object());
    doc["bitFieldStrategy"] = kBadSpelling;
    auto const messages = refusalMessages(doc, "bitfield-probe");
    ASSERT_FALSE(messages.empty())
        << "a typo'd bitFieldStrategy must be REFUSED — it would silently fall "
           "back to the target strategy, a wrong-layout miscompile";
    expectNamesExactly(messages, dss::kSelectableBitFieldStrategyNames, kIgnore,
                       "format bitFieldStrategy");
}

TEST(LinkVocabularyProjection, FormatBitFieldStrategySentinelIsRefusedByName) {
    // The mirror direction: the sentinel spells correctly, so the name lookup
    // resolves it and only an explicit selectable check refuses it. The refusal
    // must NAME the sentinel — projected, so a rename reaches the sentence —
    // and the advertised set above must NOT contain it.
    auto doc = shippedFormatDoc(kElfExecFormat);
    ASSERT_TRUE(doc.is_object());
    auto const sentinel =
        std::string{dss::bitFieldStrategyName(dss::BitFieldStrategy::None)};
    doc["bitFieldStrategy"] = sentinel;
    auto const messages = refusalMessages(doc, "bitfield-sentinel");
    ASSERT_FALSE(messages.empty())
        << "the sentinel spelling must be REFUSED on a FORMAT — it is "
           "ambiguous between 'unset' and 'override the target with nothing'";
    bool named = false;
    for (auto const& m : messages) {
        auto const q = quotedTokens(m);
        if (std::find(q.begin(), q.end(), sentinel) != q.end()) named = true;
    }
    EXPECT_TRUE(named)
        << "the sentinel refusal must NAME the reserved spelling, so an author "
           "can tell 'you wrote the reserved word' apart from 'you wrote a "
           "spelling nobody claims'";
    EXPECT_FALSE(contains(dss::kSelectableBitFieldStrategyNames, sentinel))
        << "the SELECTABLE projection must not contain the sentinel — the "
           "unknown-spelling message would then advertise the one value this "
           "arm rejects";
}

// ── (B)+(C) THE SIBLING CLASS ON THE KEY HALF ───────────────────────────
//
// A SHAPE sentence — `must be an object with the keys …` — advertises a KEY set
// exactly as a value refusal advertises a spelling set. It is reached by making
// the block the WRONG TYPE, which is the one arm that renders the shape rather
// than the block's per-field checks.
//
// ✔THIS EXACT DRIFT HAS FIRED HERE BEFORE, twice, and both are recorded at the
// sites: `tlsAccess` advertised THREE of its four keys while `tlsIndexSlotName`
// was read and never named, and `librarySynthesis` advertised `libraryPath`,
// which is not a key at all — it is the value RESOLVED from `role`.

TEST(LinkVocabularyProjection, TlsAccessShapeNamesEveryKey) {
    static constexpr std::string_view kKeys[] = {
        "model", "segmentPrefixByte", "baseDisplacement", "tlsIndexSlotName"};
    constexpr char const* kIgnore[] = {"tlsAccess", "model"};
    auto doc = shippedFormatDoc(kPeExecFormat);
    ASSERT_TRUE(doc.is_object());
    doc["tlsAccess"] = kBadSpelling;
    auto messages = refusalMessages(doc, "tlsAccess-shape");
    ASSERT_FALSE(messages.empty()) << "a non-object tlsAccess must be REFUSED";
    // The shape sentence also renders the MODEL vocabulary; those spellings are
    // legitimate non-key quotes here, and are pinned as a set by the sibling
    // file that owns that vocabulary.
    std::vector<char const*> ignore{kIgnore, kIgnore + std::size(kIgnore)};
    static constexpr auto kModelNames = allNames(dss::kTlsAccessModelTable);
    for (std::string_view const n : kModelNames) ignore.push_back(n.data());
    // ⚠ `string_view::data()` is only safe here because every row of an
    // `EnumNameTable` is a NUL-terminated literal; the comparison below is by
    // value, so a non-terminated view would compare wrong rather than crash —
    // and the model set is asserted non-empty so a silent zero-row table cannot
    // widen the ignore list to nothing.
    ASSERT_GT(kModelNames.size(), 0u);
    expectNamesExactly(messages, kKeys, ignore, "tlsAccess shape");
}

TEST(LinkVocabularyProjection, RuntimeLibraryRowShapeNamesEveryKey) {
    static constexpr std::string_view kKeys[] = {"role", "image"};
    constexpr char const* kIgnore[] = {"runtimeLibraries"};
    // The ROW arm: a non-object entry inside a well-formed array.
    auto doc = shippedFormatDoc(kElfExecFormat);
    ASSERT_TRUE(doc.is_object());
    ASSERT_TRUE(doc.contains("runtimeLibraries"));
    ASSERT_FALSE(doc.at("runtimeLibraries").empty())
        << "the base document declares an EMPTY runtimeLibraries array, so the "
           "row mutation would land on nothing";
    doc["runtimeLibraries"][0] = kBadSpelling;
    auto const rowMessages = refusalMessages(doc, "runtimeLibraries-row");
    ASSERT_FALSE(rowMessages.empty())
        << "a non-object runtimeLibraries row must be REFUSED";
    expectNamesExactly(rowMessages, kKeys, kIgnore, "runtimeLibraries row");

    // The ARRAY arm renders the same key set plus the ROLE vocabulary.
    auto arrDoc = shippedFormatDoc(kElfExecFormat);
    ASSERT_TRUE(arrDoc.is_object());
    arrDoc["runtimeLibraries"] = kBadSpelling;
    auto const arrMessages = refusalMessages(arrDoc, "runtimeLibraries-array");
    ASSERT_FALSE(arrMessages.empty())
        << "a non-array runtimeLibraries must be REFUSED";
    // The ARRAY sentence also renders the ROLE vocabulary. Its projection is
    // file-local to the loader TU, so the ignore list is derived HERE from the
    // public table — the sentinel included, because ignoring a token the
    // message may not carry is harmless while missing one would red honestly.
    std::vector<char const*> ignore{kIgnore, kIgnore + std::size(kIgnore)};
    for (auto const& row : dss::kRuntimeLibraryRoleTable.rows) {
        ignore.push_back(row.second.data());
    }
    expectNamesExactly(arrMessages, kKeys, ignore, "runtimeLibraries array");
}

TEST(LinkVocabularyProjection, SehPersonalityShapeNamesEveryKey) {
    static constexpr std::string_view kKeys[] = {"role", "mangledName"};
    constexpr char const* kIgnore[] = {"sehPersonality"};
    auto doc = shippedFormatDoc(kPeExecFormat);
    ASSERT_TRUE(doc.is_object());
    doc["sehPersonality"] = kBadSpelling;
    auto const messages = refusalMessages(doc, "sehPersonality-shape");
    ASSERT_FALSE(messages.empty())
        << "a non-object sehPersonality must be REFUSED";
    expectNamesExactly(messages, kKeys, kIgnore, "sehPersonality shape");
}

TEST(LinkVocabularyProjection, LibrarySynthesisShapeNamesEveryKeyAndVehicle) {
    static constexpr std::string_view kKeys[] = {"vehicle", "role"};
    constexpr char const* kIgnore[] = {"librarySynthesis", "runtimeLibraries",
                                       "vehicle", "role"};
    auto doc = shippedFormatDoc(kPeExecFormat);
    ASSERT_TRUE(doc.is_object());
    doc["librarySynthesis"] = kBadSpelling;
    auto const messages = refusalMessages(doc, "librarySynthesis-shape");
    ASSERT_FALSE(messages.empty())
        << "a non-object librarySynthesis must be REFUSED";

    // The sentence carries TWO vocabularies — its key set and the VEHICLE
    // spellings — so each is asserted as a set with the other declared as a
    // legitimate non-vocabulary quote. Neither half may shrink: the key arm
    // would miss a key the block accepts, the vehicle arm a vehicle it takes.
    static constexpr auto kVehicleNames =
        allNames(dss::kLibrarySynthVehicleTable);
    ASSERT_GT(kVehicleNames.size(), 0u);

    std::vector<char const*> keyIgnore{kIgnore, kIgnore + std::size(kIgnore)};
    for (std::string_view const n : kVehicleNames) keyIgnore.push_back(n.data());
    expectNamesExactly(messages, kKeys, keyIgnore, "librarySynthesis keys");

    std::vector<char const*> vehicleIgnore{kIgnore,
                                           kIgnore + std::size(kIgnore)};
    for (std::string_view const k : kKeys) vehicleIgnore.push_back(k.data());
    expectNamesExactly(messages, kVehicleNames, vehicleIgnore,
                       "librarySynthesis vehicle");
}

// ── (B)+(C) THE CROSS-TIER SITES ────────────────────────────────────────

TEST(LinkVocabularyProjection,
     EntryTrampolineNoDispatchRefusalNamesTheWholeTable) {
    static constexpr auto kDispatchNames =
        allNames(dss::kExternCallDispatchTable);

    // A by-name-import exit under a format that declares NO dispatch model.
    // Built from the SHIPPED pe64 exec document with the one key removed, so
    // every other field the emitter reads is whatever we really ship.
    auto doc = shippedFormatDoc(kPeExecFormat);
    ASSERT_TRUE(doc.is_object());
    ASSERT_TRUE(doc.contains("externCallDispatch"))
        << "the base document declares none already — the mutation would be a "
           "no-op and this pin would assert nothing";
    doc.erase("externCallDispatch");
    auto format = ObjectFormatSchema::loadFromText(doc.dump(), "no-dispatch");
    ASSERT_TRUE(format.has_value())
        << "a format may legally omit `externCallDispatch` — the fail-loud is "
           "at the CONSUMER, and this pin needs to reach that consumer: "
        << summarize(format.error());

    auto target = dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());

    // A one-function module whose bytes are irrelevant: the emitter refuses
    // before it lowers anything.
    dss::AssembledModule mod;
    mod.expectedFuncCount = 1;
    dss::AssembledFunction fn;
    fn.symbol = dss::SymbolId{1};
    fn.bytes  = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};   // mov eax,42 ; ret
    mod.functions.push_back(std::move(fn));

    dss::DiagnosticReporter rep;
    bool const ok = dss::linker::injectEntryTrampoline(mod, **target, **format,
                                                       rep);
    EXPECT_FALSE(ok)
        << "a by-name-import exit with no declared dispatch model has no "
           "defined call-site shape — a silent default miscompiles one of the "
           "two import models";
    std::vector<std::string> messages;
    // ⓘ THE TEXT LIVES IN `actual`, not a `message` field. `dss::report`
    // and this tier's local `emit` both put the rendered sentence there;
    // `ParseDiagnostic` has no `message` member at all.
    for (auto const& d : rep.all()) messages.emplace_back(d.actual);
    // The format NAME is quoted by this sentence and is not a vocabulary token.
    std::vector<char const*> ignore{kPeExecFormat};
    expectNamesExactly(messages, kDispatchNames, ignore,
                       "entry-trampoline no-dispatch");
}

namespace {

// The smallest module that reaches the MIR→LIR extern-dispatch gate: one caller
// whose only call targets a symbol declared as an extern import. The gate runs
// at Lowerer construction, before any lowering work, so nothing else is needed.
struct ExternGateProbe {
    dss::TypeInterner        interner{dss::CompilationUnitId{1}};
    dss::DiagnosticReporter  reporter;
    std::vector<std::string> messages;
};

void runExternGateProbe(ExternGateProbe&                       probe,
                        dss::TargetSchema const&               target,
                        std::optional<dss::ExternCallDispatch> dispatch) {
    constexpr std::uint32_t kExternSym = 100u;
    constexpr std::uint32_t kCallerSym = 101u;

    auto const i32  = probe.interner.primitive(dss::TypeKind::I32);
    auto const ptrT = probe.interner.primitive(dss::TypeKind::Ptr);
    auto const sig  = probe.interner.fnSig(std::span<dss::TypeId const>{}, i32,
                                           dss::CallConv::CcMS64);

    dss::MirBuilder mb;
    mb.addFunction(sig, dss::SymbolId{kCallerSym});
    {
        auto const bb = mb.createBlock(dss::StructCfMarker::EntryBlock);
        mb.beginBlock(bb);
        dss::MirLiteralValue lv;
        lv.value = std::int64_t{0};
        lv.core  = dss::TypeKind::I32;
        dss::MirInstId const zero = mb.addConst(lv, i32);
        dss::MirInstId const addr =
            mb.addGlobalAddr(dss::SymbolId{kExternSym}, ptrT);
        std::array<dss::MirInstId, 2> ops{addr, zero};
        mb.addReturn(mb.addInst(dss::MirOpcode::Call, ops, i32));
    }
    dss::Mir m = std::move(mb).finish();

    dss::ExternImport ext{};
    ext.symbol      = dss::SymbolId{kExternSym};
    ext.mangledName = "extern_fn";
    ext.libraryPath = "fictional.lib";

    (void)dss::lowerToLir(m, target, probe.interner, probe.reporter,
                          std::vector<dss::ExternImport>{ext}, dispatch);
    for (auto const& d : probe.reporter.all()) {
        probe.messages.emplace_back(d.actual);
    }
}

} // namespace

TEST(LinkVocabularyProjection, MirToLirNoDispatchRefusalNamesTheWholeTable) {
    static constexpr auto kDispatchNames =
        allNames(dss::kExternCallDispatchTable);
    auto target = dss::TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());

    ExternGateProbe probe;
    runExternGateProbe(probe, **target, std::nullopt);
    ASSERT_GT(probe.reporter.errorCount(), 0u)
        << "extern imports under a nullopt dispatch must fail loud at "
           "construction — no silent default to either shape";
    constexpr std::span<char const* const> kNoIgnore{};
    expectNamesExactly(probe.messages, kDispatchNames, kNoIgnore,
                       "MIR->LIR no-dispatch");
}

TEST(LinkVocabularyProjection, MirToLirMissingOpcodeArmNamesItsOwnSpelling) {
    // The value arms name the dispatch shape that FIRED. They used to type it
    // out; they now project it from the enumerator the branch condition has
    // already established, so a rename cannot leave one arm saying the other
    // shape's name.
    //
    // arm64 declares no `call_indirect_via_extern`, so an `indirect-slot`
    // dispatch on it is the shape that reaches this arm.
    auto target = dss::TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());

    ExternGateProbe probe;
    runExternGateProbe(probe, **target, dss::ExternCallDispatch::IndirectSlot);
    ASSERT_GT(probe.reporter.errorCount(), 0u)
        << "indirect-slot on a target lacking call_indirect_via_extern must "
           "fail loud rather than silently mis-lower";

    auto const fired = std::string{dss::externCallDispatchName(
        dss::ExternCallDispatch::IndirectSlot)};
    auto const other = std::string{dss::externCallDispatchName(
        dss::ExternCallDispatch::DirectPlt)};
    bool namesFired = false;
    bool namesOther = false;
    for (auto const& m : probe.messages) {
        if (m.find(fired) != std::string::npos) namesFired = true;
        if (m.find(other) != std::string::npos) namesOther = true;
    }
    EXPECT_TRUE(namesFired)
        << "the missing-opcode arm must NAME the dispatch shape that fired — "
           "an author reading it has to know which of the two the format "
           "declared";
    EXPECT_FALSE(namesOther)
        << "the missing-opcode arm named the OTHER dispatch shape, which is "
           "the exact confusion a projected spelling exists to prevent";
}
