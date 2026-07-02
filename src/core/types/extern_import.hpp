#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"

#include <string>

// Cross-tier extern symbol descriptor (LK6 cycle 2d substrate hoist
// — plan 14 §3.1 D-LK6-6). Carries the link-time-resolved identity
// of an external symbol referenced by an assembled / lowered module:
//
//   `symbol`      — the SymbolId that matches `Relocation::target`
//                   in every reloc that references this extern.
//   `mangledName` — the on-binary symbol name the linker's import
//                   table must carry verbatim (e.g. "printf" on
//                   Linux/ELF; "printf" on x86_64 PE; "_printf" on
//                   legacy Mach-O i386). Per-platform underscoring
//                   belongs upstream (plan 11 §2.5); MIR / LIR /
//                   assembler stamp whatever the HIR FfiMetadata
//                   provided.
//   `libraryPath` — the dynamic library that owns this symbol
//                   ("kernel32.dll" / "msvcrt.dll" on Windows;
//                   "libc.so.6" on Linux; "/usr/lib/libSystem.B.
//                   dylib" on macOS). Multiple externs sharing this
//                   field collapse to one PE
//                   IMAGE_IMPORT_DESCRIPTOR / ELF DT_NEEDED entry /
//                   Mach-O LC_LOAD_DYLIB load command. The linker
//                   groups by this field.
//
// Lives in `core/types` (alongside `SymbolId`) so HIR / MIR / LIR /
// assembler all consume the same row type without coupling to each
// other's headers. Pre-cycle-2d the type lived in `src/asm/asm.hpp`
// for assembler use only; the cycle 2d thread-through (HIR FfiMap
// → MIR pre-pass → MIR/LIR result side-tables → assembler) needs it
// upstream — hence the hoist (architect "no abstraction explosion"
// rule: hoist when 3+ tiers consume; we now have HIR / MIR / LIR /
// assembler / linker, well past the threshold).

namespace dss {

struct DSS_EXPORT ExternImport {
    SymbolId    symbol{};       // matches Relocation::target
    std::string mangledName;    // on-binary symbol name
    std::string libraryPath;    // owning dylib / DLL / SO
    // c82 (D-LK-EXTERN-DATA-IMPORT): true for an extern DATA object
    // (HIR ExternGlobal — e.g. libc's `stdout`, or a cross-TU
    // `extern const char sqlite3_version[];`), false for a function.
    // A data import that survives to the link tier (the LK11 merge
    // resolves sibling-CU-defined ones away first) currently FAILS
    // LOUD there: binding it needs the extern-data import model
    // (ELF R_*_COPY vs GOT-indirect load — a §B decision); a PLT
    // stub bound to a data symbol would be a silent miscompile.
    bool        isData = false;
};

} // namespace dss
