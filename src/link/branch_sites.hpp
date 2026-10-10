#pragma once

#include "asm/asm.hpp"                    // Relocation
#include "core/types/target_schema.hpp"   // TargetSchema, TargetEncodingVariant, relocFormulaFacts
#include "link/object_format_schema.hpp"  // ObjectFormatSchema::relocationByKind (`isCall`)

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// ── IS THE INSTRUCTION A RELOCATION PATCHES A BRANCH? ─────────────────────
// P69 review M1 case (c) and MINOR 8 (D-LK-PE-IMPORT-ADDRESS-SLOT-RESIDUE).
//
// A PC-relative reference to an import means one of two things, and a
// relocatable object does not always say which. A BRANCH — a call, a jump, a
// conditional jump — reaches the import's CALL ENTRY, which is what a branch
// wants. Any other instruction (`leaq puts(%rip)`) computes an ADDRESS, and the
// only address a displacement can reach is that same call entry — on an image
// whose call entry is not the import's address, a different value from the one
// every other reference answers.
//
// The FORMAT row may state the role (`isCall`: R_X86_64_PLT32, R_AARCH64_CALL26).
// Where it does not, x86-64 COFF spells both with ONE wire type
// (IMAGE_REL_AMD64_REL32), and an ELF object may still carry a call as
// R_X86_64_PC32 (binutils before 2.31). Then the INSTRUCTION is read: the bytes
// that end where the relocated field begins are a branch opcode the TARGET
// declares — every opcode it marks `isCall` or as a branch terminator, in each
// encoding variant whose displacement field immediately follows its opcode
// bytes (no ModR/M; a condition code OR'd into the last opcode byte matches any
// condition). GNU ld reads the same bytes for the same question
// (`elf_x86_64_check_relocs`: 0xe8 call, 0xe9 jmp, 0x0f 0x8x jcc). Only a
// PLAIN-FIELD formula is read this way: an instruction-word formula patches a
// field inside its instruction, and the ISAs that use one give a branch its own
// relocation, whose format row states the role.
namespace dss::linker {

class BranchSites {
public:
    explicit BranchSites(TargetSchema const& target) {
        for (auto const& op : target.opcodes()) {
            if (!op.isCall && op.terminatorKind == TargetTerminatorKind::None) continue;
            if (op.encoding.shape != TargetEncodingShape::X86Variable) continue;
            for (auto const& v : op.encoding.variants) {
                if (v.tmpl.opcodeBytes.empty() || v.wires.empty()) continue;
                bool displacementFollowsOpcode = true;
                bool hasDisplacement           = false;
                for (auto const& w : v.wires) {
                    switch (w.slotKind) {
                        case EncodingSlotKind::Disp32:
                        case EncodingSlotKind::BlockRel32:
                            hasDisplacement = true;
                            break;
                        default:
                            // A ModR/M, SIB, immediate or memory field sits
                            // between the opcode and any displacement: not a
                            // direct branch's shape.
                            displacementFollowsOpcode = false;
                            break;
                    }
                }
                if (!hasDisplacement || !displacementFollowsOpcode) continue;
                Pattern p;
                p.bytes                    = v.tmpl.opcodeBytes;
                p.lastByteCarriesCondition = v.tmpl.condCodeFromPayload;
                addPattern(std::move(p));
                // A two-target conditional branch also writes the fall-through
                // jump before its second field (`prefixOpcodeBytes`).
                for (auto const& w : v.wires) {
                    if (!w.prefixOpcodeBytes.empty()) {
                        addPattern(Pattern{w.prefixOpcodeBytes, false});
                    }
                }
            }
        }
    }

    // TRUE iff the bytes of `code` that END at `fieldOffset` are a declared
    // branch opcode — the instruction whose displacement field starts there is a
    // branch.
    [[nodiscard]] bool branchEndsAt(std::span<std::uint8_t const> code,
                                    std::uint32_t fieldOffset) const noexcept {
        if (fieldOffset > code.size()) return false;
        for (auto const& p : patterns_) {
            std::size_t const n = p.bytes.size();
            if (n > fieldOffset) continue;
            std::size_t const at = fieldOffset - n;
            bool match = true;
            for (std::size_t i = 0; i < n && match; ++i) {
                std::uint8_t const want = p.bytes[i];
                std::uint8_t const got  = code[at + i];
                bool const condByte = p.lastByteCarriesCondition && i + 1 == n;
                match = condByte ? ((got & 0xF0u) == (want & 0xF0u)) : (got == want);
            }
            if (match) return true;
        }
        return false;
    }

    [[nodiscard]] bool empty() const noexcept { return patterns_.empty(); }

private:
    struct Pattern {
        std::vector<std::uint8_t> bytes;
        bool                      lastByteCarriesCondition = false;
    };
    void addPattern(Pattern p) {
        auto const same = [&](Pattern const& q) {
            return q.bytes == p.bytes && q.lastByteCarriesCondition == p.lastByteCarriesCondition;
        };
        if (std::none_of(patterns_.begin(), patterns_.end(), same)) patterns_.push_back(std::move(p));
    }
    std::vector<Pattern> patterns_;
};

// The ROLE of a PC-relative CODE reference: TRUE when it reaches its target as
// a branch. The format row's statement wins; otherwise a plain field is read
// (see above); an instruction-word formula whose row states no call is an
// address computation (`adrp`/`add`, `adr`).
[[nodiscard]] inline bool isBranchReference(Relocation const&             rel,
                                            std::span<std::uint8_t const> code,
                                            TargetSchema const&           target,
                                            ObjectFormatSchema const&     format,
                                            BranchSites const&            sites) noexcept {
    if (auto const* row = format.relocationByKind(rel.kind); row != nullptr && row->isCall) {
        return true;
    }
    auto const* tri = target.relocationInfo(rel.kind);
    if (tri == nullptr) return false;
    if (!relocFormulaFacts(tri->formulaKind).patchesPlainField) return false;
    return sites.branchEndsAt(code, rel.offset);
}

}  // namespace dss::linker
