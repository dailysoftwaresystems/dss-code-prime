#pragma once

// ★★★ A BLOCK THAT DATA NAMES IS NAMED BY MIR→LIR's ID, AND BOUND AGAINST THE FINAL MODULE'S
// (D-LIR-DESCRIPTOR-BLOCK-IDS-SHIFTED-BY-A-BLOCK-INSERTING-PASS).
//
// Three MIR→LIR products name a LIR block from DATA rather than from an instruction, so no rebuild ever
// remaps them the way `lir_pass_util::remapBlockRef` remaps an operand:
//   * a dense switch's `JumpTableDescriptor` — every slot's target block, and its per-block symbols;
//   * a static initializer's `&&label` — `LirBlockSymbolBinding`;
//   * a `__try` region's `SehScopeDescriptor` — the guarded body's first and last block, and the handler.
// The pipeline binds each after `assemble()`, against the FINAL module's `blockByteOffsets`. A LIR block id
// is a position in a module-wide arena, so a pass that inserts ONE block renumbers every block after it, in
// its own function and in every later one, and an id nobody translated lands on another block's bytes.
// ✔MEASURED 2026-10-07 on the build at 71648598, through `expandAsmRegions` — the first pass to insert
// blocks (an inline-asm template's labels): a dense switch placed after a function whose template defines a
// label dispatched to the wrong cases on pe and elf, a static `&&label` table there was refused as having no
// byte offset, and a `__try` there let its access violation escape. gcc, clang and mingw gcc run the same
// programs to 42. The guard-page walk of a runtime stack descent (`lir_callconv.cpp`) is the second such pass.
//
// THE RULE EVERY REBUILD BETWEEN MIR→LIR AND THE BINDING OBEYS — checked here, per module:
//   * a pass that inserts blocks PUBLISHES its block ENTRY IMAGE (for every source block, the block of its
//     output where that block's instructions begin) and lays each source block's pieces out from that entry
//     up to the next source block's entry: `expandAsmRegions` and `materializeCallingConvention`;
//   * a pass that publishes none is PROVED block-preserving: the same functions, each with the same block
//     range, every block with the same successors — `lowerWideCallArgs`, `rewriteWithAllocation`,
//     `legalizeTwoAddress`, `runLirPeephole`. One that stops preserving is refused here, in every module, by
//     name, until it publishes its image;
//   * `assemble()` is not a rebuild: it keys each offset by the id of the block it is laying out, in one walk
//     in layout order, branch relaxation and islands included. `verifyBlockOffsetsFollowLayout` checks
//     exactly that — every block of every function has an offset, and they never decrease in layout order.
// A descriptor block id with no image is REFUSED, naming the descriptor and the id. There is no fallback:
// an untranslated id is a wrong case, a refused program or an unguarded `__try`, never a near miss.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "lir/lir.hpp"
#include "lir/lowering/mir_to_lir.hpp"

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dss {

// One LIR rebuild between MIR→LIR and the binding, in pipeline order: `in` of each step is `out` of the
// one before it.
struct LirBlockRebuild {
    std::string_view pass;           // the pass, as the diagnostics name it
    Lir const*       in  = nullptr;
    Lir const*       out = nullptr;
    // EMPTY: the pass publishes no image, so it must be PROVED block-preserving. Otherwise indexed by `in`'s
    // block arena (`LirBlockId.v`; slot 0, the arena sentinel, holds 0): the `.v` of the `out` block where
    // that block's instructions begin.
    std::span<std::uint32_t const> entryImage;
};

namespace lir_descriptor_blocks_detail {

// What one source block became: the first and the last of its pieces, in layout order.
struct Image {
    std::uint32_t first = 0;
    std::uint32_t last  = 0;
};

// Every refusal below is the guarantee of the row this header's opening comment names breaking; the id stays
// in that comment, never in a message (an anchor id is bookkeeping, and a message is compiler output).
inline void refuse(DiagnosticReporter& reporter, std::string message) {
    ParseDiagnostic d;
    d.code     = DiagnosticCode::L_SideStructureIndexDangling;
    d.severity = DiagnosticSeverity::Error;
    d.actual   = std::move(message);
    reporter.report(std::move(d));
}

// True iff `v` is a block of function #`funcIndex` of `lir` (a function's blocks are one contiguous run of
// the arena).
[[nodiscard]] inline bool isBlockOf(Lir const& lir, std::size_t funcIndex, std::uint32_t v) {
    if (funcIndex >= lir.moduleFuncCount()) return false;
    LirFuncId const fn = lir.funcAt(static_cast<std::uint32_t>(funcIndex));
    std::uint32_t const n = lir.funcBlockCount(fn);
    if (n == 0) return false;
    std::uint32_t const first = lir.funcBlockAt(fn, 0).v;
    return v >= first && v - first < n;
}

// The image of every block of `step.in`, indexed by its block arena. nullopt = refused (reported).
[[nodiscard]] inline std::optional<std::vector<Image>>
imagesOf(LirBlockRebuild const& step, DiagnosticReporter& reporter) {
    Lir const& in  = *step.in;
    Lir const& out = *step.out;
    if (in.moduleFuncCount() != out.moduleFuncCount()) {
        refuse(reporter, std::format(
            "LIR pass '{}' turned a module of {} function(s) into one of {}, so no block a jump table, a "
            "static label table or a __try names can be followed through it",
            step.pass, in.moduleFuncCount(), out.moduleFuncCount()));
        return std::nullopt;
    }
    bool const published = !step.entryImage.empty();
    if (published && step.entryImage.size() != in.blockCount()) {
        refuse(reporter, std::format(
            "LIR pass '{}' published a block image of {} entr(ies) for a module of {} block slot(s)",
            step.pass, step.entryImage.size(), in.blockCount()));
        return std::nullopt;
    }
    std::vector<Image> images(in.blockCount());
    for (std::uint32_t fi = 0; fi < in.moduleFuncCount(); ++fi) {
        LirFuncId const inFn  = in.funcAt(fi);
        LirFuncId const outFn = out.funcAt(fi);
        std::uint32_t const n = in.funcBlockCount(inFn);
        std::uint32_t const m = out.funcBlockCount(outFn);
        if (n == 0 && m == 0) continue;
        if (n == 0 || m == 0) {
            refuse(reporter, std::format(
                "LIR pass '{}' turned function #{} of {} block(s) into one of {}",
                step.pass, fi, n, m));
            return std::nullopt;
        }
        std::uint32_t const outFirst = out.funcBlockAt(outFn, 0).v;
        std::uint32_t const outLast  = out.funcBlockAt(outFn, m - 1).v;
        if (!published) {
            // THE PROOF. Ids are positions, so the same range plus the same successors, block by block, is
            // the identity — and a pass that reordered, merged or split a block fails one of the three.
            bool same = n == m && in.funcBlockAt(inFn, 0).v == outFirst;
            std::uint32_t differs = 0;
            for (std::uint32_t k = 0; same && k < n; ++k) {
                LirBlockId const b = in.funcBlockAt(inFn, k);
                auto const si = in.blockSuccessors(b);
                auto const so = out.blockSuccessors(out.funcBlockAt(outFn, k));
                same = si.size() == so.size();
                for (std::size_t s = 0; same && s < si.size(); ++s) same = si[s].v == so[s].v;
                if (!same) differs = b.v;
            }
            if (!same) {
                refuse(reporter, std::format(
                    "LIR pass '{}' rebuilt function #{} with a different block structure ({} block(s) from "
                    "block {}, then {} from block {}{}) and publishes no block image, so a block a jump "
                    "table, a static label table or a __try names cannot be followed through it",
                    step.pass, fi, n, in.funcBlockAt(inFn, 0).v, m, outFirst,
                    differs != 0 ? std::format("; block {}'s successors differ", differs) : std::string{}));
                return std::nullopt;
            }
            for (std::uint32_t k = 0; k < n; ++k) {
                std::uint32_t const v = in.funcBlockAt(inFn, k).v;
                images[v] = Image{v, v};
            }
            continue;
        }
        // A PUBLISHED image must lay the function's blocks out in order, from the output function's first
        // block, each entry inside the output function — then a block's pieces are exactly the run from its
        // entry up to the next block's entry.
        std::uint32_t prev = 0;
        for (std::uint32_t k = 0; k < n; ++k) {
            std::uint32_t const v = in.funcBlockAt(inFn, k).v;
            std::uint32_t const e = step.entryImage[v];
            bool const inside = e >= outFirst && e <= outLast;
            bool const ordered = k == 0 ? e == outFirst : e > prev;
            if (!inside || !ordered) {
                refuse(reporter, std::format(
                    "LIR pass '{}' published entry {} for block {} (#{} of function #{}), but that function's "
                    "blocks occupy {}..{}{}",
                    step.pass, e, v, k, fi, outFirst, outLast,
                    k == 0 ? std::string{" and its first block must enter at the first of them"}
                           : std::format(" and the block before it entered at {}", prev)));
                return std::nullopt;
            }
            prev = e;
        }
        for (std::uint32_t k = 0; k < n; ++k) {
            std::uint32_t const v = in.funcBlockAt(inFn, k).v;
            std::uint32_t const e = step.entryImage[v];
            std::uint32_t const last =
                k + 1 < n ? step.entryImage[in.funcBlockAt(inFn, k + 1).v] - 1 : outLast;
            images[v] = Image{e, last};
        }
    }
    return images;
}

}  // namespace lir_descriptor_blocks_detail

// Translate every descriptor's block ids from the module MIR→LIR produced (`steps.front().in`) to the final
// one (`steps.back().out`). An ENTRY — a slot's target, a label, a scope's first block, its handler — goes to
// its block's FIRST piece; a scope's LAST block goes to its LAST piece, because the guarded range must still
// end after everything that block became (the pipeline ends it at the next block laid out). False when
// anything is refused (reported); the descriptors are then unspecified and must not be bound.
[[nodiscard]] inline bool
translateDescriptorBlockIds(std::span<LirBlockRebuild const>   steps,
                            std::vector<JumpTableDescriptor>&   jumpTables,
                            std::vector<LirBlockSymbolBinding>& labelBindings,
                            std::vector<SehScopeDescriptor>&    sehScopes,
                            DiagnosticReporter&                 reporter) {
    using namespace lir_descriptor_blocks_detail;
    if (steps.empty()) return true;   // nothing was rebuilt: MIR→LIR's ids are the final ones
    for (std::size_t i = 0; i < steps.size(); ++i) {
        if (steps[i].in == nullptr || steps[i].out == nullptr
            || (i + 1 < steps.size() && steps[i].out != steps[i + 1].in)) {
            refuse(reporter, std::format(
                "the LIR rebuild chain is broken at pass '{}': each pass must take the module the one before "
                "it produced", steps[i].pass));
            return false;
        }
    }
    std::vector<std::vector<Image>> images;
    images.reserve(steps.size());
    for (auto const& s : steps) {
        auto im = imagesOf(s, reporter);
        if (!im.has_value()) return false;
        images.push_back(std::move(*im));
    }
    Lir const& produced = *steps.front().in;
    Lir const& finalLir    = *steps.back().out;

    // One id through every step. `what` names the descriptor for the refusal.
    auto const translate = [&](std::string const& what, std::size_t funcIndex, std::uint32_t v,
                               bool lastPiece) -> std::optional<std::uint32_t> {
        if (!isBlockOf(produced, funcIndex, v)) {
            refuse(reporter, std::format(
                "{} names block {} of function #{}, which is not a block of that function in the module "
                "MIR→LIR produced, so it has no image in the final LIR", what, v, funcIndex));
            return std::nullopt;
        }
        std::uint32_t id = v;
        for (std::size_t i = 0; i < steps.size(); ++i) {
            Image const im = id < images[i].size() ? images[i][id] : Image{};
            std::uint32_t const next = lastPiece ? im.last : im.first;
            if (next == 0) {
                refuse(reporter, std::format(
                    "{} names block {} of function #{}, and LIR pass '{}' gave block {} no image, so it has "
                    "none in the final LIR", what, v, funcIndex, steps[i].pass, id));
                return std::nullopt;
            }
            id = next;
        }
        if (!isBlockOf(finalLir, funcIndex, id)) {
            refuse(reporter, std::format(
                "{} names block {} of function #{}, whose image {} is not a block of that function in the "
                "final LIR", what, v, funcIndex, id));
            return std::nullopt;
        }
        return id;
    };

    for (auto& desc : jumpTables) {
        std::string const what = std::format("the jump table {{ {} }}", desc.tableSymbol.v);
        for (auto& [blockV, slot] : desc.slotBindings) {
            (void)slot;
            auto const id = translate(what, desc.funcIndex, blockV, /*lastPiece=*/false);
            if (!id.has_value()) return false;
            blockV = *id;
        }
        std::unordered_map<std::uint32_t, SymbolId> symbols;
        symbols.reserve(desc.blockSymbols.size());
        for (auto const& [blockV, sym] : desc.blockSymbols) {
            auto const id = translate(what, desc.funcIndex, blockV, /*lastPiece=*/false);
            if (!id.has_value()) return false;
            symbols.emplace(*id, sym);
        }
        desc.blockSymbols = std::move(symbols);
    }
    for (auto& b : labelBindings) {
        std::string const what = std::format("the static label-address binding {{ {} }}", b.symbol.v);
        auto const id = translate(what, b.funcIndex, b.lirBlockV, /*lastPiece=*/false);
        if (!id.has_value()) return false;
        b.lirBlockV = *id;
    }
    for (std::size_t k = 0; k < sehScopes.size(); ++k) {
        auto& s = sehScopes[k];
        std::string const what = std::format("__try scope #{}", k);
        auto const begin   = translate(what + " (its first block)", s.funcIndex, s.beginLirBlockV, false);
        if (!begin.has_value()) return false;
        auto const last    = translate(what + " (its last block)", s.funcIndex, s.endLirBlockV, true);
        if (!last.has_value()) return false;
        auto const handler = translate(what + " (its handler)", s.funcIndex, s.handlerLirBlockV, false);
        if (!handler.has_value()) return false;
        s.beginLirBlockV   = *begin;
        s.endLirBlockV     = *last;
        s.handlerLirBlockV = *handler;
    }
    return true;
}

// After `assemble()`: every block of every function of `finalLir` has a byte offset, and the offsets never
// decrease in layout order — the assembler laid the blocks out as the LIR orders them and keyed each by its
// own id, so the translated ids name the bytes they mean. `functions[i].blockByteOffsets` is function #i's
// (`AssembledFunction`; a template so this header needs nothing of the assembler). False = refused.
template <class AssembledFunctions>
[[nodiscard]] bool verifyBlockOffsetsFollowLayout(Lir const& finalLir, AssembledFunctions const& functions,
                                                  DiagnosticReporter& reporter) {
    using namespace lir_descriptor_blocks_detail;
    if (functions.size() != finalLir.moduleFuncCount()) {
        refuse(reporter, std::format(
            "the assembler produced {} function(s) for a final LIR of {}",
            functions.size(), finalLir.moduleFuncCount()));
        return false;
    }
    for (std::uint32_t fi = 0; fi < finalLir.moduleFuncCount(); ++fi) {
        LirFuncId const fn = finalLir.funcAt(fi);
        auto const& offsets = functions[fi].blockByteOffsets;
        std::uint32_t const n = finalLir.funcBlockCount(fn);
        if (offsets.size() != n) {
            refuse(reporter, std::format(
                "the assembler published {} block offset(s) for function #{}, which has {} block(s)",
                offsets.size(), fi, n));
            return false;
        }
        std::uint32_t prev = 0;
        for (std::uint32_t k = 0; k < n; ++k) {
            std::uint32_t const v = finalLir.funcBlockAt(fn, k).v;
            auto const it = offsets.find(v);
            if (it == offsets.end() || (k > 0 && it->second < prev)) {
                refuse(reporter, std::format(
                    "the assembler's offset for block {} (#{} of function #{}) {}", v, k, fi,
                    it == offsets.end() ? std::string{"is missing"}
                                        : std::format("is {}, before the {} of the block laid out ahead of it",
                                                      it->second, prev)));
                return false;
            }
            prev = it->second;
        }
    }
    return true;
}

}  // namespace dss
