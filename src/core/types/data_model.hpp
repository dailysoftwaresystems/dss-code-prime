#pragma once

#include "core/export.hpp"

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
// empty view for it (EnumNameTable miss).

namespace dss {

enum class DataModel : std::uint8_t {
    Lp64  = 1,  // int 32 / long 64 / ptr 64 — linux + darwin 64-bit
    Llp64 = 2,  // int 32 / long 32 / ptr 64 — windows 64-bit
    Ilp32 = 3,  // int 32 / long 32 / ptr 32 — declared-only (wasm32 / spirv)
};

// Hand-rolled name pair instead of the house `EnumNameTable<E,N>`: that
// template lives in `target_schema.hpp`, which this header CANNOT include
// — `semantic_config.hpp` (which carries DataModel-keyed maps) is pulled
// EARLY by `grammar_schema.hpp`, before `LoadResult`/`ConfigDiagnostic`
// are defined, and `target_schema.hpp` consumes those — the same cycle
// the `symbol_attrs.hpp` forward-declaration note in semantic_config.hpp
// documents. Three values; the switch stays trivially in sync with the
// enum (a new member without a name arm fails the -Wswitch build).
[[nodiscard]] constexpr std::string_view
dataModelName(DataModel m) noexcept {
    switch (m) {
        case DataModel::Lp64:  return "LP64";
        case DataModel::Llp64: return "LLP64";
        case DataModel::Ilp32: return "ILP32";
    }
    return {};
}
[[nodiscard]] constexpr std::optional<DataModel>
dataModelFromName(std::string_view s) noexcept {
    if (s == "LP64")  return DataModel::Lp64;
    if (s == "LLP64") return DataModel::Llp64;
    if (s == "ILP32") return DataModel::Ilp32;
    return std::nullopt;
}

} // namespace dss
