#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"

#include <cstdint>
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
    // resolves sibling-CU-defined ones away first) binds per the
    // format's declared `dataImportBinding` model (c84: the ELF
    // ET_EXEC R_*_COPY copy-relocation); a format that declares no
    // model FAILS LOUD at the linker's pre-walker gate — a PLT
    // stub bound to a data symbol would be a silent miscompile.
    bool        isData = false;
    // TLS C1 (D-CSUBSET-THREAD-LOCAL): true for an extern DATA object
    // declared with thread storage duration (`extern thread_local int e;`).
    // Set at the HIR→MIR extern pre-pass from the declaration's
    // HirThreadLocalMap entry; carried through the LK11 merge's
    // survivingExterns copy. A same-program sibling-CU definition resolves
    // the row away like any extern data (the definition's own
    // MirGlobal.isThreadLocal drives layout); a TRUE library TLS import
    // surviving to the link tier is the initial-exec/GOT-indirect model —
    // NOT implemented (D-CSUBSET-THREAD-LOCAL-INITIAL-EXEC) — and the
    // walker tier rejects it loud (slice C). Meaningless (false) for
    // function imports (S_ThreadLocalOnFunction rejects those upstream).
    bool        isThreadLocal = false;
    // c84 (D-LK-EXTERN-DATA-IMPORT): the imported DATA object's byte
    // size + alignment, DERIVED from the declared type's layout at
    // HIR→MIR (`computeLayout` under the active target's aggregate-
    // layout params + the format's DataModel — never hardcoded; a
    // `FILE*` object is the data model's pointer width). Consumed by
    // the ELF copy-relocation emitter: the exec reserves a `.bss`
    // slot of exactly this shape, exports the symbol with this
    // `st_size`, and the loader memcpy's `st_size` bytes from the
    // library's object. BOTH stay 0 when the declared type is
    // INCOMPLETE (`extern const char v[];`) — legal C for a cross-TU
    // extern the LK11 merge resolves against its defining sibling
    // CU; a TRUE library import that survives to the walker with
    // size 0 fails loud there (an unsized copy slot cannot be
    // reserved). Meaningless (0) for function imports.
    std::uint64_t dataSizeBytes  = 0;
    std::uint64_t dataAlignBytes = 0;
};

} // namespace dss
