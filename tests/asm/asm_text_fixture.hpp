#pragma once

// Test-tier fixture for the standalone assembly TEXT → LIR walker
// (`src/asm/asm_text_to_lir.cpp`).
//
// ★ WHY IT MUTATES THE SHIPPED DIALECT RATHER THAN AUTHORING A PARALLEL ONE.
// Same reasoning as `tests/test_support/mutate_target_schema.hpp`: a
// hand-written "test dialect" would rot against the real one and would let the
// tests pass while the shipped document was broken. Reading the shipped
// `asm-x86_64-att.lang.json` and applying TARGETED JSON edits keeps every test
// anchored to the real grammar, the real token table and the real
// `languageReferences` merge, while still letting one test declare the row it
// needs (a duplicate operand-role binding, a `functionEntry` directive, a
// branch/call spelling the shipped vocabulary has not grown yet).
//
// ⚠ THE `.type name, @function` MARKER IS SPELLED WITHOUT THE `@` HERE, ON
// PURPOSE. The shipped dialect deliberately leaves `@` UNDECLARED in its token
// table (its own comment says so), so a `@function` operand does not lex yet.
// That is the dialect document's business; these tests are about the ENGINE's
// marker mechanism, so they declare a marker that the existing token table can
// already produce. The engine compares operand TEXT against the row's declared
// marker and has no opinion about which characters are in it.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "asm/asm_text_to_lir.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/config_path_walk.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/target_schema.hpp"

#include <nlohmann/json.hpp>

#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dss::test_support::asm_text {

// The shipped dialect document, parsed. Throws (never `abort()`) on a
// misconfigured test environment — an abort would kill every sibling test's
// verdict along with this one.
[[nodiscard]] inline nlohmann::json shippedDialectDoc(
    std::string_view name = "asm-x86_64-att") {
    auto pathR = findShippedConfig(
        ShippedConfigLocator{name, "sources", ".lang.json", "language",
                             DiagnosticCode::C_InvalidTargetName});
    if (!pathR.has_value()) {
        throw std::runtime_error{
            std::string{"cannot locate shipped dialect "} + std::string{name}};
    }
    std::ifstream in{*pathR};
    if (!in) {
        throw std::runtime_error{"cannot open shipped dialect document"};
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return nlohmann::json::parse(buf.str());
}

// Replace the dialect's `assembly.instructions` with exactly the rows a test
// needs. Each row is `{ spelling, opcodes[], width, cond? }`.
struct InstRow {
    std::string              spelling;
    std::vector<std::string> opcodes;
    // ⚠ 0 MEANS "OMIT THE KEY", which is a DIFFERENT declaration from any
    // width: it tells the lowering to read the width off the register
    // operands, which is what a dialect encoding width in the REGISTER
    // (aarch64 `add x0..` vs `add w0..`) declares.
    unsigned                 width = 64;
    std::string              cond;   // empty = absent
};

inline void setInstructions(nlohmann::json& doc,
                            std::vector<InstRow> const& rows) {
    nlohmann::json arr = nlohmann::json::array();
    for (auto const& r : rows) {
        nlohmann::json row;
        row["spelling"] = r.spelling;
        row["opcodes"]  = r.opcodes;
        if (r.width != 0) row["width"] = r.width;
        if (!r.cond.empty()) row["cond"] = r.cond;
        arr.push_back(std::move(row));
    }
    doc["assembly"]["instructions"] = std::move(arr);
}

struct DirRow {
    std::string spelling;
    std::string verb;
    std::string marker;   // empty = absent
    // ⚠ EACH OPTIONAL KEY HAS AN "OMIT" SENTINEL RATHER THAN A DEFAULT VALUE,
    // because the loader treats present-and-wrong differently from absent on
    // every one of them: `section` is REQUIRED on `sectionData` and REFUSED
    // elsewhere, `unitBytes` likewise on `emitData`, and `operandOnly` is
    // refused on any verb that does not open a section. A fixture that always
    // emitted the keys could not express the rows those refusals are about.
    std::string section;              // empty = omit the key
    unsigned    unitBytes   = 0;      // 0     = omit the key
    bool        operandOnly = false;  // false = omit the key
    // D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED: `rule` is REQUIRED on `frameRule`
    // and REFUSED elsewhere; `offsetFromCfa` is legal only on a `frameRule`
    // stating a per-register SAVE. Both carry an omit sentinel for the same
    // reason every key above does — the loader treats present-and-wrong
    // differently from absent, so a fixture that always emitted them could not
    // express the rows those refusals are about.
    std::string rule;                 // empty = omit the key
    bool        offsetFromCfa = false;// false = omit the key
};

inline void setDirectives(nlohmann::json& doc,
                          std::vector<DirRow> const& rows) {
    nlohmann::json arr = nlohmann::json::array();
    for (auto const& r : rows) {
        nlohmann::json row;
        row["spelling"] = r.spelling;
        row["verb"]     = r.verb;
        if (!r.marker.empty())  row["marker"]      = r.marker;
        if (!r.section.empty()) row["section"]     = r.section;
        if (r.unitBytes != 0)   row["unitBytes"]   = r.unitBytes;
        if (r.operandOnly)      row["operandOnly"] = true;
        if (!r.rule.empty())    row["rule"]        = r.rule;
        if (r.offsetFromCfa)    row["offsetFromCfa"] = true;
        arr.push_back(std::move(row));
    }
    doc["assembly"]["directives"] = std::move(arr);
}

// The instruction vocabulary every structural test shares: enough of the x86_64
// opcode table to write a real function with blocks, a call and a store.
[[nodiscard]] inline std::vector<InstRow> baseInstructions() {
    return {
        // ★ THE ELECTION ROW. One spelling, three candidate target opcodes —
        // `mov` for reg/imm, `load` for a memory SOURCE, `store` for a memory
        // DESTINATION. The dialect does not say which; the target's own guards
        // do.
        {"movq",  {"mov", "load", "store"}, 64, ""},
        {"movl",  {"mov", "load", "store"}, 32, ""},
        {"leaq",  {"lea"},                  64, ""},
        {"leal",  {"lea"},                  32, ""},
        {"addq",  {"add"},                  64, ""},
        {"cmpq",  {"cmp"},                  64, ""},
        // No `width` key: the REGISTERS say. This is the aarch64 model, and
        // one row of it is carried here so the derivation is exercised
        // against a real x86_64 target too.
        {"movx",  {"mov", "load", "store"},  0, ""},
        {"jmp",   {"jmp"},                  64, ""},
        {"je",    {"jcc"},                  64, "eq"},
        {"jne",   {"jcc"},                  64, "ne"},
        {"call",  {"call"},                 64, ""},
        {"ret",   {"ret"},                  64, ""},
    };
}

[[nodiscard]] inline std::vector<DirRow> baseDirectives() {
    return {
        {"text",   "sectionText",       ""},
        {"globl",  "globalSymbol",      ""},
        {"size",   "ignoredAnnotation", ""},
        // Marker-less form: naming the symbol is enough.
        {"func",   "functionEntry",     ""},
        // Marker form: `.type main, function` marks; `.type buf, object`
        // does not.
        {"type",   "functionEntry",     "function"},
    };
}

[[nodiscard]] inline nlohmann::json baseDialectDoc() {
    auto doc = shippedDialectDoc();
    setInstructions(doc, baseInstructions());
    setDirectives(doc, baseDirectives());
    return doc;
}

// One lowering run: the grammar it used, the parsed unit, the reporter, and the
// module (nullopt on refusal).
struct LoweringRun {
    std::shared_ptr<GrammarSchema>  grammar;
    std::shared_ptr<TargetSchema>   target;
    std::unique_ptr<CompilationUnit> unit;
    DiagnosticReporter              reporter;
    std::optional<AsmTextModule>    module;
    std::vector<std::string>        loadErrors;   // non-empty ⇒ grammar refused
};

// Load `doc` as a grammar, parse `source` with it, and lower against
// `target` — an ALREADY-LOADED schema, so a caller can hand in a MUTATED one.
// ★ THIS IS THE RED-ON-DISABLE DOOR. `mutate_target_schema.hpp` builds a
// schema with one encoding variant or one relocation row removed; feeding it
// here is what turns "the lowering elected the 2-operand block-address form"
// from an observation into a claim, because the mutant must then FAIL LOUD
// rather than fall back to the 1-operand form.
// A grammar that fails to load leaves `loadErrors` non-empty and `module`
// unset — the shape the "two roles on one rule is a LOAD error" test asserts.
[[nodiscard]] inline std::unique_ptr<LoweringRun>
lowerAsmTextWithTarget(nlohmann::json const& doc, std::string_view source,
                       std::shared_ptr<TargetSchema> target) {
    auto run = std::make_unique<LoweringRun>();

    auto grammarR = GrammarSchema::loadFromText(doc.dump(), "<test-dialect>");
    if (!grammarR.has_value()) {
        for (auto const& e : grammarR.error()) {
            run->loadErrors.push_back(std::format("{}: {}", e.path, e.message));
        }
        return run;
    }
    run->grammar = *grammarR;
    run->target  = std::move(target);

    UnitBuilder builder{run->grammar, DiagnosticBudget::libraryDefault()};
    builder.addInMemory(std::string{source}, "<mem>.s");
    run->unit =
        std::make_unique<CompilationUnit>(std::move(builder).finish());
    if (run->unit->trees().empty()) {
        throw std::runtime_error{"assembly source produced no tree"};
    }

    run->module = lowerAsmTextToLir(
        run->unit->trees()[0], *run->grammar, *run->target,
        run->grammar->assembly().entryLabels, run->reporter);
    return run;
}

// The shipped-target form — every structural test's entry point.
[[nodiscard]] inline std::unique_ptr<LoweringRun>
lowerAsmText(nlohmann::json const& doc, std::string_view source,
             std::string_view targetName = "x86_64") {
    auto targetR = TargetSchema::loadShipped(targetName);
    if (!targetR.has_value()) {
        throw std::runtime_error{"cannot load shipped target schema"};
    }
    return lowerAsmTextWithTarget(doc, source, *targetR);
}

// Did the grammar load AND the parse produce zero errors? A test asserting a
// LOWERING refusal must first know the input parsed — otherwise it could be
// green for the wrong reason.
[[nodiscard]] inline bool parsedCleanly(LoweringRun const& run) {
    if (!run.loadErrors.empty()) return false;
    if (run.unit == nullptr) return false;
    if (run.unit->trees().empty()) return false;
    for (auto const& d : run.unit->trees()[0].diagnostics().all()) {
        if (d.severity == DiagnosticSeverity::Error) return false;
    }
    return true;
}

// The parse diagnostics, joined — so a test that fails because its `.s` did
// not PARSE says so instead of reporting a confusing lowering verdict.
[[nodiscard]] inline std::string parseMessages(LoweringRun const& run) {
    std::string out;
    for (auto const& e : run.loadErrors) { out += e; out += '\n'; }
    if (run.unit == nullptr || run.unit->trees().empty()) return out;
    for (auto const& d : run.unit->trees()[0].diagnostics().all()) {
        out += std::format("[{}] {}\n", static_cast<int>(d.code), d.actual);
    }
    return out;
}

// Every diagnostic message the lowering emitted, joined — for the assertions
// that pin WHICH names a refusal mentions.
[[nodiscard]] inline std::string messages(LoweringRun const& run) {
    std::string out;
    for (auto const& d : run.reporter.all()) {
        out += d.actual;
        out += '\n';
    }
    return out;
}

} // namespace dss::test_support::asm_text
