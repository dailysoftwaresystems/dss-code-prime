#include "core/types/target_schema.hpp"

#include "core/substrate/relocation_table.hpp"
#include "core/types/config_path_walk.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <algorithm>
#include <array>
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
            // `opcodeBytes` is meaningful only for the x86-variable
            // shape. fixed32 carries the analog as `fixedWord` (a
            // 32-bit base bit pattern). The "non-empty" rule
            // therefore applies only when the variant declares a
            // shape that consumes opcodeBytes.
            if (o.encoding.shape == TargetEncodingShape::X86Variable) {
                if (v.tmpl.opcodeBytes.empty()) {
                    fail(std::format("/opcodes/{}/encoding/variants/{}/template/opcode", i, vi),
                         std::format("opcode '{}' variant {}: 'opcode' bytes "
                                     "must be non-empty for x86-variable shape",
                                     o.mnemonic, vi));
                }
                if (v.tmpl.opcodeBytes.size() > 15) {
                    fail(std::format("/opcodes/{}/encoding/variants/{}/template/opcode", i, vi),
                         std::format("opcode '{}' variant {}: 'opcode' byte "
                                     "count {} exceeds 15 (x86 instruction "
                                     "length limit)",
                                     o.mnemonic, vi, v.tmpl.opcodeBytes.size()));
                }
            } else if (o.encoding.shape == TargetEncodingShape::Fixed32) {
                // fixed32 mirror: opcodeBytes / modrmRegExt are
                // never meaningful; declaring them on a fixed32
                // variant is dead data — flag for cleanliness.
                if (!v.tmpl.opcodeBytes.empty()) {
                    fail(std::format("/opcodes/{}/encoding/variants/{}/template/opcode", i, vi),
                         std::format("opcode '{}' variant {}: 'opcode' bytes "
                                     "declared on a fixed32 variant — "
                                     "fixed32 uses `fixedWord` instead",
                                     o.mnemonic, vi));
                }
                if (v.tmpl.modrmRegExt.has_value()) {
                    fail(std::format("/opcodes/{}/encoding/variants/{}/template/modrmRegExt", i, vi),
                         std::format("opcode '{}' variant {}: 'modrmRegExt' "
                                     "declared on a fixed32 variant — "
                                     "fixed32 has no ModR/M byte",
                                     o.mnemonic, vi));
                }
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

            // ── Silent-failure M-1: fixed32 register-only guard ─────
            // Cycle-3 fixed32 walker rejects non-Reg operands at
            // runtime (it has no immediate-slot support yet — Imm12
            // / ImmShift land alongside their ARM64 consumer cycle).
            // Surfacing at schema-load time catches a misauthored
            // variant once, not per-instruction.
            if (o.encoding.shape == TargetEncodingShape::Fixed32) {
                for (std::size_t ki = 0; ki < v.operandKinds.size(); ++ki) {
                    if (v.operandKinds[ki] != OperandKindFilter::Reg
                        && v.operandKinds[ki] != OperandKindFilter::SymbolRef) {
                        fail(std::format("/opcodes/{}/encoding/variants/{}/guard/operandKinds/{}", i, vi, ki),
                             std::format("opcode '{}' variant {}: "
                                         "fixed32 cycle-4 scope is "
                                         "register + symbol-ref only "
                                         "— operand kind '{}' at "
                                         "position {} needs an "
                                         "immediate-slot walker "
                                         "(Imm12 / ImmShift, per "
                                         "plan 13 §3.1 D-AS3-6)",
                                         o.mnemonic, vi,
                                         operandKindFilterName(v.operandKinds[ki]),
                                         ki));
                    }
                }
            }

            // ── Plan 13 AS4: relocationKind vs slot pairing ─────────
            // A wire targeting a symbol-bearing slot (Disp32 / Imm26)
            // MUST declare `relocationKind`; a wire to a non-symbol
            // slot MUST NOT (would be dead data, or worse misleading).
            // The loader has already resolved the name into the
            // `RelocationKind` opaque tag; here we just check
            // presence-vs-required.
            for (auto const& w : v.wires) {
                bool const needsReloc = isSymbolBearingSlot(w.slotKind);
                bool const hasReloc   = w.relocationKind.has_value();
                if (needsReloc && !hasReloc) {
                    fail(std::format("/opcodes/{}/encoding/variants/{}/wires", i, vi),
                         std::format("opcode '{}' variant {}: wire to "
                                     "symbol-bearing slot '{}' must "
                                     "declare `relocationKind` (the row "
                                     "in the schema's `relocations[]` "
                                     "whose kind the assembler stamps "
                                     "onto each emitted Relocation)",
                                     o.mnemonic, vi,
                                     encodingSlotKindName(w.slotKind)));
                }
                if (!needsReloc && hasReloc) {
                    fail(std::format("/opcodes/{}/encoding/variants/{}/wires", i, vi),
                         std::format("opcode '{}' variant {}: wire to "
                                     "non-symbol-bearing slot '{}' "
                                     "declares `relocationKind` — the "
                                     "walker never emits a Relocation "
                                     "for this slot kind",
                                     o.mnemonic, vi,
                                     encodingSlotKindName(w.slotKind)));
                }
            }

            // ── Shape-vs-slot cross-check ──────────────────────────
            // Each `EncodingSlotKind` belongs to ONE shape. A variant
            // declaring `modrm.rm` under a `fixed32`-shape opcode (or
            // `rd` under an `x86-variable`-shape opcode) would silently
            // misroute at encode time — the walker rejects the
            // unknown slot, but the diagnostic shows up per-inst
            // instead of per-row. Surface at load.
            auto const checkSlotShape = [&](EncodingSlotKind slot,
                                            std::string const& path) {
                auto const owning = slotShapeFor(slot);
                if (owning != o.encoding.shape) {
                    fail(path,
                         std::format("opcode '{}' variant {}: slot '{}' "
                                     "belongs to encoding shape '{}', "
                                     "but the opcode declares shape '{}'",
                                     o.mnemonic, vi,
                                     encodingSlotKindName(slot),
                                     targetEncodingShapeName(owning),
                                     targetEncodingShapeName(o.encoding.shape)));
                }
            };
            if (v.resultSlot.has_value()) {
                checkSlotShape(*v.resultSlot,
                               std::format("/opcodes/{}/encoding/variants/{}/resultSlot", i, vi));
            }
            for (std::size_t wi = 0; wi < v.wires.size(); ++wi) {
                checkSlotShape(v.wires[wi].slotKind,
                               std::format("/opcodes/{}/encoding/variants/{}/wires/{}/slotKind", i, vi, wi));
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
            // Two writers to the same single-writer slot would silently
            // overwrite each other at encode time. Multi-Imm32 wires
            // are legal (a future variant could append two immediates
            // in sequence). All other slots are single-writer.
            // Convergence-fix D extension: fixed32 (Rd/Rn/Rm) slots
            // are equally single-writer — extend the same rule there.
            {
                auto const isMultiWriterSlot = [](EncodingSlotKind s) noexcept {
                    return s == EncodingSlotKind::Imm32;
                };
                // Track each non-multi-writer slot's first writer.
                // Sized from the shared `kEncodingSlotKindCount` so
                // adding a new slot (Disp32 / Imm26 / etc.) is the
                // SAME change as updating the enum table, no manual
                // size update.
                std::array<bool, kEncodingSlotKindCount> sawSlot{};
                auto const markOrFail = [&](EncodingSlotKind slot,
                                            std::string const& kindLabel,
                                            std::string const& path) {
                    if (isMultiWriterSlot(slot)) return;
                    auto const idx = static_cast<std::size_t>(slot);
                    if (idx < sawSlot.size() && sawSlot[idx]) {
                        fail(path,
                             std::format("opcode '{}' variant {}: "
                                         "{} writer targets '{}' — "
                                         "the second writer would "
                                         "silently overwrite the "
                                         "first at encode time",
                                         o.mnemonic, vi, kindLabel,
                                         encodingSlotKindName(slot)));
                    } else if (idx < sawSlot.size()) {
                        sawSlot[idx] = true;
                    }
                };
                if (v.resultSlot.has_value()) {
                    markOrFail(*v.resultSlot, "result",
                               std::format("/opcodes/{}/encoding/variants/{}/resultSlot", i, vi));
                }
                for (auto const& w : v.wires) {
                    markOrFail(w.slotKind, "wire",
                               std::format("/opcodes/{}/encoding/variants/{}/wires", i, vi));
                }
            }

            // ── Convergence-fix G: result requires a destination slot ─
            // An opcode whose `result == Value/Optional` MUST route the
            // result register somewhere. Three accepted shapes:
            //   (1) explicit `resultSlot`,
            //   (2) `template.modrmRegExt` (the `/digit` form ALSO
            //       encodes the destination, via ModR/M.rm with the
            //       wired source),
            //   (3) `requires2Address: true` AND a wire on operand 0
            //       — after `lir_2addr_legalize` ensures `result ==
            //       operands[0]`, the wire for operand 0 supplies
            //       the destination's slot placement.
            // Convergence-fix C: the `requires2Address` exception
            // only counts when the operand-0 wire targets a
            // DESTINATION-bearing slot. ModRmReg / ModRmRm (x86-
            // variable destinations) and Rd (fixed32 destination)
            // qualify. A wire to Imm32 / ModRm-source / Rn / Rm
            // does NOT — those are source-only slots, and using the
            // legalize-pass "operand 0 IS the destination" assumption
            // there would silently route the destination to a
            // non-destination position.
            auto const isDestSlot = [](EncodingSlotKind s) noexcept {
                return s == EncodingSlotKind::ModRmReg
                    || s == EncodingSlotKind::ModRmRm
                    || s == EncodingSlotKind::Rd;
            };
            bool const has2AddrDestWire = o.requires2Address
                && std::any_of(v.wires.begin(), v.wires.end(),
                               [&](TargetEncodingWire const& w) {
                                   return w.index == 0
                                       && isDestSlot(w.slotKind);
                               });
            // `isCall` opcodes declare `result: optional` in LIR for
            // callee-returns-a-value semantics, but the byte
            // encoding doesn't carry the result — ML7 callconv
            // lowering materializes the return value into the
            // calling-convention's return register BEFORE the call,
            // and consumers read it AFTER. The schema's `result`
            // describes the LIR vreg semantics, not the byte form.
            // Skip rule G for call-class opcodes. (`isCall` itself
            // is constrained at schema-level below — convergence-
            // fix C / silent-failure F1.)
            if (o.result != TargetResultRule::None
                && !v.resultSlot.has_value()
                && !v.tmpl.modrmRegExt.has_value()
                && !has2AddrDestWire
                && !o.isCall) {
                fail(std::format("/opcodes/{}/encoding/variants/{}", i, vi),
                     std::format("opcode '{}' variant {}: opcode has "
                                 "`result='{}'` but variant declares "
                                 "none of `resultSlot` / "
                                 "`template.modrmRegExt` / "
                                 "(`requires2Address` + wire on operand 0) "
                                 "— destination register would be "
                                 "silently dropped",
                                 o.mnemonic, vi,
                                 targetResultRuleName(o.result)));
            }

            // ── Architect followup (inverse of G): no result + slot ──
            // The opposite trap: an opcode with `result == None`
            // (e.g. `ret`, `cmp`) declaring a `resultSlot` is
            // nonsensical — there IS no result register to route.
            // The walker would deref a default-constructed `LirReg`
            // for `hwEncodingOf` and emit a runtime diagnostic.
            // Reject at load instead.
            if (o.result == TargetResultRule::None
                && v.resultSlot.has_value()) {
                fail(std::format("/opcodes/{}/encoding/variants/{}/resultSlot", i, vi),
                     std::format("opcode '{}' variant {}: opcode has "
                                 "`result='none'` but variant declares "
                                 "a `resultSlot` — there IS no result "
                                 "register to route",
                                 o.mnemonic, vi));
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

        // ── Convergence-fix F: `requires2Address` is reg-reg shape ──
        // The 2-address legalize pass inserts `mov result,
        // operands[0]` when result != operands[0] — but only if
        // operand 0 IS a register and the opcode produces a result.
        // A schema declaring `requires2Address` on:
        //   * an opcode with `result: none` — would silently never
        //     legalize (no result register to copy to);
        //   * an opcode with `maxOperands < 1` — would never have an
        //     operand 0 to copy from;
        //   * a variant whose operandKinds[0] is NOT `reg` — would
        //     trigger the pass's hard-fail at runtime; reject at
        //     load instead.
        if (o.requires2Address) {
            if (o.result == TargetResultRule::None) {
                fail(std::format("/opcodes/{}/requires2Address", i),
                     std::format("opcode '{}': `requires2Address: true` "
                                 "requires `result != none` — the "
                                 "legalize pass copies operands[0] INTO "
                                 "result, which doesn't exist when "
                                 "result is `none`",
                                 o.mnemonic));
            }
            if (o.maxOperands < 1) {
                fail(std::format("/opcodes/{}/requires2Address", i),
                     std::format("opcode '{}': `requires2Address: true` "
                                 "requires `maxOperands >= 1` — "
                                 "operand 0 must exist to be copied "
                                 "to the destination",
                                 o.mnemonic));
            }
            for (std::size_t vi = 0; vi < o.encoding.variants.size(); ++vi) {
                auto const& v = o.encoding.variants[vi];
                if (!v.operandKinds.empty()
                    && v.operandKinds[0] != OperandKindFilter::Reg) {
                    fail(std::format("/opcodes/{}/encoding/variants/{}/guard/operandKinds/0", i, vi),
                         std::format("opcode '{}' variant {}: "
                                     "`requires2Address: true` requires "
                                     "operandKinds[0] to be 'reg' — the "
                                     "legalize pass needs a Reg operand "
                                     "to copy from",
                                     o.mnemonic, vi));
                }
            }
        }

        // ── Convergence-fix C: `isCall` constraints (silent-failure F1) ─
        // `isCall` exempts rule G (call opcodes' result is materialized
        // by callconv, not by a destination slot). To prevent the
        // exemption from becoming an escape hatch on non-call opcodes:
        //   * an `isCall: true` opcode MUST set `hasSideEffects: true`
        //     (a pure-function call is a contradiction; the regalloc
        //     tier also relies on `hasSideEffects` for call-boundary
        //     liveness tracking).
        //   * an `isCall: true` opcode MUST NOT declare a destination-
        //     bearing `resultSlot` on any variant (the result lives in
        //     the callconv return register, not in an encoding slot).
        // Together, these block a copy-paste `isCall: true` on an
        // unrelated opcode from silently dropping its destination.
        if (o.isCall) {
            if (!o.hasSideEffects) {
                fail(std::format("/opcodes/{}/isCall", i),
                     std::format("opcode '{}': `isCall: true` requires "
                                 "`hasSideEffects: true` (a pure-function "
                                 "call is a contradiction)",
                                 o.mnemonic));
            }
            for (std::size_t vi = 0; vi < o.encoding.variants.size(); ++vi) {
                auto const& v = o.encoding.variants[vi];
                if (v.resultSlot.has_value()) {
                    fail(std::format("/opcodes/{}/encoding/variants/{}/resultSlot", i, vi),
                         std::format("opcode '{}' variant {}: `isCall: "
                                     "true` opcode declares a "
                                     "`resultSlot` — the call's return "
                                     "value lives in the callconv "
                                     "return register, not in an "
                                     "encoding slot",
                                     o.mnemonic, vi));
                }
            }
        }
    }

    // ── Convergence-fix A (schema-level): `mov` opcode required ────
    // When ANY opcode declares `requires2Address`, the schema MUST
    // also declare a `mov` opcode — the legalize pass synthesizes
    // `mov result, operands[0]` to satisfy the 2-address constraint.
    // Without `mov`, the pass would emit `L_RequiredLirOpcodeMissing`
    // per-instruction at runtime; surfacing at schema-load time
    // catches the misconfiguration once.
    bool const anyRequires2Address = std::any_of(
        opcodes.begin(), opcodes.end(),
        [](TargetOpcodeInfo const& o) { return o.requires2Address; });
    if (anyRequires2Address && mnemonicIndex.find("mov") == mnemonicIndex.end()) {
        fail("/opcodes",
             "at least one opcode declares `requires2Address: true` "
             "but the schema lacks a 'mov' opcode — the 2-address "
             "legalize pass uses `mov` to synthesize the implicit "
             "register copy and cannot proceed without it");
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
    // M-1 (silent-failure follow-up): if ANY opcode declares the
    // `x86-variable` encoding shape, every GPR/FPR register's
    // `hwEncoding` must fit in 4 bits (the x86 ModR/M + REX-extended
    // field width). The walker re-checks at encode time, but that's
    // per-instruction overhead and the failure surface — failing
    // at schema-load surfaces it once.
    bool anyX86VariableOpcode = false;
    for (auto const& o : opcodes) {
        if (o.encoding.shape == TargetEncodingShape::X86Variable) {
            anyX86VariableOpcode = true;
            break;
        }
    }
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
        if (anyX86VariableOpcode
            && (r.regClass == TargetRegClass::GPR
                || r.regClass == TargetRegClass::FPR)
            && r.hwEncoding > 15) {
            fail(std::format("/registers/{}/hwEncoding", i),
                 std::format("register '{}': hwEncoding {} exceeds 4 "
                             "bits — the target declares x86-variable "
                             "encoding which can only express GPR/FPR "
                             "ordinals 0..15 via REX-extended ModR/M",
                             r.name, r.hwEncoding));
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
    substrate::validateRelocationsTable<TargetRelocationInfo>(
        relocations, fail);

    // Plan 14 LK6 cycle 1 structured-formula coherence. Four rules
    // catch silent-misapply hazards at load time:
    //
    //   (b) `widthBytes != 0` is required whenever `pcRelative` is
    //       true or `addendBias != 0` — otherwise the linker would
    //       silently reject a row that DID declare structured
    //       semantics (looks like a row author typo, not the
    //       intended "decline to apply" sentinel).
    //   (c) `addendBias != 0` ⇒ `pcRelative` — absolute relocations
    //       with a non-zero constant bias have no real consumer
    //       across x86_64 / ARM64 / PE / Mach-O. A row in this
    //       shape is almost certainly a typo (bias on the wrong
    //       row, or pcRelative dropped by accident). Reject at
    //       load (type-design O2.a convergence).
    //   (d) `widthBytes != 0` ⇒ `|addendBias|` fits signed in
    //       `widthBytes` bytes. A bias that overflows the patch
    //       slot would silently corrupt every patched site
    //       (silent-failure M1 convergence).
    //
    // Rule (a) — `widthBytes ∈ {4, 8}` — is enforced by the JSON
    // loader before this code runs.
    for (std::size_t i = 0; i < relocations.size(); ++i) {
        auto const& r = relocations[i];
        bool const hasStructuredSemantics =
            r.pcRelative || r.addendBias != 0;
        if (hasStructuredSemantics && r.widthBytes == 0) {
            fail(std::format("/relocations/{}/widthBytes", i),
                 std::format("relocation '{}': 'widthBytes' must be "
                             "declared (4 or 8) when 'pcRelative' is "
                             "true or 'addendBias' is non-zero — the "
                             "linker needs the width to apply the "
                             "structured formula. Omit pcRelative + "
                             "addendBias for relocations whose "
                             "application is not yet supported (the "
                             "LK6 in-place applier will reject them).",
                             r.name));
        }
        if (r.addendBias != 0 && !r.pcRelative) {
            fail(std::format("/relocations/{}/addendBias", i),
                 std::format("relocation '{}': 'addendBias' is "
                             "non-zero ({}) but 'pcRelative' is "
                             "false — no x86_64 / ARM64 / PE / "
                             "Mach-O relocation needs an absolute "
                             "patch with a constant bias. This is "
                             "almost certainly a typo (bias on the "
                             "wrong row, or pcRelative dropped).",
                             r.name, r.addendBias));
        }
        if (r.widthBytes != 0 && r.widthBytes < 8) {
            std::int64_t const sMax =
                (std::int64_t{1} << (8 * r.widthBytes - 1)) - 1;
            std::int64_t const sMin = -sMax - 1;
            if (r.addendBias < sMin || r.addendBias > sMax) {
                fail(std::format("/relocations/{}/addendBias", i),
                     std::format("relocation '{}': 'addendBias' "
                                 "({}) does not fit signed in "
                                 "widthBytes={} ({} ≤ bias ≤ {}). "
                                 "A bias that overflows the patch "
                                 "slot silently corrupts every "
                                 "patched site.",
                                 r.name, r.addendBias,
                                 r.widthBytes, sMin, sMax));
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
