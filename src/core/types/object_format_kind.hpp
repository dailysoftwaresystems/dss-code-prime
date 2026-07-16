#pragma once

#include "core/export.hpp"
#include "core/types/enum_name_table.hpp"   // EnumNameTable<E,N> (leaf header — no target_schema cycle)

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Canonical object-format taxonomy — the closed-enum vocabulary the
// substrate engine speaks for the OUTPUT image kind (ELF / PE/COFF /
// Mach-O / WASM / SPIR-V). Each `.format.json` declares ONE
// `ObjectFormatKind` in its top-level `kind` field; the engine reads
// the kind, per-format JSON owns the on-disk byte layout.
//
// **Cross-tier vocabulary**: this header lives under `core/types/`
// rather than `src/link/` so non-linker layers can speak the kind
// without pulling in `link/object_format_schema.hpp`'s full
// 800-LOC substrate. Concrete callers:
//   * `src/ffi/`     — FF4 C-mangling dispatches on
//                      `ObjectFormatKind` (per-format
//                      leading-underscore rule);
//                      `synthesizeFfiFromSourceDecls` reports the
//                      kind in `F_FfiNoImportLibraryForFormat`
//                      diagnostics.
//   * `src/core/types/grammar_schema_json.cpp` — semantic config
//                      loader validates the per-language
//                      `externLibraryByFormat` map keys against
//                      this enum's name table at language-load time
//                      (catches a typo like `"pee"` instead of
//                      `"pe"` at config load rather than at compile
//                      time).
// Same extraction precedent as `section_kind.hpp` (which left
// `link/object_format_schema.hpp` as the umbrella header so every
// existing consumer continues to work without changing its
// `#include`).
//
// **Sentinel discipline**: `Unknown = 0` is the project's universal
// invalid sentinel (mirrors `TargetEncodingShape::None`,
// `RelocationKind{}`, strong-ids); a default-constructed value
// reports `Unknown`, NOT a spurious ELF identity.

namespace dss {

enum class ObjectFormatKind : std::uint8_t {
    Unknown = 0,  // invalid sentinel; default-constructed images
    Elf     = 1,  // Linux + Android
    Pe      = 2,  // Windows + Windows-ARM64 (PE/COFF)
    MachO   = 3,  // macOS + iOS
    Wasm    = 4,  // Web / WASM runtime — enum slot reserved; engine +
                  // JSON arrive in plan 18
    Spirv   = 5,  // GPU shaders — enum slot reserved; engine + JSON
                  // arrive in plan 17
};

inline constexpr EnumNameTable<ObjectFormatKind, 6> kObjectFormatKindTable{{{
    { ObjectFormatKind::Unknown, "unknown" },
    { ObjectFormatKind::Elf,     "elf"     },
    { ObjectFormatKind::Pe,      "pe"      },
    { ObjectFormatKind::MachO,   "macho"   },
    { ObjectFormatKind::Wasm,    "wasm"    },
    { ObjectFormatKind::Spirv,   "spirv"   },
}}};

[[nodiscard]] constexpr std::string_view
objectFormatKindName(ObjectFormatKind k) noexcept {
    return kObjectFormatKindTable.name(k);
}
[[nodiscard]] constexpr std::optional<ObjectFormatKind>
objectFormatKindFromName(std::string_view s) noexcept {
    return kObjectFormatKindTable.fromName(s);
}

// ── Extern-call dispatch model (D-FFI-EXTERN-CALL-DISPATCH) ────────
//
// How an extern (shipped-library / cross-image) CALL is reached AT THE
// CALL SITE — a property of the OBJECT FORMAT's dynamic-import model,
// NOT the CPU target. This is keyed by the format because the SAME CPU
// target needs OPPOSITE call shapes under different formats (x86_64-PE
// vs x86_64-ELF), so it cannot live on the target schema.
//
//   * `indirect-slot` (the PE-IAT-style shape; NOTE no SHIPPED format
//     selects it today — PE moved to direct-plt FF 25 thunks at c112,
//     see the pe64-*-exec JSON comment — but the vocabulary stays for
//     formats whose import model has no stub tier): the linker points
//     the extern symbol's VA at a POINTER SLOT; the call site
//     DEREFERENCES it (x86_64 `FF 15 disp32` = `call [RIP+disp32]`).
//     The loader fixes the slot to the resolved callee address.
//   * `direct-plt` (ELF PLT/GOT, Mach-O __stubs): the linker points the
//     extern symbol's VA at a STUB (code) — ELF's PLT entry or Mach-O's
//     `__stubs` entry — and the call site is a PLAIN DIRECT call to the
//     stub (x86_64 `E8 disp32`, ARM64 `BL imm26`); the stub performs
//     the GOT/__got indirection internally. (Mach-O's `symbolVa[extern]
//     = stubVa` is why it is direct-plt, NOT indirect-slot — the slot
//     the symbol's VA names is the STUB, not the __got pointer.)
//
// Selecting the wrong shape MISCOMPILES: a `direct-plt` format reached
// via the `indirect-slot` opcode dereferences the PLT stub's CODE bytes
// as a function pointer → SIGSEGV. Consumed by MIR→LIR `lowerCall`,
// which picks `call_indirect_via_extern` (indirect-slot) vs the plain
// `call` opcode (direct-plt). Cross-tier vocabulary (the LIR lowerer
// needs it) so it lives here beside `ObjectFormatKind`, not in the
// link substrate header.
enum class ExternCallDispatch : std::uint8_t {
    IndirectSlot = 1,  // PE IAT / Mach-O __got: deref a pointer slot
    DirectPlt    = 2,  // ELF PLT: direct call to the linker's PLT stub
};

inline constexpr EnumNameTable<ExternCallDispatch, 2> kExternCallDispatchTable{{{
    { ExternCallDispatch::IndirectSlot, "indirect-slot" },
    { ExternCallDispatch::DirectPlt,    "direct-plt"    },
}}};

[[nodiscard]] constexpr std::string_view
externCallDispatchName(ExternCallDispatch d) noexcept {
    return kExternCallDispatchTable.name(d);
}
[[nodiscard]] constexpr std::optional<ExternCallDispatch>
externCallDispatchFromName(std::string_view s) noexcept {
    return kExternCallDispatchTable.fromName(s);
}

// ── Extern-DATA import binding model (D-LK-EXTERN-DATA-IMPORT) ─────
//
// How an imported library DATA OBJECT (libc's `stdout` — an extern
// object that lives in the shared library, not a function) is BOUND
// into the emitted image — a property of the OBJECT FORMAT's dynamic-
// import model, exactly like `ExternCallDispatch` above. A format that
// declares NO binding model cannot carry a data import at all: binding
// one through the FUNCTION-import machinery (PLT stub / IAT thunk)
// would make code read jump-stub BYTES as the object's value — the
// silent-miscompile class the linker's pre-walker reject exists for.
//
//   * `copy-relocation` (ELF ET_EXEC R_X86_64_COPY / R_AARCH64_COPY):
//     the executable reserves a correctly-sized `.bss` slot per
//     imported object, exports the symbol as a DEFINED OBJECT at that
//     slot, and emits one COPY relocation; the dynamic loader memcpy's
//     the library's object into the slot at startup and every image
//     (including the library itself, by symbol interposition) then
//     references the executable's copy. The standard glibc non-PIE
//     ET_EXEC mechanism — zero new instruction encodings.
//
//   * `got-indirect` (Mach-O `__got` S_NON_LAZY_SYMBOL_POINTERS; c117):
//     the address of the imported object is LOADED at run time from a
//     per-object non-lazy pointer slot (`__DATA_CONST,__got`) that the
//     dynamic loader (dyld) binds to the library's object. Code that
//     needs the object's ADDRESS loads the slot (x86_64 `mov r,[rip+
//     GOTPCREL]`; arm64 `adrp+ldr`) — one extra indirection vs a direct
//     address, but ZERO copy + PIE-compatible (macOS arm64 images are
//     always PIE). This is the model the PE `__imp_` data thunk will
//     also take when it lands; the __got slot infrastructure already
//     exists for the function-import stubs (they jump THROUGH it).
//
// The PE `__imp_` data-thunk model would be a further NEW member; the
// linker gate + walkers dispatch on the declared member, never on a
// format-name branch. Consumed by the linker's pre-walker data-import
// gate (linker.cpp) + the per-format walker that implements the
// declared mechanism (elf.cpp copy-relocation / macho.cpp __got).
enum class DataImportBinding : std::uint8_t {
    CopyRelocation = 1,  // ELF ET_EXEC R_*_COPY exec-local .bss copy
    GotIndirect    = 2,  // Mach-O __got non-lazy pointer (dyld-bound)
};

inline constexpr EnumNameTable<DataImportBinding, 2> kDataImportBindingTable{{{
    { DataImportBinding::CopyRelocation, "copy-relocation" },
    { DataImportBinding::GotIndirect,    "got-indirect" },
}}};

[[nodiscard]] constexpr std::string_view
dataImportBindingName(DataImportBinding b) noexcept {
    return kDataImportBindingTable.name(b);
}
[[nodiscard]] constexpr std::optional<DataImportBinding>
dataImportBindingFromName(std::string_view s) noexcept {
    return kDataImportBindingTable.fromName(s);
}

// ── Thread-local access model (D-CSUBSET-THREAD-LOCAL, TLS C1) ─────
//
// HOW code reaches a thread-local object's per-thread copy — a property
// of the OBJECT FORMAT's TLS runtime contract (who sets the thread
// pointer up, what it points at), exactly like `ExternCallDispatch` /
// `DataImportBinding` above. Keyed by the format because the SAME CPU
// target uses different access sequences under different formats
// (x86_64-ELF reads `fs:[0]`; x86_64-PE reads the TEB slot `gs:[0x58]`
// and indexes a per-module slot array), so it cannot live on the target
// schema. The VALUES (segment-override byte, base displacement) are
// per-format config; the SHAPES (which instructions) come from the
// target schema's opcode rows — the lowering branches only on this
// closed verb set, never on a format/CPU identity.
//
//   * `local-exec` (ELF static TLS, C1): the thread pointer register
//     is read via ONE dereference of `segmentPrefixByte:[baseDisplacement]`
//     (x86_64 Linux: `mov r, fs:[0]` — fs:[0] holds the tcbhead's own
//     address = tp), then the object's address is `tp + tpoff(sym)`
//     where tpoff is a LINK-TIME constant (the `tls-tpoff32`-class
//     relocation the walker resolves via the target's TlsIdentity
//     variant formula). Correct for a statically-merged executable
//     (module 1 is always tp-adjacent).
//   * `pe-indexed` (Windows, C3): `gs:[0x58]` = the TEB's
//     ThreadLocalStoragePointer slot array; the module's slot index is
//     loaded from `_tls_index` and the object sits at
//     `slots[_tls_index] + offset`. Declared now as vocabulary; its
//     LOWERING lands with the PE TLS cycle — the MIR→LIR arm fails
//     LOUD (never a silently-wrong local-exec sequence) until then.
//   * `macho-tlv` (Mach-O, C4): access is a CALL through the object's
//     `__thread_vars` TLV descriptor (`_tlv_bootstrap`). Declared as
//     vocabulary; fails loud at lowering until the Mach-O TLS cycle.
//
// A format that declares NO `tlsAccess` block cannot lower a
// thread-local access at all: MIR→LIR fails loud
// (`K_FormatLacksThreadLocalSupport`) on the first thread-local
// GlobalAddr — never a silent process-shared alias.
enum class TlsAccessModel : std::uint8_t {
    LocalExec = 1,  // ELF static TLS: tp-register + link-time tpoff
    PeIndexed = 2,  // PE TEB slot-array via _tls_index (lowering: PE TLS cycle)
    MachoTlv  = 3,  // Mach-O TLV descriptor call (lowering: Mach-O TLS cycle)
};

inline constexpr EnumNameTable<TlsAccessModel, 3> kTlsAccessModelTable{{{
    { TlsAccessModel::LocalExec, "local-exec" },
    { TlsAccessModel::PeIndexed, "pe-indexed" },
    { TlsAccessModel::MachoTlv,  "macho-tlv"  },
}}};

[[nodiscard]] constexpr std::string_view
tlsAccessModelName(TlsAccessModel m) noexcept {
    return kTlsAccessModelTable.name(m);
}
[[nodiscard]] constexpr std::optional<TlsAccessModel>
tlsAccessModelFromName(std::string_view s) noexcept {
    return kTlsAccessModelTable.fromName(s);
}

// D-CSUBSET-THREAD-LOCAL (TLS C3): the reserved well-known SymbolId VALUE
// that names the PE `_tls_index` slot — a LINK-TIER writer-minted SINGLETON
// (never a MIR symbol) that the `pe-indexed` access sequence's riprel read
// targets AND the PE writer binds. It is a HIGH sentinel a DENSE per-CU
// SymbolId (minted upward from 1) can never reach, so on the single-CU
// emission path (the only path a thread-local access + definition co-reside
// on today) it survives from MIR→LIR lowering to `pe.cpp` UNREMAPPED and the
// writer binds `symbolVa[reserved] = _tls_index VA` unambiguously. On a
// multi-CU merge the retarget would remap it to a fresh dense id → the riprel
// reloc resolves against no `symbolVa` entry → FAIL-LOUD undefined (multi-file
// extern-`thread_local` is the deferred `D-PIPELINE-CU5-MULTIFILE-EXTERN-DATA`
// surface; never a silent wrong-address). Both `mir_to_lir.cpp` (the lowering)
// and `pe.cpp` (the writer) read this ONE constant — a plain `std::uint32_t`
// so this leaf header stays free of the `strong_ids.hpp` dependency; each side
// wraps it in `SymbolId{...}` at use.
inline constexpr std::uint32_t kTlsIndexReservedSymbolIdValue = 0xFFFF'FF01u;

// Is `v` a writer-minted LINK-TIER reserved SymbolId VALUE — one the FORMAT
// WRITER (not the module) defines into its `symbolVa` map? Today the sole
// member is the PE `_tls_index` singleton above. The linker's pre-writer
// cross-reference unifier EXEMPTS these from its undefined-symbol check —
// exactly as it exempts extern imports (which the import-table writer
// resolves) — because the writer binds them (or fails loud if the format has
// no TLS machinery, e.g. an ELF module that somehow carried the id). Keeps the
// linker free of any format-name branch: it asks "is this a writer-reserved
// id", not "is this PE".
[[nodiscard]] constexpr bool
isWriterReservedSymbolIdValue(std::uint32_t v) noexcept {
    return v == kTlsIndexReservedSymbolIdValue;
}

// The format's TLS access block (`"tlsAccess"` in `.format.json`).
// `segmentPrefixByte` is the x86 segment-override prefix byte the
// `tlsbase` instruction emits as its FIRST byte (0x64 = fs on
// ELF-Linux; 0x65 = gs on PE) — threaded to the encoder via the LIR
// instruction's payload so the x86_64 TARGET JSON stays format-blind
// (the same `tlsbase` opcode row serves ELF and PE with config-only
// differences). `baseDisplacement` is the literal disp32 of the
// thread-pointer slot (`fs:[0]` on ELF; the TEB's `gs:[0x58]` on PE).
// Non-x86 targets ignore `segmentPrefixByte` (their tp read is a
// dedicated register, e.g. arm64 MRS TPIDR_EL0 — the opcode row
// carries the shape; this block still selects the MODEL).
struct DSS_EXPORT TlsAccessInfo {
    TlsAccessModel model             = TlsAccessModel::LocalExec;
    std::uint8_t   segmentPrefixByte = 0;  // x86 segment-override byte (0x64 fs / 0x65 gs)
    std::uint32_t  baseDisplacement  = 0;  // disp32 of the tp slot (ELF fs:[0] → 0)
    // D-CSUBSET-THREAD-LOCAL (TLS C3): the NAME of the writer-minted
    // `_tls_index` singleton the `pe-indexed` access sequence reads (PE:
    // "__dss_tls_index"). REQUIRED for `pe-indexed` (the loader validates
    // it non-empty — a pe-indexed model without a named index slot cannot
    // lower); ignored (and empty) for `local-exec`/`macho-tlv`, whose
    // access shapes do not index a module TLS array. The NUMERIC id both
    // sides agree on is `kTlsIndexReservedSymbolIdValue` above; this string
    // is the human-readable anchor (diagnostics + the loader's presence
    // gate) so the config stays self-describing.
    std::string    tlsIndexSlotName;
};

// ── Shipped-library synthesis vehicle (D-CSUBSET-C11-THREADS-MACHO) ─
//
// WHICH native primitive family a COMPILER-SYNTHESIZED shipped-library
// shim (today: C11 <threads.h>) emits its body over — a property of the
// OBJECT FORMAT's host-OS runtime, exactly like `TlsAccessInfo` /
// `ProcessArgs` above. A format whose libc does NOT export a shipped
// header's symbols has each such function SYNTHESIZED by the compiler
// (src/mir/merge/synth_threads_shim.cpp) over the OS's real primitives;
// this block declares WHICH primitive family + WHICH library to import
// them from, so the synth pass never branches on the format identity.
//
//   * `win32`   (PE / Windows): the CRT exports no thrd_*/mtx_*/… , so
//     each C11 threads function is emitted over kernel32 CRITICAL_SECTION
//     / CONDITION_VARIABLE / Fls* primitives (libraryPath = kernel32.dll).
//   * `pthread` (Mach-O / Darwin): macOS libSystem exports no C11 threads
//     symbols (verified — `_thrd_create` is undefined at link), but
//     pthread IS in libSystem, so each function is emitted over
//     pthread_mutex_* / pthread_cond_* / pthread_key_* / pthread_self /
//     sched_yield (libraryPath = /usr/lib/libSystem.B.dylib).
//
// ELF declares NO block: glibc>=2.34 exports the C11 thread API directly
// from libc.so.6, so those symbols are ordinary FFI imports and the synth
// pass is a clean no-op (its recipe map is empty on elf). A format that
// carries synthesize-tagged shipped symbols but declares NO vehicle fails
// LOUD in the synth pass (never a silently-defaulted vehicle). The pass
// dispatches on this closed verb set, never on a format/CPU identity.
enum class LibrarySynthVehicle : std::uint8_t {
    Win32   = 1,  // kernel32 CRITICAL_SECTION / CONDITION_VARIABLE / Fls*
    Pthread = 2,  // POSIX pthread_* (Darwin libSystem)
};

inline constexpr EnumNameTable<LibrarySynthVehicle, 2> kLibrarySynthVehicleTable{{{
    { LibrarySynthVehicle::Win32,   "win32"   },
    { LibrarySynthVehicle::Pthread, "pthread" },
}}};

[[nodiscard]] constexpr std::string_view
librarySynthVehicleName(LibrarySynthVehicle v) noexcept {
    return kLibrarySynthVehicleTable.name(v);
}
[[nodiscard]] constexpr std::optional<LibrarySynthVehicle>
librarySynthVehicleFromName(std::string_view s) noexcept {
    return kLibrarySynthVehicleTable.fromName(s);
}

// The format's shipped-library synthesis block (`"librarySynthesis"` in
// `.format.json`). `vehicle` selects the primitive family the synth pass
// emits over; `libraryPath` is the native library its on-demand helper
// imports name (the value that dissolves the pass's former hardcoded
// "kernel32.dll"). Present on pe/macho formats; absent on elf (direct
// FFI) and wasm/spirv (no native shim).
struct DSS_EXPORT LibrarySynthesis {
    LibrarySynthVehicle vehicle = LibrarySynthVehicle::Win32;
    std::string         libraryPath;
};

// THE single source of truth for the extern-call-site SHAPE selection
// (D-FFI-EXTERN-CALL-DISPATCH). `true`  → the call site DEREFERENCES a
// pointer slot (x86_64 `FF 15 disp32` = `call [RIP+disp]`); the LIR
// opcode is `call_indirect_via_extern`. `false` → the call site is a
// PLAIN DIRECT call (x86_64 `E8 disp32`, ARM64 `BL imm26`) to the
// linker-synthesized PLT/stub which performs the indirection itself;
// the LIR opcode is the universal `call`.
//
// Three independent consumers select the call shape from this rule:
//   * `mir_to_lir.cpp::lowerCall`     — user-level extern calls (FFI).
//   * `entry_trampoline.cpp`          — the synthesized `_exit` /
//                                       `ExitProcess` ByNameImport call.
//   * `linker.cpp::mergeModules`      — the cross-CU merge's slot-vs-
//                                       direct-bind decision (c154).
// All call THIS function so the rule lives exactly once (no `if(arch)`,
// no second copy that could drift to the opposite — and opposite is a
// SIGSEGV: dereferencing a PLT stub's code as a pointer). Keyed on the
// OBJECT FORMAT's dispatch model, never the CPU target.
[[nodiscard]] constexpr bool
externCallUsesIndirectShape(ExternCallDispatch d) noexcept {
    return d == ExternCallDispatch::IndirectSlot;
}

} // namespace dss
