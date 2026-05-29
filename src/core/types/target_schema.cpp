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
// Cross-field invariants the per-field JSON parse cannot express. See the
// header for the rule catalogue. Returns `ConfigDiagnostic`s with shaped
// JSON paths (`/opcodes/3/minOperands`, `/registers/4/subOf`, etc.) so the
// loader does not have to reshape a flat string into a diagnostic.

namespace detail {

namespace {

// Power-of-two check that also rejects zero (zero is NOT a power of two
// in the sense relevant here: alignment of zero would mean unaligned,
// which is meaningless for an ABI).
constexpr bool isPow2Nonzero(std::uint16_t v) noexcept {
    return v != 0 && (v & (v - 1)) == 0;
}

// Convenience: emit a Diagnostic with the given JSON path + message.
ConfigDiagnostic makeProblem(std::string path, std::string message) {
    return ConfigDiagnostic{
        DiagnosticCode::C_MalformedJson,
        DiagnosticSeverity::Error,
        std::move(path),
        std::move(message),
    };
}

}  // namespace

std::vector<ConfigDiagnostic> TargetSchemaData::validate() const {
    std::vector<ConfigDiagnostic> problems;
    auto fail = [&](std::string path, std::string msg) {
        problems.push_back(makeProblem(std::move(path), std::move(msg)));
    };

    // ── Opcode arity ──────────────────────────────────────────────
    for (std::size_t i = 0; i < opcodes.size(); ++i) {
        auto const& o = opcodes[i];
        if (o.minOperands > o.maxOperands) {
            fail(std::format("/opcodes/{}/minOperands", i),
                 std::format("opcode '{}': minOperands ({}) > maxOperands ({})",
                             o.mnemonic, o.minOperands, o.maxOperands));
        }
        if (o.minSuccessors > o.maxSuccessors) {
            fail(std::format("/opcodes/{}/minSuccessors", i),
                 std::format("opcode '{}': minSuccessors ({}) > maxSuccessors ({})",
                             o.mnemonic, o.minSuccessors, o.maxSuccessors));
        }
        // Terminator with min>0 + max==0 is self-contradictory. The
        // (min=0, max=0) shape stays legal — that's the Return/Unreachable
        // case.
        if (o.isTerminator && o.minSuccessors > 0 && o.maxSuccessors == 0) {
            fail(std::format("/opcodes/{}/maxSuccessors", i),
                 std::format("opcode '{}': terminator with minSuccessors>0 but maxSuccessors==0",
                             o.mnemonic));
        }
        // Non-terminator with declared successors is structurally
        // impossible — only terminators have CFG successors.
        if (!o.isTerminator && o.maxSuccessors > 0) {
            fail(std::format("/opcodes/{}/maxSuccessors", i),
                 std::format("opcode '{}': non-terminator with maxSuccessors>0 ({})",
                             o.mnemonic, o.maxSuccessors));
        }
    }

    // ── Register file ─────────────────────────────────────────────
    auto haveRegister = [&](std::string_view nm) -> bool {
        return registerIndex.find(nm) != registerIndex.end();
    };
    for (std::size_t i = 0; i < registers.size(); ++i) {
        auto const& r = registers[i];
        // Classed register must declare positive width — closes the
        // silent-zero failure mode where `widthBytes` is omitted in JSON
        // and ML6 spills 0 bytes.
        if (r.regClass != TargetRegClass::None && r.widthBytes == 0) {
            fail(std::format("/registers/{}/widthBytes", i),
                 std::format("register '{}': widthBytes must be > 0 when class is '{}'",
                             r.name, targetRegClassName(r.regClass)));
        }
        if (!r.subOf.empty() && !haveRegister(r.subOf)) {
            fail(std::format("/registers/{}/subOf", i),
                 std::format("register '{}': subOf='{}' does not resolve to a known register",
                             r.name, r.subOf));
        }
    }
    // subOf cycle detection. A misconfigured target with `eax.subOf=rax,
    // rax.subOf=eax` would loop ML6's clobber-set construction; trap at
    // load time instead. Standard DFS with white/gray/black marking;
    // each gray-revisit means we are on a cycle.
    if (!registers.empty()) {
        enum Mark : std::uint8_t { White = 0, Gray, Black };
        std::vector<Mark> marks(registers.size(), White);
        auto visit = [&](std::size_t start, auto&& self) -> bool {
            if (marks[start] == Black) return false;
            if (marks[start] == Gray)  return true;
            marks[start] = Gray;
            auto const& r = registers[start];
            if (!r.subOf.empty()) {
                auto it = registerIndex.find(r.subOf);
                if (it != registerIndex.end()) {
                    if (self(it->second, self)) return true;
                }
            }
            marks[start] = Black;
            return false;
        };
        for (std::size_t i = 0; i < registers.size(); ++i) {
            if (marks[i] != White) continue;
            if (visit(i, visit)) {
                fail(std::format("/registers/{}/subOf", i),
                     std::format("register '{}': subOf chain forms a cycle",
                                 registers[i].name));
                break;  // one cycle finding per validate() — caller fixes & retries
            }
        }
    }

    // ── Calling conventions ──────────────────────────────────────
    // Two gates here:
    //  1) For non-register-machine ABIs (WASM operand-stack, SPIR-V
    //     result-id), `registers`/`callingConventions` may legitimately
    //     be empty (those models don't have physical registers). Skip
    //     ref-resolution + alignment checks entirely.
    //  2) For register-machine ABI, ref-resolution is gated on
    //     `registers.empty() && callingConventions.empty()` — both empty
    //     is cycle-2a-shape (back-compat); anything else means the
    //     user opted into resolution.
    if (abiModel != TargetAbiModel::RegisterMachine) {
        return problems;
    }
    bool const enforceRefs = !registers.empty() || !callingConventions.empty();
    auto checkRefs = [&](std::size_t       ccIdx,
                         char const*       field,
                         std::span<std::string const> refs,
                         TargetRegClass    expectedClass) {
        auto const& cc = callingConventions[ccIdx];
        for (std::size_t k = 0; k < refs.size(); ++k) {
            auto const& ref = refs[k];
            auto it = registerIndex.find(ref);
            if (it == registerIndex.end()) {
                if (enforceRefs) {
                    fail(std::format("/callingConventions/{}/{}/{}", ccIdx, field, k),
                         std::format("callingConvention '{}'.{}: register '{}' is not in the register table",
                                     cc.name, field, ref));
                }
                continue;
            }
            auto const& reg = registers[it->second];
            if (expectedClass != TargetRegClass::None
                && reg.regClass != TargetRegClass::None
                && reg.regClass != expectedClass) {
                fail(std::format("/callingConventions/{}/{}/{}", ccIdx, field, k),
                     std::format("callingConvention '{}'.{}: register '{}' has class '{}', expected '{}'",
                                 cc.name, field, ref,
                                 targetRegClassName(reg.regClass),
                                 targetRegClassName(expectedClass)));
            }
        }
    };
    for (std::size_t i = 0; i < callingConventions.size(); ++i) {
        auto const& cc = callingConventions[i];
        checkRefs(i, "argGprs",     cc.argGprs,     TargetRegClass::GPR);
        checkRefs(i, "argFprs",     cc.argFprs,     TargetRegClass::FPR);
        checkRefs(i, "returnGprs",  cc.returnGprs,  TargetRegClass::GPR);
        checkRefs(i, "returnFprs",  cc.returnFprs,  TargetRegClass::FPR);
        checkRefs(i, "callerSaved", cc.callerSaved, TargetRegClass::None);
        checkRefs(i, "calleeSaved", cc.calleeSaved, TargetRegClass::None);

        // Link register (AAPCS64-shape). When declared, must resolve to
        // a GPR-class register — ML7 will spill it in the prologue.
        if (cc.linkRegister.has_value()) {
            std::span<std::string const> linkRefs{&*cc.linkRegister, 1};
            checkRefs(i, "linkRegister", linkRefs, TargetRegClass::GPR);
        }

        // Stack alignment must be a power of two when ANY ABI field is
        // set (since the call frame is meaningless without it). Zero is
        // legal only when the calling convention is empty (cycle-2a
        // fixture); anything else with alignment==0 or non-power-of-two
        // is a real misconfiguration.
        bool const hasAbiInfo = !cc.argGprs.empty() || !cc.argFprs.empty()
                              || !cc.returnGprs.empty() || !cc.returnFprs.empty()
                              || !cc.callerSaved.empty() || !cc.calleeSaved.empty()
                              || cc.shadowSpaceBytes != 0 || cc.redZoneBytes != 0
                              || cc.stackAlignment   != 0;
        if (hasAbiInfo) {
            if (!isPow2Nonzero(cc.stackAlignment)) {
                fail(std::format("/callingConventions/{}/stackAlignment", i),
                     std::format("callingConvention '{}': stackAlignment ({}) must be a non-zero power of two",
                                 cc.name, cc.stackAlignment));
            } else {
                if (cc.shadowSpaceBytes != 0
                    && cc.shadowSpaceBytes % cc.stackAlignment != 0) {
                    fail(std::format("/callingConventions/{}/shadowSpaceBytes", i),
                         std::format("callingConvention '{}': shadowSpaceBytes ({}) must be a multiple of stackAlignment ({})",
                                     cc.name, cc.shadowSpaceBytes, cc.stackAlignment));
                }
                if (cc.redZoneBytes != 0
                    && cc.redZoneBytes % cc.stackAlignment != 0) {
                    fail(std::format("/callingConventions/{}/redZoneBytes", i),
                         std::format("callingConvention '{}': redZoneBytes ({}) must be a multiple of stackAlignment ({})",
                                     cc.name, cc.redZoneBytes, cc.stackAlignment));
                }
            }
        }
    }

    return problems;
}

} // namespace detail

} // namespace dss
