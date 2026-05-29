#include "core/types/target_schema.hpp"

#include "core/types/config_path_walk.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <utility>

namespace dss {

LoadResult<std::shared_ptr<TargetSchema>> TargetSchema::loadFromFile(
    std::filesystem::path const& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected(std::vector<ConfigDiagnostic>{
            {DiagnosticCode::C_MissingField, DiagnosticSeverity::Error,
             path.string(), "cannot open file"}});
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return loadFromText(std::move(buf).str(), path.string());
}

LoadResult<std::shared_ptr<TargetSchema>> TargetSchema::loadShipped(
    std::string_view name) {
    auto path = findShippedConfig({name, "targets", ".target.json", "target",
                                   DiagnosticCode::C_InvalidLanguageName});
    if (!path) return std::unexpected(std::move(path).error());
    return loadFromFile(*path);
}

// `loadFromText` is implemented in target_schema_json.cpp (mirrors the
// GrammarSchema boundary — JSON dep stays off the public header).

// ── TargetSchemaData::validate() ───────────────────────────────────────────
//
// Cross-field invariants the JSON loader cannot express per-field. Returns
// the list of problem messages; an empty vector means well-formed. Caller
// (the loader) maps each message into a `C_MalformedJson` ConfigDiagnostic.
//
// Today's checks (cycle 2b):
//   - Opcode arity: minOperands <= maxOperands; minSuccessors <= maxSuccessors.
//   - Terminator-implies-successor: an opcode with isTerminator==true and
//     `maxSuccessors==0` is contradictory (a Return is the one exception;
//     callers note it by setting minSuccessors=0 AND we accept it).
//   - Register `subOf` resolution: any non-empty subOf must name another
//     register in the table.
//   - Calling-convention register references: every name in argGprs/argFprs/
//     returnGprs/returnFprs/callerSaved/calleeSaved must resolve to a
//     register entry.
//   - Calling-convention register-class agreement: argGprs entries must be
//     class==GPR (or class==None if registers section was omitted);
//     argFprs entries must be class==FPR; similarly for returns.
namespace detail {

std::vector<std::string> TargetSchemaData::validate() const {
    std::vector<std::string> problems;
    auto fail = [&](std::string s) { problems.push_back(std::move(s)); };

    // Opcode arity.
    for (std::size_t i = 0; i < opcodes.size(); ++i) {
        auto const& o = opcodes[i];
        if (o.minOperands > o.maxOperands) {
            fail(std::format(
                "opcode[{} '{}']: minOperands ({}) > maxOperands ({})",
                i, o.mnemonic, o.minOperands, o.maxOperands));
        }
        if (o.minSuccessors > o.maxSuccessors) {
            fail(std::format(
                "opcode[{} '{}']: minSuccessors ({}) > maxSuccessors ({})",
                i, o.mnemonic, o.minSuccessors, o.maxSuccessors));
        }
        // Terminators are required to have ≥1 successor unless minSuccessors
        // is explicitly 0 (Return / Unreachable / Ret-shaped opcodes). We
        // only flag the contradiction where both minSuccessors > 0 is hinted
        // AND maxSuccessors == 0.
        if (o.isTerminator && o.minSuccessors > 0 && o.maxSuccessors == 0) {
            fail(std::format(
                "opcode[{} '{}']: terminator with minSuccessors>0 but maxSuccessors==0",
                i, o.mnemonic));
        }
    }

    // Register subOf resolution.
    auto haveRegister = [&](std::string_view nm) -> bool {
        return registerIndex.find(nm) != registerIndex.end();
    };
    for (std::size_t i = 0; i < registers.size(); ++i) {
        auto const& r = registers[i];
        if (!r.subOf.empty() && !haveRegister(r.subOf)) {
            fail(std::format(
                "register[{} '{}']: subOf='{}' does not resolve to a known register",
                i, r.name, r.subOf));
        }
    }

    // Calling-convention register name + class resolution.
    auto checkRefs = [&](TargetCallingConvention const& cc,
                         char const* field,
                         std::span<std::string const> refs,
                         TargetRegClass expectedClass) {
        for (auto const& ref : refs) {
            auto it = registerIndex.find(ref);
            if (it == registerIndex.end()) {
                // Only flag as a problem if the registers section was
                // declared (cycle 2b non-empty). Cycle 2a-shape configs
                // (no registers) accept calling-convention references
                // as future-bound names.
                if (!registers.empty()) {
                    fail(std::format(
                        "callingConvention '{}'.{}: register '{}' is not in the register table",
                        cc.name, field, ref));
                }
                continue;
            }
            auto const& reg = registers[it->second];
            if (expectedClass != TargetRegClass::None
                && reg.regClass != TargetRegClass::None
                && reg.regClass != expectedClass) {
                fail(std::format(
                    "callingConvention '{}'.{}: register '{}' has class '{}', expected '{}'",
                    cc.name, field, ref,
                    targetRegClassName(reg.regClass),
                    targetRegClassName(expectedClass)));
            }
        }
    };
    for (auto const& cc : callingConventions) {
        checkRefs(cc, "argGprs",     cc.argGprs,     TargetRegClass::GPR);
        checkRefs(cc, "argFprs",     cc.argFprs,     TargetRegClass::FPR);
        checkRefs(cc, "returnGprs",  cc.returnGprs,  TargetRegClass::GPR);
        checkRefs(cc, "returnFprs",  cc.returnFprs,  TargetRegClass::FPR);
        checkRefs(cc, "callerSaved", cc.callerSaved, TargetRegClass::None);
        checkRefs(cc, "calleeSaved", cc.calleeSaved, TargetRegClass::None);
    }

    return problems;
}

} // namespace detail

} // namespace dss
