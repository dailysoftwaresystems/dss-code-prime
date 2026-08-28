// D-SEMANTIC-ASM-TEMPLATE-SIGILS-HARDCODED-BESIDE-A-CONFIG-OWNER — THE DETECTOR.
//
// ★★★ WHAT THIS FILE USED TO BE, AND WHY IT IS STRONGER NOW.
//
// From 2026-08-15 to 2026-08-17 the GNU inline-asm TEMPLATE SIGILS had TWO
// owners in this tree — hard-coded C++ literals in `scanInlineAsmTemplate` and
// per-dialect declarations in each `asm-template` lexer mode — and this file
// asserted, once per shipped dialect, that the two AGREED. It said so plainly:
// "a second-best ... it converts a silent divergence into a red test, it does
// not prevent the divergence", and it asked to be deleted when the row closed.
//
// ⚠ IT IS NOT DELETED, AND THAT IS DELIBERATE. The row is closed the way the
// operator ruled: the LANGUAGE declares the bytes
// (`semantics.inlineAsmTemplateLexemes` in `asm.lang.json`), the DIALECT
// declares the capability (`assembly.templateLexerMode` + `templateOperandRule`)
// and its own token KINDS (`bindTokens`), and the loader SYNTHESIZES the
// dialect's per-mode rows by joining them. The old assertion — "the dialect's
// bytes equal the C++ literals" — is now vacuous in both halves: there are no
// C++ literals, and there are no dialect-declared bytes. So the file is
// REPOINTED at the invariant that CAN still break:
//
//   1. NO shipped dialect declares a template lexeme of its own (R1's premise).
//   2. Every shipped dialect's LOADED template mode carries, per role, exactly
//      the LANGUAGE-declared bytes bound to the kind that dialect names.
//   3. ★★★ THE DERIVATION IS LIVE: mutate the LANGUAGE declaration and EVERY
//      dialect's mode MOVES WITH IT — the old bytes lose their meaning in that
//      mode and the new bytes gain it — and the SEMANTIC SCAN moves too.
//   4. Each of the three refusals (R1/R2/R3) actually fires, naming both sites.
//
// ★★ (3) IS STRICTLY STRONGER THAN WHAT IT REPLACES, and that is the reason to
// keep the file rather than drop it. The old pin proved two copies HAPPENED TO
// AGREE; this one proves there is only one copy, by moving it and watching
// everything downstream follow. A pin that can only be satisfied by a live
// derivation cannot be satisfied by a coincidence.
//
// ★ THE DIALECT SET IS DISCOVERED, NEVER LISTED — unchanged from the original
// file, and for the reason it gave: a hand-written list of two stems goes stale
// the day a third dialect ships, and a third dialect is exactly the event this
// whole arc exists to make possible. Every shipped `*.lang.json` that DECLARES
// `assembly.templateLexerMode` is a signatory, which is a declared property
// rather than a naming convention.
//
// ⚠ AND NO BYTE OF GNU SYNTAX IS WRITTEN ANYWHERE IN THIS FILE. Not `%`, not
// `%%`, not `[`. Every expected spelling is READ from the language document. A
// detector that hard-codes the bytes it is checking has re-created, in the test
// tier, the exact defect it is guarding — and would go green over a language
// declaration nobody looked at.

#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/grammar_schema.hpp"
#include "test_support/repo_root.hpp"
#include "test_support/scoped_env.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

using namespace dss;
using json = nlohmann::json;

namespace {

// ── the shared vocabulary ────────────────────────────────────────────────
//
// The `semantics` key that owns the bytes, and the ROLE names — which are also
// the `bindTokens` hole names, because that is the intersection seam the loader
// joins on. Both are spellings of a CONFIG vocabulary, not of any language's
// syntax, so naming them here is naming the mechanism rather than the bytes.
constexpr char const* kLexemesKey = "inlineAsmTemplateLexemes";
constexpr char const* kRoles[] = {
    "templatePlaceholder", "templateEscape", "templateLabelPlaceholder",
    "symbolicNameOpen",    "symbolicNameClose",
};
// The document that owns the role set. It is the `languageReferences` key every
// dialect and every host uses to reach the embeddable asm surface.
constexpr char const* kOwnerStem = "asm";

[[nodiscard]] std::filesystem::path sourcesDir() {
    return dss::test::configRoot() / "sources";
}

[[nodiscard]] json readDoc(std::filesystem::path const& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        ADD_FAILURE() << "cannot open config document: " << p.string();
        return json::object();
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    json doc;
    try {
        doc = json::parse(buf.str());
    } catch (std::exception const& ex) {
        ADD_FAILURE() << "config document did not parse: " << p.string()
                      << " — " << ex.what();
        return json::object();
    }
    return doc;
}

[[nodiscard]] std::string readBytes(std::filesystem::path const& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    return std::move(buf).str();
}

// The LANGUAGE's declared lexeme for one role, or "" when the role is declared
// absent (explicit null) — read from the owner document, never assumed.
[[nodiscard]] std::string declaredLexeme(json const& owner,
                                         std::string_view role) {
    if (!owner.contains("semantics")) return {};
    json const& sem = owner.at("semantics");
    if (!sem.contains(kLexemesKey)) return {};
    json const& roles = sem.at(kLexemesKey);
    std::string const key{role};
    if (!roles.contains(key) || roles.at(key).is_null()) return {};
    json const& entry = roles.at(key);
    if (!entry.is_object() || !entry.contains("lexeme")) return {};
    return entry.at("lexeme").get<std::string>();
}

struct Signatory {
    std::string           stem;             // `loadShipped` stem
    std::filesystem::path path;
    json                  doc;
    std::string           templateModeName;
};

// Every shipped `.lang.json` that declares the template CAPABILITY.
[[nodiscard]] std::vector<Signatory> discoverDialects() {
    std::vector<Signatory> out;
    std::filesystem::path const dir = sourcesDir();
    EXPECT_TRUE(std::filesystem::exists(dir))
        << "shipped sources dir is missing: " << dir.string();
    for (auto const& e : std::filesystem::directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        std::string const name = e.path().filename().string();
        if (name.size() < 11 || name.substr(name.size() - 10) != ".lang.json") {
            continue;
        }
        json doc = readDoc(e.path());
        if (!doc.contains("assembly")) continue;
        json const& as = doc.at("assembly");
        if (!as.contains("templateLexerMode")) continue;
        Signatory s;
        s.stem = name.substr(0, name.size() - 10);
        s.path = e.path();
        s.templateModeName = as.at("templateLexerMode").get<std::string>();
        s.doc  = std::move(doc);
        out.push_back(std::move(s));
    }
    return out;
}

// The token KIND this dialect binds to a shared role, or "" when it binds none.
// Every `languageReferences` entry is searched rather than one being assumed —
// the entry's KEY is the importing document's choice.
[[nodiscard]] std::string boundKindForRole(json const& doc,
                                           std::string_view role) {
    if (!doc.contains("languageReferences")) return {};
    for (auto const& [_, ref] : doc.at("languageReferences").items()) {
        if (!ref.is_object() || !ref.contains("bindTokens")) continue;
        json const& bt = ref.at("bindTokens");
        std::string const key{role};
        if (!bt.contains(key)) continue;
        return bt.at(key).get<std::string>();
    }
    return {};
}

// ── the LANGUAGE-declaration mutant ──────────────────────────────────────
//
// ★★★ THE ONLY WAY TO MUTATE THE LANGUAGE DECLARATION IS ON DISK, and that is a
// feature of the experiment rather than a cost. The role set lives in a
// REFERENCED document that `mergeLanguageReferences` resolves through
// `findShippedConfig`, so a dialect string handed to `loadFromText` cannot carry
// it — and a dialect that declared the role set ITSELF is refused by the merge
// (a key declared by both host and reference is a `C_ConflictingField`), which
// is the second-owner refusal working as intended.
//
// So this copies the whole shipped `src/dss-config` to a scratch root, rewrites
// ONE document there, and points `DSS_CONFIG_ROOT` at it. Both halves of the
// program then read the SAME tree: `findShippedConfig` is env-first and does no
// caching (`config_path_walk.cpp`), and so is `dss::test::configRoot()`.
//
// ★★ FAIL-CLOSED, BY THE FIVE CLAUSES THIS PROJECT LEARNED THE HARD WAY.
//   1. the mutation is asserted to have CHANGED the document (byte compare of
//      the file before and after — never a line count);
//   2. `changed()` is checked by every caller before it asserts anything;
//   3. the mutant is written as JSON that PARSED, so it cannot be a syntax
//      accident;
//   4. the mutant tree is the tree the process READS — asserted by
//      `dss::test::configRoot()` pointing inside the scratch root;
//   5. ★★★ AND THE RESTORE DIRECTION IS PROVEN TOO: the scratch root is torn
//      down in the destructor, and `TheShippedReadingIsRestoredAfterAMutant`
//      re-measures the shipped byte afterwards. A red-on-disable makes TWO
//      claims — mutant differs, subject unchanged — and this project has
//      measured a harness whose "good" column silently ran the mutant.
class OwnerDocMutant {
public:
    explicit OwnerDocMutant(std::function<void(json&)> const& mutate) {
        namespace fs = std::filesystem;
        static int counter = 0;
        std::error_code ec;
        root_ = fs::temp_directory_path()
              / ("dss-sigil-owner-mutant-" + std::to_string(++counter));
        fs::remove_all(root_, ec);
        fs::create_directories(root_ / "src", ec);
        fs::copy(dss::test::configRoot(), root_ / "src" / "dss-config",
                 fs::copy_options::recursive, ec);
        if (ec) {
            ADD_FAILURE() << "could not copy the shipped config tree: "
                          << ec.message();
            return;
        }
        target_ = root_ / "src" / "dss-config" / "sources"
                / (std::string{kOwnerStem} + ".lang.json");
        std::string const before = readBytes(target_);
        json doc = readDoc(target_);
        mutate(doc);
        {
            std::ofstream out(target_, std::ios::binary);
            out << doc.dump(2);
        }
        std::string const after = readBytes(target_);
        changed_ = (before != after) && !before.empty() && !after.empty();
        if (!changed_) {
            ADD_FAILURE()
                << "the mutation left " << target_.filename().string()
                << " BYTE-IDENTICAL — every assertion downstream of this mutant "
                   "would be measuring the shipped document and reporting it as "
                   "a differential";
        }
        env_ = std::make_unique<dss::test_support::ScopedEnv>(
            "DSS_CONFIG_ROOT", root_.string());
        // Clause 4: the process must now READ this tree. `configRoot()` shares
        // the compiler's env-first precedence, so proving it here proves it for
        // `findShippedConfig` too.
        auto const seen = dss::test::configRoot();
        if (seen != root_ / "src" / "dss-config") {
            ADD_FAILURE() << "the mutant tree is not the tree this process "
                             "reads — wanted " << (root_ / "src" / "dss-config")
                          << ", resolver says " << seen;
            changed_ = false;
        }
    }

    ~OwnerDocMutant() {
        namespace fs = std::filesystem;
        std::error_code ec;
        env_.reset();                       // restore BEFORE the tree vanishes
        fs::remove_all(root_, ec);
    }

    OwnerDocMutant(OwnerDocMutant const&)            = delete;
    OwnerDocMutant& operator=(OwnerDocMutant const&) = delete;

    [[nodiscard]] bool changed() const { return changed_; }

private:
    std::filesystem::path root_;
    std::filesystem::path target_;
    bool                  changed_ = false;
    std::unique_ptr<dss::test_support::ScopedEnv> env_;
};

// Rewrite every declared role's lexeme through `f`, so the whole family moves
// coherently. Roles declared null stay null.
void remapEveryLexeme(json& owner,
                      std::function<std::string(std::string const&)> const& f) {
    json& roles = owner.at("semantics").at(kLexemesKey);
    for (char const* role : kRoles) {
        if (!roles.contains(role) || roles.at(role).is_null()) continue;
        roles.at(role).at("lexeme") =
            f(roles.at(role).at("lexeme").get<std::string>());
    }
}

[[nodiscard]] std::string joined(std::vector<ConfigDiagnostic> const& v) {
    std::string out;
    for (auto const& d : v) { out += d.path; out += ": "; out += d.message;
                              out += '\n'; }
    return out;
}

// One dialect document loaded from TEXT, so a per-dialect mutation can be
// applied without touching the shipped tree. The referenced owner document
// still resolves from whichever tree `DSS_CONFIG_ROOT` names.
struct LoadedDialect {
    std::shared_ptr<GrammarSchema> grammar;
    std::vector<ConfigDiagnostic>  errors;
};

[[nodiscard]] LoadedDialect loadDialectText(json const& doc,
                                            std::string_view label) {
    LoadedDialect out;
    auto g = GrammarSchema::loadFromText(doc.dump(), label);
    if (g.has_value()) out.grammar = *g;
    else               out.errors  = g.error();
    return out;
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════
// 1. THE PREMISE: NO DIALECT OWNS A TEMPLATE LEXEME
// ═════════════════════════════════════════════════════════════════════════

// ★★★ THE ASSERTION THE ROW WAS OPENED FOR. Before P5c every dialect declared
// the sigils in its own template mode; the fix is not "they agree" but "there is
// only one of them". This reads the SHIPPED documents and asserts the dialect
// side is empty of them — in BOTH directions R1 refuses: no row may carry a
// role's LEXEME, and no row may carry the KIND that dialect binds to a role
// (spelling it differently is not a different fact).
TEST(InlineAsmTemplateSigilAgreement, NoShippedDialectDeclaresATemplateLexemeOfItsOwn) {
    json const owner = readDoc(sourcesDir()
                               / (std::string{kOwnerStem} + ".lang.json"));
    auto const dialects = discoverDialects();
    ASSERT_FALSE(dialects.empty())
        << "no shipped .lang.json declares assembly.templateLexerMode — either "
           "the template surface was removed or discovery is looking in the "
           "wrong place: " << sourcesDir().string();

    // Not vacuous: the owner must actually declare a role set, or "no dialect
    // re-declares it" is trivially true of an empty set.
    std::size_t declaredRoles = 0;
    for (char const* role : kRoles) {
        if (!declaredLexeme(owner, role).empty()) ++declaredRoles;
    }
    ASSERT_GT(declaredRoles, 0u)
        << kOwnerStem << ".lang.json declares no template lexeme roles at all — "
           "this whole file would then be asserting nothing";

    for (auto const& s : dialects) {
        SCOPED_TRACE("dialect: " + s.stem);
        json const& modes = s.doc.at("lexerModes");
        ASSERT_TRUE(modes.contains(s.templateModeName))
            << s.stem << " names template mode '" << s.templateModeName
            << "' which it does not declare";
        json const& mode = modes.at(s.templateModeName);
        if (!mode.contains("tokens")) continue;   // the shipped shape: empty
        ASSERT_TRUE(mode.at("tokens").is_object())
            << s.stem << ": the template mode's 'tokens' is not an object — a "
               "verbatim global copy is itself a second declaration of the "
               "sigils and R1 refuses it";
        for (char const* role : kRoles) {
            std::string const lexeme = declaredLexeme(owner, role);
            std::string const kind   = boundKindForRole(s.doc, role);
            if (!lexeme.empty()) {
                EXPECT_FALSE(mode.at("tokens").contains(lexeme))
                    << s.stem << " declares template role '" << role
                    << "' itself: mode '" << s.templateModeName
                    << "' has a row for the byte(s) " << kOwnerStem
                    << ".lang.json already owns. TWO OWNERS is the defect this "
                       "row closed — delete the row, keep the capability";
            }
            if (kind.empty()) continue;
            for (auto const& [lex, meanings] : mode.at("tokens").items()) {
                if (!meanings.is_array()) continue;
                for (auto const& m : meanings) {
                    if (!m.is_object() || !m.contains("kind")) continue;
                    EXPECT_NE(m.at("kind").get<std::string>(), kind)
                        << s.stem << " declares template role '" << role
                        << "' under a DIFFERENT spelling ('" << lex
                        << "'): the kind is the one it binds to that role, so "
                           "this is the same second owner wearing another byte";
                }
            }
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════
// 2. THE JOIN: THE LOADED MODE CARRIES THE LANGUAGE'S BYTES
// ═════════════════════════════════════════════════════════════════════════

// ★★ THE JSON READ ABOVE IS ONLY EVIDENCE IF IT IS THE JSON THAT LOADS — the
// original file's point, kept. This drives the REAL load path
// (`GrammarSchema::loadShipped`) and asks the REAL per-mode lookup
// (`lookupLexemeInMode`, the same call `longestMatchInMode` makes) whether the
// LANGUAGE's byte resolves to the KIND this dialect bound to that role.
TEST(InlineAsmTemplateSigilAgreement, EveryDialectsLoadedModeIsTheLanguagesBytesUnderItsOwnKinds) {
    json const owner = readDoc(sourcesDir()
                               / (std::string{kOwnerStem} + ".lang.json"));
    auto const dialects = discoverDialects();
    ASSERT_FALSE(dialects.empty());

    for (auto const& s : dialects) {
        SCOPED_TRACE("dialect: " + s.stem);
        auto loaded = GrammarSchema::loadShipped(s.stem);
        ASSERT_TRUE(loaded) << s.stem << " failed to load through loadShipped: "
                            << joined(loaded.error());
        GrammarSchema const& g = **loaded;

        LexerModeId const mode = g.assembly().templateLexerMode;
        ASSERT_TRUE(mode.valid())
            << s.stem << ": the loaded schema carries no templateLexerMode "
               "although the JSON declares '" << s.templateModeName << "'";

        std::size_t checked = 0;
        for (char const* role : kRoles) {
            std::string const lexeme = declaredLexeme(owner, role);
            if (lexeme.empty()) continue;      // declared absent — nothing owed
            std::string const kind = boundKindForRole(s.doc, role);
            ASSERT_FALSE(kind.empty())
                << s.stem << " binds no token for template role '" << role
                << "', which " << kOwnerStem << ".lang.json declares — the "
                   "loader refuses that pairing, so reaching here means the "
                   "refusal is gone";
            auto const meanings = g.lookupLexemeInMode(mode, lexeme);
            ASSERT_EQ(meanings.size(), 1u)
                << s.stem << ": the loaded template mode gives the language's "
                   "byte(s) for role '" << role << "' " << meanings.size()
                << " meaning(s) — the synthesis writes exactly one row per role";
            EXPECT_EQ(g.schemaTokens().name(meanings[0].id), kind)
                << s.stem << ": role '" << role
                << "' resolved to the wrong kind — the synthesis joined the "
                   "language's bytes to a token this dialect did not bind to "
                   "that role";
            ++checked;
        }
        EXPECT_GT(checked, 0u)
            << s.stem << ": not one role was checked — the loop found no "
               "declared lexeme and this arm asserted nothing";
    }
}

// ═════════════════════════════════════════════════════════════════════════
// 3. THE RED-ON-DISABLE: MOVE THE LANGUAGE, EVERY DIALECT MOVES
// ═════════════════════════════════════════════════════════════════════════

// ★★★ THE PIN THAT REPLACES THE OLD AGREEMENT CHECK, AND THE REASON THIS FILE
// SURVIVED THE ROW IT WAS WRITTEN FOR. Rewrite every role's lexeme in the
// LANGUAGE document — a coherent remap, so the loader's maximal-munch invariants
// still hold — and require that in EVERY shipped dialect:
//   • the NEW bytes resolve, in that dialect's own template mode, to that
//     dialect's own kind for the role; and
//   • the OLD bytes resolve to NOTHING in that mode.
// The second half is what makes it a differential rather than an addition: a
// dialect that still carried its own copy would keep answering for the old
// bytes, and this arm would catch it.
TEST(InlineAsmTemplateSigilAgreement, MutatingTheLanguageDeclarationMovesEveryDialectsMode) {
    json const shippedOwner = readDoc(sourcesDir()
                                      / (std::string{kOwnerStem} + ".lang.json"));
    auto const dialects = discoverDialects();
    ASSERT_FALSE(dialects.empty());

    // ★★ THE REMAP IS A PREFIX, NOT A SUBSTITUTION, AND THAT IS A CORRECTION
    // MADE BY RUNNING THE ARM. The first version replaced every BYTE of every
    // lexeme with one marker, which collapsed five distinct spellings onto three
    // — and the loader correctly REFUSED the mutant document ("roles
    // 'templateEscape' and 'templateLabelPlaceholder' are BOTH declared with the
    // lexeme ..."). A refused load is not a moved byte, so that mutant proved
    // nothing about the derivation. Prefixing preserves DISTINCTNESS and the
    // LENGTH ORDER the loader requires (escape and label strictly longer than
    // the placeholder), so the only thing that changes is the bytes.
    // ⓘ Recorded rather than quietly fixed: the refusal that caught it is one of
    // this change's own new guards, firing on its own author.
    constexpr char kMarker = '\x7e';        // '~'
    auto const remap = [](std::string const& s) {
        return std::string(1, kMarker) + s;
    };
    for (char const* role : kRoles) {
        std::string const lex = declaredLexeme(shippedOwner, role);
        if (lex.empty()) continue;
        ASSERT_EQ(lex.find(kMarker), std::string::npos)
            << "role '" << role << "' already contains the mutation byte — the "
               "differential below could not distinguish the two readings";
        ASSERT_NE(remap(lex), lex)
            << "the remap is the identity on role '" << role
            << "' — this mutant would assert nothing";
    }

    // ── the GOOD column, measured BEFORE the mutant exists ──
    struct Baseline { std::string stem; std::vector<std::pair<std::string,
                                                              std::string>> rows; };
    std::vector<Baseline> baseline;
    for (auto const& s : dialects) {
        auto loaded = GrammarSchema::loadShipped(s.stem);
        ASSERT_TRUE(loaded) << s.stem << ": " << joined(loaded.error());
        GrammarSchema const& g = **loaded;
        LexerModeId const mode = g.assembly().templateLexerMode;
        ASSERT_TRUE(mode.valid()) << s.stem;
        Baseline b{s.stem, {}};
        for (char const* role : kRoles) {
            std::string const lex = declaredLexeme(shippedOwner, role);
            if (lex.empty()) continue;
            auto const meanings = g.lookupLexemeInMode(mode, lex);
            ASSERT_EQ(meanings.size(), 1u)
                << s.stem << ": role '" << role << "' has no shipped row — the "
                   "differential has no 'before' to move away from";
            b.rows.emplace_back(lex, std::string{g.schemaTokens().name(meanings[0].id)});
        }
        ASSERT_FALSE(b.rows.empty()) << s.stem;
        baseline.push_back(std::move(b));
    }

    // ── the MUTANT column ──
    {
        OwnerDocMutant mutant([&](json& owner) { remapEveryLexeme(owner, remap); });
        ASSERT_TRUE(mutant.changed());

        for (auto const& b : baseline) {
            SCOPED_TRACE("dialect: " + b.stem);
            auto loaded = GrammarSchema::loadShipped(b.stem);
            ASSERT_TRUE(loaded)
                << b.stem << " stopped LOADING under the mutant — then the "
                   "differential below would be about a broken document rather "
                   "than about a moved byte: " << joined(loaded.error());
            GrammarSchema const& g = **loaded;
            LexerModeId const mode = g.assembly().templateLexerMode;
            ASSERT_TRUE(mode.valid()) << b.stem;

            for (auto const& [oldLexeme, kind] : b.rows) {
                std::string const newLexeme = remap(oldLexeme);
                auto const moved = g.lookupLexemeInMode(mode, newLexeme);
                ASSERT_EQ(moved.size(), 1u)
                    << b.stem << ": the mutated LANGUAGE byte(s) got no meaning "
                       "in this dialect's template mode — the dialect's rows are "
                       "NOT derived from the language declaration";
                EXPECT_EQ(g.schemaTokens().name(moved[0].id), kind)
                    << b.stem << ": the moved row carries the wrong kind — the "
                       "join lost this dialect's own binding";
                EXPECT_TRUE(g.lookupLexemeInMode(mode, oldLexeme).empty())
                    << b.stem << ": the SHIPPED byte(s) still have a meaning in "
                       "the template mode after the language moved them — a "
                       "second owner is answering for them, which is exactly the "
                       "defect D-SEMANTIC-ASM-TEMPLATE-SIGILS-HARDCODED-BESIDE-"
                       "A-CONFIG-OWNER names";
            }
        }
    }
}

// ★★★ THE MIRROR OF THE FIFTH CLAUSE: PROVE THE RESTORED BYTES REACH THE
// PROCESS TOO. A red-on-disable makes TWO claims, and this project has measured
// a harness whose "unmutated" column silently ran the mutant and whose two
// columns therefore agreed perfectly. Here the restore is the scratch root going
// away — so it is asserted, after the mutant's scope, that the shipped reading
// is back.
TEST(InlineAsmTemplateSigilAgreement, TheShippedReadingIsRestoredAfterAMutant) {
    json const owner = readDoc(sourcesDir()
                               / (std::string{kOwnerStem} + ".lang.json"));
    auto const dialects = discoverDialects();
    ASSERT_FALSE(dialects.empty());
    std::string const probeRole = kRoles[0];
    std::string const shipped   = declaredLexeme(owner, probeRole);
    ASSERT_FALSE(shipped.empty());

    {
        // The same coherent PREFIX remap the arm above uses — a lengthened
        // placeholder alone would violate the loader's escape-must-be-longer
        // invariant and the mutant would be refused rather than read.
        OwnerDocMutant mutant([](json& o) {
            remapEveryLexeme(o, [](std::string const& s) {
                return std::string(1, '\x7e') + s;
            });
        });
        ASSERT_TRUE(mutant.changed());
        // ⚠ EVERY dialect, never `front()`. The set is DISCOVERED, so sampling
        // one member hands the verdict to `directory_iterator` order — which is
        // sorted on NTFS and hash-ordered on ext4, and which produced a green
        // Windows / red Linux split on exactly this file.
        for (auto const& s : dialects) {
            SCOPED_TRACE("dialect: " + s.stem);
            auto loaded = GrammarSchema::loadShipped(s.stem);
            ASSERT_TRUE(loaded) << joined(loaded.error());
            // Under the mutant the shipped byte is gone from the mode.
            EXPECT_TRUE((*loaded)
                            ->lookupLexemeInMode(
                                (*loaded)->assembly().templateLexerMode, shipped)
                            .empty())
                << "the mutant did not take effect, so the restore claim below "
                   "would be measuring one state twice";
        }
    }

    for (auto const& s : dialects) {
        SCOPED_TRACE("dialect: " + s.stem);
        auto loaded = GrammarSchema::loadShipped(s.stem);
        ASSERT_TRUE(loaded) << joined(loaded.error());
        EXPECT_EQ((*loaded)
                      ->lookupLexemeInMode(
                          (*loaded)->assembly().templateLexerMode, shipped)
                      .size(),
                  1u)
            << "the SHIPPED reading did not come back after the mutant was torn "
               "down — every 'good column' in this file would then be the "
               "mutant";
    }
}

// ★★★ AND THE C++ HALF MOVES TOO. The scan in `scanInlineAsmTemplate` is the
// owner that USED to hard-code these bytes; this arm proves it now reads them.
// Under the mutated language a reference written with the NEW sigil is out of
// range (so the scan recognized it as a reference) while the SHIPPED sigil is
// ordinary text (so the scan no longer knows it). Unmutated, the two verdicts
// are exactly reversed — one variable, two opposite readings.
TEST(InlineAsmTemplateSigilAgreement, TheSemanticScanReadsTheDeclaredSigilTooNotAHardCodedOne) {
    json const owner = readDoc(sourcesDir()
                               / (std::string{kOwnerStem} + ".lang.json"));
    std::string const shipped = declaredLexeme(owner, "templatePlaceholder");
    std::string const escape  = declaredLexeme(owner, "templateEscape");
    ASSERT_FALSE(shipped.empty());
    // ⚠ THE MOVED SIGIL MUST SHARE NO BYTE WITH THE SHIPPED ONE, and the first
    // version of this arm got that wrong in a way only running it exposed: a
    // repeat of the shipped byte (`%%%`) CONTAINS the shipped escape and
    // placeholder, so the SHIPPED scan read `%%` + `%9` and reported the very
    // out-of-range it was supposed to be blind to. A marker byte declared by
    // neither dialect is the only spelling that makes the two readings disjoint.
    std::string const moved = "\x7e";
    ASSERT_EQ(shipped.find(moved), std::string::npos);
    ASSERT_EQ(escape.find(moved),  std::string::npos);

    // ONE output, so index 0 is valid and index 9 never is. The refusal
    // S_InlineAsmPlaceholderOutOfRange fires ONLY when the scan recognized a
    // placeholder — which makes its presence the recognition witness.
    auto const probe = [](std::string const& sigil) {
        return "int main(void){ unsigned o = 0; __asm__(\"nop " + sigil
             + "9\" : \"=r\"(o)); return (int)o; }\n";
    };
    auto const outOfRange = [](std::string src) {
        auto cu = dss::sem_test::buildShippedUnit("c", {std::move(src)});
        auto m  = analyze(cu, DiagnosticBudget::libraryDefault());
        for (auto const& d : m.diagnostics().all()) {
            if (d.code == DiagnosticCode::S_InlineAsmPlaceholderOutOfRange) {
                return true;
            }
        }
        return false;
    };

    // Shipped tree: the shipped sigil is recognized, the moved one is not.
    EXPECT_TRUE(outOfRange(probe(shipped)))
        << "the scan did not recognize the language's OWN declared placeholder";
    EXPECT_FALSE(outOfRange(probe(moved)))
        << "the scan recognized a sigil no language declares";

    {
        OwnerDocMutant mutant([&](json& o) {
            o.at("semantics").at(kLexemesKey).at("templatePlaceholder")
                .at("lexeme") = moved;
            // Keep the escape strictly longer than the placeholder — the loader
            // refuses the pairing that maximal munch cannot separate, and a
            // refused load would make this arm green for the wrong reason.
            o.at("semantics").at(kLexemesKey).at("templateEscape")
                .at("lexeme") = moved + moved;
        });
        ASSERT_TRUE(mutant.changed());
        EXPECT_TRUE(outOfRange(probe(moved)))
            << "the scan did NOT follow the language declaration — it is still "
               "reading a sigil of its own, which is the C++ half of "
               "D-SEMANTIC-ASM-TEMPLATE-SIGILS-HARDCODED-BESIDE-A-CONFIG-OWNER";
        EXPECT_FALSE(outOfRange(probe(shipped)))
            << "the scan still recognizes the SHIPPED sigil after the language "
               "moved it — a hard-coded byte is answering";
    }
}

// ═════════════════════════════════════════════════════════════════════════
// 4. THE THREE REFUSALS, EACH SHOWN FIRING
// ═════════════════════════════════════════════════════════════════════════

// R1 — a dialect that declares a template lexeme independently. BOTH
// directions: the same BYTES, and the same role-bound KIND under other bytes.
TEST(InlineAsmTemplateSigilAgreement, R1ADialectRedeclaringATemplateLexemeIsRefusedNamingBothSites) {
    json const owner = readDoc(sourcesDir()
                               / (std::string{kOwnerStem} + ".lang.json"));
    auto const dialects = discoverDialects();
    ASSERT_FALSE(dialects.empty());
    std::string const lexeme = declaredLexeme(owner, "templatePlaceholder");
    ASSERT_FALSE(lexeme.empty());

    for (auto const& s : dialects) {
        SCOPED_TRACE("dialect: " + s.stem);
        std::string const kind = boundKindForRole(s.doc, "templatePlaceholder");
        ASSERT_FALSE(kind.empty());

        // (a) the SAME bytes.
        json sameBytes = s.doc;
        sameBytes["lexerModes"][s.templateModeName]["tokens"][lexeme] =
            json::parse("[{\"kind\": \"" + kind + "\"}]");
        auto const a = loadDialectText(sameBytes, s.stem);
        EXPECT_EQ(a.grammar, nullptr)
            << s.stem << ": a dialect re-declaring the placeholder LEXEME "
               "loaded clean — the second owner is permitted again";
        std::string const whyA = joined(a.errors);
        EXPECT_NE(whyA.find(s.templateModeName), std::string::npos)
            << "the refusal must name the dialect's declaration site: " << whyA;
        EXPECT_NE(whyA.find(kLexemesKey), std::string::npos)
            << "the refusal must name the LANGUAGE's declaration site: " << whyA;

        // (b) a DIFFERENT spelling, same role-bound KIND. This is the arm that
        // makes the refusal about the FACT rather than about the bytes.
        json otherBytes = s.doc;
        otherBytes["lexerModes"][s.templateModeName]["tokens"]["\x7e\x7e\x7e"] =
            json::parse("[{\"kind\": \"" + kind + "\"}]");
        auto const b = loadDialectText(otherBytes, s.stem);
        EXPECT_EQ(b.grammar, nullptr)
            << s.stem << ": a dialect declaring the placeholder KIND under its "
               "own spelling loaded clean — 'spell it differently' is still a "
               "second owner";
        EXPECT_NE(joined(b.errors).find(kLexemesKey), std::string::npos)
            << joined(b.errors);
    }
}

// R2, direction 1 — the CAPABILITY with no VOCABULARY. The dialect keeps
// `templateLexerMode`; the language document loses the role set entirely.
TEST(InlineAsmTemplateSigilAgreement, R2TheCapabilityWithNoRoleSetIsRefusedAtLoad) {
    auto const dialects = discoverDialects();
    ASSERT_FALSE(dialects.empty());

    OwnerDocMutant mutant([](json& o) {
        ASSERT_EQ(o.at("semantics").erase(kLexemesKey), 1u)
            << "the owner document declares no role set — this arm would be "
               "green for the state it exists to forbid";
    });
    ASSERT_TRUE(mutant.changed());

    for (auto const& s : dialects) {          // EVERY dialect, never `front()`
        SCOPED_TRACE("dialect: " + s.stem);
        auto loaded = GrammarSchema::loadShipped(s.stem);
        EXPECT_FALSE(loaded.has_value())
            << s.stem << " loaded with the template CAPABILITY declared and NO "
                         "language role set — its whole template surface would "
                         "be silently dead";
        if (loaded.has_value()) continue;
        std::string const why = joined(loaded.error());
        EXPECT_NE(why.find("templateLexerMode"), std::string::npos)
            << "the refusal must name the capability that has nothing to lex: "
            << why;
        EXPECT_NE(why.find(kLexemesKey), std::string::npos)
            << "the refusal must name the vocabulary that is missing: " << why;
    }
}

// R2, direction 2 — the ROLE with no KIND. The language declares the role; the
// dialect binds no token for it, so no row can be synthesized.
TEST(InlineAsmTemplateSigilAgreement, R2ADeclaredRoleWithNoTokenBindingIsRefusedAtLoad) {
    auto const dialects = discoverDialects();
    ASSERT_FALSE(dialects.empty());

    for (auto const& s : dialects) {
        SCOPED_TRACE("dialect: " + s.stem);
        json stripped = s.doc;
        std::size_t erased = 0;
        for (auto& [_, ref] : stripped.at("languageReferences").items()) {
            if (!ref.is_object() || !ref.contains("bindTokens")) continue;
            erased += ref.at("bindTokens").erase("templateEscape");
        }
        ASSERT_EQ(erased, 1u)
            << s.stem << " binds no 'templateEscape' to begin with — this arm "
               "would be asserting nothing";
        auto const r = loadDialectText(stripped, s.stem);
        EXPECT_EQ(r.grammar, nullptr)
            << s.stem << ": a declared template role with NO token binding "
               "loaded clean — that sigil would be unlexable in every template "
               "with a clean build log";
        std::string const why = joined(r.errors);
        EXPECT_NE(why.find("templateEscape"), std::string::npos)
            << "the refusal must name the unbound role: " << why;
    }
}

// ★★ R2, direction 2, ON THE SHAPE THAT ACTUALLY REACHES THE SYNTHESIS' OWN
// REFUSAL — and this arm exists because the one above does NOT reach it.
//
// ✔MEASURED while writing this file: when the role set arrives through a
// REFERENCE, an unbound role trips the merge's pre-existing Direction-2 check
// FIRST ("hole '…' … is UNBOUND"), which drops the whole reference — so the role
// set never merges and the synthesis reports the no-vocabulary case instead.
// Both refusals are correct and both name the role; but the per-role branch
// would have shipped UNEXERCISED, which is the unexercised-failure-arm trap this
// project has been burned by ("a suite printing failed=0 had been exiting 2").
//
// The reachable shape is a document that declares the role set ITSELF — a
// self-contained language with its own embedded-asm surface, which is legal and
// is exactly what a second host would look like. It is built here by MOVING the
// shipped role set from the referenced document into the dialect: no collision
// (the reference no longer declares it), no unspelled-hole binding (removed with
// it), and one role left unbound.
TEST(InlineAsmTemplateSigilAgreement, R2ASelfDeclaredRoleSetWithNoTokenBindingIsRefusedAtLoad) {
    auto const dialects = discoverDialects();
    ASSERT_FALSE(dialects.empty());
    json const owner = readDoc(sourcesDir()
                               / (std::string{kOwnerStem} + ".lang.json"));
    json const roleSet = owner.at("semantics").at(kLexemesKey);
    ASSERT_TRUE(roleSet.is_object());
    ASSERT_FALSE(roleSet.at("templateEscape").is_null())
        << "the shipped role set declares no escape — this arm has no role to "
           "leave unbound";

    // The reference loses the role set...
    OwnerDocMutant mutant([](json& o) {
        ASSERT_EQ(o.at("semantics").erase(kLexemesKey), 1u);
    });
    ASSERT_TRUE(mutant.changed());

    for (auto const& s : dialects) {
        SCOPED_TRACE("dialect: " + s.stem);
        json self = s.doc;
        // ...the dialect gains it, minus the binding for one role.
        self["semantics"][kLexemesKey] = roleSet;
        std::size_t erased = 0;
        for (auto& [_, ref] : self.at("languageReferences").items()) {
            if (!ref.is_object() || !ref.contains("bindTokens")) continue;
            erased += ref.at("bindTokens").erase("templateEscape");
        }
        ASSERT_EQ(erased, 1u) << s.stem;

        auto const r = loadDialectText(self, s.stem);
        EXPECT_EQ(r.grammar, nullptr)
            << s.stem << ": a SELF-DECLARED template role with no token binding "
               "loaded clean — that sigil would be unlexable in every template "
               "with a clean build log";
        std::string const why = joined(r.errors);
        EXPECT_NE(why.find("binds NO token kind for it"), std::string::npos)
            << "the SYNTHESIS' own per-role refusal must be the one firing here "
               "— if it is not, that branch is unexercised: " << why;
        EXPECT_NE(why.find("templateEscape"), std::string::npos)
            << "the refusal must name the role: " << why;
    }
}

// R3 — an unknown role key, named. A misspelled role would otherwise load clean
// and leave that sigil undeclared.
TEST(InlineAsmTemplateSigilAgreement, R3AnUnknownRoleKeyIsRefusedByName) {
    auto const dialects = discoverDialects();
    ASSERT_FALSE(dialects.empty());
    constexpr char const* kTypo = "templatePlacehodler";

    OwnerDocMutant mutant([](json& o) {
        o.at("semantics").at(kLexemesKey)[kTypo] =
            json::parse("{\"lexeme\": \"\x7e\"}");
    });
    ASSERT_TRUE(mutant.changed());

    for (auto const& s : dialects) {          // EVERY dialect, never `front()`
        SCOPED_TRACE("dialect: " + s.stem);
        auto loaded = GrammarSchema::loadShipped(s.stem);
        EXPECT_FALSE(loaded.has_value())
            << "an unknown template role key loaded clean — a typo would "
               "silently leave that sigil undeclared";
        if (loaded.has_value()) continue;
        EXPECT_NE(joined(loaded.error()).find(kTypo), std::string::npos)
            << "the refusal must NAME the unknown role: "
            << joined(loaded.error());
    }
}

// ★★ ALL-OR-NOTHING, WITH THE EXPLICIT NULL — the `operandForms` shape the
// operator's ruling named. An OMITTED role is a load error; an explicitly NULL
// one is legal and its row simply does not exist. The pair is what stops a
// partial block from silently meaning "this form is unrecognized".
// ★★★ THE ALL-OR-NOTHING PAIR — AND THE ARM THAT CAUGHT A REAL DEFECT BY
// DISAGREEING ACROSS PLATFORMS (2026-08-17).
//
// ⚠⚠ WHAT THIS TEST GOT WRONG THE FIRST TIME, RECORDED BECAUSE THE LESSON IS
// WORTH MORE THAN THE FIX. It asked `dialects.front()` — ONE dialect — and the
// two shipped dialects give OPPOSITE answers to `symbolicNameClose: null`:
//   • `asm-x86_64-att` binds that role to `PlaceholderNameClose`, a kind NO
//     global lexeme of that document declares (x86 AT&T gas has no bracket of
//     its own), so the synthesized template row was its ONLY declaration —
//     nulling the role un-minted the kind and the load FAILED;
//   • `asm-arm64-gas` binds it to `BracketClose`, which its GLOBAL table
//     declares for the memory form, so nulling the role changed NOTHING and the
//     template went on accepting the symbolic name through the mode's global
//     fallback.
// `directory_iterator` then decided the verdict: NTFS enumerates sorted (arm64
// first ⇒ the silent-accept path ⇒ GREEN) and ext4 returns hash order (att first
// ⇒ the loud path ⇒ RED). ⇒ **the platform split was the SYMPTOM; two meanings
// for one declaration was the defect**, and the passing platform was the one
// hiding an accept-and-do-nothing.
// ⇒ Two corrections, both here: the loader now REFUSES `null` + a binding (see
// the arm below), and every arm in this file iterates EVERY dialect. A pin that
// samples one member of a discovered set is a pin whose verdict is chosen by the
// filesystem.
//
// ★★ AND THE OPERATOR'S "EXPLICIT NULL PERMITTED" CLAUSE IS ASSERTED WHERE IT IS
// ACTUALLY MEANINGFUL — on a host that declares the role set and hosts no
// dialect. That is the language genuinely lacking a form: it SCANS templates and
// never lexes one, so the null costs it a recognized form and nothing else.
TEST(InlineAsmTemplateSigilAgreement, AnOmittedRoleIsRefusedAndAnExplicitNullIsHonoured) {
    auto const dialects = discoverDialects();
    ASSERT_FALSE(dialects.empty());
    constexpr char const* kDropped = "symbolicNameClose";
    // ⚠ CAPTURED BEFORE ANY MUTANT EXISTS. Inside a mutant scope every resolver
    // — `configRoot()` included — points at the scratch tree, so reading the
    // "shipped" byte there would read the mutation and the absence assertion
    // would compare the mutant against itself.
    std::string const shippedClose = declaredLexeme(
        readDoc(sourcesDir() / (std::string{kOwnerStem} + ".lang.json")),
        kDropped);
    ASSERT_FALSE(shippedClose.empty())
        << "the shipped owner declares no '" << kDropped
        << "' — no arm below would be about anything";

    // ── (1) OMITTED ⇒ refused, on EVERY dialect ──
    {
        OwnerDocMutant mutant([kDropped](json& o) {
            ASSERT_EQ(o.at("semantics").at(kLexemesKey).erase(kDropped), 1u);
        });
        ASSERT_TRUE(mutant.changed());
        for (auto const& s : dialects) {
            SCOPED_TRACE("dialect: " + s.stem);
            auto loaded = GrammarSchema::loadShipped(s.stem);
            EXPECT_FALSE(loaded.has_value())
                << "an OMITTED template role loaded clean — omission and 'this "
                   "language has no such form' would then be the same "
                   "declaration";
            if (!loaded.has_value()) {
                EXPECT_NE(joined(loaded.error()).find(kDropped),
                          std::string::npos)
                    << joined(loaded.error());
            }
        }
    }

    // ── (2) NULL + a binding ⇒ refused, on EVERY dialect, by the SAME
    //        diagnostic — the platform-independent replacement for the two
    //        outcomes described above ──
    {
        OwnerDocMutant mutant([kDropped](json& o) {
            o.at("semantics").at(kLexemesKey).at(kDropped) = nullptr;
        });
        ASSERT_TRUE(mutant.changed());
        for (auto const& s : dialects) {
            SCOPED_TRACE("dialect: " + s.stem);
            ASSERT_FALSE(boundKindForRole(s.doc, kDropped).empty())
                << s.stem << " binds no '" << kDropped
                << "' — this arm's premise does not hold for it";
            auto loaded = GrammarSchema::loadShipped(s.stem);
            EXPECT_FALSE(loaded.has_value())
                << s.stem << ": a role declared ABSENT while this dialect still "
                   "BINDS a token for it loaded clean. Whether that is harmless "
                   "depends on whether some unrelated lexeme happens to declare "
                   "the same kind — which is exactly the accident that made this "
                   "pin platform-dependent";
            if (!loaded.has_value()) {
                std::string const why = joined(loaded.error());
                EXPECT_NE(why.find(kDropped), std::string::npos)
                    << "the refusal must name the role: " << why;
                EXPECT_NE(why.find("ABSENT (null)"), std::string::npos)
                    << "the refusal must be the CONTRADICTION diagnostic, not an "
                       "unresolvable-reference cascade two tiers away — the "
                       "cascade is what differed between the two dialects: "
                    << why;
            }
        }
    }

    // ── (3) NULL is HONOURED where the language genuinely lacks the form: a
    //        host with the role set and NO dialect capability ──
    // ★ THE OBSERVABLE EFFECT IS THE POINT. "Honoured" cannot mean "loads"; it
    // has to mean the scan stopped recognizing the form. `%[name]` must go from
    // a recognized-but-unresolvable operand reference to ordinary template text,
    // while `%0` keeps being recognized — one variable, two readings.
    {
        constexpr char const* kHost = "c";
        auto const namedRefIsRecognized = [] {
            // ONE output, so `%[nope]` names no operand: the refusal fires ONLY
            // if the scan recognized the bracketed form at all.
            auto cu = dss::sem_test::buildShippedUnit(
                kHost,
                {"int main(void){ unsigned o = 0; "
                 "__asm__(\"nop %[nope]\" : [got] \"=r\"(o)); return (int)o; }\n"});
            auto m = analyze(cu, DiagnosticBudget::libraryDefault());
            for (auto const& d : m.diagnostics().all()) {
                if (d.code == DiagnosticCode::S_InlineAsmPlaceholderOutOfRange) {
                    return true;
                }
            }
            return false;
        };
        ASSERT_TRUE(namedRefIsRecognized())
            << "the shipped host does not recognize a symbolic operand "
               "reference at all — arm (3) would assert nothing";

        OwnerDocMutant mutant([kDropped](json& o) {
            o.at("semantics").at(kLexemesKey).at(kDropped) = nullptr;
        });
        ASSERT_TRUE(mutant.changed());
        auto loaded = GrammarSchema::loadShipped(kHost);
        ASSERT_TRUE(loaded.has_value())
            << "an EXPLICIT null role was refused for a host that declares NO "
               "template capability — a language that genuinely lacks a form "
               "must be able to say so, which is the operator's ruling verbatim: "
            << joined(loaded.error());
        EXPECT_FALSE(namedRefIsRecognized())
            << "the host loaded, but the scan still recognizes the symbolic "
               "operand form the language declared ABSENT — 'honoured' has to "
               "mean the form stopped being read, not merely that the document "
               "still loads";
    }
}
