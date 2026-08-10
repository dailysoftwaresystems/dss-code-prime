#pragma once

#include "core/export.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Plan 11 FFI substrate — `ImportSurface` is the binary-reader output
// row (FF1) and the C-header-parser output row (FF2) — both writers
// produce this same row type so the consumer (HIR FfiMap population +
// `collectExterns` MIR pre-pass) sees a uniform surface regardless of
// where the metadata came from.
//
// Distinct from `ExternImport` (core/types/extern_import.hpp) — that's
// the link-time row stamped onto an `AssembledModule` AFTER the user's
// source declares `extern fn foo(...)`. `ImportSurface` is upstream:
// it enumerates "what the binary / header CAN expose" so the source-
// translation step can validate the user's `extern` declarations
// against the ABI surface.
//
// Format-blind by construction:
//   * ELF readers populate `mangledName` from the `.dynsym` ST_NAME
//     index'd into `.dynstr`; visibility from `st_other`/`STV_*`;
//     libraryPath from the source `.so` path.
//   * PE readers populate from `.edata` (export table); libraryPath
//     from the source `.dll` path.
//   * Mach-O readers populate from `LC_SYMTAB` n_strx; libraryPath
//     from `LC_LOAD_DYLIB` paths.
//   * FF2 C header parser populates synthetically — mangledName from
//     the C decl's identifier verbatim (FF4 will apply per-platform
//     underscoring rules downstream; FF2 leaves the name as-declared);
//     libraryPath from the caller-supplied `importLibrary` argument
//     (per plan 11 §2.3 — headers carry declarations, not library
//     identity, so the caller passes the owning library at the
//     `readCHeader(..., importLibrary, ...)` boundary).
//
// Closed-enum discriminators (visibility / kind / linkage) mirror
// the codebase's pattern (RelocFormulaKind, ObjectFormatKind, etc.):
// adding a new format reader = populate the same enum, no string
// drift.

namespace dss::ffi {

// Symbol visibility from the binary's symbol table. ELF STV_*,
// PE export-table flags, Mach-O n_type bits all map to one of these
// closed values.
enum class SymbolVisibility : std::uint8_t {
    Default   = 0,  // ELF STV_DEFAULT / PE default / Mach-O default
    Hidden    = 1,  // ELF STV_HIDDEN / PE has-no-export / Mach-O N_PEXT
    Protected = 2,  // ELF STV_PROTECTED — semantic between Default + Hidden
    Internal  = 3,  // ELF STV_INTERNAL — strict use-within-DSO
};

// What kind of symbol the binary exposes.
enum class SymbolKind : std::uint8_t {
    Function  = 0,  // ELF STT_FUNC; PE export whose EAT RVA lands in an executable section; Mach-O N_SECT in __text
    Object    = 1,  // ELF STT_OBJECT (data); PE export whose EAT RVA lands in a non-executable data section; Mach-O N_SECT in __data
    Tls       = 2,  // ELF STT_TLS; PE __declspec(thread); Mach-O thread-local
    NoType    = 3,  // ELF STT_NOTYPE (unknown -- common in stripped .so)
    Forwarder = 4,  // PE export whose EAT RVA falls inside the export
                    // directory span — the RVA points at a "DLL.Symbol"
                    // forward string, not code/data (the row carries the
                    // target in `forwardTarget`). kernel32 is full of these
                    // (HeapAlloc → NTDLL.RtlAllocateHeap); the ELF analog is
                    // an IFUNC/alias, the Mach-O analog a reexport — both
                    // reserved for their readers when a corpus needs them.
};

// Linkage class — only relevant for header-mode (FF2). Binary readers
// (FF1) set this from the binary's bind class.
enum class SymbolLinkage : std::uint8_t {
    External = 0,  // ELF STB_GLOBAL / PE export / Mach-O N_EXT
    Weak     = 1,  // ELF STB_WEAK / PE weak alias / Mach-O N_PEXT|N_WEAK_DEF
    Local    = 2,  // ELF STB_LOCAL (rare in `.dynsym`; common in `.symtab`)
};

// ── Per-symbol VERSIONING — an ELF-only concept, said in the type ──
//
// TF-C124 (D-FFI-BINARY-READER-SURFACES-NO-SYMBOL-VERSION). ELF is the
// ONLY one of the three object formats with a PER-SYMBOL version, so this
// is the one place `ImportSurface`'s otherwise format-blind rows carry a
// format-named member — deliberately, because the honest alternative to a
// name that says ELF is a field named `version` that silently means
// NOTHING on two thirds of the readers.
//
// The mechanism (gABI + the Sun/GNU version extension): `.gnu.version`
// (SHT_GNU_versym) holds ONE u16 per `.dynsym` row. For a DEFINED symbol
// that index selects an entry of `.gnu.version_d` (SHT_GNU_verdef) — the
// versions this library DEFINES for its own exports. For an UNDEFINED row
// it selects a `.gnu.version_r` (verneed) vernaux instead — the versions
// the library REQUIRES of OTHERS. ⚠ Only the FORMER can answer "what
// version does this library export this name at": `.gnu.version_r`
// describes the library's own IMPORTS, i.e. exactly the SHN_UNDEF rows
// `readElf64` filters out as not-exports
// ([[D-LK-ELF-EMITS-ONE-DT-NEEDED-WHEN-TWO-LIBRARIES-ARE-REFERENCED]]).
// MEASURED on this host: `libz.so.1` records `deflateBound@@ZLIB_1.2.0`
// in a 15-entry `.gnu.version_d`, while its 1-entry `.gnu.version_r`
// speaks only about the glibc symbols libz itself calls.
//
// The other two formats, STATED rather than mapped onto this field:
//   * PE — has NO per-symbol version, at all. An import binds by NAME or
//     by ORDINAL out of an IMAGE_IMPORT_DESCRIPTOR and there is no third
//     coordinate. What PE versions is the FILE (a VERSIONINFO resource,
//     or the version baked into an api-set DLL's own NAME, e.g.
//     `api-ms-win-crt-stdio-l1-1-0.dll`) — library identity, which this
//     row already carries as `libraryPath` / `soname`.
//   * Mach-O — also has no per-symbol version. `LC_ID_DYLIB` /
//     `LC_LOAD_DYLIB` carry a dylib's current + COMPATIBILITY version,
//     and `LC_BUILD_VERSION` a platform floor; both are per-LIBRARY, and
//     dyld's per-symbol availability story is `$ld$` linker-set aliases,
//     which are distinct SYMBOL NAMES rather than a version attached to
//     one name.
// So this stays `nullopt` on every PE and Mach-O row by those formats'
// CONSTRUCTION — not "pending a reader that will fill it in later".
struct DSS_EXPORT ElfSymbolVersion {
    // The version NAME exactly as the binary records it: "ZLIB_1.2.0",
    // "GLIBC_2.3". NEVER empty in a live value — a row with no version
    // carries no `ElfSymbolVersion` at all, which is precisely why the
    // member below is an `optional` rather than a possibly-empty string
    // (an empty string cannot say whether the reader looked and found
    // nothing, or never looked).
    std::string name;
    // TRUE  — `sym@@VER`: the DEFAULT definition of this name. An
    //         unversioned reference to the name binds HERE, and a
    //         reference REQUESTING this version binds here too. Safe to
    //         re-request from an observation.
    // FALSE — `sym@VER`: a NON-default (VERSYM_HIDDEN, the 0x8000 bit of
    //         the versym slot) COMPAT definition, kept alive only for
    //         binaries linked before the default moved. ⚠ Pinning one of
    //         these from a mere observation is how a working import
    //         becomes a broken one: libc.so.6 defines BOTH
    //         `realpath@@GLIBC_2.3` and `realpath@GLIBC_2.2.5`, and the
    //         latter is the pre-2.3 form that EINVALs a NULL resolved
    //         buffer — the exact misbind [[D-LK-ELF-SYMBOL-VERSIONING]]
    //         was opened for. The consumer must NOT treat the two alike;
    //         `ffi/ingest.cpp` requires this bit to re-request a version
    //         it only OBSERVED.
    bool isDefaultVersion = true;
};

// One row in the import surface — describes a single symbol the
// dynamic library exports. FF3 (ABI catalog) will resolve typed
// signatures off the HIR side-table — not via a field on this row —
// so no `cSignature` slot lives here (D-FF2-1: dropped post-FF2-#2
// type-design fold; re-add as `optional<FnSigTypeId>` ONLY if FF3
// surfaces a concrete need to attach the resolved sig to the row
// itself instead of the HIR node).
struct DSS_EXPORT ImportSurface {
    std::string      mangledName;   // on-binary symbol name (verbatim)
    std::string      libraryPath;   // owning binary's path (or caller label)
    // D-FF1-READER-SONAME (c171): the library's EMBEDDED, loader-resolvable
    // identity — ELF DT_SONAME / Mach-O LC_ID_DYLIB install_name / PE export-
    // directory DllName. EMPTY when the binary declares none (a `.so` built
    // without `-Wl,-soname`, a relocatable object, an FF2 header row). The
    // consumer (`ingest()`) PREFERS this over the file basename when binding
    // an extern's owning library (`meta.importLibrary`), mirroring what a real
    // linker records as DT_NEEDED / LC_LOAD_DYLIB / the import DllName; the
    // basename stays the fallback. Same binary-wide-per-row shape as
    // `libraryPath` (every row of one binary carries the same value).
    std::string      soname;
    SymbolKind       kind       = SymbolKind::NoType;
    SymbolVisibility visibility = SymbolVisibility::Default;
    SymbolLinkage    linkage    = SymbolLinkage::External;
    // For a `kind == SymbolKind::Forwarder` row only: the verbatim
    // "DLL.Symbol" target the export forwards to (e.g. PE kernel32's
    // HeapAlloc forwards to "NTDLL.RtlAllocateHeap"). Empty for every
    // non-forwarder kind. Carrying the target explicitly (rather than
    // rejecting the export) keeps a forwarder-heavy binary like kernel32
    // readable — the consumer follows the redirect to the real owner.
    std::string      forwardTarget;
    // TF-C124 (D-FFI-BINARY-READER-SURFACES-NO-SYMBOL-VERSION): the
    // PER-SYMBOL version this binary records for this export, or `nullopt`
    // for "no version" — see `ElfSymbolVersion` above for the mechanism and
    // for why PE and Mach-O rows are permanently `nullopt`. Populated ONLY
    // by `readElf64`, from `.gnu.version` + `.gnu.version_d`. Without it a
    // `--resolve-library` import was ALWAYS unversioned and only a shipped
    // DESCRIPTOR could pin a version — which made
    // [[D-LK-ELF-SYMBOL-VERSIONING]]'s emission mechanism unreachable for
    // every third-party library this project acquires (Tcl, zlib), none of
    // which has a descriptor. LAST field, so every positional aggregate
    // initializer in the FF1 test fixtures keeps compiling and defaults to
    // "no version".
    std::optional<ElfSymbolVersion> elfSymbolVersion;
};

} // namespace dss::ffi
