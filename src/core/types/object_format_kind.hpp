#pragma once

#include "core/export.hpp"
#include "core/types/enum_name_table.hpp"   // EnumNameTable<E,N> (leaf header — no target_schema cycle)

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
//   * `src/ffi/`     — `synthesizeFfiFromSourceDecls` reports the
//                      kind in `F_FfiNoImportLibraryForFormat`
//                      diagnostics; `abi_catalog.cpp`'s `kAbiCatalog`
//                      still SELECTS THE CALLING CONVENTION from a
//                      table keyed on (target name, this enum) — an
//                      OPEN identity branch, anchored at [[D-FFI-ABI-
//                      CATALOG-SELECTS-CALLING-CONVENTION-BY-FORMAT-
//                      IDENTITY]], NOT a sanctioned use.
//                      ⚠ THIS BULLET USED TO READ "FF4 C-mangling
//                      dispatches on `ObjectFormatKind` (per-format
//                      leading-underscore rule)", which `c_mangle.cpp`
//                      has CONTRADICTED since TF-C122 deleted
//                      `kCManglingRules` and made decoration a DECLARED
//                      per-format verb. Corrected TF-C125.
//   * `src/program/` — `compile_pipeline.cpp`'s archive-member reader
//                      switch, `program.cpp`'s `.obj`/`.o` member-
//                      extension pick, and `cross_validate_target_
//                      format.cpp`'s machine-code switch — also OPEN
//                      identity branches of the same species, MEASURED
//                      in TF-C125 and anchored. Listed so the next
//                      reader does not mistake "it is in this list"
//                      for "it is sanctioned".
//   * `src/core/types/grammar_schema_json.cpp` — semantic config
//                      loader validates the per-language
//                      `externLibraryByFormat` map keys against
//                      this enum's name table at language-load time
//                      (catches a typo like `"pee"` instead of
//                      `"pe"` at config load rather than at compile
//                      time).
//   ★ `src/link/` IS NO LONGER A CALLER, and that is the TF-C125 result.
//     The object-format schema tier resolves an `ObjectFormatBackend`
//     instead, and its two loader TUs carry a COMPILE-ERROR PIN that makes
//     this enum's name AMBIGUOUS inside them: `ObjectFormatKind::Elf` is
//     unspellable there, not merely absent
//     (D-LINK-OBJECT-FORMAT-SCHEMA-RETAINS-KIND-IDENTITY-BRANCHES).
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

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kObjectFormatKindTable);

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

// The reserved SPELLING of the invalid sentinel, as it appears in a config
// file. Derived from the shared name table rather than written out, so it
// cannot drift from the enum.
//
// ★ WHY A NAME-LEVEL FORM EXISTS AT ALL (TF-C125). The format loader must
// still distinguish "you wrote the reserved word `unknown`" from "you wrote a
// spelling nobody claims" — two different author mistakes that earn two
// different diagnostics — but after the identity-branch removal that TU cannot
// spell `ObjectFormatKind::Unknown`, and must not: it resolves a declared
// spelling to a BACKEND, and no backend claims the sentinel. Comparing a
// declared spelling against the reserved spelling is config-vocabulary against
// config-vocabulary, which is the one shape the ruling does permit.
inline constexpr std::string_view kObjectFormatKindSentinelName =
    kObjectFormatKindTable.name(ObjectFormatKind::Unknown);

[[nodiscard]] constexpr bool
isObjectFormatKindSentinelName(std::string_view s) noexcept {
    return s == kObjectFormatKindSentinelName;
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

// The SELECTABLE spellings — every row a config author may actually write,
// i.e. the table minus the reserved `unknown` sentinel. This is what a
// diagnostic's "expected one of …" half must render.
//
// ⚠ IT IS NOT `keysOf(kObjectFormatKindTable.rows, …)`: that would advertise
// `'unknown'` as an accepted value, which the sentinel note above spends a
// paragraph explaining is the ONE spelling that passes the name lookup and
// then silently matches nothing. The filter is the point.
//
// ⚠⚠ THE COUNT USED TO BE `kObjectFormatKindCount - 1` AND THAT SPELLING COULD
// NOT FAIL. D-CORE-NAMESWHERE-COUNT-DERIVED-FROM-THE-TABLE-IS-A-TAUTOLOGY.
// `kObjectFormatKindCount` IS `kObjectFormatKindTable.rows.size()`, and
// `namesWhere<M>` compares `M` against the rows THIS TABLE's predicate accepts:
// with a predicate that rejects exactly the one sentinel row, both sides were
// computed from the same array and moved together. A SEVENTH format kind
// compiled clean — while the comment that stood here said it "makes this
// initializer a compile error", which was ✔MEASURED FALSE.
//
// ★ WHAT REPLACES IT, AND WHY IT TAKES TWO PARTS. ✔MEASURED with
// `g++ -std=c++23 -fsyntax-only` over a nine-arm probe (see the full write-up
// at `kSelectableExitMechanismNames` in `core/types/target_schema.hpp`): the
// literal count reds on a NEW SELECTABLE enumerator but NOT on a second
// UNSELECTABLE one, which is exactly the case the derived spelling did catch.
// The literal alone therefore moves the blind spot rather than closing it. The
// `static_assert` below pins the number of rows the predicate REJECTS, relating
// the table's own row count to the projection's literal count — so both
// mutations red, and the sentence above is true as written.
inline constexpr auto kSelectableObjectFormatKindNames =
    namesWhere<5>(kObjectFormatKindTable, isSelectableObjectFormatKind);
static_assert(kObjectFormatKindTable.rows.size()
                  == kSelectableObjectFormatKindNames.size() + 1,
              "kObjectFormatKindTable must have exactly ONE unselectable row "
              "(the 'unknown' sentinel) — a second one leaves `namesWhere`'s "
              "literal count matching while every 'expected one of …' message "
              "silently stops naming the set the check accepts");

// ── C-symbol decoration scheme (D-FFI-CMANGLING-RULE-NOT-CONFIG-DRIVEN) ──
//
// HOW this object format decorates a canonical C identifier to obtain the
// name the LINKER sees — Apple's leading `_` versus nothing at all. A
// property of the OBJECT FORMAT's C ABI, exactly like `ExternCallDispatch`
// / `DataImportBinding` above, and keyed by the format for the same reason:
// the SAME CPU target decorates under macho64-x86_64-darwin and does not
// under elf64-x86_64-linux, so the rule cannot live on the target schema.
//
// ★ WHY THIS VOCABULARY EXISTS AT ALL — the defect it removes. Before this
// axis, ONE per-format fact had TWO OWNERS: the closed C++ table
// `kCManglingRules` (src/ffi/mangling/c_mangle.cpp, keyed on
// `ObjectFormatKind` and pinned by a `static_assert`), AND a duplicate
// LITERAL encoding inside the format descriptors themselves — the
// `macho64-*-exec` files ship `processExit.importMangledName: "_exit"`
// while their elf/pe siblings ship `"exit"`. A per-format fact encoded
// twice, in two languages, with nothing forcing the two to agree. That is
// the agnosticism bar's own shape inverted: per-format vocabulary belongs
// in `.format.json`, never in a C++ table keyed on a format enum.
//
// ★ THE DECORATION IS NOT COSMETIC, and the reason is MEASURED rather than
// stylistic: on the undecorated formats the underscored spelling ALREADY
// NAMES A DIFFERENT FUNCTION. `nm -D /lib/x86_64-linux-gnu/libc.so.6`
// (MEASURED 2026-08-06, WSL Ubuntu) shows `exit@@GLIBC_2.2.5` AND
// `_exit@@GLIBC_2.2.5` as two distinct exported symbols — C `exit(3)` (which
// flushes stdio) and POSIX `_exit(2)` (which does not); `objdump -p
// C:\Windows\System32\ucrtbase.dll` (MEASURED 2026-08-06) likewise exports
// `exit`, `_exit` AND `_Exit` as three distinct entries. So a format that
// wrongly claimed `leading-underscore` would not merely produce an ugly
// name — it would bind a working program to the WRONG FUNCTION, silently.
//
// ── Why a CLOSED ENUM and not a prefix STRING ────────────────────────
// The first design carried the decoration as a literal prefix
// (`cSymbolDecoration: "_"` / `""`). That was refuted and the refutation
// adopted: a free string makes every unintended value REPRESENTABLE (`"__"`,
// `" "`, a trailing-`@N` stdcall suffix that the prefix shape cannot even
// express), so the config could ask for decorations no engine arm
// implements and nothing would refuse it at load. A closed verb set can
// only name schemes that HAVE an implementation, and a typo fails loud at
// the `…FromName` lookup. Adding 32-bit PE's `_func@N` stdcall rule later
// is a new enumerator plus its engine arm — never a new string spelling
// that silently starts working.
enum class CSymbolDecorationScheme : std::uint8_t {
    // Zero is the INVALID sentinel: a hand-built `ObjectFormatData` that
    // never set the field is rejected by `ObjectFormatData::validate()`,
    // exactly as a zero `DataModel` / `HeaderNameMatching` is. The loader
    // path always sets it or fails.
    Unspecified       = 0,
    // This format does not decorate C symbols: the linker-visible name IS
    // the canonical C identifier. ELF (System V), PE/COFF x64, WASM (which
    // reaches imports through its import namespace) and SPIR-V (no C ABI
    // surface at all).
    None              = 1,
    // Apple's convention: exactly ONE leading underscore on every C symbol,
    // on both 32- and 64-bit Mach-O (the rule has no bitness axis).
    LeadingUnderscore = 2,
};

// ★ THE ARGUMENT THIS COMMENT USED TO MAKE WAS RIGHT, AND IT HAS BEEN ANSWERED
// IN THE TEMPLATE. Read `objectFormatKindName`'s "★ THE SENTINEL SPELLS
// CORRECTLY" note directly above before changing this. `EnumNameTable::name`
// FALLS BACK TO `rows[0].second` for an unlisted value, so a table that omits
// the sentinel would report the sentinel under the FIRST REAL SCHEME'S name
// ("none") — the worst possible answer, since `none` is a legitimate scheme
// and `validate()` would then see a populated field where none was declared.
// Listing the sentinel with an EMPTY name instead makes `fromName("")` RESOLVE
// TO IT, re-opening the same hole from the other side.
//
// Neither shape could express "this value has no spelling" — so three enums
// (this one, `dataModelName`, `headerNameMatchingName`) were each written out
// as a switch, and each thereby became a SECOND OWNER of its spellings beside
// its own `…FromName`. `EnumNameTable::nameOrEmpty` now expresses exactly that
// missing case IN THE SHARED TEMPLATE: leave the sentinel out of the table, so
// `fromName` cannot resolve it from any spelling, and ask `nameOrEmpty` on the
// name side, so an unlisted value renders EMPTY. All three are converted, and
// the `…Name(x).empty()` idiom `validate()` uses is unchanged.
inline constexpr EnumNameTable<CSymbolDecorationScheme, 2>
kCSymbolDecorationSchemeTable{{{
    { CSymbolDecorationScheme::None,              "none"               },
    { CSymbolDecorationScheme::LeadingUnderscore, "leading-underscore" },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kCSymbolDecorationSchemeTable);

[[nodiscard]] constexpr std::string_view
cSymbolDecorationSchemeName(CSymbolDecorationScheme s) noexcept {
    // The `-Werror=switch` backstop; it owns no spelling. A new scheme with no
    // `case` fails the BUILD rather than becoming a value with no name.
    switch (s) {
        case CSymbolDecorationScheme::Unspecified:
        case CSymbolDecorationScheme::None:
        case CSymbolDecorationScheme::LeadingUnderscore:
            break;
    }
    return kCSymbolDecorationSchemeTable.nameOrEmpty(s);   // NOT .name()
}

// `Unspecified` deliberately has NO JSON spelling — a spellable "unspecified"
// would let a typo look deliberate, and would let a format DECLARE the
// invalid sentinel and pass the required-declaration rule while meaning
// nothing. The empty string is rejected for the same reason: it is what a
// `"scheme": ""` typo produces — and leaving the sentinel OUT of the table is
// what makes `fromName("")` refuse it rather than resolve to it.
[[nodiscard]] constexpr std::optional<CSymbolDecorationScheme>
cSymbolDecorationSchemeFromName(std::string_view s) noexcept {
    return kCSymbolDecorationSchemeTable.fromName(s);
}

// The format's C-symbol decoration block (`"cSymbolDecoration"` in
// `.format.json`). A BLOCK carrying one `scheme` verb rather than a bare
// scalar, mirroring `processExit`'s `mechanism` dispatch: a future scheme
// that needs per-arm PARAMETERS (32-bit PE stdcall's `@N` byte count, say)
// gains a sibling key INSIDE this block instead of a second root key that
// only some formats may legally declare. REQUIRED on every format — see
// `ObjectFormatData::validate()` for why the rule is unconditional rather
// than exec-flavor-gated.
struct DSS_EXPORT CSymbolDecoration {
    CSymbolDecorationScheme scheme = CSymbolDecorationScheme::Unspecified;
};

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

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kExternCallDispatchTable);

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
//   * `got-indirect` (ELF `.got` + R_*_GLOB_DAT; Mach-O `__got`
//     S_NON_LAZY_SYMBOL_POINTERS; PE the IAT slot itself — c117/c149):
//     the ADDRESS of the imported object is LOADED at run time from a
//     per-object pointer slot the dynamic loader binds to the library's
//     object. Code that needs the object's address loads the slot
//     (x86_64 `mov r,[rip+disp]`; arm64 `adrp+ldr`) — one extra
//     indirection vs a direct address, zero copies, and it works in a
//     non-PIE image exactly as in a PIE one.
//
// ── WHY `copy-relocation` IS NOT A MEMBER, AND MUST NOT COME BACK ───
// D-LK-ELF-COPY-RELOC-CLAIMS-ONE-NAME-OF-AN-ALIAS-SET (born CLOSED).
// There USED to be a second member, `copy-relocation`: the ELF non-PIE
// ET_EXEC mechanism — the exec reserves a correctly-sized `.bss` slot
// per imported object, exports the symbol as a DEFINED OBJECT at that
// slot, emits one R_X86_64_COPY / R_AARCH64_COPY, and ld.so memcpy's
// the library's object in at startup so that every image referencing
// THAT NAME converges on the exec's copy.
// It is UNSOUND, and not merely inelegant: the convergence is
// NAME-SCOPED. glibc exports ONE `environ` object under THREE names
// (`__environ` GLOBAL, `environ` / `_environ` WEAK, all at one
// address), and an exec that claims only the spelling its descriptor
// happened to name splits that object in two — `__libc_start_main`
// writes envp into the exec's copy while libc's own `environ` slot is
// never written, so any OTHER image reading `environ` sees an EMPTY
// object with no diagnostic anywhere. That violates C23 6.2.2's
// guarantee that an identifier with external linkage denotes the SAME
// object throughout the program.
// The member is therefore DELETED, not left inert: an enum value no
// shipped format declares is a loaded gun — a future format file could
// declare it and silently reintroduce the whole class. Every ELF
// format now declares `got-indirect`, which binds the ADDRESS of the
// library's ONE object and so cannot split it. Reintroducing the
// mechanism would need a way to prove the alias set complete AT LINK
// TIME WITHOUT reading the target's libc (which would break "build ANY
// target inside ANY host"), and nobody has one.
//
// A genuinely new mechanism would be a NEW member; the linker gate +
// walkers dispatch on the declared member, never on a format-name
// branch — and never (since the deletion above) on the image flavour
// either. Consumed by the linker's pre-walker data-import gate
// (linker.cpp) + the per-format walker that implements the declared
// mechanism (elf.cpp `.got`/GLOB_DAT, macho.cpp __got, pe.cpp IAT).
enum class DataImportBinding : std::uint8_t {
    GotIndirect = 1,  // loader-bound pointer slot holding the address
};

inline constexpr EnumNameTable<DataImportBinding, 1> kDataImportBindingTable{{{
    { DataImportBinding::GotIndirect, "got-indirect" },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kDataImportBindingTable);

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
// DSS-linked EXEC formats never declare it (their data imports take the
// `dataImportBinding: "got-indirect"` slot path and their functions a
// direct address); the PIE/dyn formats use the c117 DSS-local-slot
// path. Only
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

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kExternAddrBindingTable);

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

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kTlsAccessModelTable);

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

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kLibrarySynthVehicleTable);

[[nodiscard]] constexpr std::string_view
librarySynthVehicleName(LibrarySynthVehicle v) noexcept {
    return kLibrarySynthVehicleTable.name(v);
}
[[nodiscard]] constexpr std::optional<LibrarySynthVehicle>
librarySynthVehicleFromName(std::string_view s) noexcept {
    return kLibrarySynthVehicleTable.fromName(s);
}

// ── UCRT-P4 (D-FFI-PE-CRT-UCRT-MIGRATION): the per-format RUNTIME
//    LIBRARY ROLE table ─────────────────────────────────────────────
//
// WHICH IMAGE plays WHICH RUNTIME ROLE for this object format. A format file
// declares ONE `runtimeLibraries` block mapping each role it uses to an image
// identity; every spine block then NAMES A ROLE instead of carrying a path.
//
// ★★ WHY A TABLE AND NOT ONE `crt` STRING. A single per-format CRT string
// would model an ACCIDENT of one DLL configuration, and it is already wrong in
// two configurations this repo ships:
//   * MEASURED (`nm -D`, 2026-08-10): the ELF unwinder personality lives in
//     `libgcc_s.so.1`; `libc.so.6` exports NO `_Unwind_*` symbol at all. (Do
//     not be fooled by `personality@@GLIBC_2.2.5` in libc — that is the
//     `personality(2)` SYSCALL wrapper, unrelated to unwinding.) On ELF, one
//     image demonstrably cannot serve both `cLibrary` and `unwindPersonality`.
//   * Microsoft names the C library and the VC runtime SEPARATELY in BOTH
//     configurations (`/MD` ⇒ `ucrt.lib` + `vcruntime.lib`; `/MT` ⇒
//     `libucrt.lib` + `libvcruntime.lib`), and DSS already ships static linking
//     for all formats (#47), so the split configuration is live in-tree.
//   * ucrtbase ALSO exporting `__C_specific_handler` (MEASURED ord 30; msvcrt
//     106, ntdll 2332, vcruntime140 8) is a CONVENIENCE of the pe DLL config,
//     not Microsoft's model.
// ★ The pe table pointing `cLibrary` and `unwindPersonality` at the SAME image
// is therefore FINE: the table PERMITS sameness without ASSERTING it. That is
// the whole difference from a single string, and it is what stops the value
// from having to be re-cut every time a new configuration arrives
// (`D-TEST-PE64-CONFOUND-PIN-WEAKENED-BY-ITS-OWN-SUBJECT`).
//
// ROLE IS A CLOSED ENUM, deliberately: every per-format BEHAVIORAL rule in this
// schema family already is one, and free-form strings stay reserved for names
// and paths. A typo'd role must fail loud at LOAD, never resolve to "some
// image".
//
// ★ C23 POSITION, stated so the scope is honest: SEH (`__try`/`__except`) is
// NOT C23 — it is a Microsoft extension. That is PRECISELY why its personality
// must be a per-format CONFIG DECLARATION and never a literal in shared
// substrate: a non-standard, platform-specific mechanism is the last thing that
// should be spelled inside `src/mir`. (`setjmp`/`longjmp` ARE C23 7.13, but they
// ride `setjmp.json` — a DESCRIPTOR with its own per-format library map — so
// they do not gate this table.)
enum class RuntimeLibraryRole : std::uint8_t {
    None              = 0,  // default-constructed sentinel; loader rejects "none"
    // The C library the program is bound to: the image that owns `exit`, the
    // stdio family, and the CRT program-entry argument machinery.
    CLibrary          = 1,
    // The image that owns the UNWINDER PERSONALITY routine an exception /
    // structured-exception region names in its unwind info (pe
    // `__C_specific_handler`; the ELF `_Unwind_*` family in `libgcc_s.so.1`
    // when ELF unwind lands; gcc/mingw-w64 on Windows uses libgcc rather than
    // vcruntime, and a role table accommodates that future target without a
    // schema change).
    UnwindPersonality = 2,
    // The OS PRIMITIVE image a compiler-synthesized shim emits over
    // (`kernel32.dll` on Windows: CRITICAL_SECTION / CONDITION_VARIABLE /
    // Fls*). Distinct from `cLibrary` on pe; on Mach-O the same image serves
    // both, which the table expresses by pointing both roles at it.
    SystemPrimitives  = 3,
};

inline constexpr EnumNameTable<RuntimeLibraryRole, 4> kRuntimeLibraryRoleTable{{{
    { RuntimeLibraryRole::None,              "none"              },
    { RuntimeLibraryRole::CLibrary,          "cLibrary"          },
    { RuntimeLibraryRole::UnwindPersonality, "unwindPersonality" },
    { RuntimeLibraryRole::SystemPrimitives,  "systemPrimitives"  },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kRuntimeLibraryRoleTable);

[[nodiscard]] constexpr std::string_view
runtimeLibraryRoleName(RuntimeLibraryRole r) noexcept {
    return kRuntimeLibraryRoleTable.name(r);
}
[[nodiscard]] constexpr std::optional<RuntimeLibraryRole>
runtimeLibraryRoleFromName(std::string_view s) noexcept {
    return kRuntimeLibraryRoleTable.fromName(s);
}

// ── THE SELECTABLE SPELLINGS — the table MINUS the `none` sentinel ────────
//
// D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET: the roles a
// `.format.json` may actually write. Every `runtimeLibraries[].role` arm
// resolves the spelling and then rejects `RuntimeLibraryRole::None` explicitly,
// so the accepted set is the table minus that one row.
//
// ★ BESIDE THE ENUM, NOT IN THE LOADER. The projection is a property of the
// vocabulary; a copy inside whichever TU happens to render it makes the COUNT a
// per-reader decision, which is how one of them ended up spelled so it could
// never fail. `object_format_schema_json.cpp` and the projection pin in
// `tests/link/` both consume this now.
[[nodiscard]] constexpr bool
isSelectableRuntimeLibraryRole(RuntimeLibraryRole r) noexcept {
    return r != RuntimeLibraryRole::None;
}

// ⚠ Literal `M` plus the sentinel-count assert, both halves required — see the
// nine-arm measurement written out at `kSelectableExitMechanismNames` in
// `core/types/target_schema.hpp`
// (D-CORE-NAMESWHERE-COUNT-DERIVED-FROM-THE-TABLE-IS-A-TAUTOLOGY).
inline constexpr auto kSelectableRuntimeLibraryRoleNames =
    namesWhere<3>(kRuntimeLibraryRoleTable, isSelectableRuntimeLibraryRole);
static_assert(kRuntimeLibraryRoleTable.rows.size()
                  == kSelectableRuntimeLibraryRoleNames.size() + 1,
              "kRuntimeLibraryRoleTable must have exactly ONE unselectable row "
              "(the 'none' sentinel) — a second one leaves `namesWhere`'s "
              "literal count matching while the role refusals silently stop "
              "naming the set the loader accepts");

// One `runtimeLibraries` row: a role and the image identity that plays it.
struct DSS_EXPORT RuntimeLibraryBinding {
    RuntimeLibraryRole role = RuntimeLibraryRole::None;
    std::string        image;   // "ucrtbase.dll" / "libgcc_s.so.1" / a dylib path
};

// The format's whole role table. A VECTOR of rows rather than a fixed struct of
// three optional strings, so adding a fourth role is an enum slot + a JSON row
// and touches no reader. Roles are unique cross-row (loader-enforced).
struct DSS_EXPORT RuntimeLibraryTable {
    std::vector<RuntimeLibraryBinding> bindings;

    // The image playing `role`, or nullopt if this format declares no such row.
    // ★ NULLOPT IS THE FAIL-LOUD SIGNAL, never a fallback: the loader refuses a
    // format whose spine block names a role this table does not declare, so no
    // consumer ever has to invent an image.
    [[nodiscard]] std::optional<std::string_view>
    imageForRole(RuntimeLibraryRole role) const noexcept {
        for (auto const& b : bindings) {
            if (b.role == role) return std::string_view{b.image};
        }
        return std::nullopt;
    }
    [[nodiscard]] bool empty() const noexcept { return bindings.empty(); }
};

// ── UCRT-P4: the unwinder-personality declaration ──────────────────
//
// The routine an emitted unwind record names as its handler, and the image it
// is imported from. Populated from the format JSON's `sehPersonality` block,
// whose `role` is resolved against `runtimeLibraries` AT LOAD — so `libraryPath`
// here is a DERIVED copy of a fact the role table owns, exactly as
// `sectionKindIndex` is a derived copy of `sections`.
//
// ★★ WHAT THIS REPLACES, AND WHY IT IS NOT A REFACTOR: `src/mir/merge/
// synth_seh_funclets.cpp` hardcoded BOTH `pers.mangledName =
// "__C_specific_handler"` and `pers.libraryPath = "msvcrt.dll"` — two platform
// literals in shared MIR substrate, on a pass that runs for EVERY format. A
// format that declares no personality now fails LOUD when a SEH region
// resolves, instead of silently emitting an msvcrt import into an ELF image.
struct DSS_EXPORT SehPersonality {
    RuntimeLibraryRole role = RuntimeLibraryRole::None;  // declared in JSON
    std::string libraryPath;   // DERIVED: resolved from `role` at load
    std::string mangledName;   // "__C_specific_handler"
};

// The format's shipped-library synthesis block (`"librarySynthesis"` in
// `.format.json`). `vehicle` selects the primitive family the synth pass
// emits over; `libraryPath` is the native library its on-demand helper
// imports name (the value that dissolves the pass's former hardcoded
// "kernel32.dll"). Present on pe/macho formats; absent on elf (direct
// FFI) and wasm/spirv (no native shim).
struct DSS_EXPORT LibrarySynthesis {
    LibrarySynthVehicle vehicle = LibrarySynthVehicle::Win32;
    // UCRT-P4: the ROLE this block names in the format's `runtimeLibraries`
    // table, and the image the loader RESOLVED it to. The JSON declares only the
    // role; `libraryPath` is the derived copy, kept so `synthesizeThreadsShim`
    // reads exactly what it read before. `validate()` re-checks the pair against
    // the table, so a hand-built schema cannot set one without the other.
    RuntimeLibraryRole  role = RuntimeLibraryRole::None;
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
// names a structure of ONE image format, so the loader cross-checks it against
// the backend that IMPLEMENTS the vehicle: declaring `pe-optional-header` on an
// ELF schema is dead config whose request the ELF walker would silently drop.
//
// ★ THE SENTENCE THAT USED TO SIT HERE — *"the loader cross-checks it against
// `format.kind` (a config-coherence rule, not an engine branch)"* — WAS STRUCK
// IN TF-C125, together with its twin in `object_format_schema_json.cpp` which
// defended the same `kVehicleKinds` table as *"a TABLE … not an if-chain"*.
// Both were wrong in the same way: a table keyed on format identity carries the
// identity dependency it claims to avoid, because every consumer must know an
// identity to index it. The operator's ruling ("we must never have identity
// branches") admits no loader exception, and the precedent is `kCManglingRules`
// — DELETED in TF-C122, not relocated. The cross-check now asks
// `ObjectFormatBackend::stackReserveVehicles()` which backend implements the
// vehicle. Recorded rather than silently deleted, because the two comments are
// exactly what would re-authorize the pattern for the next reader.
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

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kStackReserveVehicleTable);

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

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kStackReserveUnsupportedReasonTable);

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

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kStackReserveUnsupportedRemedyTable);

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

// ── Weak-DEFINITION dialect (D-CONFIG-WEAK-DEFINITION-DIALECT-NOT-DECLARED) ─
//
// HOW this object format SPELLS a weak definition — a symbol several
// translation units may define, of which the linker keeps one. A property of
// the OBJECT FORMAT's symbol/section model, exactly like `ExternCallDispatch` /
// `DataImportBinding` above, and keyed by the format for the same reason: the
// SAME CPU target spells it one way under `pe64-x86_64-windows` and another
// under `elf64-x86_64-linux`, so the rule cannot live on the target schema.
//
// ★★★ THIS IS A DIALECT ROW, NOT A CAPABILITY FLAG, AND THE DISTINCTION IS THE
// WHOLE POINT (operator ruling 2026-08-20, which authorized the COFF weak
// machinery: config gets ONE row — which weak-DEFINITION dialect the writer
// emits — and explicitly NOT a `canExpressWeakAlias` capability flag). A
// DIALECT row says *how* a format spells something it CAN express. A CAPABILITY
// flag asserts it CANNOT. [[D-LK-PE-ALTERNATENAME-DECLARE-AND-REFUSE]] is the
// record of why the second shape is dangerous: **an implementation gap recorded
// as a format incapability is a false fact in the place most likely to be
// trusted later, and it does not reverse cleanly.** That row was closed the day
// its "incapability" turned out to be an unbuilt writer.
//
// ★★ ONE PROPERTY PER FIELD — carried across from that same row, because it is
// the half that outlived the decision. The SEMANTIC fact ("this format spells a
// weak definition as COMDAT select-any") and any CAPABILITY fact ("DSS emits /
// does not emit this record") are TWO statements needing TWO fields. Merged,
// the declaration asserts something the writer then refuses, and the next
// reader loses an hour deciding which one lied. `dialect` therefore states ONLY
// the spelling. No capability field exists here, and none may be added to this
// block: a capability is a separate declaration with a separate name.
//
// ★ AND THE KEY IS ONE THE WRITER CONSULTS. A key the writer does not READ is
// worse than no key — it drifts silently and reads as authoritative — so the
// declaration is asked for at the point a weak definition is actually encoded,
// and a dialect the walker has no encoder for is REFUSED
// (`K_FormatLacksWeakDefinitionDialect`), never silently re-spelled.
//
//   * `comdat` (PE/COFF): the duplicate-resolution policy lives on the
//     SECTION. Each weak body gets a section of its own flagged
//     IMAGE_SCN_LNK_COMDAT whose Selection byte is IMAGE_COMDAT_SELECT_ANY
//     ("any section that defines the same COMDAT symbol can be linked; the
//     rest are removed"). ✔The published PE/COFF format, "COMDAT Sections
//     (Object Only)". NOT `IMAGE_SYM_CLASS_WEAK_EXTERNAL`, which is a weak
//     REFERENCE with a fallback alias and serves the separate ALIAS path —
//     see `pe.cpp`'s weak-definition block for the measured reference
//     encodings that fix this choice.
//   * `symbol-binding` (ELF): the weakness lives in the symbol's BINDING
//     field — `STB_WEAK` in the high nibble of `st_info`. No section flag is
//     involved (`elf.cpp`, `stbForBinding`).
//   * `symbol-flag` (Mach-O): the weakness lives in a per-symbol FLAG bit
//     ALONGSIDE an ordinary binding — `N_WEAK_DEF` (0x0080) in the nlist's
//     `n_desc`, on a symbol that is otherwise `N_SECT|N_EXT` (`macho.cpp`).
//
// ★ WHY ALL THREE SHIP AS VOCABULARY. Each names an encoder that EXISTS in
// this tree today, so none is a verb shipped ahead of its walker arm (the
// `StackReserveVehicle` discipline).
//
// ★★ ALL THREE ARE NOW CONSULTED BY THE WALKER THAT WRITES THEM
// ([[D-LK-WEAK-DEFINITION-DIALECT-UNCONSULTED-BY-ELF-AND-MACHO-WRITERS]],
// cycle P28). The shared gate is `link/format/weak_definition_gate.hpp`, called
// once by `pe::encode`'s Obj arm, once by `elf::encode`, and once by
// `macho::encode`'s MH_OBJECT arm. A format document gains the key IN THE SAME
// CHANGE as its writer's consultation, never before it — which is why the
// shipped corpus declares it on 16 of 24 documents and not on 24: the four
// Mach-O IMAGE documents, the two pe IMAGE documents, wasm and spirv encode no
// weak definition in any spelling, so a declaration there would be a key nobody
// reads, drifting silently while reading as authoritative.
//
// ★ AND THE SET IS CHECKED FROM BOTH ENDS. `ObjectFormatBackend::
// weakDefinitionDialects()` reports which dialects a WALKER spells, so the
// loader refuses a document declaring a dialect its own backend does not write
// — a mis-declaration fails at LOAD rather than at the first weak definition.
enum class WeakDefinitionDialect : std::uint8_t {
    // Zero is the INVALID sentinel and has NO JSON spelling — deliberately
    // absent from the table below, so `weakDefinitionDialectFromName` cannot
    // resolve it from ANY string (including `""`, which is what a
    // `"dialect": ""` typo produces) and `nameOrEmpty` renders it EMPTY. A
    // hand-built `WeakDefinition{}` is rejected by
    // `ObjectFormatData::validate()` through the `…Name(x).empty()` idiom.
    Unspecified   = 0,
    Comdat        = 1,  // COFF: IMAGE_SCN_LNK_COMDAT + IMAGE_COMDAT_SELECT_ANY
    SymbolBinding = 2,  // ELF: STB_WEAK in st_info
    SymbolFlag    = 3,  // Mach-O: N_WEAK_DEF (0x0080) in n_desc
};

inline constexpr EnumNameTable<WeakDefinitionDialect, 3>
kWeakDefinitionDialectTable{{{
    { WeakDefinitionDialect::Comdat,        "comdat"         },
    { WeakDefinitionDialect::SymbolBinding, "symbol-binding" },
    { WeakDefinitionDialect::SymbolFlag,    "symbol-flag"    },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kWeakDefinitionDialectTable);

[[nodiscard]] constexpr std::string_view
weakDefinitionDialectName(WeakDefinitionDialect d) noexcept {
    // The `-Werror=switch` backstop; it owns no spelling. A new dialect with
    // no `case` fails the BUILD rather than becoming a value with no name.
    // (The `cSymbolDecorationSchemeName` shape — read its docblock for why the
    // sentinel is left OUT of the table and asked for through `nameOrEmpty`.)
    switch (d) {
        case WeakDefinitionDialect::Unspecified:
        case WeakDefinitionDialect::Comdat:
        case WeakDefinitionDialect::SymbolBinding:
        case WeakDefinitionDialect::SymbolFlag:
            break;
    }
    return kWeakDefinitionDialectTable.nameOrEmpty(d);   // NOT .name()
}
[[nodiscard]] constexpr std::optional<WeakDefinitionDialect>
weakDefinitionDialectFromName(std::string_view s) noexcept {
    return kWeakDefinitionDialectTable.fromName(s);
}

// The format's weak-definition block (`"weakDefinition"` in `.format.json`).
// A BLOCK carrying one `dialect` verb rather than a bare scalar — the
// `CSymbolDecoration` precedent, for its stated reason: a future dialect that
// needs per-arm PARAMETERS gains a sibling key INSIDE this block instead of a
// second root key only some formats may legally declare.
//
// Held in an `std::optional` on the schema, so PRESENCE is the declaration.
// Absence is NOT an assertion that the format cannot express a weak definition
// — that would be the capability flag this row exists to refuse. It is the
// absence of an answer, and the writer says exactly that when it needs one.
struct DSS_EXPORT WeakDefinition {
    WeakDefinitionDialect dialect = WeakDefinitionDialect::Unspecified;
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
