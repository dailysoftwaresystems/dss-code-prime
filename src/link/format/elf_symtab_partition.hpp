#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

// The ELF `.symtab` local-before-global invariant, as a pure predicate over
// the bytes that were actually emitted — so it can be EXERCISED by a test
// rather than only read.
//
// ELF requires every STB_LOCAL symbol to precede the first non-local one, and
// `.symtab.sh_info` to name that boundary. Both `.symtab` builders in `elf.cpp`
// DERIVE `sh_info` from the bytes already emitted rather than predicting it —
// but a derived boundary only proves `sh_info` matched the position it was
// taken at, never that nothing was appended on the WRONG SIDE of it afterwards.
//
// That second half is what an ALIAS pass makes reachable
// (D-LK-ALIAS-NAME-ABSENT-FROM-REEMITTED-OBJECT-SYMTAB): the extra names bound
// to an already-emitted atom are appended AFTER both binding-ordered passes
// have run, so an alias that ever resolved to STB_LOCAL would land past
// `sh_info` and the section header would lie about where the locals end.
// `definedAliases` yields only externally-visible rows today, which is exactly
// the kind of premise that stops being true quietly — and a mis-partitioned
// `.symtab` is invisible to DSS's own reader (which never consults `sh_info`)
// while `ld`, `readelf`, `nm` and `gdb` all silently mis-partition. So the
// invariant is CHECKED over the finished bytes rather than argued for in a
// comment.
//
// This is ELF's record layout, not shared vocabulary: it belongs to the ELF
// writer exactly as the Mach-O nlist layout belongs to that writer.

namespace dss::link::format {

// Elf64_Sym: st_name(4) st_info(1) st_other(1) st_shndx(2) st_value(8)
// st_size(8). st_info's high nibble is the binding; STB_LOCAL is 0.
inline constexpr std::size_t kElf64SymSize   = 24;
inline constexpr std::size_t kElf64StInfoOff = 4;

// Returns an empty string when the partition holds; otherwise a sentence
// naming the offending symbol index and the side it sits on, for the caller's
// diagnostic.
[[nodiscard]] inline std::string
elfSymtabPartitionBreach(std::span<std::uint8_t const> symtab,
                         std::uint32_t                 firstNonLocal) {
    std::size_t const n = symtab.size() / kElf64SymSize;
    if (symtab.size() % kElf64SymSize != 0) {
        return "the `.symtab` is " + std::to_string(symtab.size())
               + " bytes, which is not a whole number of "
               + std::to_string(kElf64SymSize) + "-byte Elf64_Sym records";
    }
    if (firstNonLocal > n) {
        return "sh_info names symbol index " + std::to_string(firstNonLocal)
               + " but the table holds only " + std::to_string(n) + " symbols";
    }
    for (std::size_t i = 0; i < n; ++i) {
        bool const isLocal =
            (symtab[i * kElf64SymSize + kElf64StInfoOff] >> 4) == 0u;
        if (i < firstNonLocal && !isLocal) {
            return "symbol #" + std::to_string(i)
                   + " is non-LOCAL but sits BEFORE sh_info ("
                   + std::to_string(firstNonLocal) + ")";
        }
        if (i >= firstNonLocal && isLocal) {
            return "symbol #" + std::to_string(i)
                   + " is STB_LOCAL but sits AFTER sh_info ("
                   + std::to_string(firstNonLocal) + ")";
        }
    }
    return {};
}

} // namespace dss::link::format
