#pragma once

#include "asm/asm.hpp"                     // AssembledModule, Relocation
#include "core/export.hpp"                 // DSS_EXPORT
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>

// ── GOT-SLOT-RELATIVE REFERENCES AND THE SLOTS THAT ANSWER THEM ──────────
// P68 round 11: D-LK-ELF-READER-REFUSES-GOTPCREL-BLOCKS-REAL-GLIBC-MEMBERS
// (x86_64: R_X86_64_GOTPCREL / GOTPCRELX / REX_GOTPCRELX) and
// D-LK-ELF-AARCH64-EXEC-REFUSES-GOT-RELOCATIONS (aarch64: R_AARCH64_ADR_GOT_PAGE
// + R_AARCH64_LD64_GOT_LO12_NC).
//
// A GOT-slot-relative relocation does not address its symbol: it addresses a
// SLOT that holds the symbol's address, and the code loads the pointer found
// there. A relocatable object leaves the slot to the final linker, so every
// object DSS reads from a default-built (PIE) gcc or clang archive carries
// them — for an extern datum, an extern array element's address, an extern
// function's address — and so does DSS's own aarch64 staticlib
// (`externAddrBinding: got`).
//
// ★★ ONE MECHANISM, AT THE LINK, FOR EVERY IMAGE WRITER. `lowerGotSlotReferences`
// mints each slot as an ordinary relocation-bearing CONST data item — eight
// bytes and ONE absolute-64-bit relocation to the symbol — and rewrites the
// reference into its DIRECT twin addressing that item. After it, no
// GOT-slot-relative relocation reaches any writer, and the slot is filled by
// the path that already fills every pointer-holding data item, per image kind:
// a link-time constant in a non-PIE exec, R_*_RELATIVE in a PIE (and for a
// hidden definition in a .so), a symbol-based row for a preemptible definition
// of a .so or for an import. ✔MEASURED the need 2026-09-24: every ELF image the
// driver builds goes through the DYNAMIC writer (the exec documents import libc
// `exit`), and with the GOT kinds declared each of exec, PIE and .so refused the
// reference there; the closed x86 fix's `.got` lived in the STATIC writer, which
// only a module with no import at all reaches.
// ⓘ NO RELAXATION. The `X` of GOTPCRELX is a permission the assembler grants
// the linker to rewrite the instruction when the target is local; it is an
// optimisation, never a meaning — an indirect load through a slot holding the
// same address is always correct, so the instruction stays as written.
// ⓘ THE SYMBOL-BASED ROW IS THE TARGET'S ABSOLUTE POINTER RELOCATION
// (R_X86_64_64, R_AARCH64_ABS64), NOT GNU ld's GLOB_DAT — and the loader treats
// the two alike for these slots, DOCUMENTED by glibc's own `elf_machine_rela`
// (sysdeps/x86_64/dl-machine.h and sysdeps/aarch64/dl-machine.h, glibc master,
// read 2026-09-24): `elf_machine_type_class` puts neither type in the PLT or
// COPY class, so the symbol is looked up the same way; x86_64's GLOB_DAT stores
// the resolved address and R_X86_64_64 that address plus `r_addend` (every x86
// slot minted here has addend 0), and aarch64 stores the address plus
// `r_addend` for GLOB_DAT and ABS64 alike — the addend an S+A slot needs.

namespace dss::linker {

// ⓘ WHAT A GOT FORMULA IS — slot-relative, where its addend goes, the formula of
// its direct twin — is `relocFormulaFacts` (core/types/target_schema.hpp), the
// one exhaustive table the target loader checks a GOT row's declared twin
// against. It lived here until P68 round 11, when the loader began asking.

// One slot: the symbol it holds the address of, plus the addend that address
// carries where the formula puts the addend IN the slot (0 otherwise).
struct GotSlotKey {
    SymbolId     symbol{};
    std::int64_t addend = 0;
    friend bool operator==(GotSlotKey const&, GotSlotKey const&) = default;
};
struct GotSlotKeyHash {
    [[nodiscard]] std::size_t operator()(GotSlotKey const& k) const noexcept {
        return std::hash<std::uint32_t>{}(k.symbol.v)
             ^ (std::hash<std::int64_t>{}(k.addend) * 0x9E3779B97F4A7C15ull);
    }
};

// The slot a GOT-slot-relative relocation names — the ONE place its key is
// formed.
[[nodiscard]] inline GotSlotKey gotSlotKeyFor(Relocation const& rel,
                                              RelocFormulaFacts const& f) noexcept {
    return GotSlotKey{rel.target, f.slotHoldsAddend ? rel.addend : 0};
}

// The target's row that addresses a slot DIRECTLY with the arithmetic `gotRow`
// applies to it: the row `gotRow` DECLARES as its `gotSlotTwin`, checked by
// `gotSlotTwinMismatch` (the loader already did; this repeats it for a schema
// built past the loader). It is declared, never searched for, because two rows
// can share the arithmetic and differ only in a format's spelling (x86_64's
// `rel32` and `riprel32`): a search would have to pick. nullptr + `why` when
// the row names no twin, names a row the target does not declare, or names one
// with other arithmetic.
[[nodiscard]] DSS_EXPORT TargetRelocationInfo const*
gotSlotDirectTwin(TargetSchema const& target, TargetRelocationInfo const& gotRow,
                  std::string& why);

// The lowering. For an IMAGE format: mint one slot per distinct `GotSlotKey`
// the module's CODE names through a GOT-slot-relative relocation (first-
// reference order, so the image is a deterministic function of the module) and
// rewrite each such relocation into its direct twin against its slot, keeping
// the addend only where the formula keeps it on the reference. A RELOCATABLE
// format is left alone — its GOT relocations are the final linker's.
// `resolvedToNothing` names the WEAK data symbols the link bound to no
// definition (the reference gate's null-bound set): their slot HOLDS 0 — S is 0
// — so it is minted with no relocation, and a nonzero addend (a slot that would
// hold the bare number A) is refused rather than guessed.
// Returns TRUE when the module needs no change (`out` untouched); FALSE when
// `out` holds the lowered module OR an error was reported — the caller tells
// the two apart by the reporter's error count (the convention
// `materializeObjectImportSlots` follows).
[[nodiscard]] DSS_EXPORT bool
lowerGotSlotReferences(AssembledModule const&    in,
                       AssembledModule&          out,
                       TargetSchema const&       target,
                       ObjectFormatSchema const& format,
                       DiagnosticReporter&       reporter,
                       std::span<SymbolId const> resolvedToNothing = {});

}  // namespace dss::linker
