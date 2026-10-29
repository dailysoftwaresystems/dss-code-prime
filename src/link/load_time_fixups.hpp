#pragma once

#include "asm/asm.hpp"                       // AssembledFunction
#include "core/export.hpp"                   // DSS_EXPORT
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/strong_ids.hpp"         // SymbolId
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"     // cCallingConvention, tlsAccess

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

// ── A FUNCTION THE IMAGE RUNS AT LOAD TO FILL WHAT ITS LOADER CANNOT ──────
// P69 review M1, cases (a) and (b) (D-LK-PE-IMPORT-ADDRESS-SLOT-RESIDUE).
//
// Some pointer slots hold a value only the LOAD can know and no loader
// relocation can express: an import's address PLUS an offset (`&arr[3]` of a
// DLL datum — the Windows loader writes an import's address and nothing else),
// or an import's address in a thread-local TEMPLATE (the loader copies the
// starting thread's block from the template before it binds any import). Each
// such slot is one FIX-UP: load the pointer found in a slot the loader DOES
// fill (the import's IAT entry), add the offset, store it into the data item —
// and, for a template item, also into the running thread's own copy.
//
// The function is SYNTHESIZED here, target-blind: every instruction is one the
// target declares (`cmp`, `jcc`, `lea`, `load`, `add`, `mov`, `store`,
// `tlsbase`, `ret`), the registers are the caller-saved general registers of
// the format's C calling convention, the thread's block is reached through the
// format's `tlsAccess` model, and the whole runs only when one argument equals
// one value (the caller's to name: a TLS callback's `reason`). The image writer
// decides WHEN it runs (on PE, the first TLS callback, before any other code of
// the image) and lays the fixed-up items out where they can be written.
namespace dss::linker {

struct LoadTimeFixup {
    SymbolId      sourceSlot{};            // a pointer-sized slot the loader fills
    std::int64_t  addend = 0;              // added to the pointer found there
    SymbolId      item{};                  // the data item stored into...
    std::uint64_t offsetInItem = 0;        // ...at this offset
    bool          threadTemplate = false;  // `item` is a thread-local TEMPLATE item
};

// Runs the fix-ups only when argument `guardArgumentIndex` of the format's C
// calling convention equals `guardValue` (compared at 32 bits); returns the
// assembled function named `symbol`, or nullopt after reporting.
// `threadTemplateBase` names the first byte of the thread-local TEMPLATE (the
// image writer binds it); it is required when a fix-up writes a template item,
// because a thread-local item's own symbol resolves to its OFFSET in a thread's
// block — the template item is reached as that base plus that offset, the same
// `[base + offset(item)]` form the running thread's copy is reached by.
[[nodiscard]] DSS_EXPORT std::optional<AssembledFunction>
synthesizeLoadTimeFixupRunner(std::span<LoadTimeFixup const> fixups,
                              SymbolId                       symbol,
                              SymbolId                       threadTemplateBase,
                              std::size_t                    guardArgumentIndex,
                              std::int32_t                   guardValue,
                              TargetSchema const&            target,
                              ObjectFormatSchema const&      format,
                              DiagnosticReporter&            reporter);

}  // namespace dss::linker
