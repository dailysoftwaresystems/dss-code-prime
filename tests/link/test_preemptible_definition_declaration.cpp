// ★★ [[D-MIR-DYLIB-SELF-CALL-BYPASSES-WEAK-COALESCING]] — the DECLARATION half.
//
// `preemptibleDefinitionBindings` names which of an artifact's OWN
// externally-visible definitions its LOADER may replace with another image's
// body, so a call made inside the artifact must be resolved by the loader
// instead of branched to the local body. Skipping that is a MEANING divergence,
// not a slow path: one process ends up holding two answers for one identifier,
// silently (✔MEASURED on both rails — a DSS-built ELF `.so` returned rc 4 where
// the gcc-built control returned rc 6; a DSS-built darwin dylib rc 1 where
// ld64's returned rc 2).
//
// ⚠ EVERY FAILURE MODE OF THIS KEY IS SILENT IN THE SAME DIRECTION — the routing
// quietly does not happen and the artifact looks fine — so the loader's refusals
// are the only thing standing between a typo and the defect coming back. They
// are pinned here for that reason, one arm per refusal, plus the two DEFAULTS
// (absent, and non-`default` visibility) that every shipped artifact depends on.

#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "link/object_format_schema.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

// The same minimal ELF descriptor the sibling loader tests use, plus an
// `externCallDispatch` — the key this one PAIRS with, so the pairing rule is
// satisfied except in the arm that deliberately removes it.
constexpr std::string_view kBase = R"({
  "dssObjectFormatVersion": 1,
  "cSymbolDecoration": { "scheme": "none" },
  "cCallingConvention": { "convention": "none" },
  "outputExtension": ".so",
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
  "externCallDispatch": "direct-plt",
  "format": { "name": "elf64-x86_64-linux-dyn", "version": "1.0", "kind": "elf" },
  "elf": { "class": "elf64", "data": "lsb", "machine": 62 },
  "relocations": [ { "name": "R_X86_64_PC32", "kind": 1, "nativeId": 2 } ]
})";

[[nodiscard]] std::string withKey(std::string_view body,
                                  std::string_view base = kBase) {
    std::string json{base};
    json.insert(json.rfind('}'),
                std::string{",\"preemptibleDefinitionBindings\":"}
                    + std::string{body});
    return json;
}

} // namespace

// The ELF shared-object declaration, and the accessor that carries it verbatim
// to MIR→LIR and to the optimizer's inline gate.
TEST(PreemptibleDefinitionDeclaration, TheDeclaredListLoadsVerbatim) {
    auto r = ObjectFormatSchema::loadFromText(withKey(R"(["global","weak"])"));
    ASSERT_TRUE(r.has_value());
    auto const& listed = (*r)->preemptibleDefinitionBindings();
    ASSERT_EQ(listed.size(), 2u);
    EXPECT_EQ(listed[0], SymbolBinding::Global);
    EXPECT_EQ(listed[1], SymbolBinding::Weak);

    // The rule, through its ONE owner. Both listed bindings are preemptible at
    // DEFAULT visibility and neither is at any other — visibility is a
    // universal precondition the format cannot widen.
    EXPECT_TRUE((*r)->definitionIsPreemptible(SymbolBinding::Weak,
                                              SymbolVisibility::Default));
    EXPECT_TRUE((*r)->definitionIsPreemptible(SymbolBinding::Global,
                                              SymbolVisibility::Default));
    for (auto const v : {SymbolVisibility::Hidden, SymbolVisibility::Protected,
                         SymbolVisibility::Internal}) {
        EXPECT_FALSE((*r)->definitionIsPreemptible(SymbolBinding::Weak, v))
            << "a definition outside the dynamic export set is invisible to "
               "every loader, so no declaration can make it preemptible";
        EXPECT_FALSE((*r)->definitionIsPreemptible(SymbolBinding::Global, v));
    }
    EXPECT_FALSE((*r)->definitionIsPreemptible(SymbolBinding::Local,
                                               SymbolVisibility::Default))
        << "a module-private definition is not published at all";
}

// The Mach-O dylib declaration is a STRICT SUBSET of the ELF one — dyld's
// two-level namespace coalesces only WEAK definitions while ELF's search scope
// makes every default-visibility definition in a `.so` interposable. That
// subset relationship is the entire reason this is DECLARED rather than a rule,
// so it is asserted rather than described.
TEST(PreemptibleDefinitionDeclaration, ANarrowerDeclarationPreemptsLess) {
    auto r = ObjectFormatSchema::loadFromText(withKey(R"(["weak"])"));
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE((*r)->definitionIsPreemptible(SymbolBinding::Weak,
                                              SymbolVisibility::Default));
    EXPECT_FALSE((*r)->definitionIsPreemptible(SymbolBinding::Global,
                                              SymbolVisibility::Default))
        << "dyld binds a STRONG dylib definition locally — routing it would buy "
           "an indirection and nothing else";
}

// THE BACK-COMPAT DEFAULT, stated rather than assumed: an absent key means
// NOTHING is preemptible. Every relocatable object, every static library and
// every main-executable flavour in the shipped corpus relies on it, and its
// polarity is the OPPOSITE of `indirectSlotBindings`' absent-key meaning.
TEST(PreemptibleDefinitionDeclaration, AnAbsentKeyPreemptsNothing) {
    auto r = ObjectFormatSchema::loadFromText(std::string{kBase});
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE((*r)->preemptibleDefinitionBindings().empty());
    for (auto const b : {SymbolBinding::Local, SymbolBinding::Global,
                         SymbolBinding::Weak}) {
        EXPECT_FALSE((*r)->definitionIsPreemptible(b,
                                                   SymbolVisibility::Default));
    }
}

// An EMPTY array says exactly what omitting the key says, and a key that
// declares nothing still reads as an authoritative answer to whoever greps for
// it next. Refused rather than absorbed.
TEST(PreemptibleDefinitionDeclaration, AnEmptyDeclarationIsRefused) {
    EXPECT_FALSE(ObjectFormatSchema::loadFromText(withKey("[]")).has_value());
}

// A typo must be a HARD reject. Absorbing it would leave the artifact silently
// unrouted — which is the defect, reached by a misspelling.
TEST(PreemptibleDefinitionDeclaration, AnUnknownSpellingIsRefused) {
    EXPECT_FALSE(
        ObjectFormatSchema::loadFromText(withKey(R"(["interposable"])"))
            .has_value());
    EXPECT_FALSE(ObjectFormatSchema::loadFromText(withKey(R"("weak")"))
                     .has_value())
        << "the key is an ARRAY of binding names, not a scalar";
}

// `local` can never match: a module-private definition is in no image's dynamic
// export set. A member that can never match reads as a capability and is not
// one (the D-LK-PE-ALTERNATENAME-DECLARE-AND-REFUSE shape).
TEST(PreemptibleDefinitionDeclaration, LocalIsRefusedAsAMemberThatCannotMatch) {
    EXPECT_FALSE(ObjectFormatSchema::loadFromText(withKey(R"(["local"])"))
                     .has_value());
    EXPECT_FALSE(
        ObjectFormatSchema::loadFromText(withKey(R"(["weak","local"])"))
            .has_value())
        << "one unmatched member poisons the declaration however many valid "
           "ones sit beside it";
}

// A repeated member is an authoring mistake this loader does not absorb — the
// list is a SET.
TEST(PreemptibleDefinitionDeclaration, ADuplicateMemberIsRefused) {
    EXPECT_FALSE(
        ObjectFormatSchema::loadFromText(withKey(R"(["weak","weak"])"))
            .has_value());
}

// THE PAIRING RULE. The reference that replaces the direct branch is an
// ordinary loader-resolved call, and WHAT SHAPE it takes is
// `externCallDispatch`'s answer. Declaring preemption without it leaves MIR→LIR
// choosing a call shape blind — and the wrong choice is a SIGSEGV, not a slow
// path. Refused at LOAD, where the document that made the claim can be named.
TEST(PreemptibleDefinitionDeclaration,
     PreemptionWithoutACallShapeIsRefusedAtLoad) {
    // The same base with `externCallDispatch` removed — one key is the only
    // variable between this and `TheDeclaredListLoadsVerbatim`.
    std::string base{kBase};
    auto const at = base.find(R"(  "externCallDispatch": "direct-plt",)");
    ASSERT_NE(at, std::string::npos);
    base.erase(at, std::string_view{R"(  "externCallDispatch": "direct-plt",)"}
                       .size() + 1);
    // CONTROL: the stripped base still loads on its own, so a failure below is
    // the pairing rule and not the surgery.
    ASSERT_TRUE(ObjectFormatSchema::loadFromText(base).has_value())
        << "the base document must stay loadable without the dispatch key";
    EXPECT_FALSE(
        ObjectFormatSchema::loadFromText(withKey(R"(["weak"])", base))
            .has_value());
}

// ── THE SHIPPED DOCUMENTS THEMSELVES, AND THE ASYMMETRY BETWEEN THEM ─────────
//
// ★★★ EVERY CASE ABOVE DRIVES `loadFromText` ON A SYNTHETIC DOCUMENT, SO NOT ONE
// OF THEM READS WHAT DSS ACTUALLY SHIPS. ✔MEASURED at the moment this block was
// written: deleting `preemptibleDefinitionBindings` from BOTH
// `elf64-{x86_64,aarch64}-linux-dyn.format.json` left the whole tree green —
// the reader was pinned, the CLAIM that DSS declares it was not. "100% config
// driven" is a property of the shipped configs, and a property nothing observes
// is a property that decays the first time someone tidies a document.
//
// ★★ THE ASYMMETRY IS THE OTHER HALF, AND IT IS DELIBERATE — pinning it is what
// stops a later cycle from "completing" the set by reflex. ELF declares
// ["global","weak"]; the two darwin dylib documents declare ["weak"].
// ✔MEASURED, each reference probed separately with controls: gcc 13.3.0 and
// clang 18.1.3 both route a `.so`'s call to its own STRONG GLOBAL through the
// PLT, so ELF's set is the wider one; Apple clang 21.0.0 / ld-1267 on Apple
// Silicon leaves a dylib's call to its own strong global and to a `static` a
// DIRECT `bl` with an empty weak-bind stream, so Mach-O's set is `weak` only.
// The assertion below is an EXACT SET COMPARISON in both directions, so
// "completing" either list goes red on size.
//
// ⓘ HISTORY, KEPT BECAUSE THE REASON OUTLIVES THE STATE. The two darwin rows
// read `{}` until cycle P62: the sets were known, but the Mach-O writer had no
// encoding meaning "resolve from the loader's coalescing scope", and declaring
// the key without one would have turned a silent wrong answer into a LOUD
// REFUSAL of a program Apple clang builds correctly — one violation of the bar
// traded for another. The writer gained the encoding in that cycle (the
// LC_DYLD_INFO_ONLY weak-bind stream, byte-identical to ld64's, plus the
// chained `<weak-def-coalesce>` ordinal), so the declaration landed with it and
// D-MIR-DYLIB-SELF-CALL-BYPASSES-WEAK-COALESCING closed on both rails.
// ⇒ THE STANDING RULE THAT PRODUCED THAT UPDATE STILL BINDS: a format that
//   gains or loses a preemptible binding updates THIS case in the SAME commit.
//   It is written to fail loudly on the day a shipped document's list moves,
//   rather than to pass quietly through the change it is meant to notice.
TEST(PreemptibleDefinitionDeclaration, TheShippedDocumentsDeclareTheMeasuredSets) {
    struct Row {
        std::string_view format;
        std::vector<SymbolBinding> expected;   // empty = the key is absent
        std::string_view why;
    };
    const Row rows[] = {
        {"elf64-x86_64-linux-dyn", {SymbolBinding::Global, SymbolBinding::Weak},
         "gcc and clang both route a .so's call to its own strong global "
         "through the PLT"},
        {"elf64-aarch64-linux-dyn", {SymbolBinding::Global, SymbolBinding::Weak},
         "same measurement, run under qemu with a matching aarch64 control"},
        // ★ THE WITHHELD PAIR IS NOW DECLARED, AND THE ASYMMETRY IS STILL THE
        // POINT. These two read `{}` while the Mach-O writer had no way to
        // encode a coalescing-scope reference — declaring the key then would
        // have turned a SILENT wrong answer into a LOUD REFUSAL of a program
        // Apple clang builds correctly. The writer gained the encoding (the
        // LC_DYLD_INFO_ONLY weak-bind stream, byte-identical to ld64's, and the
        // chained `<weak-def-coalesce>` ordinal), so the declaration lands with
        // it. ✔MEASURED on Apple Silicon 2026-09-06, the discriminating run: a
        // DSS-built dylib under an Apple-built executable defining a rival weak
        // body returns the CONSUMER's answer (rc 2) where it returned its own
        // (rc 1) before, with the ld64-built control returning rc 2 in the same
        // session. ⚠ ONE configuration: that run's second, "release" arm shipped
        // the SAME BYTES (the witness source was too small for the release
        // pipeline to transform it), so it witnessed nothing extra and the
        // release half is carried at the BYTE level instead, on a subject whose
        // release artifact genuinely differs — see the note in
        // `link/test_macho_writer`.
        // ⚠ WEAK ONLY, and that is the measurement rather than caution: in the
        // same probe a dylib's call to its own STRONG GLOBAL and to a `static`
        // both stayed a DIRECT `bl` with an empty weak-bind stream, while ELF
        // routes the strong one through the PLT. The two rails genuinely
        // differ, which is why this is declared per format instead of being one
        // rule in the code — do not "complete" this list to match ELF's.
        {"macho64-arm64-darwin-dylib", {SymbolBinding::Weak},
         "dyld coalesces WEAK definitions only; strong and static stay direct"},
        {"macho64-x86_64-darwin-dylib", {SymbolBinding::Weak},
         "same rule, same loader — the port does not change what dyld coalesces"},
    };
    for (auto const& row : rows) {
        SCOPED_TRACE(std::string{row.format});
        auto schema = ObjectFormatSchema::loadShipped(row.format);
        ASSERT_TRUE(schema.has_value())
            << "the shipped document must load: " << row.format;
        auto const& listed = (*schema)->preemptibleDefinitionBindings();
        ASSERT_EQ(listed.size(), row.expected.size())
            << "shipped set size for " << row.format << " — " << row.why;
        for (std::size_t i = 0; i < row.expected.size(); ++i) {
            EXPECT_EQ(listed[i], row.expected[i])
                << "entry " << i << " of " << row.format;
        }
    }
}
