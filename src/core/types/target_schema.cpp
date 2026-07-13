#include "core/types/target_schema.hpp"

#include "core/substrate/relocation_table.hpp"
#include "core/types/ascii_case.hpp"
#include "core/types/config_path_walk.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <span>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace dss {

// All three helpers iterate `kRelocFormulaTable` — single source of
// truth (post-fold #2 — was 3 independent hand-rolled enumerations).
// Adding a new variant = one row in the table + one enum entry.
std::string_view relocFormulaName(RelocFormulaKind k) noexcept {
    return kRelocFormulaTable.name(k);
}

std::optional<RelocFormulaKind> parseRelocFormulaKind(std::string_view s) noexcept {
    return kRelocFormulaTable.fromName(asciiToLower(s));
}

std::string acceptedRelocFormulaList() {
    std::string out;
    for (auto const& row : kRelocFormulaTable.rows) {
        if (!out.empty()) out += ", ";
        out += "'";
        out += row.second;
        out += "'";
    }
    return out;
}

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
                                   DiagnosticCode::C_InvalidTargetName});
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
            // D-ASM-AARCH64-FRAME-OFFSET-BEYOND-IMM12: immMin/immMax
            // coherence. (a) Both present ⇒ immMin <= immMax (an inverted
            // range matches NOTHING — a silent dead variant). (b) Either
            // present ⇒ the guard MUST declare an immediate-bearing operand
            // (ImmInt or MemOffset) for the magnitude matcher to read;
            // otherwise the bound keys on a value that does not exist and
            // the variant would match nothing (or match-any inconsistently).
            if (v.immMin.has_value() && v.immMax.has_value()
                && *v.immMin > *v.immMax) {
                fail(std::format("/opcodes/{}/encoding/variants/{}/guard", i, vi),
                     std::format("opcode '{}' variant {}: immMin ({}) > immMax "
                                 "({}) — an inverted magnitude range matches no "
                                 "instruction (silent dead variant)",
                                 o.mnemonic, vi, *v.immMin, *v.immMax));
            }
            if (v.immMin.has_value() || v.immMax.has_value()) {
                bool const hasImmOperand = std::any_of(
                    v.operandKinds.begin(), v.operandKinds.end(),
                    [](OperandKindFilter f) {
                        return f == OperandKindFilter::ImmInt
                            || f == OperandKindFilter::MemOffset;
                    });
                if (!hasImmOperand) {
                    fail(std::format("/opcodes/{}/encoding/variants/{}/guard", i, vi),
                         std::format("opcode '{}' variant {}: declares "
                                     "immMin/immMax but its operandKinds carry "
                                     "no 'imm32' or 'memoffset' operand — there "
                                     "is no immediate magnitude to key on",
                                     o.mnemonic, vi));
                }
            }
            // D-AS4-ARM64-NEGATIVE-DISP-LEA-NATIVE-SUB: negMemoffset requires a
            // `memoffset` operand to sign-route on — the same coherence family
            // as immMin/immMax. A negMemoffset flag on a variant with no
            // memoffset operand keys on a sign that does not exist (the sign
            // axis reads only ImmInt/MemOffset; an ImmInt is never a
            // displacement, so negMemoffset is memoffset-specific). Reject at
            // load rather than match nothing silently.
            if (v.negMemoffset) {
                bool const hasMemOffset = std::any_of(
                    v.operandKinds.begin(), v.operandKinds.end(),
                    [](OperandKindFilter f) {
                        return f == OperandKindFilter::MemOffset;
                    });
                if (!hasMemOffset) {
                    fail(std::format("/opcodes/{}/encoding/variants/{}/guard", i, vi),
                         std::format("opcode '{}' variant {}: declares "
                                     "negMemoffset but its operandKinds carry "
                                     "no 'memoffset' operand — there is no "
                                     "displacement to sign-route on",
                                     o.mnemonic, vi));
                }
            }
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
                // FC2 Part B: the 15-byte architectural cap counts
                // prefixes too — a mandatoryPrefix + opcode pair that
                // alone exceeds it can never encode.
                if (v.tmpl.mandatoryPrefix.size() + v.tmpl.opcodeBytes.size() > 15) {
                    fail(std::format("/opcodes/{}/encoding/variants/{}/template/mandatoryPrefix", i, vi),
                         std::format("opcode '{}' variant {}: 'mandatoryPrefix' "
                                     "({} bytes) plus 'opcode' ({} bytes) "
                                     "exceeds 15 (x86 instruction length limit)",
                                     o.mnemonic, vi,
                                     v.tmpl.mandatoryPrefix.size(),
                                     v.tmpl.opcodeBytes.size()));
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
                // FC2 Part B: legacy prefixes are an x86-variable-only
                // concept — a fixed-word ISA has no prefix bytes.
                // Silent ignore would let a misauthored row believe
                // its prefix is emitted.
                if (!v.tmpl.mandatoryPrefix.empty()) {
                    fail(std::format("/opcodes/{}/encoding/variants/{}/template/mandatoryPrefix", i, vi),
                         std::format("opcode '{}' variant {}: 'mandatoryPrefix' "
                                     "declared on a fixed32 variant — "
                                     "fixed32 has no legacy-prefix bytes",
                                     o.mnemonic, vi));
                }
                // TLS C1 (D-CSUBSET-THREAD-LOCAL): the payload-byte
                // prefix is likewise an x86-variable-only concept
                // (prefix group 2 segment override); a fixed-word ISA
                // has no prefix bytes.
                if (v.tmpl.payloadBytePrefix) {
                    fail(std::format("/opcodes/{}/encoding/variants/{}/template/payloadBytePrefix", i, vi),
                         std::format("opcode '{}' variant {}: 'payloadBytePrefix' "
                                     "declared on a fixed32 variant — "
                                     "fixed32 has no prefix bytes",
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

            // ── Silent-failure M-1: fixed32 supported-operand guard ──
            // The fixed32 walker handles Reg (register slots), SymbolRef
            // (the symbol-bearing Imm26 slot), and — since D-LK10-ENTRY-
            // ARM64 (v0.0.2 V2-1) — ImmInt (the Imm16 immediate slot,
            // AArch64 MOVZ) plus MemBase + MemOffset (the unscaled
            // LDUR/STUR memory form: base reg → Rn, MemOffset → the
            // signed Imm9 slot, MemBase's scale validated == 1). Since
            // D-AS3-BLOCK-REL-IMM19/26 (ARM64 conditional control-flow)
            // it ALSO handles BlockRef (an intra-function branch target
            // on the Imm19 [B.cond] or Imm26 [B] slot — resolved at
            // assemble time, no relocation). The remaining operand kinds
            // (index forms) have no fixed32 walker yet and land alongside
            // their consumer cycle. Surfacing at schema-load time catches
            // a misauthored variant once, not per-instruction. (The
            // walker still enforces the operand→slot pairing — ImmInt→
            // Imm16, MemOffset→Imm9, SymbolRef→Imm26, BlockRef→Imm19/
            // Imm26 — the immediate/offset range, and MemBase scale==1;
            // this gate only screens the KIND.)
            if (o.encoding.shape == TargetEncodingShape::Fixed32) {
                for (std::size_t ki = 0; ki < v.operandKinds.size(); ++ki) {
                    auto const k = v.operandKinds[ki];
                    if (k != OperandKindFilter::Reg
                        && k != OperandKindFilter::SymbolRef
                        && k != OperandKindFilter::ImmInt
                        && k != OperandKindFilter::MemBase
                        && k != OperandKindFilter::MemOffset
                        && k != OperandKindFilter::BlockRef) {
                        fail(std::format("/opcodes/{}/encoding/variants/{}/guard/operandKinds/{}", i, vi, ki),
                             std::format("opcode '{}' variant {}: "
                                         "fixed32 supports register, "
                                         "symbol-ref, immediate, memory "
                                         "(base+offset), and block-ref "
                                         "operands — "
                                         "operand kind '{}' at position {} "
                                         "needs a fixed32 walker for that "
                                         "kind (indexed forms, "
                                         "per plan 13 §3.1 D-AS3-6)",
                                         o.mnemonic, vi,
                                         operandKindFilterName(k),
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
            //
            // D-AS3-BLOCK-REL-IMM19/26 (operand-aware exemption): the
            // Imm26 slot is DUAL-USE — symbol-bearing for the BL/`call`
            // form (a SymbolRef operand → `call26` relocation) but
            // BLOCK-relative for the `B` form (a BlockRef operand →
            // assemble-time patch, NO relocation). `isSymbolBearingSlot`
            // keys on the slot alone and so reports Imm26 as symbol-
            // bearing; cross-reference the WIRE's guard operand kind to
            // tell the two uses apart. A wire whose guard operand is a
            // BlockRef is the block-relative use: it is EXEMPT from the
            // "must declare relocationKind" rule AND must NOT carry one
            // (the assemble-time resolver owns the field, no linker
            // reloc). x86 sidesteps this by using a distinct non-symbol
            // slot (BlockRel32); ARM64 reuses Imm26, so the disambiguator
            // lives here. (The `Imm19` slot is plainly non-symbol-bearing
            // — only the dual-use Imm26 needs the operand-kind check, but
            // applying it uniformly is harmless and future-proof.)
            for (auto const& w : v.wires) {
                bool const wireIsBlockRef =
                    w.index < v.operandKinds.size()
                    && v.operandKinds[w.index] == OperandKindFilter::BlockRef;
                bool const needsReloc =
                    isSymbolBearingSlot(w.slotKind) && !wireIsBlockRef;
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
            // D-AS4-3: extra result placements (multi-word macros) must
            // also belong to the opcode's shape.
            for (std::size_t ei = 0; ei < v.extraResultSlots.size(); ++ei) {
                checkSlotShape(v.extraResultSlots[ei].slotKind,
                               std::format("/opcodes/{}/encoding/variants/{}/extraResultSlots/{}/slotKind", i, vi, ei));
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
            //
            // D-CSUBSET-COMPUTED-GOTO (operand-aware exemption, mirroring
            // the BlockRef-aware exemption the relocationKind rule above
            // already applies): a `BlockRef` guard position is EXEMPT. A
            // block reference is never byte-encoded data — it is either
            // wired to a block-relative displacement slot (jmp/jcc,
            // resolved at assemble time) OR it is the SYMBOL ↔ BLOCK
            // binding directive carried by the block-address `lea` (its
            // trailing BlockRef, intentionally UNWIRED — the encoder reads
            // it from the operand list and records a `BlockSymPatch`, NOT a
            // byte). In neither case is anything "silently dropped from the
            // encoding" in the sense this rule guards (a dropped register /
            // immediate / displacement that should have contributed bytes).
            // The encoder fail-loud handles a BlockRef it cannot consume,
            // so this exemption never hides a real drop.
            {
                std::vector<bool> covered(v.operandKinds.size(), false);
                for (auto const& w : v.wires) {
                    if (w.index < covered.size()) covered[w.index] = true;
                }
                for (std::size_t gi = 0; gi < covered.size(); ++gi) {
                    if (v.operandKinds[gi] == OperandKindFilter::BlockRef) {
                        continue;  // BlockRef positions carry no bytes (see above)
                    }
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

            // ── TLS C1 (D-CSUBSET-THREAD-LOCAL): memory-displacement
            // single-ownership. A `[base + disp32]` memory operand has
            // exactly ONE displacement field; a variant wiring BOTH a
            // literal `disp32.mem` AND a relocated `memreloc.disp32`
            // would double-emit at encode time (8 bytes where the CPU
            // decodes 4 — a silent instruction-stream corruption). And
            // the absolute-SIB form (`absdisp32.mem`) owns the WHOLE
            // memory operand (no base register, index, or second
            // displacement) — co-wiring it with any other memory-
            // operand slot is contradictory. Reject both at load; the
            // encoder re-checks defensively at emit.
            {
                bool hasDisp32Mem = false, hasMemReloc = false;
                bool hasAbsDisp   = false, hasOtherMem = false;
                for (auto const& w : v.wires) {
                    if (w.slotKind == EncodingSlotKind::Disp32Mem)         hasDisp32Mem = true;
                    if (w.slotKind == EncodingSlotKind::MemRelocDisp32)    hasMemReloc  = true;
                    if (w.slotKind == EncodingSlotKind::AbsoluteDisp32Mem) hasAbsDisp   = true;
                    if (w.slotKind == EncodingSlotKind::ModRmRmMem
                        || w.slotKind == EncodingSlotKind::SibIndex
                        || w.slotKind == EncodingSlotKind::MemBaseScale) {
                        hasOtherMem = true;
                    }
                }
                if (hasDisp32Mem && hasMemReloc) {
                    fail(std::format("/opcodes/{}/encoding/variants/{}/wires", i, vi),
                         std::format("opcode '{}' variant {}: wires BOTH a "
                                     "literal 'disp32.mem' AND a relocated "
                                     "'memreloc.disp32' — a memory operand "
                                     "has exactly one displacement field; "
                                     "the double emission would corrupt the "
                                     "instruction stream",
                                     o.mnemonic, vi));
                }
                if (hasAbsDisp && (hasDisp32Mem || hasMemReloc || hasOtherMem)) {
                    fail(std::format("/opcodes/{}/encoding/variants/{}/wires", i, vi),
                         std::format("opcode '{}' variant {}: 'absdisp32.mem' "
                                     "(the base-register-less absolute-SIB "
                                     "form) cannot be co-wired with any other "
                                     "memory-operand slot (modrm.rm.mem / "
                                     "sib.index / membase.scale / disp32.mem "
                                     "/ memreloc.disp32) — the absolute form "
                                     "owns the whole memory operand",
                                     o.mnemonic, vi));
                }
                // The relocated displacement REQUIRES a ModRmRmMem base
                // wire (it is the disp32 OF a `[base + disp32]` memory
                // operand) — without one, no memory ModR/M state exists
                // and the relocation would silently never be recorded.
                if (hasMemReloc) {
                    bool hasMemBase = false;
                    for (auto const& w : v.wires) {
                        if (w.slotKind == EncodingSlotKind::ModRmRmMem) {
                            hasMemBase = true;
                            break;
                        }
                    }
                    if (!hasMemBase) {
                        fail(std::format("/opcodes/{}/encoding/variants/{}/wires", i, vi),
                             std::format("opcode '{}' variant {}: "
                                         "'memreloc.disp32' requires a "
                                         "paired 'modrm.rm.mem' base wire "
                                         "(the relocated displacement is "
                                         "the disp32 of a [base + disp32] "
                                         "memory operand)",
                                         o.mnemonic, vi));
                    }
                }
            }

            // ── D-AS4-3: multi-word (multi-instruction-macro) invariants ─
            // The number of 32-bit words this variant emits. 1 for the
            // single-word `fixedWord` path; N for `fixedWords`.
            std::size_t const nWords = v.tmpl.wordCount();
            // (a) `fixedWords` is a fixed32-only construct — a multi-word
            //     x86 macro would need a different (variable-length)
            //     emission model. Reject a multi-word template on any
            //     non-fixed32 shape.
            if (!v.tmpl.fixedWords.empty()
                && o.encoding.shape != TargetEncodingShape::Fixed32) {
                fail(std::format("/opcodes/{}/encoding/variants/{}/template/fixedWords", i, vi),
                     std::format("opcode '{}' variant {}: 'fixedWords' "
                                 "(multi-word macro) is only valid on the "
                                 "'fixed32' shape, but the opcode declares "
                                 "shape '{}'",
                                 o.mnemonic, vi,
                                 targetEncodingShapeName(o.encoding.shape)));
            }
            // (b) extra result placements require a primary resultSlot —
            //     an extra placement of a result that has no primary slot
            //     is malformed (nothing to thread through the words).
            if (!v.extraResultSlots.empty() && !v.resultSlot.has_value()) {
                fail(std::format("/opcodes/{}/encoding/variants/{}/extraResultSlots", i, vi),
                     std::format("opcode '{}' variant {}: 'extraResultSlots' "
                                 "is set but the variant has no 'resultSlot' "
                                 "— an extra placement of the result register "
                                 "requires a primary result slot",
                                 o.mnemonic, vi));
            }
            // (c) every wire / extra-result `wordIndex` must address a
            //     word the template actually emits — an out-of-range
            //     index would write into a non-existent word at encode
            //     time (the walker bounds-checks too; surface at load).
            for (std::size_t wi = 0; wi < v.wires.size(); ++wi) {
                if (v.wires[wi].wordIndex >= nWords) {
                    fail(std::format("/opcodes/{}/encoding/variants/{}/wires/{}/wordIndex", i, vi, wi),
                         std::format("opcode '{}' variant {}: wire wordIndex "
                                     "{} addresses a word beyond the template's "
                                     "{} word(s)",
                                     o.mnemonic, vi, v.wires[wi].wordIndex, nWords));
                }
            }
            for (std::size_t ei = 0; ei < v.extraResultSlots.size(); ++ei) {
                if (v.extraResultSlots[ei].wordIndex >= nWords) {
                    fail(std::format("/opcodes/{}/encoding/variants/{}/extraResultSlots/{}/wordIndex", i, vi, ei),
                         std::format("opcode '{}' variant {}: extra-result "
                                     "wordIndex {} addresses a word beyond the "
                                     "template's {} word(s)",
                                     o.mnemonic, vi,
                                     v.extraResultSlots[ei].wordIndex, nWords));
                }
            }

            // ── Convergence-fix A (validate half): slot uniqueness ────
            // Two writers to the same single-writer slot WITHIN ONE WORD
            // would silently overwrite each other at encode time. Multi-
            // Imm32 wires are legal (a future variant could append two
            // immediates in sequence). All other slots are single-writer.
            // Convergence-fix D extension: fixed32 (Rd/Rn/Rm) slots are
            // equally single-writer.
            //
            // D-AS4-3: uniqueness is PER-WORD. The SAME slot kind
            // legitimately appears once in EACH word of a multi-word
            // macro — AArch64 `lea` writes Rd into word 0 (ADRP) AND
            // word 1 (ADD), and the symbol-patch marker into both words.
            // Those are distinct byte positions, not a double-write, so
            // the tracker is keyed by (wordIndex, slotKind), and a
            // collision is a violation only WITHIN one word.
            {
                auto const isMultiWriterSlot = [](EncodingSlotKind s) noexcept {
                    // D-CSUBSET-WHILE-LOOP-SUBSTRATE (step 13.5 cycle 1):
                    // BlockRel32 admits multiple wires per variant (jcc
                    // emits TWO trailing rel32 placeholders — taken
                    // target + fallthrough target — separated by the
                    // wire-1's `prefixOpcodeBytes: [0xE9]` bridge). Each
                    // wire targets a distinct LIR operand, so there's
                    // no "silently overwrite" risk; the encoder appends
                    // them in declaration order to the output stream.
                    return s == EncodingSlotKind::Imm32
                        || s == EncodingSlotKind::BlockRel32;
                };
                // Per-word slot tracking — one bitset PER WORD. Each
                // bitset sized from the shared `kEncodingSlotKindCount`
                // so adding a slot to the enum table is the SAME change,
                // no manual size update.
                std::vector<std::array<bool, kEncodingSlotKindCount>>
                    sawSlot(nWords);
                auto const markOrFail = [&](EncodingSlotKind slot,
                                            std::uint8_t word,
                                            std::string const& kindLabel,
                                            std::string const& path) {
                    if (isMultiWriterSlot(slot)) return;
                    auto const idx = static_cast<std::size_t>(slot);
                    // Out-of-range word/slot already reported by the
                    // wordIndex-bounds + enum invariants — skip here to
                    // avoid OOB access + a duplicate diagnostic.
                    if (word >= sawSlot.size() || idx >= kEncodingSlotKindCount)
                        return;
                    if (sawSlot[word][idx]) {
                        fail(path,
                             std::format("opcode '{}' variant {}: "
                                         "{} writer targets '{}' in word {} — "
                                         "the second writer would silently "
                                         "overwrite the first at encode time",
                                         o.mnemonic, vi, kindLabel,
                                         encodingSlotKindName(slot), word));
                    } else {
                        sawSlot[word][idx] = true;
                    }
                };
                if (v.resultSlot.has_value()) {
                    markOrFail(*v.resultSlot, /*word=*/0, "result",
                               std::format("/opcodes/{}/encoding/variants/{}/resultSlot", i, vi));
                }
                for (auto const& e : v.extraResultSlots) {
                    markOrFail(e.slotKind, e.wordIndex, "extra result",
                               std::format("/opcodes/{}/encoding/variants/{}/extraResultSlots", i, vi));
                }
                for (auto const& w : v.wires) {
                    markOrFail(w.slotKind, w.wordIndex, "wire",
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
            // Destination-bearing slot taxonomy:
            //   * ModRmReg — x86 register-direct destination (mod=11
            //     + reg field carries the dest's hwEncoding).
            //   * ModRmRm — x86 register-direct destination via rm
            //     field (the `requires2Address: true` 2-addr shape
            //     where operand 0 IS the dest).
            //   * ModRmRmMem — x86 memory destination (mod=10 + rm
            //     field carries the base reg). The 2-addr shape's
            //     arithmetic-to-memory form (e.g. a future `add
            //     r/m64, imm32` with a memory dest) routes here.
            //   * Rd — fixed32 destination.
            //
            // Disp32Mem and MemBaseScale are NEVER destinations:
            // Disp32Mem carries the displacement VALUE (an immediate);
            // MemBaseScale carries the shape (validates scale==1).
            // Excluding them here is intentional.
            //
            // D-LK10-2 post-fold (silent-failure F-G1): omitting
            // ModRmRmMem would cause future memory-destination 2-addr
            // forms to silently fail rule-G validation with a
            // misleading "destination would be silently dropped"
            // message.
            auto const isDestSlot = [](EncodingSlotKind s) noexcept {
                return s == EncodingSlotKind::ModRmReg
                    || s == EncodingSlotKind::ModRmRm
                    || s == EncodingSlotKind::ModRmRmMem
                    || s == EncodingSlotKind::Rd;
            };
            bool const has2AddrDestWire = o.requires2Address
                && std::any_of(v.wires.begin(), v.wires.end(),
                               [&](TargetEncodingWire const& w) {
                                   return w.index == 0
                                       && isDestSlot(w.slotKind);
                               });
            // D-ASM-AARCH64-FRAME-OFFSET-BEYOND-16MIB: a variant that wires
            // an operand to the `Imm32MovzMovk` slot materializes its value
            // into a scratch register (MOVZ/MOVK) and applies an EXTENDED-
            // register operation whose destination is BAKED into the
            // template's operation word (the sp-adjust `sub sp,sp,x16` /
            // `add sp,sp,x16` bake Rd=sp=31 — there is no result-register
            // FIELD for a `resultSlot` to fill). The result is architecturally
            // implicit, exactly like the `isCall` rationale below: the byte
            // encoding carries it via the baked register, not a routed slot.
            // (The lea form of this slot DOES route its dest via resultSlot/
            // extraResultSlots — scratch-free into Xd — so this exemption
            // only relaxes the sp-adjust form that has no dest field to fill.)
            bool const hasMovzMovkBakedDest =
                std::any_of(v.wires.begin(), v.wires.end(),
                            [&](TargetEncodingWire const& w) {
                                return w.slotKind
                                    == EncodingSlotKind::Imm32MovzMovk;
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
                && !hasMovzMovkBakedDest
                && !o.isCall) {
                fail(std::format("/opcodes/{}/encoding/variants/{}", i, vi),
                     std::format("opcode '{}' variant {}: opcode has "
                                 "`result='{}'` but variant declares "
                                 "none of `resultSlot` / "
                                 "`template.modrmRegExt` / "
                                 "(`requires2Address` + wire on operand 0) / "
                                 "(`imm32.movzmovk` baked-dest) "
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
        // Two variants with identical `operandKinds` AND the same
        // `guard.width` would first-match silently win — the second is
        // unreachable. FC3 c2 (D-CSUBSET-32BIT-ALU-FORMS) extends the
        // identity to the (operandKinds, width) pair AND rejects the
        // AMBIGUOUS MIX: a width-absent (match-any) variant alongside a
        // width-keyed same-kind sibling. The match-any variant either
        // shadows the keyed one (if first) or silently absorbs the
        // widths the keyed one does NOT declare (if last) — both are
        // config bugs; the author must key EVERY same-kind sibling
        // once any of them is width-discriminated.
        // D-ASM-AARCH64-FRAME-OFFSET-BEYOND-IMM12: the immediate-magnitude
        // axis is a SECOND disambiguator. Two same-operandKinds variants
        // whose [immMin,immMax] ranges are DISJOINT are unambiguously
        // selectable by VALUE (a frame ≤4095 → the single-word imm12
        // variant; >4095 → the shifted-imm12 variant), so they do NOT
        // overlap — regardless of width. Only when the imm-ranges OVERLAP
        // (or both are absent ⇒ both match-any) does the width check below
        // decide reachability. Effective range: absent immMin ⇒ 0, absent
        // immMax ⇒ UINT32_MAX (the match-any bounds).
        auto const effLo = [](TargetEncodingVariant const& v) -> std::uint32_t {
            return v.immMin.value_or(0u);
        };
        auto const effHi = [](TargetEncodingVariant const& v) -> std::uint32_t {
            return v.immMax.value_or(std::numeric_limits<std::uint32_t>::max());
        };
        for (std::size_t a = 0; a < o.encoding.variants.size(); ++a) {
            for (std::size_t b = a + 1; b < o.encoding.variants.size(); ++b) {
                auto const& va = o.encoding.variants[a];
                auto const& vb = o.encoding.variants[b];
                if (va.operandKinds != vb.operandKinds) continue;
                // D-AS4-ARM64-NEGATIVE-DISP-LEA-NATIVE-SUB: the SIGN axis is a
                // THIRD disambiguator. Two same-operandKinds variants that
                // differ in `negMemoffset` route on DISJOINT sign domains (one
                // matches only a negative memoffset, the other only a
                // non-negative one), so neither shadows the other — regardless
                // of width or imm-range. The effLo/effHi magnitude ranges below
                // are per-sign (a |disp| bound on the negative half, a disp
                // bound on the non-negative half); comparing them ACROSS the
                // sign boundary is meaningless, so skip the overlap check when
                // the sign axis already separates them.
                if (va.negMemoffset != vb.negMemoffset) continue;
                // Disjoint imm-ranges ⇒ value-distinguishable, never a
                // shadow. `[loA,hiA]` and `[loB,hiB]` are disjoint iff
                // hiA < loB or hiB < loA.
                if (effHi(va) < effLo(vb) || effHi(vb) < effLo(va)) {
                    continue;
                }
                if (va.guardWidthBits == vb.guardWidthBits) {
                    fail(std::format("/opcodes/{}/encoding/variants/{}", i, b),
                         std::format("opcode '{}': variant {} has the "
                                     "same `operandKinds` and `width` as "
                                     "variant {} — first-match dispatch "
                                     "makes variant {} unreachable",
                                     o.mnemonic, b, a, b));
                } else if (va.guardWidthBits == 0
                           || vb.guardWidthBits == 0) {
                    fail(std::format("/opcodes/{}/encoding/variants/{}", i, b),
                         std::format("opcode '{}': variants {} and {} "
                                     "share `operandKinds` but mix a "
                                     "width-keyed guard with a width-"
                                     "absent (match-any) one — key every "
                                     "same-kind sibling once any is "
                                     "width-discriminated "
                                     "(D-CSUBSET-32BIT-ALU-FORMS)",
                                     o.mnemonic, a, b));
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

        // Implicit-register-constraint resolution + per-field
        // validation happens at LOAD time in target_schema_json.cpp
        // (after the register table is populated). validate() stays
        // const + cross-opcode-only — see the loader's
        // "Implicit-register-constraint resolution + validation"
        // block for the resolution + per-field reject arms.
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

    // ── registerClassOps (FC2 Part B) ───────────────────────────
    // Every DECLARED per-class mnemonic must resolve to an opcode row
    // — a typo here would otherwise surface per-instruction at the
    // consumer (`regClassOpOpcode` returning nullopt looks identical
    // to "op not declared"); load-time is the one place the mistake
    // is distinguishable from trigger-disciplined omission.
    for (std::size_t ci = 0; ci < registerClassOps.size(); ++ci) {
        auto const& row = registerClassOps[ci];
        if (!row.declared) continue;
        auto const clsName =
            targetRegClassName(static_cast<TargetRegClass>(ci));
        auto checkOp = [&](char const* field, std::string const& name) {
            if (name.empty()) return;  // omitted op — consumer fails loud
            if (mnemonicIndex.find(name) == mnemonicIndex.end()) {
                fail(std::format("/registerClassOps/{}/{}", clsName, field),
                     std::format("registerClassOps class '{}': '{}' "
                                 "mnemonic '{}' does not resolve to any "
                                 "opcode row",
                                 clsName, field, name));
            }
        };
        checkOp("move",  row.move);
        checkOp("load",  row.load);
        checkOp("store", row.store);
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
        // Rule (e) — D-LK6-1 closure coherence: non-Linear formulas
        // encode pcRelative + addendBias + widthBytes intrinsically.
        // The JSON loader gates this at load time (and the shipped
        // configs go through that gate), but a programmatically-
        // constructed schema (test fixture, future variant reshape,
        // fuzz harness) could bypass the loader and reach `validate()`
        // with an incoherent row. The kernel would then silently
        // misapply (e.g. `widthBytes=8` on a non-Linear arm patches
        // 8 LE bytes into a 32-bit instruction word; `pcRelative=true`
        // on Call26 double-subtracts P). Defense-in-depth here so
        // every schema reaching the kernel has rule (e) enforced.
        // (silent-failure audit CRITICAL-2 post-fold #2.)
        if (r.formulaKind != RelocFormulaKind::Linear) {
            if (r.widthBytes != 4) {
                fail(std::format("/relocations/{}/widthBytes", i),
                     std::format("relocation '{}': non-Linear formula "
                                 "'{}' requires widthBytes=4 (ARM64 "
                                 "instruction word); got {}.",
                                 r.name,
                                 relocFormulaName(r.formulaKind),
                                 r.widthBytes));
            }
            if (r.pcRelative) {
                fail(std::format("/relocations/{}/pcRelative", i),
                     std::format("relocation '{}': non-Linear formula "
                                 "'{}' encodes PC-relativity "
                                 "intrinsically; 'pcRelative' must be "
                                 "false.",
                                 r.name,
                                 relocFormulaName(r.formulaKind)));
            }
            if (r.addendBias != 0) {
                fail(std::format("/relocations/{}/addendBias", i),
                     std::format("relocation '{}': non-Linear formula "
                                 "'{}' encodes any addend bias "
                                 "intrinsically; 'addendBias' must be 0.",
                                 r.name,
                                 relocFormulaName(r.formulaKind)));
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
        // D-CSUBSET-VLA (C1b): the frame-pointer register (rbp / x29), when
        // declared, must resolve to a GPR (it becomes a fixed frame base).
        if (cc.framePointer.has_value()) {
            std::span<std::string const> fpRefs{&cc.framePointer->name, 1};
            checkRefs(i, "framePointer", fpRefs, TargetRegClass::GPR);
        }
        // FC7 (D-FC7-STRUCT-BY-VALUE-ARG-RETURN): the sret indirect-result
        // register (AAPCS64 x8), when declared, must resolve to a GPR.
        if (cc.indirectResultRegister.has_value()) {
            std::span<std::string const> irRefs{&cc.indirectResultRegister->name, 1};
            checkRefs(i, "indirectResultRegister", irRefs, TargetRegClass::GPR);
        }
        // FC7 C2: a CC declaring a real aggregate-classification STRATEGY must
        // also declare a non-zero `aggregateMaxRegBytes` — otherwise every
        // by-value aggregate classifies BY REFERENCE (`size <= 0` is always
        // false), a silent wrong-but-self-consistent ABI. Fail loud at load so a
        // future target.json that wires the strategy but forgets the budget is
        // caught instead of quietly mis-passing every struct.
        if (cc.aggregateClassification != AggregateClassKind::None
            && cc.aggregateMaxRegBytes == 0) {
            fail(std::format("/callingConventions/{}/aggregateMaxRegBytes", i),
                 std::format("callingConvention '{}' declares aggregateClassification "
                             "'{}' but aggregateMaxRegBytes is 0 — a real strategy "
                             "needs a non-zero register budget",
                             cc.name,
                             aggregateClassKindName(cc.aggregateClassification)));
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
                              || cc.stackAlignment   != 0
                              // D-LK10-ENTRY-TRAMP-PROLOGUE: a cc
                              // declaring ONLY entryStackPointerBias
                              // (with all other ABI fields zeroed)
                              // would otherwise silently bypass the
                              // `< stackAlignment` check below
                              // (type-design C1 at the standing
                              // audit). Include it in the trigger.
                              || cc.entryStackPointerBias != 0
                              // D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY:
                              // same protection for the new
                              // callPushBytes field (a cc declaring
                              // only callPushBytes would bypass the
                              // multiple-of-alignment check below).
                              || cc.callPushBytes != 0
                              // D-WIN64-LARGE-FRAME-STACK-PROBE: same
                              // protection for stackProbePageBytes (a cc
                              // declaring only it would bypass the
                              // power-of-two check below).
                              || cc.stackProbePageBytes != 0;
        if (hasAbiInfo) {
            // D-WIN64-LARGE-FRAME-STACK-PROBE: the probe page size IS the
            // loop's per-iteration step; if it is not a power of two the
            // guard-page geometry is wrong and a typo'd 4000 would
            // silently skip a guard page. Independent of stackAlignment
            // (checked unconditionally so a malformed stackAlignment does
            // not also mask this).
            if (cc.stackProbePageBytes != 0
                && !isPow2Nonzero(cc.stackProbePageBytes)) {
                fail(std::format("/callingConventions/{}/stackProbePageBytes", i),
                     std::format("callingConvention '{}': stackProbePageBytes ({}) must be a power of two",
                                 cc.name, cc.stackProbePageBytes));
            }
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
                // D-LK10-ENTRY-TRAMP-PROLOGUE: entryStackPointerBias
                // is an offset INTO the alignment quantum, NOT a
                // multiple of it — must be strictly < stackAlignment.
                if (cc.entryStackPointerBias >= cc.stackAlignment) {
                    fail(std::format(
                             "/callingConventions/{}/entryStackPointerBias", i),
                         std::format(
                             "callingConvention '{}': entryStackPointerBias "
                             "({}) must be < stackAlignment ({}) — bias "
                             "is an offset INTO the alignment quantum",
                             cc.name, cc.entryStackPointerBias,
                             cc.stackAlignment));
                }
                // D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY: callPushBytes is
                // ALSO an offset INTO the alignment quantum (the
                // CALL instruction's RSP delta is bounded by the
                // architecture's pointer width, which divides
                // stackAlignment). Strict-less-than matches
                // entryStackPointerBias's invariant.
                if (cc.callPushBytes >= cc.stackAlignment) {
                    fail(std::format(
                             "/callingConventions/{}/callPushBytes", i),
                         std::format(
                             "callingConvention '{}': callPushBytes "
                             "({}) must be < stackAlignment ({}) — bias "
                             "is an offset INTO the alignment quantum",
                             cc.name, cc.callPushBytes,
                             cc.stackAlignment));
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
