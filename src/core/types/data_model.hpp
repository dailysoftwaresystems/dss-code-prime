#pragma once

#include "core/export.hpp"
#include "core/types/enum_name_table.hpp"   // EnumNameTable<E,N> (leaf header — no target_schema cycle)

#include <cstdint>
#include <optional>
#include <string_view>

// ── Data model (FC3 c1, plan 23): the C-family primitive-width contract ──
//
// A DATA MODEL names the (int, long, pointer) width triple an OPERATING
// SYSTEM's ABI fixes for C-family languages. It is a property of the
// OBJECT FORMAT schema (the format is the per-OS artifact: pe64-*-windows
// vs elf64-*-linux), NOT the CPU target — the SAME x86_64 target serves
// Windows (LLP64, `long` = 32-bit) and Linux (LP64, `long` = 64-bit).
//
// Declared as a REQUIRED field on every `.format.json` (loader fails loud
// on a missing or unknown value — a silent default would bake wrong
// widths into every `long`). Consumed by the SEMANTIC tier: per-language
// type tables (`builtinTypes` / `typeSpecifiers` `coreByDataModel`
// overrides), the integer-literal ladder (`integerLiteralTyping`), the
// usual-arithmetic-conversions block, and the shipped-lib descriptor
// reader (`signatureByDataModel`) all resolve dataModel-dependent names
// through the active model. The engine never branches on the format
// NAME — only on this closed enum, which the format JSON declares.
//
// **Cross-tier vocabulary**: lives under `core/types/` (not `src/link/`)
// because the semantic analyzer + HIR lowering + the grammar-schema
// loader all speak it without pulling the 900-LOC link substrate header
// — the `object_format_kind.hpp` extraction precedent.
//
// Closed vocabulary (C-family width triples; values match the JSON
// spellings used by `coreByDataModel` / `signatureByDataModel` keys):
//   * LP64  — int 32, long 64, pointer 64. Linux / macOS / *BSD 64-bit.
//   * LLP64 — int 32, long 32 (long long 64), pointer 64. 64-bit Windows.
//   * ILP32 — int 32, long 32, pointer 32. 32-bit targets + wasm32.
//     DECLARED-ONLY this cycle: the wasm/spirv skeleton formats carry it,
//     but the semantic consumer fails loud when an ILP32 format is
//     actually selected (S_UnsupportedDataModel) — no untested width
//     path is silently exercised.
//
// **Sentinel discipline**: no `Unknown = 0` member is declared, but a
// default-constructed / zero `DataModel` is NOT a valid model — the
// format loader requires the field, `validate()` rejects a zero value
// from hand-built `ObjectFormatData`, and `dataModelName` returns the
// empty view for it — the zero value is deliberately NOT a row of
// `kDataModelTable`, and the name side asks `nameOrEmpty`, whose whole
// job is to render an unlisted value as the empty view rather than as
// row 0's spelling.

namespace dss {

enum class DataModel : std::uint8_t {
    Lp64  = 1,  // int 32 / long 64 / ptr 64 — linux + darwin 64-bit
    Llp64 = 2,  // int 32 / long 32 / ptr 64 — windows 64-bit
    Ilp32 = 3,  // int 32 / long 32 / ptr 32 — declared-only (wasm32 / spirv)
};

// ── THE ONE OWNER OF THE DATA-MODEL SPELLINGS ─────────────────────────────
//
// ⚠ THIS USED TO BE A SWITCH PLUS AN IF-CHAIN, and the comment that stood here
// justified it with an include cycle that NO LONGER EXISTS. `EnumNameTable<E,N>`
// was extracted out of `target_schema.hpp` into the dependency-free
// `core/types/enum_name_table.hpp` precisely so leaf enum headers could carry
// tables — that header's own comment names THIS FILE as one of the two
// motivating cases. The stated blocker had been stale for cycles, and the
// hand-rolled shape it protected left the spellings with TWO owners (the
// `switch` and the `if`-chain) plus a THIRD in every loader message that
// retyped them (D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET).
//
// ★ THE SENTINEL IS DELIBERATELY NOT A ROW. A zero / default-constructed
// `DataModel` is not a valid model, and it must have NO spelling: giving it one
// (even `""`) would let `dataModelFromName` resolve it. It is left out, and the
// name side asks `nameOrEmpty` rather than `name` so an unlisted value renders
// EMPTY — which is exactly what `ObjectFormatData::validate()` tests with
// `dataModelName(dataModel).empty()`. Using `name()` here would return `"LP64"`
// for a never-declared model and validate() would ACCEPT it.
inline constexpr EnumNameTable<DataModel, 3> kDataModelTable{{{
    { DataModel::Lp64,  "LP64"  },
    { DataModel::Llp64, "LLP64" },
    { DataModel::Ilp32, "ILP32" },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kDataModelTable);

[[nodiscard]] constexpr std::string_view
dataModelName(DataModel m) noexcept {
    // ★ THE `-Werror=switch` BACKSTOP, AND IT OWNS NO SPELLING. The table above
    // owns every name; this switch names only the ENUMERATORS, which the
    // compiler itself keeps exhaustive (project-wide `-Werror=switch` /
    // `/we4062`, CMakeLists.txt). Adding a data model without a table row
    // therefore still fails the BUILD — the one guarantee the hand-written name
    // switch gave that a bare table lookup would have quietly dropped — while
    // the spellings stay in exactly one place. The arms fall through to a
    // single `break` on purpose: there is nothing to return per-model.
    switch (m) {
        case DataModel::Lp64:
        case DataModel::Llp64:
        case DataModel::Ilp32:
            break;
    }
    return kDataModelTable.nameOrEmpty(m);   // NOT .name() — see the table's note
}
[[nodiscard]] constexpr std::optional<DataModel>
dataModelFromName(std::string_view s) noexcept {
    return kDataModelTable.fromName(s);
}

// ── Long-double format (FC17.9(e), D-CSUBSET-LONG-DOUBLE): the per-format
// `long double` representation axis ────────────────────────────────────────
//
// `long double` is the ONE C primitive whose FORMAT (not just width) is
// ABI-divergent per OS/format: 64-bit IEEE (MSVC pe64 + Apple arm64), x87
// 80-bit extended (SysV x86_64 + darwin-x86_64), IEEE binary128 (AAPCS64
// linux-arm64). Like `DataModel` it is a property of the OBJECT FORMAT (the
// same x86_64 target serves pe64's f64 AND elf64's x87-80), declared as an
// OPTIONAL `"longDoubleFormat"` field on `.format.json` (closed enum, loader
// fails loud on an unknown spelling; the bitFieldStrategy optional-field
// precedent). Consumed by the SEMANTIC tier: the per-language
// `coreByLongDoubleFormat` row overrides (typeSpecifiers + the float-literal
// ladder's load-resolved refs) resolve `long double` to its per-axis core
// (f64 → F64, x87-80 → F80, ieee128 → F128).
//
// **`None` is the UNDECLARED sentinel, never a silent fallback**: a row that
// carries a `coreByLongDoubleFormat` map is UNREALIZED under a None axis
// (wasm/spirv skeletons + direct-API callers) — resolving it emits
// S_LongDoubleFormatUndeclared rather than quietly binding the base core
// (the knob-that-lies / LLP64-`long` lesson). Non-long-double programs are
// untouched (the row stays dormant).
enum class LongDoubleFormat : std::uint8_t {
    None    = 0,  // format declares no axis — `long double` rows unrealized
    F64     = 1,  // 64-bit IEEE double (MSVC x64, Apple arm64) — ≡ `double`
    X87_80  = 2,  // x87 80-bit extended, 16/16 storage (SysV/darwin x86_64)
    Ieee128 = 3,  // IEEE binary128, 16/16 (AAPCS64 linux-arm64)
};

// The ONE owner of the long-double-axis spellings — the `kDataModelTable`
// shape, for the same reason (the include-cycle argument that kept both of
// these hand-rolled is stale; see that table's note).
//
// ★ `None` IS DELIBERATELY NOT A ROW, and here the omission is load-bearing in
// BOTH directions: omission is the only way to leave the axis undeclared, so a
// spellable "none" would let a typo'd config look deliberate — and a row
// carrying the EMPTY name would make `longDoubleFormatFromName("")` resolve to
// `None`, i.e. `"longDoubleFormat": ""` would read as a declaration. Left out
// of the table, `fromName` refuses every spelling of it and
// `nameOrEmpty(None)` renders empty, which is the pre-existing contract.
inline constexpr EnumNameTable<LongDoubleFormat, 3> kLongDoubleFormatTable{{{
    { LongDoubleFormat::F64,     "f64"     },
    { LongDoubleFormat::X87_80,  "x87-80"  },
    { LongDoubleFormat::Ieee128, "ieee128" },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kLongDoubleFormatTable);

[[nodiscard]] constexpr std::string_view
longDoubleFormatName(LongDoubleFormat f) noexcept {
    // The `-Werror=switch` backstop — see `dataModelName`. `None` is listed
    // here (the compiler requires it) and carries no arm of its own: it is the
    // value the table deliberately omits, and `nameOrEmpty` renders it empty.
    switch (f) {
        case LongDoubleFormat::None:
        case LongDoubleFormat::F64:
        case LongDoubleFormat::X87_80:
        case LongDoubleFormat::Ieee128:
            break;
    }
    return kLongDoubleFormatTable.nameOrEmpty(f);
}
[[nodiscard]] constexpr std::optional<LongDoubleFormat>
longDoubleFormatFromName(std::string_view s) noexcept {
    return kLongDoubleFormatTable.fromName(s);
}

} // namespace dss
