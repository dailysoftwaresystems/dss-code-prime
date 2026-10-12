#pragma once

// EVERY SYMBOL ID A MIR MODULE NAMES — THE ONE STATEMENT OF IT
// (D-MIR-SYNTHESIZED-SYMBOL-MINTED-INSIDE-THE-NAME-TABLE).
//
// A module names a symbol id in six places and no more: the id a FUNCTION is given,
// the id a GLOBAL is given, a symbol address nested anywhere in a global's
// INITIALIZER, a `GlobalAddr` instruction's payload, the symbol a
// `BlockAddressExport` publishes its block under, and a symbol address nested
// anywhere in a `Const` instruction's literal. Three readers must agree on that
// list, and each used to carry its own:
//   * the `.dssir` WRITER, whose `symbols` preamble declares the ids the text names
//     (it left `BlockAddressExport` out, so that id travelled as a raw payload the
//     preamble never declared);
//   * the `.dssir` READER, which states the module's id space from what the text
//     declares and defines — so an id the text NAMES and neither declares nor
//     defines lies past the end the reader states, and the next mint hands it out
//     again (✔MEASURED by reading, P69: `globaladdr (%5)` in a text that defines
//     `%1` alone read back stated at 2; the `symaddr` arm refused the same thing
//     by name, the `globaladdr` arm did not);
//   * the VERIFIER, which holds every module to "an id it names is below the end
//     it states".
// They read `forEachSymbolIdNamed` now. A seventh place a later change adds is
// added HERE, and all three see it; a literal arm a later change adds stops the
// build until it says whether it names a symbol (the `static_assert` below).
//
// The ORDER is the writer's and is observable: the preamble lists ids as they are
// first met — the module's functions, then each global followed by its initializer's
// symbols, then the instructions in id order.

#include "core/types/strong_ids.hpp"
#include "mir/mir.hpp"
#include "mir/mir_literal_pool.hpp"
#include "mir/mir_opcode.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <variant>
#include <vector>

namespace dss {

// WHERE a module names a symbol id.
enum class MirSymbolMention : std::uint8_t {
    Function,            // a function of the module is given the id (it DEFINES it)
    Global,              // a global of the module is given the id (it DEFINES it)
    GlobalInitializer,   // a symbol address nested in a global's initializer literal
    GlobalAddr,          // a `globaladdr` instruction's payload
    BlockAddressExport,  // the symbol a `blockaddress_export` publishes its block under
    ConstLiteral,        // a symbol address nested in a `const` instruction's literal
};

// The mention as the `.dssir` text spells the construct that makes it: what a refusal
// names. A symbol address is `symaddr` wherever its literal stands.
[[nodiscard]] constexpr std::string_view mirSymbolMentionSpelling(MirSymbolMention m) noexcept {
    switch (m) {
        case MirSymbolMention::Function:           return "function";
        case MirSymbolMention::Global:             return "global";
        case MirSymbolMention::GlobalInitializer:  return "symaddr";
        case MirSymbolMention::GlobalAddr:         return "globaladdr";
        case MirSymbolMention::BlockAddressExport: return "blockaddress_export";
        case MirSymbolMention::ConstLiteral:       return "symaddr";
    }
    return "symbol";
}

// Does the mention DEFINE the id (give it to a function or a global of the module)?
[[nodiscard]] constexpr bool mirSymbolMentionDefines(MirSymbolMention m) noexcept {
    return m == MirSymbolMention::Function || m == MirSymbolMention::Global;
}

namespace detail {

// Every symbol id nested anywhere in one literal, in field order, through the one
// literal walker (an explicit work stack: a deeply braced initializer costs heap,
// never host stack). ★ CLOSED OVER THE WHOLE VARIANT: of `MirLiteralValue`'s arms
// only `MirSymbolAddrValue` names a symbol, and `MirAggregateValue` names one
// exactly when a field does (the walker visits the fields).
template <typename Visit>
void forEachSymbolIdALiteralNames(MirLiteralValue const& root, Visit&& visit) {
    forEachLiteralNode(root, [&](MirLiteralValue const& node) {
        std::visit([&](auto const& arm) {
            using T = std::decay_t<decltype(arm)>;
            if constexpr (std::is_same_v<T, MirSymbolAddrValue>) {
                visit(SymbolId{arm.symbol});
            } else {
                static_assert(std::is_same_v<T, MirAggregateValue>
                                  || std::is_same_v<T, std::monostate> || std::is_same_v<T, bool>
                                  || std::is_same_v<T, std::int64_t>
                                  || std::is_same_v<T, std::uint64_t>
                                  || std::is_same_v<T, double> || std::is_same_v<T, std::string>
                                  || std::is_same_v<T, BitIntValue>
                                  || std::is_same_v<T, WideFloatValue>,
                              "a new MirLiteralValue arm must say whether it names a symbol: "
                              "visit it above, or add it to this list of the arms that name none");
            }
        }, node.value);
    });
}

}  // namespace detail

// Calls `visit(SymbolId, MirSymbolMention, MirInstId)` once for every place `mir`
// names a symbol id, in the writer's order (see the head of this file). The
// instruction is the invalid `MirInstId{}` for a definition and for an initializer.
// The invalid `SymbolId{}` is never visited: it names nothing, and each producer
// refuses it where it is written.
template <typename Visit>
void forEachSymbolIdNamed(Mir const& mir, Visit&& visit) {
    auto const named = [&](SymbolId s, MirSymbolMention m, MirInstId at) {
        if (s.valid()) visit(s, m, at);
    };
    for (std::uint32_t i = 0; i < mir.moduleFuncCount(); ++i) {
        named(mir.funcSymbol(mir.funcAt(i)), MirSymbolMention::Function, MirInstId{});
    }
    for (std::uint32_t i = 0; i < mir.moduleGlobalCount(); ++i) {
        MirGlobalId const g = mir.globalAt(i);
        named(mir.globalSymbol(g), MirSymbolMention::Global, MirInstId{});
        if (std::uint32_t const litIdx = mir.globalInitLiteralIndex(g); litIdx != UINT32_MAX) {
            detail::forEachSymbolIdALiteralNames(mir.literalValue(litIdx), [&](SymbolId s) {
                named(s, MirSymbolMention::GlobalInitializer, MirInstId{});
            });
        }
    }
    for (std::uint32_t i = 1; i < mir.instCount(); ++i) {
        MirInstId const id{i, mir.id().v};
        switch (mir.instOpcode(id)) {
            case MirOpcode::GlobalAddr:
                named(mir.globalAddrSymbol(id), MirSymbolMention::GlobalAddr, id);
                break;
            case MirOpcode::BlockAddressExport:
                named(mir.blockAddressExportSymbol(id), MirSymbolMention::BlockAddressExport, id);
                break;
            case MirOpcode::Const:
                detail::forEachSymbolIdALiteralNames(
                    mir.literalValue(mir.constLiteralIndex(id)),
                    [&](SymbolId s) { named(s, MirSymbolMention::ConstLiteral, id); });
                break;
            default:
                break;
        }
    }
}

// One place a module names a symbol id it should not.
struct MirSymbolMentionAt {
    SymbolId         symbol{};
    MirSymbolMention mention = MirSymbolMention::GlobalAddr;
    MirInstId        inst{};   // invalid for an initializer's
};

// THE READER'S RULE: every place `mir` names an id that the module does not define
// and `declared(id)` does not answer for — each id once, at its first mention. A text
// is the name table its module is made from, so such an id is one nothing introduced:
// it lies past the end the reader states, and a later mint would hand it out again.
template <typename Declared>
[[nodiscard]] std::vector<MirSymbolMentionAt>
symbolIdsNamedButNeverIntroduced(Mir const& mir, Declared&& declared) {
    std::unordered_set<std::uint32_t> defined;
    forEachSymbolIdNamed(mir, [&](SymbolId s, MirSymbolMention m, MirInstId) {
        if (mirSymbolMentionDefines(m)) defined.insert(s.v);
    });
    std::vector<MirSymbolMentionAt>   out;
    std::unordered_set<std::uint32_t> reported;
    forEachSymbolIdNamed(mir, [&](SymbolId s, MirSymbolMention m, MirInstId at) {
        if (mirSymbolMentionDefines(m) || defined.contains(s.v) || declared(s)) return;
        if (!reported.insert(s.v).second) return;
        out.push_back(MirSymbolMentionAt{s, m, at});
    });
    return out;
}

// THE VERIFIER'S RULE: every place `mir` names an id at or past the end it STATES —
// each id once. Empty for a module whose end is only counted: nobody said where its
// id space ends, so there is no statement to hold it to (and nothing can be minted
// for it — both leaves of the door refuse a counted end).
//
// The LAST id value has no "one past": an end SATURATES on it (`Mir::symbolIdEnd`), so
// a module that holds the last value states an end equal to it, and that id is inside.
[[nodiscard]] inline std::vector<MirSymbolMentionAt> symbolIdsNamedPastTheStatedEnd(Mir const& mir) {
    std::vector<MirSymbolMentionAt> out;
    if (!mir.symbolIdEndIsStated()) return out;
    constexpr std::uint32_t kLast = 0xFFFFFFFFu;
    std::uint32_t const end = mir.symbolIdEnd();
    std::unordered_set<std::uint32_t> reported;
    forEachSymbolIdNamed(mir, [&](SymbolId s, MirSymbolMention m, MirInstId at) {
        if (s.v < end || (s.v == kLast && end == kLast)) return;
        if (!reported.insert(s.v).second) return;
        out.push_back(MirSymbolMentionAt{s, m, at});
    });
    return out;
}

}  // namespace dss
