#pragma once

// EVERY SYMBOL ID A MIR MODULE NAMES — THE ONE STATEMENT OF IT
// (D-MIR-SYNTHESIZED-SYMBOL-MINTED-INSIDE-THE-NAME-TABLE).
//
// A module names a symbol id in eight places and no more: the id a FUNCTION is given,
// the id a GLOBAL is given, a symbol address nested anywhere in a global's
// INITIALIZER, a `GlobalAddr` instruction's payload, the symbol a
// `BlockAddressExport` publishes its block under, a symbol address nested
// anywhere in a `Const` instruction's literal, and an ALIAS's two ids — the name it
// defines and the definition it names (P69, lane `cs`). Three readers must agree on that
// list, and each used to carry its own:
//   * the `.dssir` WRITER, whose `symbols` preamble declares the ids the text names
//     (it left `BlockAddressExport` out, so that id travelled as a raw payload the
//     preamble never declared);
//   * the `.dssir` READER, which states the module's id space from what the text
//     declares and defines — so an id the text NAMES and neither declares nor
//     defines lies past the end the reader states, and the next mint hands it out
//     again (READ, P69: `globaladdr (%5)` in a text that defines `%1` alone read
//     back stated at 2; the `symaddr` arm refused the same thing by name, the
//     `globaladdr` arm did not);
//   * the VERIFIER, which holds every module to "an id it names is below the end
//     it states" and to "an id is defined once".
// They read `forEachSymbolIdNamed` now. A place a later change adds is added HERE,
// and all three see it: an opcode a later change adds stops the build until the
// opcode `switch` below lists it (it has no `default`, and the build refuses an
// unlisted enumerator: `-Werror=switch` / `/we4062`), and a literal arm a later
// change adds stops the build until it says whether it names a symbol (the
// `static_assert` below).
//
// FOUR of the places DEFINE the id (`mirSymbolMentionDefines`): a function's, a
// global's, an alias's name, and a `blockaddress_export`'s — the object defines
// that symbol, as a local at its block. One id, one definition: an id two of them
// give is refused by name (`symbolIdsDefinedTwice`).
//
// The ORDER is the writer's and is observable: the preamble lists ids as they are
// first met — the module's functions, then each global followed by its initializer's
// symbols, then the instructions in id order, then each alias's name and target.

#include "core/types/strong_ids.hpp"
#include "mir/mir.hpp"
#include "mir/mir_literal_pool.hpp"
#include "mir/mir_opcode.hpp"

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
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
    BlockAddressExport,  // the symbol a `blockaddress_export` publishes its block under (it DEFINES it)
    ConstLiteral,        // a symbol address nested in a `const` instruction's literal
    Alias,               // an alias of the module is given the id (it DEFINES it)
    AliasTarget,         // the definition an alias is one more name on
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
        case MirSymbolMention::Alias:              return "alias";
        case MirSymbolMention::AliasTarget:        return "alias";
    }
    return "symbol";
}

// Does the mention DEFINE the id — give it to a function, a global or an alias of the
// module, or publish a block under it? A `blockaddress_export` is a definition: the
// object writes its symbol as a local defined at the block (mir_merge.cpp, the export's
// remap), so a text naming it there introduces it as a global's definition does, and
// it is one of the definitions an id may have only once. (P69, lane `cs`, fold f26b:
// it was left out, and a text holding `global %9` and a `blockaddress_export` under %9
// read OK in reader and verifier.)
[[nodiscard]] constexpr bool mirSymbolMentionDefines(MirSymbolMention m) noexcept {
    return m == MirSymbolMention::Function || m == MirSymbolMention::Global
           || m == MirSymbolMention::Alias || m == MirSymbolMention::BlockAddressExport;
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
//
// A literal index PAST THE POOL names nothing here either, and is read as no
// literal: the verifier walks this list over every module it is handed, a module
// made by hand among them, and a refusal that aborts is no refusal. No builder
// finishes such a module (`addConst` makes a const's literal, `finish` refuses a
// global's), and the verifier reports a const's as a finding of its own (`const
// payload … out of literal-pool range`). ✔MEASURED, P69 fold f26b (windows-x86_64-
// debug): once the one-definition rule walked every module, the hand-made module
// of `MirVerifier.TerminatorWithTooManySuccessorsIsReported` — a `const` with no
// literal — killed the verifier's binary in the pool's own bound check.
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
        if (std::uint32_t const litIdx = mir.globalInitLiteralIndex(g);
            litIdx != UINT32_MAX && litIdx < mir.literalPool().size()) {
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
                if (std::uint32_t const litIdx = mir.constLiteralIndex(id);
                    litIdx < mir.literalPool().size()) {
                    detail::forEachSymbolIdALiteralNames(
                        mir.literalValue(litIdx),
                        [&](SymbolId s) { named(s, MirSymbolMention::ConstLiteral, id); });
                }
                break;
            // EVERY OTHER OPCODE NAMES NO SYMBOL ID, each listed by name. The switch has
            // no `default`, so an opcode a later change adds stops the build here until
            // it is listed — above when it names a symbol (and the writer, the reader and
            // the verifier all see it), below when it names none. (P69, lane `cs`, fold
            // f26b: a `default: break;` made a new symbol-carrying opcode invisible to all
            // three at once.)
            case MirOpcode::Invalid: case MirOpcode::Arg: case MirOpcode::BlockAddress: case MirOpcode::Add:
            case MirOpcode::Sub: case MirOpcode::Mul: case MirOpcode::SDiv: case MirOpcode::UDiv:
            case MirOpcode::SMod: case MirOpcode::UMod: case MirOpcode::Neg: case MirOpcode::UMulH:
            case MirOpcode::AtomicCas: case MirOpcode::AtomicLoad: case MirOpcode::AtomicStore:
            case MirOpcode::CompilerBarrier: case MirOpcode::AtomicFence: case MirOpcode::FAdd:
            case MirOpcode::FSub: case MirOpcode::FMul: case MirOpcode::FDiv: case MirOpcode::FNeg:
            case MirOpcode::And: case MirOpcode::Or: case MirOpcode::Xor: case MirOpcode::Shl:
            case MirOpcode::LShr: case MirOpcode::AShr: case MirOpcode::Not: case MirOpcode::Popcount:
            case MirOpcode::Clz: case MirOpcode::Ctz: case MirOpcode::Bswap: case MirOpcode::ICmpEq:
            case MirOpcode::ICmpNe: case MirOpcode::ICmpSlt: case MirOpcode::ICmpSle:
            case MirOpcode::ICmpSgt: case MirOpcode::ICmpSge: case MirOpcode::ICmpUlt:
            case MirOpcode::ICmpUle: case MirOpcode::ICmpUgt: case MirOpcode::ICmpUge:
            case MirOpcode::FCmpOeq: case MirOpcode::FCmpOne: case MirOpcode::FCmpOlt:
            case MirOpcode::FCmpOle: case MirOpcode::FCmpOgt: case MirOpcode::FCmpOge:
            case MirOpcode::FCmpUeq: case MirOpcode::FCmpUne: case MirOpcode::FCmpUlt:
            case MirOpcode::FCmpUle: case MirOpcode::FCmpUgt: case MirOpcode::FCmpUge:
            case MirOpcode::Alloca: case MirOpcode::Load: case MirOpcode::Store: case MirOpcode::Gep:
            case MirOpcode::ExtractValue: case MirOpcode::InsertValue: case MirOpcode::Trunc:
            case MirOpcode::SExt: case MirOpcode::ZExt: case MirOpcode::FPTrunc: case MirOpcode::FPExt:
            case MirOpcode::Bitcast: case MirOpcode::IntToPtr: case MirOpcode::PtrToInt:
            case MirOpcode::FPToSI: case MirOpcode::FPToUI: case MirOpcode::SIToFP: case MirOpcode::UIToFP:
            case MirOpcode::Call: case MirOpcode::IntrinsicCall: case MirOpcode::ReturnPiece:
            case MirOpcode::InlineAsm: case MirOpcode::InlineAsmGoto: case MirOpcode::ReadIndirectResult:
            case MirOpcode::VaRegSaveAreaAddr: case MirOpcode::VaOverflowArgAreaAddr:
            case MirOpcode::VaHomeArgAreaAddr: case MirOpcode::ByValueStackArg:
            case MirOpcode::RecvByValueStackParam: case MirOpcode::Phi: case MirOpcode::Br:
            case MirOpcode::CondBr: case MirOpcode::Switch: case MirOpcode::Return:
            case MirOpcode::Unreachable: case MirOpcode::IndirectBr: case MirOpcode::SehTryBegin:
            case MirOpcode::SehFilterReturn: case MirOpcode::SehTryEnd: case MirOpcode::SehExceptionCode:
            case MirOpcode::SehExceptionInfo: case MirOpcode::RecoverParentFrameSlot:
            case MirOpcode::StackSave: case MirOpcode::StackRestore: case MirOpcode::VAdd:
            case MirOpcode::VSub: case MirOpcode::VMul: case MirOpcode::VShuffle: case MirOpcode::VExtract:
            case MirOpcode::VInsert: case MirOpcode::Count_:
                break;
        }
    }
    for (MirAlias const& a : mir.aliases()) {
        named(a.alias, MirSymbolMention::Alias, MirInstId{});
        named(a.target, MirSymbolMention::AliasTarget, MirInstId{});
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

// One id two places of a module DEFINE.
struct MirSymbolDefinedTwice {
    SymbolId         symbol{};
    MirSymbolMention first  = MirSymbolMention::Function;   // the definition met first, in the writer's order
    MirSymbolMention second = MirSymbolMention::Function;   // the one that repeats it
    MirInstId        inst{};   // the second's instruction (a `blockaddress_export`'s), invalid for the rest
};

// ONE ID, ONE DEFINITION — the rule the verifier holds every module to, and so every
// module the `.dssir` reader hands out (it verifies what it reads, in the same words):
// every id two DEFINING mentions give — a function's and a global's, an alias's name
// beside either or beside another alias, a `blockaddress_export`'s beside any of them
// or beside another export — each id once, at its second definition in the writer's
// order. The object would define one symbol twice.
[[nodiscard]] inline std::vector<MirSymbolDefinedTwice> symbolIdsDefinedTwice(Mir const& mir) {
    std::unordered_map<std::uint32_t, MirSymbolMention> firstDefinition;
    std::unordered_set<std::uint32_t>                   reported;
    std::vector<MirSymbolDefinedTwice>                  out;
    forEachSymbolIdNamed(mir, [&](SymbolId s, MirSymbolMention m, MirInstId at) {
        if (!mirSymbolMentionDefines(m)) return;
        auto const [first, fresh] = firstDefinition.emplace(s.v, m);
        if (fresh || !reported.insert(s.v).second) return;
        out.push_back(MirSymbolDefinedTwice{s, first->second, m, at});
    });
    return out;
}

// The sentence both the verifier and a reader's caller read: the id and the two
// definitions, as the text spells them.
[[nodiscard]] inline std::string symbolDefinedTwiceSentence(MirSymbolDefinedTwice const& twice) {
    return std::format("symbol %{} is defined twice in this module, as its {} and as its {} — one id "
                       "names one definition",
                       twice.symbol.v, mirSymbolMentionSpelling(twice.first),
                       mirSymbolMentionSpelling(twice.second));
}

}  // namespace dss
