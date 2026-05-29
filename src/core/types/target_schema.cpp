#include "core/types/target_schema.hpp"

#include "core/types/config_path_walk.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <unordered_map>
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

    // ── Encoding facet (per-opcode variants) ──────────────────────
    //
    // Substrate-tier rules the per-row JSON parse cannot express:
    //   * `shape != None` requires `variants` non-empty (otherwise
    //     the dispatch arm has nothing to match against → the walker
    //     would silently emit `A_NoMatchingEncodingVariant` for every
    //     instruction).
    //   * `shape == None` forbids non-empty `variants` (variants
    //     without a shape walker are dead data).
    //   * Each variant's `tmpl.opcodeBytes` MUST be non-empty (a
    //     variant with no opcode is a contradiction).
    //   * Each variant's `wires[k].index` must be `<
    //     operandKinds.size()` — the index targets a position in the
    //     variant's own guard tuple. (The LIR-side bound against the
    //     opcode's `maxOperands` is implicit: `operandsMatchGuard`
    //     at the walker entry requires `instOps.size() ==
    //     operandKinds.size()`.)
    //   * `modrmRegExt` (when set) must fit in 3 bits (0..7).
    //   * `tmpl.opcodeBytes.size() <= 15` — the architectural cap
    //     for x86 instructions (defense-in-depth against accidental
    //     huge byte lists in JSON).
    for (std::size_t i = 0; i < opcodes.size(); ++i) {
        auto const& o = opcodes[i];
        if (o.encoding.shape == TargetEncodingShape::None
            && !o.encoding.variants.empty()) {
            fail(std::format("/opcodes/{}/encoding/variants", i),
                 std::format("opcode '{}': `encoding.shape` is 'none' "
                             "but `variants` is non-empty (variant rows "
                             "without a walker are dead data)",
                             o.mnemonic));
        }
        if (o.encoding.shape != TargetEncodingShape::None
            && o.encoding.variants.empty()) {
            fail(std::format("/opcodes/{}/encoding/variants", i),
                 std::format("opcode '{}': `encoding.shape` is '{}' "
                             "but `variants` is empty (every "
                             "instruction would fire "
                             "A_NoMatchingEncodingVariant)",
                             o.mnemonic,
                             targetEncodingShapeName(o.encoding.shape)));
        }
        for (std::size_t vi = 0; vi < o.encoding.variants.size(); ++vi) {
            auto const& v = o.encoding.variants[vi];
            if (v.tmpl.opcodeBytes.empty()) {
                fail(std::format("/opcodes/{}/encoding/variants/{}/template/opcode", i, vi),
                     std::format("opcode '{}' variant {}: 'opcode' bytes "
                                 "must be non-empty",
                                 o.mnemonic, vi));
            }
            if (v.tmpl.opcodeBytes.size() > 15) {
                fail(std::format("/opcodes/{}/encoding/variants/{}/template/opcode", i, vi),
                     std::format("opcode '{}' variant {}: 'opcode' byte "
                                 "count {} exceeds 15 (x86 instruction "
                                 "length limit)",
                                 o.mnemonic, vi, v.tmpl.opcodeBytes.size()));
            }
            if (v.tmpl.modrmRegExt.has_value() && *v.tmpl.modrmRegExt > 7) {
                fail(std::format("/opcodes/{}/encoding/variants/{}/template/modrmRegExt", i, vi),
                     std::format("opcode '{}' variant {}: 'modrmRegExt' "
                                 "({}) must fit in 3 bits [0, 7]",
                                 o.mnemonic, vi, *v.tmpl.modrmRegExt));
            }
            for (std::size_t oi = 0; oi < v.wires.size(); ++oi) {
                if (v.wires[oi].index >= v.operandKinds.size()) {
                    fail(std::format("/opcodes/{}/encoding/variants/{}/wires/{}/index", i, vi, oi),
                         std::format("opcode '{}' variant {}: operand "
                                     "wire `index` {} is out of range "
                                     "[0, {})",
                                     o.mnemonic, vi,
                                     v.wires[oi].index,
                                     v.operandKinds.size()));
                }
            }

            // ── Convergence-fix B: `modrmRegExt` + ModRmReg wire ─────
            // The `/digit` extension fills the ModR/M.reg field; co-
            // declaring a wire targeting ModRmReg would silently
            // overwrite either the digit or the wired register at
            // encode time. Reject at load.
            if (v.tmpl.modrmRegExt.has_value()) {
                bool const resultIsModRmReg =
                    v.resultSlot == EncodingSlotKind::ModRmReg;
                bool anyWireIsModRmReg = false;
                for (auto const& w : v.wires) {
                    if (w.slotKind == EncodingSlotKind::ModRmReg) {
                        anyWireIsModRmReg = true;
                        break;
                    }
                }
                if (resultIsModRmReg || anyWireIsModRmReg) {
                    fail(std::format("/opcodes/{}/encoding/variants/{}/template/modrmRegExt", i, vi),
                         std::format("opcode '{}' variant {}: 'modrmRegExt' "
                                     "(/{} digit extension) conflicts with "
                                     "a wire targeting 'modrm.reg' — the "
                                     "digit IS the reg field, so the wired "
                                     "register would be silently dropped",
                                     o.mnemonic, vi, *v.tmpl.modrmRegExt));
                }
            }

            // ── Convergence-fix C: every guard position needs a wire ─
            // A variant whose `operandKinds` declares N positions but
            // whose `wires` covers only K<N would match an N-operand
            // LIR inst then silently drop the unwired operand.
            {
                std::vector<bool> covered(v.operandKinds.size(), false);
                for (auto const& w : v.wires) {
                    if (w.index < covered.size()) covered[w.index] = true;
                }
                for (std::size_t gi = 0; gi < covered.size(); ++gi) {
                    if (!covered[gi]) {
                        fail(std::format("/opcodes/{}/encoding/variants/{}/wires", i, vi),
                             std::format("opcode '{}' variant {}: guard "
                                         "position {} (operandKind '{}') "
                                         "has no matching wire (the "
                                         "operand would be silently "
                                         "dropped from the encoding)",
                                         o.mnemonic, vi, gi,
                                         operandKindFilterName(v.operandKinds[gi])));
                    }
                }
            }

            // ── Convergence-fix A (validate half): slot uniqueness ────
            // Two writers to the same ModR/M slot (ModRmReg or ModRmRm)
            // would silently overwrite each other at encode time.
            // Multi-Imm32 wires are legal (a future variant could
            // append two immediates in sequence). Detect duplicate
            // ModR/M-slot writers (resultSlot + each wire) here.
            {
                bool sawResultModRmReg = false;
                bool sawResultModRmRm  = false;
                if (v.resultSlot.has_value()) {
                    if (*v.resultSlot == EncodingSlotKind::ModRmReg)
                        sawResultModRmReg = true;
                    if (*v.resultSlot == EncodingSlotKind::ModRmRm)
                        sawResultModRmRm = true;
                }
                bool sawWireModRmReg = false;
                bool sawWireModRmRm  = false;
                for (auto const& w : v.wires) {
                    if (w.slotKind == EncodingSlotKind::ModRmReg) {
                        if (sawWireModRmReg || sawResultModRmReg) {
                            fail(std::format("/opcodes/{}/encoding/variants/{}/wires", i, vi),
                                 std::format("opcode '{}' variant {}: "
                                             "two slot writes target "
                                             "'modrm.reg' — the second "
                                             "would silently overwrite "
                                             "the first at encode time",
                                             o.mnemonic, vi));
                        }
                        sawWireModRmReg = true;
                    }
                    if (w.slotKind == EncodingSlotKind::ModRmRm) {
                        if (sawWireModRmRm || sawResultModRmRm) {
                            fail(std::format("/opcodes/{}/encoding/variants/{}/wires", i, vi),
                                 std::format("opcode '{}' variant {}: "
                                             "two slot writes target "
                                             "'modrm.rm' — the second "
                                             "would silently overwrite "
                                             "the first at encode time",
                                             o.mnemonic, vi));
                        }
                        sawWireModRmRm = true;
                    }
                }
            }

            // ── Convergence-fix G: result requires a destination slot ─
            // An opcode whose `result == Value/Optional` MUST route the
            // result register somewhere — either an explicit
            // `resultSlot`, or via `modrmRegExt` (the `/digit` form
            // ALSO encodes the destination, via ModR/M.rm with the
            // wired source). Otherwise the destination register is
            // silently dropped from the encoding.
            if (o.result != TargetResultRule::None
                && !v.resultSlot.has_value()
                && !v.tmpl.modrmRegExt.has_value()) {
                fail(std::format("/opcodes/{}/encoding/variants/{}", i, vi),
                     std::format("opcode '{}' variant {}: opcode has "
                                 "`result='{}'` but variant declares "
                                 "neither `resultSlot` nor "
                                 "`template.modrmRegExt` — destination "
                                 "register would be silently dropped",
                                 o.mnemonic, vi,
                                 targetResultRuleName(o.result)));
            }
        }

        // ── Convergence-fix D: overlapping variant guards ─────────
        // Two variants with identical `operandKinds` would first-match
        // silently win — the second is unreachable.
        for (std::size_t a = 0; a < o.encoding.variants.size(); ++a) {
            for (std::size_t b = a + 1; b < o.encoding.variants.size(); ++b) {
                if (o.encoding.variants[a].operandKinds
                    == o.encoding.variants[b].operandKinds) {
                    fail(std::format("/opcodes/{}/encoding/variants/{}", i, b),
                         std::format("opcode '{}': variant {} has the "
                                     "same `operandKinds` as variant "
                                     "{} — first-match dispatch makes "
                                     "variant {} unreachable",
                                     o.mnemonic, b, a, b));
                }
            }
        }
    }

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
        // Non-terminator opcodes (`terminatorKind == None`) cannot have
        // declared CFG successors. Only terminators have CFG-out edges.
        if (!o.isTerminator() && o.maxSuccessors > 0) {
            fail(std::format("/opcodes/{}/maxSuccessors", i),
                 std::format("opcode '{}': non-terminator (terminatorKind=none) "
                             "with maxSuccessors>0 ({})",
                             o.mnemonic, o.maxSuccessors));
        }
        // Per-kind successor-count contract via the `kTargetTerminatorShapes`
        // table — single source of truth shared between the validator
        // AND the `.dsslir` parser dispatch in `lir_text.cpp`. The
        // 255-sentinel on `Switch.maxSuccessors` means "unbounded above
        // the minimum"; the validator treats it as no upper bound.
        if (auto const* shape = findTerminatorShape(o.terminatorKind)) {
            bool const minWrong = o.minSuccessors < shape->minSuccessors;
            bool const maxWrong =
                (shape->maxSuccessors != 255)
                ? (o.maxSuccessors != shape->maxSuccessors
                   || o.minSuccessors != shape->minSuccessors)
                : (o.maxSuccessors < shape->minSuccessors);
            if (minWrong || maxWrong) {
                fail(std::format("/opcodes/{}/maxSuccessors", i),
                     std::format("opcode '{}': terminatorKind={} requires "
                                 "successors in [{}, {}] (got min={}, max={})",
                                 o.mnemonic,
                                 targetTerminatorKindName(o.terminatorKind),
                                 shape->minSuccessors,
                                 (shape->maxSuccessors == 255 ? "+inf" :
                                  std::format("{}", shape->maxSuccessors)),
                                 o.minSuccessors, o.maxSuccessors));
            }
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

    // ── Relocation taxonomy (AS1 §2.6) ──────────────────────────
    //
    // Placed BEFORE the ABI gating below — relocations are
    // independent of `abiModel`/register-file presence. A WASM /
    // SPIR-V target with no registers can still declare a reloc
    // taxonomy if its container format (custom-sections, output
    // metadata) needs one. Empty section is legal; non-empty rows
    // must satisfy three invariants the per-row JSON parse cannot
    // catch:
    //   * `kind != 0` — slot-0 reserved as the invalid sentinel so a
    //     default-constructed `Relocation{}.kind == 0` is loudly
    //     distinguishable from any declared kind.
    //   * `kind` unique across all rows — two rows with the same tag
    //     means the assembler+linker disagree on which formula a
    //     relocation refers to (silent miscompile at link time).
    //   * `name` non-empty — the linker's `*.format.json` cross-
    //     reference uses `name` as the lookup key (plan 14 §2.0);
    //     an empty name would silently mis-resolve to whichever
    //     format-side row also has an empty key.
    {
        std::unordered_map<RelocationKind, std::size_t> seenKind;
        for (std::size_t i = 0; i < relocations.size(); ++i) {
            auto const& r = relocations[i];
            if (r.name.empty()) {
                fail(std::format("/relocations/{}/name", i),
                     "relocation row: 'name' must be a non-empty string");
            }
            if (!r.kind.valid()) {
                fail(std::format("/relocations/{}/kind", i),
                     std::format("relocation '{}': 'kind' must be != 0 "
                                 "(slot 0 is reserved as the invalid sentinel)",
                                 r.name));
                continue;  // skip the uniqueness check for the bad row
            }
            auto [it, fresh] = seenKind.emplace(r.kind, i);
            if (!fresh) {
                fail(std::format("/relocations/{}/kind", i),
                     std::format("relocation '{}': duplicate 'kind' value {} "
                                 "(already declared by relocation '{}' at /relocations/{})",
                                 r.name, r.kind.v,
                                 relocations[it->second].name, it->second));
            }
        }
    }

    // ── Calling conventions ──────────────────────────────────────
    // Three gates here, in order:
    //
    //  1) Non-register-machine ABIs (WASM operand-stack, SPIR-V result-
    //     id, etc.) that DECLARE empty registers + empty callingConventions
    //     legitimately bypass the rest of the loop. But if a non-register-
    //     machine target SHIPS calling-conventions or register entries
    //     anyway (copy-paste error, leftover from a template), those
    //     references must still resolve — otherwise typos hide in
    //     unloadable-anyway data.
    //
    //  2) For register-machine ABI, a target that declared a calling-
    //     convention WITHOUT registers is the silent-failure trap closed
    //     in cycle 2b's review — references resolve to nothing. Enforce.
    //
    //  3) Cycle-2a-shape (registers AND callingConventions BOTH empty)
    //     is the back-compat shape and validate has nothing to check.
    bool const hasAbiContent =
        !registers.empty() || !callingConventions.empty();
    if (!hasAbiContent) {
        return problems;
    }
    // From here, the target opted into the register-machine validation
    // surface by populating at least one of the two sections — even if
    // its declared abiModel is non-register-machine.
    // (`enforceRefs` was a tautological alias of `hasAbiContent` here
    // — the early return above guarantees this branch is unreachable
    // when `!hasAbiContent`. Removed per simplifier review.)
    auto checkRefs = [&](std::size_t       ccIdx,
                         char const*       field,
                         std::span<std::string const> refs,
                         TargetRegClass    expectedClass) {
        auto const& cc = callingConventions[ccIdx];
        for (std::size_t k = 0; k < refs.size(); ++k) {
            auto const& ref = refs[k];
            auto it = registerIndex.find(ref);
            if (it == registerIndex.end()) {
                fail(std::format("/callingConventions/{}/{}/{}", ccIdx, field, k),
                     std::format("callingConvention '{}'.{}: register '{}' is not in the register table",
                                 cc.name, field, ref));
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
        // The loader pre-resolved name → ordinal atomically, so this
        // check enforces the class invariant; the resolution invariant
        // is enforced by the loader itself (an unresolved name emits a
        // diagnostic at load time, before validate() runs).
        if (cc.linkRegister.has_value()) {
            std::span<std::string const> linkRefs{&cc.linkRegister->name, 1};
            checkRefs(i, "linkRegister", linkRefs, TargetRegClass::GPR);
        }
        if (cc.stackPointer.has_value()) {
            std::span<std::string const> spRefs{&cc.stackPointer->name, 1};
            checkRefs(i, "stackPointer", spRefs, TargetRegClass::GPR);
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
            // ML7 callconv lowering requires a stack-pointer register
            // for any register-machine cc carrying ABI info. Surface
            // the misconfiguration at schema-load time instead of
            // letting ML7 fail at run time with a misleading
            // "missing opcode" diagnostic.
            if (abiModel == TargetAbiModel::RegisterMachine
                && !cc.stackPointer.has_value()) {
                fail(std::format("/callingConventions/{}/stackPointer", i),
                     std::format("callingConvention '{}': register-machine ABI "
                                 "requires a stackPointer register declaration",
                                 cc.name));
            }
        }
    }

    return problems;
}

} // namespace detail

} // namespace dss
