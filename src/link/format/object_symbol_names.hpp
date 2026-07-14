#pragma once

#include "asm/asm.hpp"                  // AssembledModule, ModuleSymbol, ExternImport
#include "core/types/strong_ids.hpp"    // SymbolId
#include "core/types/symbol_attrs.hpp"  // SymbolBinding, SymbolVisibility, isExternallyVisible

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

// Shared RELOCATABLE-OBJECT symbol-NAMING substrate for the object writers
// (ELF ET_REL, PE/COFF Obj, Mach-O MH_OBJECT) — D-LK-OBJECT-EXTERN-SYMBOL-NAMES.
//
// A relocatable object's `.symtab` must carry the SOURCE-LEVEL C identifier
// name for every externally-visible DEFINED function — otherwise a foreign
// linker fails `gcc main.c dss.o` with "undefined reference to foo". An
// externally-visible symbol (`isExternallyVisible` binding/visibility) gets
// its `ModuleSymbol.name` (already format-mangled by the compile pipeline's
// `nameOf` — identity on ELF/PE, a leading `_` on Mach-O, so it is emitted
// VERBATIM and a re-mangle would DOUBLE it). Everything else — a STATIC/Local
// function, a compiler-SYNTHESIZED symbol (string-literal rodata, an init
// thunk), an interior `&&label` block symbol, or a hand-built substrate
// module with an empty `symbols` table — keeps the caller's internal
// `<prefix><id>` fallback: those are resolved INTRA-object (by the DSS linker,
// or by a reloc within the same `.o`), never by a foreign linker by name, so
// the anchor explicitly permits them to "stay internal". A static
// (Local-binding) function therefore falls through to the fallback — this is
// the `may stay internal` name carve-out (flipping it to STB_LOCAL binding,
// vs its current GLOBAL `sym_<id>`, is the separate symtab-ordering concern
// the anchor defers).
//
// The IMPORT side (`externName`) names an UNDEFINED extern reference so a
// foreign linker resolves it — its `ExternImport.mangledName` (already the
// on-binary form) replaces the internal `<prefix><id>`. Consumed by the ET_REL
// writer's undefined-symbol loop once the format declares an `externCallDispatch`
// (D-LK-OBJECT-EXTERN-CALL-RELOCATABLE): an extern call lowers to a `call rel32`
// + a reloc against the extern, and the writer emits the extern as an SHN_UNDEF
// symtab entry the final (foreign) linker resolves.
//
// FORMAT-NEUTRAL: the one per-format value, the internal-fallback PREFIX
// ("sym_" on ELF/PE, "_sym_" on Mach-O), is a caller PARAMETER — the same
// per-format vocabulary those writers already carry, NOT an `if(format)`
// branch in shared substrate. Data comes from `AssembledModule.symbols`, which
// flows intact into every `encode()` — no signature change, no plumbing.

namespace dss::link::format {

class ObjectSymbolNames {
public:
    explicit ObjectSymbolNames(AssembledModule const& module) {
        definedBySym_.reserve(module.symbols.size());
        for (ModuleSymbol const& ms : module.symbols) {
            definedBySym_.emplace(ms.symbol.v, &ms);
        }
        externBySym_.reserve(module.externImports.size());
        for (ExternImport const& e : module.externImports) {
            externBySym_.emplace(e.symbol.v, &e);
        }
    }

    // The `.symtab` name for a DEFINED function symbol. Returns the real
    // (already-mangled) source name for an externally-visible definition;
    // otherwise the internal `<internalPrefix><id>` (static/local, synthesized,
    // or a substrate module with no `ModuleSymbol` row for `id`).
    [[nodiscard]] std::string
    definedName(SymbolId id, std::string_view internalPrefix) const {
        if (auto const it = definedBySym_.find(id.v); it != definedBySym_.end()) {
            ModuleSymbol const& ms = *it->second;
            if (!ms.name.empty()
                && isExternallyVisible(ms.binding, ms.visibility)) {
                return ms.name;
            }
        }
        return std::string{internalPrefix} + std::to_string(id.v);
    }

    // The `.symtab` name for an UNDEFINED extern reference. Returns the import's
    // (already-mangled) on-binary name so a foreign linker resolves it;
    // otherwise the internal `<internalPrefix><id>` (a reloc target that is
    // neither a defined symbol nor a known import).
    [[nodiscard]] std::string
    externName(SymbolId id, std::string_view internalPrefix) const {
        if (auto const it = externBySym_.find(id.v); it != externBySym_.end()) {
            if (!it->second->mangledName.empty()) {
                return it->second->mangledName;
            }
        }
        return std::string{internalPrefix} + std::to_string(id.v);
    }

private:
    // Keyed by SymbolId.v — pointers alias the caller's `AssembledModule`,
    // which outlives this helper (the writer builds it on the stack inside
    // `encode()`, before the symtab loop, and discards it after).
    std::unordered_map<std::uint32_t, ModuleSymbol const*> definedBySym_;
    std::unordered_map<std::uint32_t, ExternImport const*> externBySym_;
};

} // namespace dss::link::format
