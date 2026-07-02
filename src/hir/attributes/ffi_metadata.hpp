#pragma once

#include <cstdint>
#include <string>

// FFI metadata side-table value (HR4). Attached per-node via
// `HirAttribute<FfiMetadata>` to `ExternFunction` / `ExternGlobal` HIR nodes,
// carrying what the FnSig type CANNOT: the linker-visible symbol name, linkage,
// visibility, and the originating import library. Calling convention is
// deliberately NOT here — it lives in the node's `FnSig` `typeId` (the type
// lattice's `CallConv` scalar), so there is a single source of truth for it.
//
// This is the minimal, forward-compatible subset of `11-ffi-plan`'s
// `ExternSymbol` design. The full attribute SYSTEM (a catalog header + the other
// standard per-node attributes, e.g. source spans) lands at HR5; actual
// POPULATION of real linkage/library values comes from FFI ingestion
// (`11-ffi-plan`). HR4 establishes the metadata's home so the extern surface is
// structurally complete. This header has no `Hir` dependency on purpose —
// consumers bind it as `HirAttribute<FfiMetadata>`.

namespace dss {

// Symbol linkage. `Strong` is the ordinary definition/reference; `Weak` may be
// overridden and resolves to null if unprovided; `Common` is the tentative-
// definition (C `int x;` at file scope) merge class. (TLS is a separate axis
// added when 11-ffi-plan FF5 needs it — not in v1's minimal set.)
enum class FfiLinkage : std::uint8_t { Strong, Weak, Common };

// Symbol visibility (ELF/Mach-O); on PE this maps onto dllimport/dllexport
// semantics. `Default` is externally visible; `Hidden`/`Protected` narrow it.
enum class FfiVisibility : std::uint8_t { Default, Hidden, Protected };

struct FfiMetadata {
    // The verbatim symbol name the linker must import/resolve against — preserved
    // exactly (mangled, if the source ABI mangles) for the linker's import table.
    // Empty means "use the node's own declared name".
    std::string mangledName;

    FfiLinkage    linkage    = FfiLinkage::Strong;
    FfiVisibility visibility = FfiVisibility::Default;

    // Originating import library, e.g. "libc.so.6" / "msvcrt.dll". Empty until
    // FFI ingestion resolves which library provides the symbol.
    std::string importLibrary;

    // ELF DT_SONAME / Mach-O install_name of `importLibrary`. Empty if not yet
    // known. Used by the linker to record the runtime dependency.
    std::string soname;

    // c86 (D-CSUBSET-BARE-PROTO-EXTERN-SYNTHESIS): TRUE ⇒ this extern
    // deliberately carries NO import library (a bare-prototype cross-TU
    // reference, C 6.2.2p5). The HIR→MIR extern pre-pass then ADMITS the empty
    // `importLibrary` (its ExternImport row's libraryPath stays empty) instead
    // of failing loud; downstream, the LK11 merge resolves the import against a
    // sibling TU's definition, and an unresolved survivor is rejected LOUD at
    // link as an undefined symbol (K_SymbolUndefined naming the symbol). FALSE
    // (every other producer) keeps the hard every-extern-must-declare-its-
    // library contract unchanged.
    bool noLibraryBinding = false;
};

} // namespace dss
