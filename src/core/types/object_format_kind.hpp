#pragma once

#include "core/export.hpp"
#include "core/types/enum_name_table.hpp"   // EnumNameTable<E,N> (leaf header — no target_schema cycle)

#include <cstddef>
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

// ★ THE SENTINEL SPELLS CORRECTLY. `Unknown` carries the name "unknown" in the
// table above, so `objectFormatKindFromName("unknown")` SUCCEEDS. A config site
// that validates a format name ONLY through that function therefore accepts the
// project's universal invalid sentinel as if it named a real format — and every
// downstream per-kind dispatch then matches NOTHING and silently takes a default
// arm. That is a DIFFERENT species from a typo hole (`"elff"` fails the name
// lookup and is caught): the sentinel passes the lookup and dies later, quietly.
//
// So EVERY config surface that resolves a declared object-format NAME must ALSO
// ask `isSelectableObjectFormatKind` and reject the sentinel explicitly — the
// `bitFieldStrategy` "none" discipline (object_format_schema_json.cpp), applied
// to this vocabulary. `kObjectFormatKindSentinelRejection` is the shared wording
// so the diagnostics cannot drift site to site; each site supplies its own
// diagnostic CODE and JSON path.
[[nodiscard]] constexpr bool
isSelectableObjectFormatKind(ObjectFormatKind k) noexcept {
    return k != ObjectFormatKind::Unknown;
}

inline constexpr std::string_view kObjectFormatKindSentinelRejection =
    "'unknown' is the invalid sentinel, not a selectable object format — it "
    "names no real format, so a declaration under it could never fire";

// The number of `ObjectFormatKind` enumerators INCLUDING the `Unknown`
// sentinel — i.e. one past the largest ordinal, so an
// `std::array<T, kObjectFormatKindCount>` indexed by
// `static_cast<std::size_t>(kind)` is always in range. Derived from the name
// table rather than written as a literal, so adding an enumerator cannot leave
// a per-kind array one slot short (which would silently truncate the new kind
// to whatever the bounds check does). Consumed by `TargetSchema`'s per-format
// `charIsUnsigned` override table.
inline constexpr std::size_t kObjectFormatKindCount =
    kObjectFormatKindTable.rows.size();

// The derivation above is only a SAFE array size while every enumerator's
// ordinal is < the row count (the ordinals are dense from 0 today). A future
// sparse enumerator would make `array[ordinal]` an out-of-range index that no
// runtime bounds check on the array's own `.size()` could catch — so the
// density is asserted here, at the one place the count is derived.
static_assert([] {
    for (auto const& r : kObjectFormatKindTable.rows) {
        if (static_cast<std::size_t>(r.first) >= kObjectFormatKindCount) {
            return false;
        }
    }
    return true;
}(), "ObjectFormatKind ordinals must stay dense from 0 — a per-kind array "
     "sized by the name-table row count would otherwise index out of range");

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

// ── Extern-ADDRESS materialization binding (D-LK-ARM64-EXTERN-DATA-
//     ADDR-PIE-GOT, TF-C52) ───────────────────────────────────────
//
// How code MATERIALIZES the ADDRESS of an undefined/preemptible extern
// (function OR data) as a LIVE code-form VALUE — an argument, an
// initializer of an automatic, a returned function pointer: any use
// that is NOT a call (that folds to a direct branch) and NOT a
// static-data-item initializer (a separate abs64 data reloc). A
// property of the OBJECT FORMAT's link contract, exactly like
// `ExternCallDispatch` / `DataImportBinding` above.
//
//   * `got` (ELF relocatable / static-archive member): materialize the
//     address THROUGH a GOT slot the FINAL (foreign) linker synthesizes
//     — arm64 `adrp x,:got:sym` (R_AARCH64_ADR_GOT_PAGE) + `ldr x,
//     [x,:got_lo12:sym]` (R_AARCH64_LD64_GOT_LO12_NC). A relocatable
//     `.o`/`.a` is linked by a foreign default-PIE toolchain (gcc/clang
//     with no `-no-pie`): an ABSOLUTE `adrp`+`add` (ADR_PREL_PG_HI21 +
//     ADD_ABS_LO12_NC) against a symbol that may bind externally is
//     REJECTED ("relocation ... which may bind externally can not be
//     used when making a shared object"). The GOT indirection is what
//     gcc/clang themselves emit for `&extern` in a default-PIE `.o`, so
//     it links cleanly. Distinct from `DataImportBinding::GotIndirect`
//     (the Mach-O __got model, a DSS-resolved lea-of-slot + deref): here
//     the FOREIGN linker owns the slot, reached via the arm64 GOT-page
//     relocs, not a DSS-local slot.
//
// A format that declares NO `externAddrBinding` materializes an
// `&extern` value via the ordinary lea (an absolute page-pair on arm64;
// a PC-relative rel32 on x86_64 — already foreign-PIE-safe there). The
// DSS-linked EXEC formats never declare it (they use
// `dataImportBinding: "copy-relocation"` for data + a direct address for
// functions); the PIE/dyn formats use the c117 DSS-local-slot path. Only
// the arm64 relocatable + static-archive formats declare `got`. Consumed
// by MIR→LIR `lowerGlobalAddr` (the value-form arm, keyed on the
// derived symbol set — never a format/arch identity branch). An unknown
// VALUE fails loud at load (the closed-enum check).
enum class ExternAddrBinding : std::uint8_t {
    Got = 1,  // ELF relocatable: address via a foreign-linker GOT slot
};

inline constexpr EnumNameTable<ExternAddrBinding, 1> kExternAddrBindingTable{{{
    { ExternAddrBinding::Got, "got" },
}}};

[[nodiscard]] constexpr std::string_view
externAddrBindingName(ExternAddrBinding b) noexcept {
    return kExternAddrBindingTable.name(b);
}
[[nodiscard]] constexpr std::optional<ExternAddrBinding>
externAddrBindingFromName(std::string_view s) noexcept {
    return kExternAddrBindingTable.fromName(s);
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

// ── Per-PROGRAM stack-reserve control (D-SQLITE-PE64-FULL-TIER-STACK-DEPTH) ─
//
// WHERE — if anywhere — an object format can record "this PROGRAM needs N
// bytes of stack". The required stack is a property of the PROGRAM (its
// deepest call chain), not of the format, so it cannot live as a fixed
// number in a `.format.json`: the measured motivating case is sqlite's
// `e_fkey-63.1.1` (1000 levels of nested trigger recursion), which
// stack-overflows at the Windows 1 MiB default even when built by GCC.
// Every real toolchain exposes it as a link-time request (MSVC `/STACK`,
// GNU ld `-Wl,--stack`); this block is how a format DECLARES that it can
// carry one, so the engine asks the CAPABILITY and never the identity.
//
// The capability is NOT uniform across formats, and — critically — not even
// uniform within one format KIND:
//   * PE **executable** — IMAGE_OPTIONAL_HEADER64.SizeOfStackReserve is
//     exactly this field, read by the Windows loader when it commits the
//     initial thread's stack. Declares the block.
//   * PE **dll** — has the same header field, but the loader IGNORES it
//     (the .exe's value governs the main thread; other threads take the
//     .exe default or an explicit CreateThread size). Declares NOTHING: an
//     `if (kind == pe)` engine branch would silently write a field nothing
//     reads. This asymmetry is the reason the capability is per-FORMAT
//     config rather than a kind test.
//   * Mach-O executable — `LC_MAIN.stacksize` is a genuine equivalent.
//     NOT declared yet: no walker arm implements it (see the vehicle enum).
//   * ELF — has NO image field for stack SIZE. `PT_GNU_STACK` encodes
//     EXECUTABILITY, not size; the size comes from the kernel/`ulimit -s`
//     (or `-z stacksize` on the few linkers that honour it). Declares
//     nothing, so a request on an ELF target fails LOUD.
//
// WHICH header field / load command a declared reserve lands in. A vehicle
// names a structure of ONE image format, so the loader cross-checks it
// against `format.kind` (a config-coherence rule, not an engine branch):
// declaring `pe-optional-header` on an ELF schema is dead config whose
// request the ELF walker would silently drop.
//
// Exactly ONE vehicle ships today because exactly one walker arm
// implements one (`src/link/format/pe.cpp`). `macho-lc-main` is the
// natural second row and lands WITH its walker arm — never before it, or
// a Mach-O schema could declare a reserve that is silently dropped.
enum class StackReserveVehicle : std::uint8_t {
    // IMAGE_OPTIONAL_HEADER64.SizeOfStackReserve (PE/COFF §3.4), at
    // optional-header offset +72. Implemented by pe.cpp's image arm.
    PeOptionalHeader = 1,
};

inline constexpr EnumNameTable<StackReserveVehicle, 1> kStackReserveVehicleTable{{{
    { StackReserveVehicle::PeOptionalHeader, "pe-optional-header" },
}}};

[[nodiscard]] constexpr std::string_view
stackReserveVehicleName(StackReserveVehicle v) noexcept {
    return kStackReserveVehicleTable.name(v);
}
[[nodiscard]] constexpr std::optional<StackReserveVehicle>
stackReserveVehicleFromName(std::string_view s) noexcept {
    return kStackReserveVehicleTable.fromName(s);
}

// The format's stack-reserve capability block (`"stackReserveControl"` in
// `.format.json`). Presence IS the capability: a format that omits it
// cannot express a per-program stack reserve, and a request against it
// fails loud (`K_FormatLacksStackReserveControl`) rather than being
// silently dropped.
//
// The three bounds make the REQUEST validation config-driven: the engine
// range-checks a request against the numbers the FORMAT declares, so no
// PE-specific constant is baked into shared substrate. All three are
// REQUIRED when the block is present — a capability that does not state
// its own legal range cannot validate anything, and a defaulted range
// would silently admit an absurd value.
// ── WHY a format cannot carry a stack reserve (the remedy axis) ─────
//
// The refusal above is only half the job. A user who asked for 4 MiB needs to
// know WHERE the property actually lives for THIS artifact, and that differs
// sharply between formats that all merely "declare no capability":
//
//   * a PE **dll** HAS the header field and it is INERT (measured: 12
//     unrelated Microsoft system DLLs -- ntdll, ucrtbase, kernel32, msvcrt,
//     shell32, crypt32, ... -- all carry the IDENTICAL untuned 0x40000, while
//     8 system EXEs carry tuned, VARYING values [0x80000 / 0x100000] with
//     SizeOfStackCommit spread across 8 KiB..1008 KiB. Uniformity across
//     unrelated DLLs versus per-program tuning on EXEs is the signature of a
//     field nothing reads. Documented Win32 semantics agree: CreateThread's
//     `dwStackSize` "uses the default size for the EXECUTABLE" when zero --
//     the executable's, not the loaded module's). Remedy: the host .exe.
//   * a relocatable object / static archive / dylib has NO such field at all.
//     Remedy: the final link that produces the executable.
//   * ELF has no image field on ANY flavor -- the size is set by the OS at
//     process/thread start. Remedy: `ulimit -s` / setrlimit.
//   * Mach-O exec COULD carry it (`LC_MAIN.stacksize`) but no DSS walker arm
//     writes it. Remedy: none -- an honest, named compiler gap.
//
// Those four remedies cannot be derived from any other declared verb without
// asking WHICH FORMAT this is, so the reason is DECLARED, one closed verb per
// schema, and the engine looks the remedy up in the table below. Declaring the
// reason is OPTIONAL: a format that omits it still fails LOUD, just with the
// generic message (fail-closed degrades to less-specific, never to silent).
enum class StackReserveUnsupportedReason : std::uint8_t {
    // The OS sets the stack at process/thread start; no image field has a say.
    RuntimeControlled   = 1,
    // The header HAS the field, but this platform's loader does not read it.
    LoaderIgnoresField  = 2,
    // This artifact form has no header field for a stack size at all.
    NoImageField        = 3,
    // The format CAN express it, but no DSS walker arm writes it yet.
    // UNLIKE the three verbs above — which are PERMANENT facts about a
    // platform — this one names a gap that is OURS and is CLOSABLE, so it is
    // tracked as a real backlog item with a trigger and defined closing work:
    // D-LK-MACHO-STACK-RESERVE-LC-MAIN. A comment tells whoever HITS the gap;
    // the registry row is what keeps it from being forgotten.
    WalkerNotImplemented = 4,
};

inline constexpr EnumNameTable<StackReserveUnsupportedReason, 4>
kStackReserveUnsupportedReasonTable{{{
    { StackReserveUnsupportedReason::RuntimeControlled,    "runtime-controlled"    },
    { StackReserveUnsupportedReason::LoaderIgnoresField,   "loader-ignores-field"  },
    { StackReserveUnsupportedReason::NoImageField,         "no-image-field"        },
    { StackReserveUnsupportedReason::WalkerNotImplemented, "walker-not-implemented"},
}}};

[[nodiscard]] constexpr std::string_view
stackReserveUnsupportedReasonName(StackReserveUnsupportedReason r) noexcept {
    return kStackReserveUnsupportedReasonTable.name(r);
}
[[nodiscard]] constexpr std::optional<StackReserveUnsupportedReason>
stackReserveUnsupportedReasonFromName(std::string_view s) noexcept {
    return kStackReserveUnsupportedReasonTable.fromName(s);
}

// The REMEDY prose, one row per declared reason. Kept beside the vocabulary so
// the verb and its explanation cannot drift apart, and looked up by the
// diagnostic through a generic table walk — the engine never asks which format
// it is holding, only which reason that format DECLARED.
inline constexpr EnumNameTable<StackReserveUnsupportedReason, 4>
kStackReserveUnsupportedRemedyTable{{{
    { StackReserveUnsupportedReason::RuntimeControlled,
      "on this platform the stack size is a RUNTIME property, not an image "
      "property: the kernel sizes the initial thread's stack from the process "
      "limit (`ulimit -s` / setrlimit RLIMIT_STACK) and additional threads "
      "from pthread_attr_setstacksize. Nothing the linker writes can change "
      "it, so raise the limit where the program RUNS." },
    { StackReserveUnsupportedReason::LoaderIgnoresField,
      "this artifact's header does contain the field, but the platform loader "
      "IGNORES it for a module of this kind -- a thread's stack is sized from "
      "the EXECUTABLE that loads this module (its own header) or explicitly "
      "at thread creation (Win32 CreateThread `dwStackSize`). Writing the "
      "field here would produce a build that reports success and changes "
      "NOTHING, so request the reserve on the EXECUTABLE instead." },
    { StackReserveUnsupportedReason::NoImageField,
      "this artifact form carries no image header field for a stack size at "
      "all -- it is an input to a later link, not a loadable program. Request "
      "the reserve on the build that produces the final EXECUTABLE image." },
    { StackReserveUnsupportedReason::WalkerNotImplemented,
      "this format CAN express a stack reserve, but no DSS walker arm writes "
      "it yet, so the request cannot be honoured. This is a named compiler "
      "gap, not a property of the platform -- the request is refused rather "
      "than silently dropped so the gap stays visible. Tracked as "
      "D-LK-MACHO-STACK-RESERVE-LC-MAIN." },
}}};

[[nodiscard]] constexpr std::string_view
stackReserveUnsupportedRemedy(StackReserveUnsupportedReason r) noexcept {
    return kStackReserveUnsupportedRemedyTable.name(r);
}

struct DSS_EXPORT StackReserveControl {
    StackReserveVehicle vehicle = StackReserveVehicle::PeOptionalHeader;
    // Smallest honourable request. Below this the image is legal but the
    // program cannot start (the loader must still commit the initial
    // stack pages), so a smaller request is rejected rather than emitted.
    std::uint64_t minimumBytes     = 0;
    // Largest honourable request — the "absurd value" boundary. A reserve
    // is VIRTUAL address space, so a generous cap is correct; the point is
    // that a fat-fingered 0x100000000000 fails loud at link instead of
    // producing an image the loader refuses to start.
    std::uint64_t maximumBytes     = 0;
    // Required alignment of a request, in bytes (page granularity on PE).
    // A misaligned request is rejected rather than silently rounded — a
    // silent round is the knob-that-lies class.
    std::uint64_t granularityBytes = 0;
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
