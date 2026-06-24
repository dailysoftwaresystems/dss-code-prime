#pragma once

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "link/format/byte_emit.hpp"

#include <cstdint>
#include <format>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

// Shared exec-image INTERIOR-BLOCK-SYMBOL substrate for the format walkers
// (ELF ET_EXEC, PE PE32+, Mach-O MH_EXECUTE) — D-CSUBSET-COMPUTED-GOTO
// (`&&label` / `goto *p`).
//
// A block-address `lea` materializes the runtime address of an INTERIOR
// basic block into a register, exactly like `lowerGlobalAddr` materializes
// a global — via a `lea` of a SYNTHETIC per-block LOCAL symbol (rel32 on
// x86; ADRP+ADD on arm64). That symbol is neither a function nor a data
// item, so it gets no VA from the usual function / data symbolVa
// population. The assembler instead records, per function, a
// `SyntheticBlockSymbol{ symbol, blockByteOffset }` (the byte offset of the
// target block WITHIN that function's bytes). This helper assigns each such
// symbol its interior-block VA:
//
//     symbolVa[symbol] = sectionVa + funcTextStart[fi] + blockByteOffset
//
// — the SAME geometry a function symbol gets (`sectionVa +
// funcTextStart[fi]`), but pointing at an interior block rather than the
// function entry. With the symbol's VA in the map BEFORE
// `applyExecRelocations`, the EXISTING rel32 / adr_prel_pg_hi21 /
// add_abs_lo12_nc reloc formulas resolve the block-address `lea` with ZERO
// kernel change (and the `lea`'s relocation carries addend 0 — which is WHY
// a synthetic own-VA symbol is used rather than funcSymbol + addend).
//
// FORMAT-NEUTRAL: each walker passes the SAME `sectionVa` it already uses
// for its FUNCTION symbol VAs (so block VA = funcVA + blockOffset stays
// consistent) and the SAME `funcTextStart` table. There is no `if(format)`
// here — the helper is NOT forked three ways; the three walkers call it
// identically, differing only in the values they already compute for their
// function symbols. A duplicate symbol id (the `emplace` collides) is a
// caller / IR bug — a synthetic block symbol is minted unique per module —
// so it fails loud (`K_DuplicateDataSymbol`, the same code
// `addDataSymbolVas` uses for a colliding data symbol).

namespace dss::link::format {

// Add each function's synthetic block symbols' interior-block VAs to
// `symbolVa`. `funcTextStart[fi]` MUST be function `fi`'s start offset
// within the `.text` section (the SAME table the walker passes to
// `applyExecRelocations`); `sectionVa` MUST be the runtime VA of that
// section (the SAME value used for the function symbols). Emits exactly one
// diagnostic and returns false on a duplicate symbol id; returns true (no
// diagnostics) when every function has an empty `blockSymbols` (the common
// case — no block address taken).
[[nodiscard]] inline bool addInteriorBlockSymbolVas(
    AssembledModule const&                       module,
    std::span<std::uint64_t const>               funcTextStart,
    std::uint64_t                                sectionVa,
    std::unordered_map<SymbolId, std::uint64_t>& symbolVa,
    std::string_view                             writerName,
    DiagnosticReporter&                          reporter) {
    using ::dss::link::format::detail::emit;
    for (std::size_t fi = 0; fi < module.functions.size(); ++fi) {
        for (auto const& bs : module.functions[fi].blockSymbols) {
            std::uint64_t const va =
                sectionVa + funcTextStart[fi] + bs.blockByteOffset;
            if (!symbolVa.emplace(bs.symbol, va).second) {
                emit(reporter, DiagnosticCode::K_DuplicateDataSymbol,
                     std::format("{}: synthetic block SymbolId={{ {} }} "
                                 "collides with another symbol — a per-block "
                                 "`&&label` symbol must be unique within the "
                                 "module (D-CSUBSET-COMPUTED-GOTO).",
                                 writerName, bs.symbol.v));
                return false;
            }
        }
    }
    return true;
}

} // namespace dss::link::format
