#include "mir/mir_text.hpp"

#include "core/types/config_key_vocabulary.hpp"  // renderAllowedList (the refusals project their table)
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/enum_name_table.hpp"  // EnumNameTable / allNames — ONE owner per text spelling set
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"   // symbolBindingName / symbolVisibilityName
#include "core/types/target_schema.hpp"  // callConvName / kCallConvTable
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_literal_pool.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_verifier.hpp"

#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace dss {

// TF-C78 (D-CSUBSET-NOINLINE-PER-FUNCTION-SINK): the ONE spelling of the
// `noinline` function attribute in the `.dssir` text format. The printer
// (`appendFuncAttrs`) and the parser sit in two SEPARATE anonymous namespaces
// several hundred lines apart, so a bare literal in each is two independently
// editable copies of one fact — and the error message that lists the accepted
// names is a third. Its neighbours `binding`/`visibility` never had this
// problem because they route through `symbolBindingName`/`symbolVisibilityName`
// name tables; `noInline` is a plain bool with no table to borrow, which is
// exactly how the literal crept in. Same drift this project closed for the
// object-format sentinel message with a shared constant.
// NOTE this is the TEXT FORMAT's own keyword, NOT a source-language attribute
// name: `.dssir` is a compiler-defined serialization format, so its vocabulary
// legitimately lives in code. The C-side spelling stays config-declared.
inline constexpr std::string_view kMirTextNoInlineAttr = "noinline";
// TF-C81 (D-CSUBSET-ALWAYSINLINE): the same one-spelling discipline for the
// cost-model-bypass flag. Deliberately NOT `always_inline` with an underscore —
// this is the `.dssir` text format's own keyword, matching LLVM's `alwaysinline`
// / this file's `noinline` style, not the C source attribute's spelling.
inline constexpr std::string_view kMirTextAlwaysInlineAttr = "alwaysinline";
// TF-C85: the same one-spelling discipline for the per-function optimizer
// opt-out. Again the `.dssir` text format's own keyword, matching this file's
// `noinline` / `alwaysinline` style, not the MSVC pragma's spelling.
inline constexpr std::string_view kMirTextNoOptimizeAttr = "nooptimize";
// TF-C92 (D-CSUBSET-NO-SANITIZE-THREAD): the same one-spelling discipline for the
// per-function thread-sanitizer exclusion. Again the `.dssir` text format's own
// keyword, matching this file's `noinline` / `alwaysinline` / `nooptimize` style
// rather than the C attribute's `no_sanitize_thread` underscores.
// ★★ AND FOR THIS AXIS THE PRINTER IS THE SINK, NOT A CONVENIENCE. `noinline`
// reaches the inliner, `nooptimize` reaches the rebuilder; `no_sanitize_thread`
// reaches NOTHING in this compiler (MEASURED: `grep -rni sanitiz src/` is empty),
// so `appendFuncAttrs` emitting this keyword IS the observable effect the whole
// source→MIR chain exists to produce. Deleting the printer arm below does not
// degrade a diagnostic — it erases the feature.
inline constexpr std::string_view kMirTextNoSanitizeThreadAttr = "nosanitizethread";

// ── shared helpers ────────────────────────────────────────────────────

namespace {

constexpr int kVersion = 1;

[[nodiscard]] std::string quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') { out += "\\n"; }
        else if (c == '\t') { out += "\\t"; }
        else { out += c; }
    }
    out += '"';
    return out;
}

// A dependent `false` for the `static_assert` that closes `appendLiteral`'s
// `if constexpr` ladder. A bare `static_assert(false, …)` there would fire during
// the template's DEFINITION rather than only on an unhandled instantiation; making
// the value depend on `T` defers it to the arm that is actually reached.
template <typename>
inline constexpr bool kMirTextNoSuchLiteralArm = false;

// ★★★ D-TEXT-TIER-READERS-KEEP-HAND-WRITTEN-FROMNAME-IF-CHAINS. Both spelling
// sets below used to exist twice in this file — a `…Name` switch for the writer
// and a `…FromName` if-chain for the reader — which is two owners of one fact on
// a WRITE-THEN-READ surface. The failure is not a stale message: rename a
// spelling on one side and `.dssmir` text this compiler EMITS stops loading in
// this compiler's own reader, in one direction only, with both halves compiling
// and a green suite. One table, both directions projected off it; see the same
// note in `hir_text.cpp` for why the table lives at the text tier rather than
// beside the enum.
//
// ⚠ `primName` is a deliberate SUBSET of `TypeKind` (aggregates and pointers
// have their own bracketed syntax), so it projects through `nameOrEmpty`:
// `EnumNameTable::name`'s row-0 fall-back would render a `Struct` as the keyword
// `bool`. `markerName`'s fall-back IS `"linear"` and `Linear` is row 0, so
// `name()` there reproduces the old switch exactly.
inline constexpr EnumNameTable<TypeKind, 19> kMirTextPrimTable{{{
    { TypeKind::Bool, "bool" },
    { TypeKind::Char, "char" },
    { TypeKind::Byte, "byte" },
    { TypeKind::Void, "void" },
    { TypeKind::I8,   "i8"   },
    { TypeKind::I16,  "i16"  },
    { TypeKind::I32,  "i32"  },
    { TypeKind::I64,  "i64"  },
    { TypeKind::I128, "i128" },
    { TypeKind::U8,   "u8"   },
    { TypeKind::U16,  "u16"  },
    { TypeKind::U32,  "u32"  },
    { TypeKind::U64,  "u64"  },
    { TypeKind::U128, "u128" },
    { TypeKind::F16,  "f16"  },
    { TypeKind::F32,  "f32"  },
    { TypeKind::F64,  "f64"  },
    { TypeKind::F80,  "f80"  },
    { TypeKind::F128, "f128" },
    // ⚠ `NullptrT` is deliberately ABSENT, and the absence is a CONTRACT, not an
    // oversight: `I_NullptrTypeInMir` is a verifier tripwire, so a nullptr_t
    // type never reaches MIR at all. The hir-tier table DOES carry it, which is
    // why these two tables are not one table — the two tiers accept genuinely
    // different sets, and merging them would teach the MIR reader a spelling the
    // MIR verifier refuses.
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kMirTextPrimTable);
DSS_CHECK_KEY_VOCABULARY(allNames(kMirTextPrimTable));

[[nodiscard]] std::string_view primName(TypeKind k) noexcept {
    return kMirTextPrimTable.nameOrEmpty(k);
}

[[nodiscard]] std::optional<TypeKind> primKindFromName(std::string_view s) noexcept {
    return kMirTextPrimTable.fromName(s);
}

// ── the LITERAL-CORE vocabulary, which is NOT the prim vocabulary ──────────
//
// A `MirLiteralValue::core` is a bare keyword in both directions, and the set of
// keywords legal THERE is the prim table PLUS the structural kinds an aggregate /
// pointer / enum constant can carry. `kMirTextPrimTable` deliberately omits those
// (a structural TYPE has bracketed syntax — `ptr<T>`, `arr<T,N>` — and a row here
// would make the bare keyword resolve to a prim and swallow the operand), so this
// position needs its own table rather than a row added over there.
//
// ★ IT HAD A HAND-WRITTEN IF-CHAIN ON EACH SIDE, which is exactly the two-owners
// shape `D-TEXT-TIER-READERS-KEEP-HAND-WRITTEN-FROMNAME-IF-CHAINS` closed for the
// prim and marker sets in this same file — the sweep that converted those two
// walked past this one. The writer's chain and the reader's chain were separate
// copies of one set several hundred lines apart, and the reader's had no final
// `else`: an unrecognized core spelling left `core` at `Void` and the parse
// SUCCEEDED (`D-MIR-TEXT-UNKNOWN-LITERAL-CORE-SILENTLY-DEGRADED-TO-VOID`).
inline constexpr EnumNameTable<TypeKind, 6> kMirTextLiteralCoreTable{{{
    { TypeKind::Struct, "struct" },
    { TypeKind::Union,  "union"  },
    { TypeKind::Array,  "array"  },
    { TypeKind::Ptr,    "ptr"    },
    { TypeKind::Ref,    "ref"    },
    { TypeKind::Enum,   "enum"   },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kMirTextLiteralCoreTable);
DSS_CHECK_KEY_VOCABULARY(allNames(kMirTextLiteralCoreTable));

// The spelling of a literal's core, or EMPTY when this format has none for it.
// ⚠ `nameOrEmpty` on BOTH tables, never `name`: neither lists an invalid
// sentinel, so the row-0 fall-back would render an unspelled kind as `bool` /
// `struct` — a wrong answer that reads as a legitimate declaration.
[[nodiscard]] std::string_view literalCoreName(TypeKind k) noexcept {
    std::string_view const p = kMirTextPrimTable.nameOrEmpty(k);
    return p.empty() ? kMirTextLiteralCoreTable.nameOrEmpty(k) : p;
}

[[nodiscard]] std::optional<TypeKind> literalCoreFromName(std::string_view s) noexcept {
    if (auto const k = kMirTextPrimTable.fromName(s); k.has_value()) return k;
    return kMirTextLiteralCoreTable.fromName(s);
}

// The accepted set for that position, projected off BOTH owning tables so the
// sentence cannot be narrower, wider or staler than the lookup above it.
[[nodiscard]] std::string literalCoreAccepted() {
    std::string out{detail::renderAllowedList(allNames(kMirTextPrimTable))};
    out += ", ";
    out += detail::renderAllowedList(allNames(kMirTextLiteralCoreTable));
    return out;
}

inline constexpr EnumNameTable<StructCfMarker, 12> kMirTextMarkerTable{{{
    { StructCfMarker::Linear,     "linear"     },
    { StructCfMarker::EntryBlock, "entry"      },
    { StructCfMarker::ExitBlock,  "exit"       },
    { StructCfMarker::LoopHeader, "loopheader" },
    { StructCfMarker::LoopLatch,  "looplatch"  },
    { StructCfMarker::LoopExit,   "loopexit"   },
    { StructCfMarker::IfThen,     "ifthen"     },
    { StructCfMarker::IfElse,     "ifelse"     },
    { StructCfMarker::IfJoin,     "ifjoin"     },
    { StructCfMarker::SwitchHead, "switchhead" },
    { StructCfMarker::SwitchCase, "switchcase" },
    { StructCfMarker::SwitchJoin, "switchjoin" },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kMirTextMarkerTable);
DSS_CHECK_KEY_VOCABULARY(allNames(kMirTextMarkerTable));

[[nodiscard]] std::string_view markerName(StructCfMarker m) noexcept {
    // The `-Werror=switch` backstop, owning no spelling: a new marker with no
    // table row would otherwise take the row-0 fall-back and be WRITTEN OUT as
    // `linear`, silently losing the structural role on every round trip.
    switch (m) {
        case StructCfMarker::Linear: case StructCfMarker::EntryBlock:
        case StructCfMarker::ExitBlock: case StructCfMarker::LoopHeader:
        case StructCfMarker::LoopLatch: case StructCfMarker::LoopExit:
        case StructCfMarker::IfThen: case StructCfMarker::IfElse:
        case StructCfMarker::IfJoin: case StructCfMarker::SwitchHead:
        case StructCfMarker::SwitchCase: case StructCfMarker::SwitchJoin:
            break;
    }
    return kMirTextMarkerTable.name(m);
}

// ── D-TEXT-TIER-REFUSALS-NAME-NO-ACCEPTED-SET: the TYPE-keyword position ────
//
// The STRUCTURAL keywords `parseType` dispatches on, beside the primitive names
// `kMirTextPrimTable` already owns. `unknown type 'foo'` named neither set, which
// is the same silence the flag / attribute / for-clause arms in the sibling
// `hir_text.cpp` were carrying.
//
// ⚠ THE SET, NOT THE DISPATCH. Unlike the keyword ladders in `hir_text.cpp`, this
// one is NOT converted into a `switch` over the table: the arms are structurally
// unlike each other (a wrap-1 keyword, a two-field `arr<T,N>`, a quoted-name
// composite, a signature), several share a body by design, and one — `unsigned` —
// is a PREFIX rather than a head keyword. A table used only for the SET keeps one
// owner of the spellings without reshaping a production this format's whole type
// grammar goes through. What replaces `-Werror=switch` here is a FEED-BACK PIN:
// every spelling this list advertises is driven back through `parseType` and must
// not come back as `unknown type`, so the list cannot advertise a keyword the
// ladder does not handle, nor go stale when one is added without it.
inline constexpr std::array<std::string_view, 16> kMirTextTypeKeywords{
    "invalid", "ptr", "ref", "nullable", "optional", "slice", "complex",
    "arr", "tuple", "struct", "union", "enum", "fn", "_BitInt", "unsigned",
    "ext",
};
DSS_CHECK_KEY_VOCABULARY(kMirTextTypeKeywords);

// The accepted set at a type position: the primitive names plus the structural
// keywords, projected off both owners.
[[nodiscard]] std::string typeKeywordsAccepted() {
    std::string out{detail::renderAllowedList(allNames(kMirTextPrimTable))};
    out += ", ";
    out += detail::renderAllowedList(kMirTextTypeKeywords);
    return out;
}

// D-MIR-TEXT-ROUND-TRIP-INCOMPLETE-FOR-OPERAND-CARRYING-FORMS: the
// `byvaluestackarg` payload's EXHAUST CLASS, spelled by name in both directions.
//
// ★ THE ROWS ARE THE `kByValueStackArgExhaust*` CONSTANTS THEMSELVES, not a
// private re-listing: the table is keyed on the `std::uint8_t` those constants
// already are, so this file mints no vocabulary of its own and cannot drift from
// `mir_opcode.hpp`. Spelling the class by NAME rather than as the raw 2-bit
// ordinal is the same rule this file applies to the enum underlying kind and the
// literal core, and it has the same reason: the field is 2 bits wide but only
// THREE of its four values are allocated, so an ordinal spelling lets the
// unallocated `3` round-trip as if it meant something.
inline constexpr EnumNameTable<std::uint8_t, 3> kMirTextExhaustTable{{{
    { kByValueStackArgExhaustNone, "none" },
    { kByValueStackArgExhaustGpr,  "gpr"  },
    { kByValueStackArgExhaustFpr,  "fpr"  },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kMirTextExhaustTable);
DSS_CHECK_KEY_VOCABULARY(allNames(kMirTextExhaustTable));

[[nodiscard]] std::optional<MirOpcode> opcodeFromMnemonic(std::string_view s) noexcept {
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(MirOpcode::Count_); ++i) {
        auto const op = static_cast<MirOpcode>(i);
        if (opcodeInfo(op).mnemonic == s) return op;
    }
    return std::nullopt;
}

// The accepted set for a mnemonic position, projected off the SAME walk
// `opcodeFromMnemonic` performs. The `.dssir` twin of `hir_text.cpp`'s
// `coreOpAccepted`, and minting nothing: `mir_opcode.hpp`'s `opcodeInfo` already
// owns every mnemonic, so a table here would be the second owner this row exists
// to remove (D-TEXT-TIER-REFUSALS-NAME-NO-ACCEPTED-SET).
//
// ⓘ Built on demand: it is reached only on a refusal, and materializing every
// mnemonic on every parse to serve a diagnostic that usually never fires is the
// wrong trade.
[[nodiscard]] std::string opcodeMnemonicsAccepted() {
    std::vector<std::string_view> names;
    names.reserve(static_cast<std::size_t>(MirOpcode::Count_));
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(MirOpcode::Count_); ++i) {
        names.push_back(opcodeInfo(static_cast<MirOpcode>(i)).mnemonic);
    }
    return detail::renderAllowedList(names);
}

// CallConv name mapping previously hand-rolled here (and in
// hir_text.cpp) — duplication caught in the 2026-06-02 cross-
// codebase audit. Call sites now use `callConvName(cc)` /
// `callConvFromName(s)` directly from the single source of truth
// (`kCallConvTable` in target_schema.hpp).

// ── Emitter ──────────────────────────────────────────────────────────

class Emitter {
public:
    Emitter(Mir const& m, MirTextContext const& ctx, DiagnosticReporter& reporter)
        : mir_(m), ctx_(ctx), reporter_(reporter) {}

    [[nodiscard]] std::string run() {
        // Collect symbols referenced by the module: functions, globals,
        // and GlobalAddr instruction payloads. Each gets a stable handle
        // (its SymbolId.v value, so the parser can re-mint the same id).
        collectSymbols();
        out_ += std::format("dssir {}\n", kVersion);
        emitSymbolsPreamble();
        out_ += "module {\n";
        for (std::uint32_t i = 0; i < mir_.moduleGlobalCount(); ++i) {
            emitGlobal(mir_.globalAt(i));
        }
        for (std::uint32_t i = 0; i < mir_.moduleFuncCount(); ++i) {
            emitFunction(mir_.funcAt(i));
        }
        out_ += "}\n";
        return std::move(out_);
    }

private:
    Mir const&            mir_;
    MirTextContext const& ctx_;
    DiagnosticReporter&   reporter_;
    std::string           out_;
    std::vector<std::uint32_t> symOrder_;            // declaration-order list
    std::unordered_map<std::uint32_t, bool> symSet_; // dedup set

    // ★★ THE SEVERITY IS A REQUIRED ARGUMENT, and it used to default to Warning
    // here while the sibling `hir_text.cpp` helper defaulted to Error — so the
    // SAME class of writer-side drop (a value this format cannot spell, whose
    // text therefore will not read back) was loud in one text tier and a warning
    // in the other, on two files that describe themselves as twins
    // (D-MIR-TEXT-ROUND-TRIP-INCOMPLETE-FOR-OPERAND-CARRYING-FORMS).
    //
    // ⓘ The fix is to DELETE the default rather than to pick one, because the two
    // severities are both right for different sites: a value that cannot be
    // rendered is an Error (the emitted text is not readable), while a caller who
    // supplied no interner has chosen a lossy dump and is told so once, at
    // Warning. A default makes that choice by omission, which is how the two
    // tiers came to disagree without either one deciding anything.
    void report(std::string what, DiagnosticSeverity sev) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::I_TextMalformed;
        d.severity = sev;
        d.actual   = std::move(what);
        reporter_.report(std::move(d));
    }

    void noteSymbol(std::uint32_t v) {
        if (v == 0) return;
        if (symSet_.emplace(v, true).second) symOrder_.push_back(v);
    }

    void collectSymbols() {
        for (std::uint32_t i = 0; i < mir_.moduleFuncCount(); ++i) {
            noteSymbol(mir_.funcSymbol(mir_.funcAt(i)).v);
        }
        for (std::uint32_t i = 0; i < mir_.moduleGlobalCount(); ++i) {
            noteSymbol(mir_.globalSymbol(mir_.globalAt(i)).v);
        }
        for (std::uint32_t i = 1; i < mir_.instCount(); ++i) {
            MirInstId const id{i, mir_.id().v};
            if (mir_.instOpcode(id) == MirOpcode::GlobalAddr) {
                noteSymbol(mir_.globalAddrSymbol(id).v);
            }
        }
    }

    void emitSymbolsPreamble() {
        if (symOrder_.empty()) return;
        out_ += "symbols {\n";
        for (std::uint32_t v : symOrder_) {
            std::string_view name;
            if (ctx_.symbolNames != nullptr && v < ctx_.symbolNames->size()) {
                name = (*ctx_.symbolNames)[v];
            }
            out_ += std::format("  %{} {}\n", v, quote(name));
        }
        out_ += "}\n";
    }

    // Recursive structural type emitter — mirrors the HIR text emitter's
    // discipline. The grammar must stay in sync with `parseType` below.
    bool internerWarned_ = false;
    void appendType(TypeId t) {
        if (!t.valid()) { out_ += "invalid"; return; }
        if (ctx_.interner == nullptr) {
            if (!internerWarned_) {
                // Warning, and the sibling `hir_text.cpp` site says Warning for the
                // same reason: this is not a property of the MODULE, it is the
                // caller's choice of a type-less dump. ⚠ The text it produces is
                // NOT re-parseable — `parseType` refuses `?` by name — which is
                // what `mir_text.hpp`'s context note now says out loud.
                report("no TypeInterner supplied; types render as '?', and text "
                       "containing '?' is refused on the way back in",
                       DiagnosticSeverity::Warning);
                internerWarned_ = true;
            }
            out_ += '?';
            return;
        }
        TypeInterner const& in = *ctx_.interner;
        auto args = [&](std::span<TypeId const> ops) {
            bool first = true;
            for (TypeId o : ops) {
                if (!first) out_ += ", ";
                appendType(o);
                first = false;
            }
        };
        switch (in.kind(t)) {
            case TypeKind::Ptr:      out_ += "ptr<";      appendType(in.operands(t)[0]); out_ += '>'; return;
            case TypeKind::Ref:      out_ += "ref<";      appendType(in.operands(t)[0]); out_ += '>'; return;
            case TypeKind::Nullable: out_ += "nullable<"; appendType(in.operands(t)[0]); out_ += '>'; return;
            case TypeKind::Optional: out_ += "optional<"; appendType(in.operands(t)[0]); out_ += '>'; return;
            case TypeKind::Slice:    out_ += "slice<";    appendType(in.operands(t)[0]); out_ += '>'; return;
            case TypeKind::Array:
                out_ += "arr<"; appendType(in.operands(t)[0]);
                out_ += std::format(", {}>", in.scalars(t)[0]);
                return;
            // C99 _Complex (D-CSUBSET-COMPLEX): a complex slot is a Ptr<complex<elem>>
            // in MIR; spell the pointee so the .dssmir dump is legible (not '?').
            case TypeKind::Complex:
                out_ += "complex<"; appendType(in.operands(t)[0]); out_ += '>'; return;
            case TypeKind::Tuple:
                out_ += "tuple<"; args(in.operands(t)); out_ += '>'; return;
            case TypeKind::Struct:
                out_ += "struct "; out_ += quote(in.name(t)); out_ += " {";
                args(in.operands(t)); out_ += '}'; return;
            case TypeKind::Union:
                out_ += "union "; out_ += quote(in.name(t)); out_ += " {";
                args(in.operands(t)); out_ += '}'; return;
            // ★★ THE UNDERLYING KIND IS SPELLED BY NAME, NOT BY ITS ORDINAL, and
            // that is a correctness requirement rather than a readability one.
            // This arm used to write `std::to_string(sc[0])` — the raw `TypeKind`
            // integer — and the reader cast it straight back. Two facts make that
            // wrong: `core_type.hpp` states in three separate placement notes that
            // NO TypeKind ordinal is serialized (which is why new kinds may be
            // appended freely), and the `wfloat` literal arm in the sibling HIR
            // tier already wrote the rule out — serialize "a STABLE semantic
            // discriminator, NOT the version-fragile TypeKind ordinal". The name
            // comes off the SAME table the rest of the type grammar reads, so the
            // two directions cannot drift and inserting an enumerator cannot
            // silently re-point an existing `.dssir`
            // (D-TEXT-TIER-ENUM-UNDERLYING-SERIALIZED-AS-A-TYPEKIND-ORDINAL).
            case TypeKind::Enum: {
                out_ += "enum "; out_ += quote(in.name(t));
                auto sc = in.scalars(t);
                if (!sc.empty() && static_cast<TypeKind>(sc[0]) != TypeKind::I32) {
                    auto const k = static_cast<TypeKind>(sc[0]);
                    std::string_view const n = primName(k);
                    if (n.empty()) {
                        // No spelling for it ⇒ say so and emit NOTHING, which makes
                        // the type read back as the plain `enum "N"` (I32) rather
                        // than as some other kind. A wrong underlying type silently
                        // resizes every enumerator.
                        report(std::format(
                            "enum '{}' has an underlying TypeKind (ordinal {}) this "
                            "format has no spelling for; the underlying type is NOT "
                            "rendered and the text will read back as the default",
                            in.name(t), sc[0]),
                            DiagnosticSeverity::Error);
                    } else {
                        out_ += " : ";
                        out_ += n;
                    }
                }
                return;
            }
            case TypeKind::FnSig: {
                out_ += "fn(";
                args(in.fnParams(t));
                out_ += ") -> ";
                appendType(in.fnResult(t));
                auto sc = in.scalars(t);
                if (!sc.empty()) {
                    auto const cc = static_cast<CallConv>(sc[0]);
                    if (cc != CallConv::CcSysV) {
                        out_ += " cc "; out_ += callConvName(cc);
                    }
                }
                return;
            }
            // ★★ WRITE-ONLY SPELLING, MADE LOUD RATHER THAN LEFT AS A WARNING
            // (D-MIR-TEXT-TYPE-GRAMMAR-HAS-WRITE-ONLY-SPELLINGS — the type-grammar
            // half of the same class the instruction grammar carried: four
            // spellings `appendType` rendered that `parseType` had no keyword for).
            // `parseType` has no `ext` keyword and never will without a
            // `TypeRegistry` to resolve the extension kind against — which this
            // reader does not take. So this arm emits text this reader cannot
            // read; that is an Error about the MODULE (an Extension type reached
            // MIR at all), not a note about the dump.
            case TypeKind::Extension: {
                report("MIR carries a TypeKind::Extension type — the HIR→MIR "
                       "boundary should have resolved it; `ext` has no spelling "
                       "this reader accepts, so the text will not read back",
                       DiagnosticSeverity::Error);
                out_ += "ext "; out_ += quote(in.name(t));
                return;
            }
            // C23 `_BitInt(N)` (D-CSUBSET-BITINT). ★ THE SPELLING IS THE HIR
            // TIER'S, DELIBERATELY: MIR already shares that tier's `bitint`
            // LITERAL syntax on the stated grounds that two syntaxes for one
            // serialization is two owners, and the TYPE is the same fact one
            // level up. Before this arm existed a `_BitInt` type fell into
            // `default:`, where `primName(BitInt)` is empty (the table
            // deliberately carries no row for it) and the writer emitted `?` —
            // i.e. a MIR module holding a bit-precise type could be dumped but
            // never read back.
            case TypeKind::BitInt: {
                if (!in.bitIntIsSigned(t)) out_ += "unsigned ";
                out_ += std::format("_BitInt({})", in.bitIntWidth(t));
                return;
            }
            default: {
                std::string_view const p = primName(in.kind(t));
                if (!p.empty()) out_ += p;
                else {
                    // Error, not Warning: `?` is not a type this reader accepts,
                    // so the text this produces cannot be read back.
                    report("unprintable type kind", DiagnosticSeverity::Error);
                    out_ += '?';
                }
                return;
            }
        }
    }

    // Render a `MirLiteralValue` inline. Format mirrors HIR's: `lit
    // <variant-tag> <value>` where `<variant-tag>` disambiguates the
    // variant arm (int / uint / float / bool / str / agg).
    void appendLiteral(MirLiteralValue const& lv) {
        out_ += "lit ";
        std::visit([&](auto const& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                out_ += "monostate";
            } else if constexpr (std::is_same_v<T, bool>) {
                out_ += "bool ";
                out_ += v ? "true" : "false";
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                out_ += std::format("int {}", v);
            } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                out_ += std::format("uint {}", v);
            } else if constexpr (std::is_same_v<T, double>) {
                out_ += std::format("float {}", v);
            } else if constexpr (std::is_same_v<T, std::string>) {
                out_ += "str "; out_ += quote(v);
            } else if constexpr (std::is_same_v<T, MirAggregateValue>) {
                out_ += "agg {";
                bool first = true;
                for (auto const& f : v.fields) {
                    if (!first) out_ += ", ";
                    appendLiteral(f);
                    first = false;
                }
                out_ += '}';
            } else if constexpr (std::is_same_v<T, MirSymbolAddrValue>) {
                // F5: link-time symbol-address literal (`&sym [+ addend]`).
                out_ += std::format("symaddr %{}", v.symbol);
                if (v.addend != 0) out_ += std::format(" + {}", v.addend);
            } else if constexpr (std::is_same_v<T, BitIntValue>) {
                // C4b `_BitInt` value. SAME spelling as the HIR tier's
                // `appendLiteralValue` — the two pools hold the SAME host type, so a
                // second syntax for it would be a second owner of one serialization.
                out_ += std::format("bitint {} {} {}", v.width(),
                                    v.isSigned() ? 1 : 0, v.limbs().size());
                for (std::uint64_t l : v.limbs()) out_ += std::format(" {}", l);
            } else if constexpr (std::is_same_v<T, WideFloatValue>) {
                // LD-3 folded F80/F128 value, again the HIR tier's spelling: the
                // FORMAT BIT-WIDTH (80|128) is the stable discriminator, never the
                // TypeKind ordinal.
                WideFloatValue::Packed const p = v.pack();
                out_ += std::format("wfloat {} {} {}",
                                    (v.kind() == TypeKind::F128) ? 128 : 80, p.hi, p.lo);
            } else {
                // ★★★ THE ARM THAT DID NOT EXIST, AND ITS ABSENCE WAS SILENT.
                // This chain had no final `else`, so the two arms above it — both
                // reachable, `toMirLiteral` copies a `BitIntValue`/`WideFloatValue`
                // straight across from the HIR pool — rendered NOTHING AT ALL: a
                // `_BitInt` or folded long-double constant came out as `lit  : ?`,
                // its value dropped, with no diagnostic on either side
                // (D-MIR-TEXT-WRITER-DROPS-UNHANDLED-LITERAL-VARIANT-ARMS).
                //
                // ★ A COMPILE ERROR, NOT A RUNTIME REFUSAL, because the set is
                // closed AT COMPILE TIME: `MirLiteralValue::value` is a variant, so
                // "did every arm get rendered" is decidable by the compiler, and a
                // runtime diagnostic would only report the miss on the runs that
                // happen to reach it. Adding an arm to the variant now FAILS THE
                // BUILD here, naming this writer, which is what the previous two
                // arms needed and did not get.
                static_assert(kMirTextNoSuchLiteralArm<T>,
                              "MirLiteralValue gained a variant arm that this "
                              "writer does not render — add an arm above (and its "
                              "twin in parseLiteral) rather than letting the value "
                              "serialize as nothing");
            }
        }, lv.value);
        out_ += " : ";
        // ⚠ ONE OWNER FOR THE CORE SPELLINGS, both directions. The `else if` ladder
        // that used to stand here was a second copy of `parseLiteral`'s ladder, and
        // its final arm wrote a bare `?` under a message that named no kind — so the
        // reader (whose ladder had no final arm at all) took the `?` as `Void` and
        // the round trip silently changed the literal's core.
        std::string_view const n = literalCoreName(lv.core);
        if (!n.empty()) { out_ += n; return; }
        report(std::format(
            "literal core TypeKind ordinal {} has no spelling in this format; "
            "rendered as '?', which the reader REFUSES — accepted: {}",
            static_cast<std::uint32_t>(lv.core), literalCoreAccepted()),
            DiagnosticSeverity::Error);
        out_ += '?';
    }

    void emitGlobal(MirGlobalId g) {
        out_ += std::format("  global %{} : ", mir_.globalSymbol(g).v);
        appendType(mir_.globalType(g));
        std::uint32_t const litIdx = mir_.globalInitLiteralIndex(g);
        MirFuncId const initFn = mir_.globalInitFunc(g);
        if (litIdx != UINT32_MAX) {
            out_ += " = ";
            appendLiteral(mir_.literalValue(litIdx));
        } else if (initFn.valid()) {
            out_ += std::format(" = initfunc %f{}", initFn.v);
        } else {
            out_ += " = zero";
        }
        out_ += '\n';
    }

    // TF-C78 (D-CSUBSET-NOINLINE): the function-attribute list — the per-MirFunc
    // metadata that is NOT recoverable from the symbol + signature alone.
    //
    // ★ THIS PRINTER PREVIOUSLY DROPPED `binding` AND `visibility` OUTRIGHT.
    // `emitFunction` emitted only `function %sym : <type> {`, and `parseFunction`
    // called the 2-arg `addFunction`, so every round-tripped function came back
    // (Global, Default) no matter what it was: a `static` function re-read as
    // externally visible, a `weak` one as strong. Adding `noInline` beside them
    // without fixing that would have reproduced the identical defect one field
    // over — so all three are carried, by the SAME mechanism, and the round-trip
    // pin covers all three.
    //
    // Printed ONLY when something is non-default (the `blockMarker != Linear`
    // discipline), so existing golden text for ordinary functions is unchanged.
    // Names come from the SHARED `symbolBindingName` / `symbolVisibilityName`
    // tables — not a second spelling table that could drift from them.
    void appendFuncAttrs(MirFuncId f) {
        SymbolBinding    const b  = mir_.funcBinding(f);
        SymbolVisibility const v  = mir_.funcVisibility(f);
        bool             const ni = mir_.funcNoInline(f);
        bool             const ai = mir_.funcAlwaysInline(f);   // TF-C81
        bool             const no = mir_.funcNoOptimize(f);     // TF-C85
        bool             const ns = mir_.funcNoSanitizeThread(f);   // TF-C92
        if (b == SymbolBinding::Global && v == SymbolVisibility::Default && !ni
            && !ai && !no && !ns) {
            return;
        }
        out_ += " [";
        bool first = true;
        auto const sep = [&] { if (!first) out_ += ", "; first = false; };
        if (b != SymbolBinding::Global)     { sep(); out_ += symbolBindingName(b); }
        if (v != SymbolVisibility::Default) { sep(); out_ += symbolVisibilityName(v); }
        if (ni)                             { sep(); out_ += kMirTextNoInlineAttr; }
        if (ai)                             { sep(); out_ += kMirTextAlwaysInlineAttr; }
        if (no)                             { sep(); out_ += kMirTextNoOptimizeAttr; }
        if (ns)                             { sep(); out_ += kMirTextNoSanitizeThreadAttr; }
        out_ += "]";
    }

    void emitFunction(MirFuncId f) {
        out_ += std::format("  function %{} : ", mir_.funcSymbol(f).v);
        appendType(mir_.funcSignature(f));
        appendFuncAttrs(f);
        out_ += " {\n";
        std::uint32_t const nBlocks = mir_.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nBlocks; ++bi) {
            emitBlock(mir_.funcBlockAt(f, bi));
        }
        out_ += "  }\n";
    }

    void emitBlock(MirBlockId b) {
        out_ += std::format("    block %b{}", b.v);
        StructCfMarker const m = mir_.blockMarker(b);
        if (m != StructCfMarker::Linear) {
            out_ += std::format(" [{}]", markerName(m));
        }
        out_ += " {\n";
        std::uint32_t const n = mir_.blockInstCount(b);
        for (std::uint32_t i = 0; i < n; ++i) {
            emitInst(mir_.blockInstAt(b, i), b);
        }
        out_ += "    }\n";
    }

    void emitInst(MirInstId id, MirBlockId block) {
        out_ += "      ";
        MirOpcode const op = mir_.instOpcode(id);
        MirOpcodeInfo const& info = opcodeInfo(op);
        bool const hasResult = mir_.instType(id).valid();
        if (hasResult) {
            out_ += std::format("%v{} = ", id.v);
        }
        out_ += info.mnemonic;
        if (hasResult) {
            out_ += " : ";
            appendType(mir_.instType(id));
        }
        // Per-opcode operand rendering.
        switch (op) {
            case MirOpcode::Const: {
                out_ += " (";
                appendLiteral(mir_.literalValue(mir_.constLiteralIndex(id)));
                out_ += ')';
                break;
            }
            case MirOpcode::Arg: {
                // Print the class ordinal; append the flat call-operand
                // position ONLY when it differs (single-class signatures keep
                // the golden `(N)` form; mixed-class emits `(ord, pos)`) so the
                // full payload survives a text round-trip (arg_payload.hpp).
                std::uint32_t const ord = mir_.argIndex(id);
                std::uint32_t const pos = mir_.argPosition(id);
                if (pos == ord) out_ += std::format(" ({})", ord);
                else            out_ += std::format(" ({}, {})", ord, pos);
                break;
            }
            case MirOpcode::GlobalAddr: {
                out_ += std::format(" (%{})", mir_.globalAddrSymbol(id).v);
                break;
            }
            case MirOpcode::IntrinsicCall: {
                out_ += std::format(" (intrinsic {}", mir_.intrinsicId(id));
                for (MirInstId const op2 : mir_.instOperands(id)) {
                    out_ += std::format(", %v{}", op2.v);
                }
                out_ += ')';
                break;
            }
            case MirOpcode::Phi: {
                out_ += " [";
                bool first = true;
                for (MirPhiIncoming const& inc : mir_.phiIncomings(id)) {
                    if (!first) out_ += ", ";
                    out_ += std::format("(%v{}, %b{})", inc.value.v, inc.pred.v);
                    first = false;
                }
                out_ += ']';
                break;
            }
            case MirOpcode::Br: {
                auto succs = mir_.blockSuccessors(block);
                if (!succs.empty()) out_ += std::format(" %b{}", succs[0].v);
                break;
            }
            case MirOpcode::CondBr: {
                auto operands = mir_.instOperands(id);
                auto succs = mir_.blockSuccessors(block);
                if (!operands.empty()) {
                    out_ += std::format(" %v{}", operands[0].v);
                }
                if (succs.size() >= 2) {
                    out_ += std::format(" %b{} %b{}", succs[0].v, succs[1].v);
                }
                break;
            }
            case MirOpcode::Switch: {
                auto operands = mir_.instOperands(id);
                auto succs = mir_.blockSuccessors(block);
                // operands[0] = discriminant; operands[1..N] = case constants.
                // succs[0..N-1] = case targets; succs.back() = default.
                if (!operands.empty()) {
                    out_ += std::format(" %v{}", operands[0].v);
                }
                out_ += " {";
                std::size_t const ncases = (succs.size() > 0) ? succs.size() - 1 : 0;
                for (std::size_t i = 0; i < ncases; ++i) {
                    if (i > 0) out_ += ", ";
                    out_ += std::format("case %v{} -> %b{}",
                        (i + 1 < operands.size()) ? operands[i + 1].v : 0,
                        succs[i].v);
                }
                if (!succs.empty()) {
                    if (ncases > 0) out_ += ", ";
                    out_ += std::format("default -> %b{}", succs.back().v);
                }
                out_ += '}';
                break;
            }
            case MirOpcode::IndirectBr: {
                // D-CSUBSET-COMPUTED-GOTO: render `indirectbr %v{addr} { %b.. }` —
                // the address operand then the full address-taken successor list.
                auto operands = mir_.instOperands(id);
                auto succs = mir_.blockSuccessors(block);
                if (!operands.empty()) out_ += std::format(" %v{}", operands[0].v);
                out_ += " { ";
                bool first = true;
                for (MirBlockId const b : succs) {
                    if (!first) out_ += ", ";
                    out_ += std::format("%b{}", b.v);
                    first = false;
                }
                out_ += " }";
                break;
            }
            case MirOpcode::Return: {
                // FC7 C1c: a by-value struct returned IN REGISTERS carries N piece
                // operands — emit ALL of them (space-separated), not just operand 0,
                // so the round-trip contract (emitMir∘parseMir∘emitMir == emitMir)
                // holds for multi-piece struct returns. Scalar = 1, void = 0.
                for (auto const& o : mir_.instOperands(id)) {
                    out_ += std::format(" %v{}", o.v);
                }
                break;
            }
            case MirOpcode::Unreachable:
                break;
            case MirOpcode::SehTryBegin: {
                // c115 SEH: `seh_try_begin <region> %b<try> %b<filter>`.
                auto succs = mir_.blockSuccessors(block);
                out_ += std::format(" {}", mir_.instPayload(id));
                if (succs.size() >= 2) {
                    out_ += std::format(" %b{} %b{}", succs[0].v, succs[1].v);
                }
                break;
            }
            case MirOpcode::SehFilterReturn: {
                // `seh_filter_return <region> %v<val> %b<handler>`.
                auto operands = mir_.instOperands(id);
                auto succs = mir_.blockSuccessors(block);
                out_ += std::format(" {}", mir_.instPayload(id));
                if (!operands.empty()) out_ += std::format(" %v{}", operands[0].v);
                if (!succs.empty())    out_ += std::format(" %b{}", succs[0].v);
                break;
            }
            case MirOpcode::SehTryEnd:
                // `seh_try_end <region>` — the payload is the region id.
                out_ += std::format(" {}", mir_.instPayload(id));
                break;
            // ★★★ INLINE ASM: RENDER THE EDGES, AND SAY WHAT CANNOT BE RENDERED
            // (D-MIR-TEXT-INLINE-ASM-RENDERS-A-POOL-INDEX-AND-NO-EDGES).
            //
            // Both asm opcodes used to fall into `default:`, which rendered the
            // operands and then the raw `instPayload` — an index into the module's
            // `MirAsmDescriptorPool` that means NOTHING once the text is detached
            // from the module that produced it — and, because `default:` renders no
            // successors, an `asm goto` printed with **no CFG edges at all**.
            // ⚠ THE HALF THAT IS A DECLARED LIMITATION IS NOT THIS ONE. The parser
            // REFUSES both mnemonics by name and states the reason (the descriptor
            // carries text and constraints this format does not spell), so the text
            // is deliberately one-way. A one-way dump is a choice; a dump that
            // silently omits a terminator's edges while looking complete is not, and
            // an `asm goto` is the first terminator this reached.
            // ★ WHAT IS RENDERED: the operands, then every successor — the labels in
            // source order followed by the FALL-THROUGH, which is the successor
            // convention `MirBuilder::addInlineAsmGoto` establishes and the MIR
            // verifier enforces — then an explicit marker in place of the descriptor.
            // The marker names the absence rather than substituting a number for it,
            // which is the whole difference between this and what it replaced.
            case MirOpcode::InlineAsm:
            case MirOpcode::InlineAsmGoto: {
                auto operands = mir_.instOperands(id);
                if (!operands.empty()) {
                    out_ += " (";
                    bool first = true;
                    for (MirInstId const op2 : operands) {
                        if (!first) out_ += ", ";
                        out_ += std::format("%v{}", op2.v);
                        first = false;
                    }
                    out_ += ')';
                }
                if (op == MirOpcode::InlineAsmGoto) {
                    auto succs = mir_.blockSuccessors(block);
                    for (std::size_t k = 0; k < succs.size(); ++k) {
                        // The last successor is the fall-through; labelling it in the
                        // text is what lets a reader check the convention instead of
                        // counting on having remembered it.
                        out_ += std::format(" {}%b{}",
                                            (k + 1 == succs.size()) ? "fallthrough " : "",
                                            succs[k].v);
                    }
                }
                // ⚠ ONE TOKEN, HYPHENATED, AND THAT IS A REQUIREMENT RATHER
                // THAN A STYLE. ✔MEASURED: the first spelling of this marker was
                // the English phrase `<descriptor not spelled by this format>`,
                // and the round-trip ABORTED the process --
                // `addInst: opcode 'not' takes [1, 1] operands but got 0`. The
                // parser refuses the `inlineasm*` mnemonic and its recovery then
                // re-tokenizes the rest of the line, where the bare word `not` is
                // a real MIR mnemonic. A writer must not hand the parser a valid
                // opcode inside prose, so the marker is a single token with no
                // interior spaces.
                out_ += " <asm-descriptor-unspelled>";
                break;
            }
            // ★★★ THESE TWO ARMS USED TO LIVE INSIDE `default:` AS `if (op == …)`
            // TESTS, AND THE PLACEMENT WAS THE DEFECT.
            //
            // The reader's `default:` is the exact inverse of the writer's
            // `default:` — operands in parens, then an optional `payload <n>`.
            // Two opcodes rendered a DIFFERENT tail from inside that arm, so
            // the writer's generic case and the reader's generic case stopped
            // being inverses of each other while still looking like a matched
            // pair. Hoisted to real `case` arms, the invariant is visible: an
            // opcode is handled by paired EXPLICIT arms on both sides, or by the
            // paired GENERIC arms on both sides, and never by one of each
            // (D-MIR-TEXT-ROUND-TRIP-INCOMPLETE-FOR-OPERAND-CARRYING-FORMS).
            //
            // D-CSUBSET-COMPUTED-GOTO: the `blockaddress` payload is the target
            // BLOCK id — rendered as `%b{}` because a raw integer reads as a
            // meaningless value and breaks the block-relative reading.
            case MirOpcode::BlockAddress: {
                out_ += std::format(" %b{}", mir_.instPayload(id));
                break;
            }
            // D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS: the payload
            // PACKS the byte size (low 30 bits) + the exhaust class (high 2) —
            // print the unpacked fields, not the raw integer (which reads as a
            // ~2.1e9 garbage value when an exhaust bit is set).
            //
            // ⚠ THE EXHAUST CLASS IS SPELLED BY NAME. It was the raw 2-bit
            // ordinal, and the field has FOUR representable values of which only
            // three are allocated — so `exhaust 3` was a legal-looking spelling
            // for a class that does not exist, on both sides. The name comes off
            // `kMirTextExhaustTable`, whose rows ARE the `mir_opcode.hpp`
            // constants.
            case MirOpcode::ByValueStackArg: {
                auto operands = mir_.instOperands(id);
                if (!operands.empty()) {
                    out_ += " (";
                    bool first = true;
                    for (MirInstId const op2 : operands) {
                        if (!first) out_ += ", ";
                        out_ += std::format("%v{}", op2.v);
                        first = false;
                    }
                    out_ += ')';
                }
                std::uint32_t const payload = mir_.instPayload(id);
                auto const cls = static_cast<std::uint8_t>(
                    (payload >> kByValueStackArgExhaustShift) & 0x3u);
                std::string_view const clsName = kMirTextExhaustTable.nameOrEmpty(cls);
                if (clsName.empty()) {
                    report(std::format(
                        "byvaluestackarg carries exhaust class {}, which is not one "
                        "of the allocated classes — accepted: {}", cls,
                        detail::renderAllowedList(allNames(kMirTextExhaustTable))),
                        DiagnosticSeverity::Error);
                }
                out_ += std::format(" size {} exhaust {}",
                                    payload & kByValueStackArgSizeMask,
                                    clsName.empty() ? std::string_view{"?"} : clsName);
                break;
            }
            default: {
                // Generic: render all operands.
                auto operands = mir_.instOperands(id);
                if (!operands.empty()) {
                    out_ += " (";
                    bool first = true;
                    for (MirInstId const op2 : operands) {
                        if (!first) out_ += ", ";
                        out_ += std::format("%v{}", op2.v);
                        first = false;
                    }
                    out_ += ')';
                }
                // Payload tail (for ExtractValue / InsertValue which
                // pack indices into Const operands; here we just expose
                // the raw payload integer if non-zero and not already
                // covered).
                std::uint32_t const payload = mir_.instPayload(id);
                if (payload != 0
                 && op != MirOpcode::ExtractValue
                 && op != MirOpcode::InsertValue) {
                    out_ += std::format(" payload {}", payload);
                }
                break;
            }
        }
        out_ += '\n';
    }
};

} // namespace

std::string emitMir(Mir const& mir, MirTextContext const& ctx,
                    DiagnosticReporter& reporter) {
    Emitter e{mir, ctx, reporter};
    return e.run();
}

// ── Lexer ────────────────────────────────────────────────────────────

namespace {

enum class TokKind {
    End,
    Ident,
    Integer,
    Float,
    String,
    Percent,        // `%`
    LBrace, RBrace,
    LParen, RParen,
    LBracket, RBracket,
    LAngle, RAngle,
    Colon,
    Comma,
    Arrow,          // `->`
    Eq,
    Dot,            // `.` (for `icmp.eq` etc.)
    Minus,
    Unknown,
};

struct Tok {
    TokKind     kind = TokKind::End;
    std::string text;
    std::size_t off  = 0;
};

class Lexer {
public:
    explicit Lexer(std::string_view text) : text_(text) {}

    Tok take() {
        skipWhitespaceAndComments();
        Tok t;
        t.off = pos_;
        if (pos_ >= text_.size()) { t.kind = TokKind::End; return t; }
        char const c = text_[pos_];
        if (c == '%') { ++pos_; t.kind = TokKind::Percent; return t; }
        if (c == '{') { ++pos_; t.kind = TokKind::LBrace;  return t; }
        if (c == '}') { ++pos_; t.kind = TokKind::RBrace;  return t; }
        if (c == '(') { ++pos_; t.kind = TokKind::LParen;  return t; }
        if (c == ')') { ++pos_; t.kind = TokKind::RParen;  return t; }
        if (c == '[') { ++pos_; t.kind = TokKind::LBracket; return t; }
        if (c == ']') { ++pos_; t.kind = TokKind::RBracket; return t; }
        if (c == '<') { ++pos_; t.kind = TokKind::LAngle;  return t; }
        if (c == '>') { ++pos_; t.kind = TokKind::RAngle;  return t; }
        if (c == ':') { ++pos_; t.kind = TokKind::Colon;   return t; }
        if (c == ',') { ++pos_; t.kind = TokKind::Comma;   return t; }
        if (c == '=') { ++pos_; t.kind = TokKind::Eq;      return t; }
        if (c == '.') { ++pos_; t.kind = TokKind::Dot;     return t; }
        if (c == '-') {
            if (pos_ + 1 < text_.size() && text_[pos_ + 1] == '>') {
                pos_ += 2; t.kind = TokKind::Arrow; return t;
            }
            // Negative numeric literal — fall through to number scan
            // by including the '-' in the text.
            std::size_t const start = pos_;
            ++pos_;
            while (pos_ < text_.size() && (std::isdigit(static_cast<unsigned char>(text_[pos_]))
                                         || text_[pos_] == '.'
                                         || text_[pos_] == 'e' || text_[pos_] == 'E'
                                         || text_[pos_] == '+' || text_[pos_] == '-')) {
                ++pos_;
            }
            t.text = std::string(text_.substr(start, pos_ - start));
            t.kind = (t.text.find('.') != std::string::npos
                   || t.text.find('e') != std::string::npos
                   || t.text.find('E') != std::string::npos)
                ? TokKind::Float : TokKind::Integer;
            return t;
        }
        if (c == '"') {
            ++pos_;
            std::string s;
            while (pos_ < text_.size() && text_[pos_] != '"') {
                if (text_[pos_] == '\\' && pos_ + 1 < text_.size()) {
                    char const e = text_[pos_ + 1];
                    if      (e == 'n')  s += '\n';
                    else if (e == 't')  s += '\t';
                    else if (e == '"')  s += '"';
                    else if (e == '\\') s += '\\';
                    else                s += e;
                    pos_ += 2;
                } else {
                    s += text_[pos_++];
                }
            }
            if (pos_ < text_.size()) ++pos_;  // consume closing '"'
            t.kind = TokKind::String;
            t.text = std::move(s);
            return t;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::size_t const start = pos_;
            while (pos_ < text_.size() && (std::isdigit(static_cast<unsigned char>(text_[pos_]))
                                         || text_[pos_] == '.'
                                         || text_[pos_] == 'e' || text_[pos_] == 'E'
                                         || text_[pos_] == '+' || text_[pos_] == '-')) {
                ++pos_;
            }
            t.text = std::string(text_.substr(start, pos_ - start));
            t.kind = (t.text.find('.') != std::string::npos
                   || t.text.find('e') != std::string::npos
                   || t.text.find('E') != std::string::npos)
                ? TokKind::Float : TokKind::Integer;
            return t;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::size_t const start = pos_;
            while (pos_ < text_.size()
                && (std::isalnum(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == '_')) {
                ++pos_;
            }
            t.text = std::string(text_.substr(start, pos_ - start));
            t.kind = TokKind::Ident;
            return t;
        }
        ++pos_;
        t.kind = TokKind::Unknown;
        t.text = std::string(1, c);
        return t;
    }

    Tok peek() {
        std::size_t const save = pos_;
        Tok t = take();
        pos_ = save;
        return t;
    }

    [[nodiscard]] std::size_t pos() const noexcept { return pos_; }
    void setPos(std::size_t p) noexcept { pos_ = p; }

    // The raw source. ⓘ ONE caller needs it, and it needs it because the
    // question it asks is about LINES rather than about tokens: `parseInstruction`
    // refuses an instruction whose line still holds unconsumed text, which is the
    // class guard for a writer arm out-running its reader arm. A token stream
    // cannot answer "is this on the same line", and reconstructing line structure
    // by re-lexing would be a second lexer.
    [[nodiscard]] std::string_view text() const noexcept { return text_; }

private:
    void skipWhitespaceAndComments() {
        while (pos_ < text_.size()) {
            char const c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '/') {
                while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;
            } else {
                break;
            }
        }
    }

    std::string_view text_;
    std::size_t      pos_ = 0;
};

// ── Parser ───────────────────────────────────────────────────────────

class Parser {
public:
    Parser(std::string_view text, CompilationUnitId cuId, DiagnosticReporter& reporter)
        : lex_(text), reporter_(reporter), cuId_(cuId), interner_(cuId) {}

    [[nodiscard]] std::unique_ptr<MirParseResult> run() {
        if (!expectIdent("dssir")) return makeEmptyResult();
        Tok const ver = lex_.take();
        if (ver.kind != TokKind::Integer || ver.text != "1") {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::I_TextVersionMismatch;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format("expected version 1, got '{}'", ver.text);
            reporter_.report(std::move(d));
            return makeEmptyResult();
        }
        // Optional `symbols { ... }` preamble.
        if (peekIdent("symbols")) {
            parseSymbolsPreamble();
        }
        if (!expectIdent("module")) return makeEmptyResult();
        if (!expect(TokKind::LBrace)) return makeEmptyResult();
        // Body: zero or more `global` / `function` items. Panic-mode
        // recovery: on an unexpected token, emit ONE diagnostic and
        // skip until the next `global`/`function` keyword or the
        // closing `}` — avoids per-token cascade after a parse
        // failure inside `parseFunction` / `parseGlobal` returned
        // mid-production.
        while (true) {
            Tok t = lex_.peek();
            if (t.kind == TokKind::RBrace || t.kind == TokKind::End) break;
            if (t.kind == TokKind::Ident && t.text == "global") {
                lex_.take();
                parseGlobal();
            } else if (t.kind == TokKind::Ident && t.text == "function") {
                lex_.take();
                parseFunction();
            } else {
                emitMalformed(std::format("expected 'global' or 'function', got '{}'", t.text));
                // Skip tokens until we find a recovery anchor.
                while (true) {
                    Tok pk = lex_.peek();
                    if (pk.kind == TokKind::End) break;
                    if (pk.kind == TokKind::RBrace) break;
                    if (pk.kind == TokKind::Ident
                     && (pk.text == "global" || pk.text == "function")) break;
                    lex_.take();
                }
            }
        }
        (void)expect(TokKind::RBrace);
        return finalize();
    }

private:
    Lexer                lex_;
    DiagnosticReporter&  reporter_;
    CompilationUnitId    cuId_;
    TypeInterner         interner_;
    MirBuilder           builder_;
    std::vector<std::string> symbolNames_;     // SymbolId.v → name
    std::unordered_map<std::uint32_t, MirBlockId> blockMap_;  // text slot v → builder block id
    std::unordered_map<std::uint32_t, MirInstId>  valueMap_;  // text slot v → builder inst id
    std::unordered_map<std::uint32_t, MirFuncId>  funcMap_;   // text slot v → builder func id
    // Globals whose `initfunc` references a function-text-slot that
    // wasn't yet parsed at the time the global was declared. Resolved
    // at finalize() by replaying the global with the resolved MirFuncId.
    struct PendingGlobalInit {
        TypeId        ty;
        SymbolId      sym;
        std::uint32_t initFuncSlot;
    };
    std::vector<PendingGlobalInit> pendingInitFuncGlobals_;
    // Forward-reference book-keeping for phi incomings whose value or
    // pred wasn't yet emitted at parse time. Resolved at finalize().
    struct PendingPhi {
        MirInstId    phi;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> incomings;
    };
    std::vector<PendingPhi> pendingPhis_;
    bool errors_ = false;

    [[nodiscard]] std::unique_ptr<MirParseResult> makeEmptyResult() {
        return std::make_unique<MirParseResult>(
            Mir{}, TypeInterner{cuId_}, std::vector<std::string>{});
    }

    void emitMalformed(std::string what) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::I_TextMalformed;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::move(what);
        reporter_.report(std::move(d));
        errors_ = true;
    }

    void emitUnknownName(std::string what) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::I_TextUnknownName;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::move(what);
        reporter_.report(std::move(d));
        errors_ = true;
    }

    // Resolve a spelling against the vocabulary's OWN table, and REFUSE a miss
    // rather than keeping `dflt` — the twin of `hir_text.cpp`'s `orMalformed`,
    // and the reason it is a template over the TABLE rather than over an
    // already-resolved `std::optional`: the lookup and the advertised set then
    // come off the same rows, so the sentence cannot be narrower, wider or
    // staler than the check
    // (D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET).
    //
    // ⚠ `dflt` is returned ONLY so the parse can keep collecting diagnostics;
    // `emitUnknownName` has already set `errors_`, so `finalize()` discards the
    // module and the caller never sees a value built on it.
    template <class E, std::size_t N>
    [[nodiscard]] E orUnknownName(EnumNameTable<E, N> const& table,
                                  std::string_view name, char const* what, E dflt) {
        if (auto const v = table.fromName(name); v.has_value()) return *v;
        emitUnknownName(std::format("unknown {} '{}' — accepted: {}", what, name,
                                    detail::renderAllowedList(allNames(table))));
        return dflt;
    }

    bool expect(TokKind k) {
        Tok t = lex_.take();
        if (t.kind != k) {
            emitMalformed(std::format("expected token kind {}, got '{}'",
                static_cast<int>(k), t.text));
            return false;
        }
        return true;
    }

    bool expectIdent(std::string_view name) {
        Tok t = lex_.take();
        if (t.kind != TokKind::Ident || t.text != name) {
            emitMalformed(std::format("expected '{}', got '{}'", name, t.text));
            return false;
        }
        return true;
    }

    // Parse a numeric token; emit an `I_TextMalformed` diagnostic on
    // `from_chars` failure (the silent-zero default that bare
    // `std::from_chars` produces hides malformed numeric input from
    // the user; verify-on-load might catch a downstream structural
    // mismatch but loses the precise blame).
    template <typename T>
    [[nodiscard]] T parseNumber(std::string_view text,
                                std::string_view what) {
        T v{};
        auto [ptr, ec] = std::from_chars(text.data(),
                                         text.data() + text.size(), v);
        if (ec != std::errc{} || ptr != text.data() + text.size()) {
            emitMalformed(std::format("malformed {} value '{}'", what, text));
        }
        return v;
    }
    [[nodiscard]] double parseDouble(std::string_view text) {
        // `std::from_chars` for float is not universally supported
        // pre-GCC 11; fall back to strtod which signals via errno + endptr.
        std::string buf{text};
        char* end = nullptr;
        errno = 0;
        double const d = std::strtod(buf.c_str(), &end);
        if (errno == ERANGE || end == buf.c_str()) {
            emitMalformed(std::format("malformed float value '{}'", text));
        }
        return d;
    }

    bool peekIdent(std::string_view name) {
        Tok t = lex_.peek();
        return t.kind == TokKind::Ident && t.text == name;
    }

    [[nodiscard]] std::uint32_t parsePercentValue() {
        if (!expect(TokKind::Percent)) return 0;
        // Optional kind prefix: 'b' / 'v' / 'f' / 'g' (ignore — the
        // grammar uses position to disambiguate; the prefix is for
        // human readability only). Parse `[bvgf]?digits`.
        Tok t = lex_.take();
        if (t.kind == TokKind::Ident
         && (t.text.front() == 'b' || t.text.front() == 'v'
          || t.text.front() == 'f' || t.text.front() == 'g')) {
            // Could be `b3`, `v12` etc. — strip the prefix letter and
            // parse the rest as integer.
            std::string_view const num = t.text;
            std::uint32_t v = 0;
            std::size_t i = 1;
            while (i < num.size() && std::isdigit(static_cast<unsigned char>(num[i]))) {
                v = v * 10 + static_cast<std::uint32_t>(num[i] - '0');
                ++i;
            }
            // ⚠ THE DIGITS ARE THE HANDLE; NO DIGITS IS NOT HANDLE ZERO. The loop
            // above used to be the whole arm, so `%b`, `%vx` or `%g_tmp` fell out
            // with `v == 0` and NO diagnostic — and 0 is a LIVE SLOT NUMBER here,
            // which is what turns a typo into a wrong answer rather than a lookup
            // miss. ✔MEASURED 2026-08-21 with this guard removed: in a function
            // whose entry block is `%b0`, `br %b` silently became `br %b0` — a
            // SELF-LOOP on the entry block, from text that named no block at all.
            // `i == 1` means the class letter was followed by nothing numeric;
            // `i != num.size()` means digits then trailing junk (`%b3x`), which is
            // equally not a handle
            // (D-MIR-TEXT-PERCENT-HANDLE-WITHOUT-DIGITS-SILENTLY-BECOMES-ZERO).
            if (i == 1 || i != num.size()) {
                emitMalformed(std::format(
                    "malformed handle '%{}': a '%' handle is an optional "
                    "'b'/'v'/'f'/'g' class letter followed by DIGITS", t.text));
                return 0;
            }
            return v;
        }
        if (t.kind == TokKind::Integer) {
            return parseNumber<std::uint32_t>(t.text, "% handle");
        }
        emitMalformed(std::format("expected handle after '%', got '{}'", t.text));
        return 0;
    }

    void parseSymbolsPreamble() {
        (void)expectIdent("symbols");
        if (!expect(TokKind::LBrace)) return;
        while (true) {
            Tok t = lex_.peek();
            if (t.kind == TokKind::RBrace || t.kind == TokKind::End) break;
            std::uint32_t const v = parsePercentValue();
            Tok name = lex_.take();
            if (name.kind != TokKind::String) {
                emitMalformed(std::format("expected symbol name string, got '{}'",
                    name.text));
                continue;
            }
            if (symbolNames_.size() <= v) symbolNames_.resize(v + 1);
            symbolNames_[v] = name.text;
        }
        (void)expect(TokKind::RBrace);
    }

    // Parse a structural type. Grammar mirrors `appendType`.
    [[nodiscard]] TypeId parseType() {
        Tok t = lex_.take();
        if (t.kind != TokKind::Ident) {
            // ⚠ `?` IS THE WRITER'S OWN MARK AND IS REFUSED BY NAME. `appendType`
            // emits it when no `TypeInterner` was supplied (or for a kind it has
            // no spelling for), so a `?` here is this compiler's own output coming
            // back, not an author's typo — and the two need different sentences,
            // for the same reason `hir_text.cpp`'s unspelled-aggregate marker is
            // refused by name rather than as an unknown tag.
            if (t.kind == TokKind::Unknown && t.text == "?") {
                emitMalformed(
                    "'?' is the emitter's mark for a type it could not render — "
                    "either no TypeInterner was supplied to emitMir, or the kind "
                    "has no spelling in this format. The type was NOT serialized, "
                    "so this text cannot be read back into the module it came from");
                return InvalidType;
            }
            emitMalformed(std::format("expected type, got '{}'", t.text));
            return InvalidType;
        }
        if (t.text == "invalid") return InvalidType;
        if (auto k = primKindFromName(t.text); k.has_value()) {
            return interner_.primitive(*k);
        }
        if (t.text == "ptr" || t.text == "ref" || t.text == "nullable"
         || t.text == "optional" || t.text == "slice"
         // C99 `_Complex` (D-CSUBSET-COMPLEX). ⚠ FOUND IN PASSING AND IT IS THE
         // SAME DEFECT THE ROW NAMES, one tier over: `appendType` has rendered
         // `complex<elem>` since the complex arm landed and this reader had no
         // keyword for it, so every MIR module carrying a complex slot — which is
         // every one that survives `_Complex` lowering, the arm's own comment says
         // a complex slot IS a `Ptr<complex<elem>>` — emitted text that came back
         // as `unknown type 'complex'`.
         || t.text == "complex") {
            if (!expect(TokKind::LAngle)) return InvalidType;
            TypeId const inner = parseType();
            (void)expect(TokKind::RAngle);
            if (t.text == "ptr")      return interner_.pointer(inner);
            if (t.text == "ref")      return interner_.reference(inner);
            if (t.text == "nullable") return interner_.nullable(inner);
            if (t.text == "optional") return interner_.optional(inner);
            if (t.text == "slice")    return interner_.slice(inner);
            if (t.text == "complex")  return interner_.complex(inner);
        }
        // C23 `_BitInt(N)` / `unsigned _BitInt(N)` — the inverse of `appendType`'s
        // arm, and the HIR tier's spelling (this format already shares that tier's
        // `bitint` LITERAL syntax on the stated grounds that one serialization
        // must not have two syntaxes).
        if (t.text == "unsigned" || t.text == "_BitInt") {
            bool const isSigned = (t.text != "unsigned");
            if (!isSigned && !expectIdent("_BitInt")) return InvalidType;
            if (!expect(TokKind::LParen)) return InvalidType;
            Tok const w = lex_.take();
            auto const width = parseNumber<std::int64_t>(w.text, "_BitInt width");
            (void)expect(TokKind::RParen);
            return interner_.bitInt(width, isSigned);
        }
        // `ext "Name"` is WRITE-ONLY BY CONSTRUCTION and says so. Resolving an
        // extension kind needs a `TypeRegistry`, which `parseMir` does not take;
        // the writer already reports the module-level defect (an Extension type
        // reached MIR at all) at Error severity. Refusing by name here means an
        // author reading the text is told WHY, instead of `unknown type 'ext'`.
        if (t.text == "ext") {
            emitMalformed(
                "'ext' names a TypeKind::Extension type, which this reader cannot "
                "resolve: an extension kind is identified against a TypeRegistry "
                "and parseMir takes none. An Extension type reaching MIR is itself "
                "a defect — the HIR→MIR boundary should have resolved it");
            return InvalidType;
        }
        if (t.text == "arr") {
            if (!expect(TokKind::LAngle)) return InvalidType;
            TypeId const elem = parseType();
            (void)expect(TokKind::Comma);
            Tok len = lex_.take();
            if (len.kind != TokKind::Integer) {
                emitMalformed("expected array length integer");
                return InvalidType;
            }
            std::int64_t lv = parseNumber<std::int64_t>(len.text, "array length");
            (void)expect(TokKind::RAngle);
            return interner_.array(elem, lv);
        }
        if (t.text == "tuple") {
            if (!expect(TokKind::LAngle)) return InvalidType;
            std::vector<TypeId> ops;
            while (true) {
                Tok pk = lex_.peek();
                if (pk.kind == TokKind::RAngle) break;
                if (!ops.empty()) (void)expect(TokKind::Comma);
                ops.push_back(parseType());
            }
            (void)expect(TokKind::RAngle);
            return interner_.tuple(ops);
        }
        if (t.text == "struct" || t.text == "union") {
            Tok name = lex_.take();
            if (name.kind != TokKind::String) {
                emitMalformed("expected struct/union name");
                return InvalidType;
            }
            if (!expect(TokKind::LBrace)) return InvalidType;
            std::vector<TypeId> fields;
            while (true) {
                Tok pk = lex_.peek();
                if (pk.kind == TokKind::RBrace) break;
                if (!fields.empty()) (void)expect(TokKind::Comma);
                fields.push_back(parseType());
            }
            (void)expect(TokKind::RBrace);
            if (t.text == "struct") return interner_.structType(name.text, fields);
            return interner_.unionType(name.text, fields);
        }
        if (t.text == "enum") {
            Tok name = lex_.take();
            if (name.kind != TokKind::String) {
                emitMalformed("expected enum name"); return InvalidType;
            }
            // ⚠ FAIL LOUD, AND SPELL THE KIND BY NAME. This arm read an INTEGER and
            // cast it straight to `TypeKind` with no range check whatsoever, so
            // `enum "E" : 9999` produced an enum whose underlying kind was not a
            // kind at all, and `enum "E" : 24` silently made an aggregate the
            // underlying type of an enumeration — in both cases the parse SUCCEEDED.
            // The `hir_text.cpp` twin at least bounded the ordinal by `Count_`, and
            // there an out-of-range value silently kept `I32`: the same defect
            // wearing a range check
            // (D-TEXT-TIER-ENUM-UNDERLYING-SERIALIZED-AS-A-TYPEKIND-ORDINAL).
            // The spelling now comes off `kMirTextPrimTable`, the SAME table the
            // rest of this type grammar reads, so an unrecognized name is refused
            // with the accepted set projected from those rows.
            TypeKind underlying = TypeKind::I32;
            if (lex_.peek().kind == TokKind::Colon) {
                lex_.take();
                Tok n = lex_.take();
                if (n.kind != TokKind::Ident) {
                    emitMalformed(std::format(
                        "expected an enum underlying type name after ':', got '{}'",
                        n.text));
                } else {
                    underlying = orUnknownName(kMirTextPrimTable, n.text,
                                               "enum underlying type", TypeKind::I32);
                }
            }
            return interner_.enumType(name.text, underlying);
        }
        if (t.text == "fn") {
            if (!expect(TokKind::LParen)) return InvalidType;
            std::vector<TypeId> params;
            while (true) {
                Tok pk = lex_.peek();
                if (pk.kind == TokKind::RParen) break;
                if (!params.empty()) (void)expect(TokKind::Comma);
                params.push_back(parseType());
            }
            (void)expect(TokKind::RParen);
            (void)expect(TokKind::Arrow);
            TypeId const ret = parseType();
            CallConv cc = CallConv::CcSysV;
            if (peekIdent("cc")) {
                lex_.take();
                Tok n = lex_.take();
                // ⚠ FAIL LOUD, and this arm was SILENT — the same shape as the
                // block marker below, one tier further down and with a worse
                // consequence. It read `if (auto c = callConvFromName(n.text);
                // c.has_value()) cc = *c;` with no `else`, so an unrecognized
                // calling convention left `cc` at `CcSysV` and the parse
                // SUCCEEDED. A `.dssir` naming `ms64` or `aapcs64` under any
                // spelling this table does not carry would come back SysV: an
                // ABI CHANGE — different argument registers, different stack
                // discipline — applied silently to a function signature, on a
                // format whose whole contract is lossless round-trip
                // (D-MIR-TEXT-UNKNOWN-CALLING-CONVENTION-SILENTLY-DEGRADED-TO-SYSV).
                if (auto c = kCallConvTable.fromName(n.text); c.has_value()) {
                    cc = *c;
                } else {
                    emitUnknownName(std::format(
                        "unknown calling convention '{}' — accepted: {}", n.text,
                        detail::renderAllowedList(allNames(kCallConvTable))));
                }
            }
            return interner_.fnSig(params, ret, cc);
        }
        // D-TEXT-TIER-REFUSALS-NAME-NO-ACCEPTED-SET: this named nothing at all.
        emitMalformed(std::format("unknown type '{}' — accepted: {}", t.text,
                                  typeKeywordsAccepted()));
        return InvalidType;
    }

    // Parse `lit <variant-tag> <value> : <type-core>`.
    [[nodiscard]] MirLiteralValue parseLiteral() {
        MirLiteralValue lv;
        if (!expectIdent("lit")) return lv;
        Tok tag = lex_.take();
        if (tag.kind != TokKind::Ident) {
            emitMalformed("expected literal variant tag"); return lv;
        }
        if (tag.text == "bool") {
            // ⚠ FAIL LOUD. This read `lv.value = (v.text == "true");`, so EVERY
            // spelling other than `true` — `False`, `1`, `treu`, a truncated token,
            // anything — silently became `false` and the parse SUCCEEDED. The
            // `hir_text.cpp` twin already refused it by name; the two readers of one
            // format disagreed about what a boolean literal is
            // (D-MIR-TEXT-UNKNOWN-BOOL-SPELLING-SILENTLY-DEGRADED-TO-FALSE).
            Tok v = lex_.take();
            if (v.text == "true")       lv.value = true;
            else if (v.text == "false") lv.value = false;
            else emitMalformed(std::format(
                "expected 'true' or 'false' after a bool literal, got '{}'", v.text));
        } else if (tag.text == "int") {
            Tok v = lex_.take();
            lv.value = parseNumber<std::int64_t>(v.text, "int literal");
        } else if (tag.text == "uint") {
            Tok v = lex_.take();
            lv.value = parseNumber<std::uint64_t>(v.text, "uint literal");
        } else if (tag.text == "float") {
            Tok v = lex_.take();
            lv.value = parseDouble(v.text);
        } else if (tag.text == "str") {
            Tok v = lex_.take();
            lv.value = v.text;
        } else if (tag.text == "agg") {
            if (!expect(TokKind::LBrace)) return lv;
            MirAggregateValue agg;
            while (true) {
                Tok pk = lex_.peek();
                if (pk.kind == TokKind::RBrace) break;
                if (!agg.fields.empty()) (void)expect(TokKind::Comma);
                agg.fields.push_back(parseLiteral());
            }
            (void)expect(TokKind::RBrace);
            lv.value = std::move(agg);
        } else if (tag.text == "bitint") {
            // C4b: the inverse of `appendLiteral`'s `bitint` arm, and the SAME
            // spelling `hir_text.cpp`'s `parseLiteralValue` reads —
            // `bitint <width> <signed 0|1> <nLimbs> <limb…>`.
            std::uint64_t const width  = parseNumber<std::uint64_t>(lex_.take().text, "bitint width");
            std::uint64_t const sgn    = parseNumber<std::uint64_t>(lex_.take().text, "bitint signedness");
            std::uint64_t const nLimbs = parseNumber<std::uint64_t>(lex_.take().text, "bitint limb count");
            std::vector<std::uint64_t> limbs;
            limbs.reserve(static_cast<std::size_t>(nLimbs));
            for (std::uint64_t i = 0; i < nLimbs; ++i) {
                limbs.push_back(parseNumber<std::uint64_t>(lex_.take().text, "bitint limb"));
            }
            lv.value = BitIntValue(std::move(limbs),
                                   static_cast<std::uint32_t>(width), sgn != 0);
        } else if (tag.text == "wfloat") {
            // LD-3: `wfloat <bits> <hi> <lo>`; the BIT-WIDTH selects the unpack
            // layout, never a serialized TypeKind ordinal.
            std::uint64_t const bits = parseNumber<std::uint64_t>(lex_.take().text, "wfloat bit width");
            std::uint64_t const hi   = parseNumber<std::uint64_t>(lex_.take().text, "wfloat high word");
            std::uint64_t const lo   = parseNumber<std::uint64_t>(lex_.take().text, "wfloat low word");
            lv.value = WideFloatValue::fromPacked(
                lo, hi, (bits == 128) ? TypeKind::F128 : TypeKind::F80);
        } else if (tag.text == "monostate") {
            // monostate already default
        } else {
            emitMalformed(std::format("unknown literal tag '{}'", tag.text));
        }
        (void)expect(TokKind::Colon);
        // ⚠ THE LADDER HAD NO FINAL ARM. An unrecognized core spelling left
        // `lv.core` at `Void` and the parse SUCCEEDED — and the WRITER reaches this
        // exact state, emitting a bare `?` for any kind it has no spelling for, so
        // this compiler's own output read back with the literal's core silently
        // changed. Refused by name now, with the accepted set projected off BOTH
        // owning tables
        // (D-MIR-TEXT-UNKNOWN-LITERAL-CORE-SILENTLY-DEGRADED-TO-VOID).
        Tok coreT = lex_.take();
        if (auto k = literalCoreFromName(coreT.text); k.has_value()) {
            lv.core = *k;
        } else {
            emitUnknownName(std::format(
                "unknown literal core type '{}' — accepted: {}",
                coreT.text, literalCoreAccepted()));
        }
        return lv;
    }

    // NOTE the `.dssmir` text format does not carry the per-global FLAG CLASS
    // — binding/visibility, isConst, isThreadLocal (TLS C1), alignment — so a
    // text round-trip re-mints every global with the defaults (Global/Default,
    // mutable, process-shared, natural alignment). Precedent-consistent: the
    // format is a test/debug surface for CFG + literal shapes, never a
    // codegen input; a future flag-preserving text syntax would extend the
    // grammar here AND the printer symmetrically.
    void parseGlobal() {
        std::uint32_t const sym = parsePercentValue();
        if (!expect(TokKind::Colon)) return;
        TypeId const ty = parseType();
        if (!expect(TokKind::Eq)) return;
        // ★★★ A REFUSAL THAT CRASHES IS NOT A REFUSAL — the rule this file states
        // three times already, at the arm it had not reached.
        //
        // ✔MEASURED 2026-08-23 on `global %2 : bogus = zero`:
        //   `dss::MirBuilder fatal: addGlobal: type TypeId must be valid`
        // and exit 0xC0000409. `parseType` had ALREADY refused `bogus` by name and
        // set `errors_`, so `finalize()` would have discarded the module — but
        // `finalize()` never runs, because every arm below hands the invalid
        // `TypeId` straight to a builder that aborts on it. The guard protected
        // the last step of a walk that dies several steps earlier, which is
        // exactly what `resolveBlockRef`'s note above says about its own
        // predecessor (D-MIR-TEXT-INVALID-TYPE-REACHES-A-BUILDER-THAT-ABORTS).
        //
        // ⓘ No second diagnostic: `parseType` named the offending spelling and
        // this adds nothing an author does not already have. Returning here skips
        // the initializer, which is what a global with no type has anyway.
        if (!ty.valid()) return;
        Tok pk = lex_.peek();
        if (pk.kind == TokKind::Ident && pk.text == "zero") {
            lex_.take();
            builder_.addGlobal(ty, SymbolId{sym}, UINT32_MAX, MirFuncId{},
                               SymbolBinding::Global, SymbolVisibility::Default,
                               /*isConst=*/false, MirThreadStorage::Shared);
        } else if (pk.kind == TokKind::Ident && pk.text == "initfunc") {
            lex_.take();
            std::uint32_t const fnSlot = parsePercentValue();
            // Resolve via funcMap_ if available (function declared before
            // this global); else defer to finalize() — the function may
            // appear later in the text.
            auto it = funcMap_.find(fnSlot);
            if (it != funcMap_.end()) {
                builder_.addGlobal(ty, SymbolId{sym}, UINT32_MAX, it->second,
                                   SymbolBinding::Global,
                                   SymbolVisibility::Default,
                                   /*isConst=*/false, MirThreadStorage::Shared);
            } else {
                pendingInitFuncGlobals_.push_back({ty, SymbolId{sym}, fnSlot});
            }
        } else {
            MirLiteralValue lv = parseLiteral();
            std::uint32_t const litIdx = builder_.literalPoolAdd(std::move(lv));
            builder_.addGlobal(ty, SymbolId{sym}, litIdx, MirFuncId{},
                               SymbolBinding::Global, SymbolVisibility::Default,
                               /*isConst=*/false, MirThreadStorage::Shared);
        }
    }

    void parseFunction() {
        std::uint32_t const sym = parsePercentValue();
        if (!expect(TokKind::Colon)) return;
        TypeId const sig = parseType();
        // TF-C78 (D-CSUBSET-NOINLINE): the optional `[...]` function-attribute
        // list `appendFuncAttrs` emits. Unambiguous after the signature: types
        // bracket with `<>`, never `[]`. Absent ⇒ the (Global, Default, not-
        // noinline) defaults, which is what an un-annotated function prints as.
        // An UNRECOGNIZED name FAILS LOUD rather than being skipped — a silently
        // ignored attribute here is precisely how `binding`/`visibility` went
        // missing for as long as they did.
        SymbolBinding    binding    = SymbolBinding::Global;
        SymbolVisibility visibility = SymbolVisibility::Default;
        bool             noInline   = false;
        bool             alwaysInline = false;   // TF-C81
        bool             noOptimize   = false;   // TF-C85
        bool             noSanitizeThread = false;   // TF-C92
        if (lex_.peek().kind == TokKind::LBracket) {
            lex_.take();
            while (true) {
                Tok a = lex_.take();
                if (a.kind == TokKind::RBracket) break;
                if (a.kind == TokKind::Comma) continue;
                if (a.kind != TokKind::Ident) {
                    emitMalformed(std::format(
                        "expected a function attribute name, got '{}'", a.text));
                    break;
                }
                if (a.text == kMirTextNoInlineAttr) { noInline = true; continue; }
                if (a.text == kMirTextAlwaysInlineAttr) {   // TF-C81
                    alwaysInline = true;
                    continue;
                }
                if (a.text == kMirTextNoOptimizeAttr) {   // TF-C85
                    noOptimize = true;
                    continue;
                }
                if (a.text == kMirTextNoSanitizeThreadAttr) {   // TF-C92
                    noSanitizeThread = true;
                    continue;
                }
                if (auto b = symbolBindingFromName(a.text); b.has_value()) {
                    binding = *b;
                    continue;
                }
                if (auto v = symbolVisibilityFromName(a.text); v.has_value()) {
                    visibility = *v;
                    continue;
                }
                // ⚠ THIS SENTENCE USED TO RETYPE BOTH CLOSED SETS —
                // `(local | global | weak)` and `(default | hidden | protected |
                // internal)` — as string literals beside checks that read
                // `symbolBindingFromName` / `symbolVisibilityFromName`. Two owners
                // of one fact, on the same surface and in the same cycle that
                // converted the marker and calling-convention arms below/above:
                // add a binding and the check takes it while the sentence keeps
                // calling it invalid
                // (D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET).
                // Both halves are projected off the tables the lookups use.
                emitMalformed(std::format(
                    "unknown function attribute '{}' — expected a binding ({}), "
                    "a visibility ({}), '{}', '{}', '{}' or '{}'",
                    a.text,
                    detail::renderAllowedList(allNames(kSymbolBindingTable), " | "),
                    detail::renderAllowedList(allNames(kSymbolVisibilityTable), " | "),
                    kMirTextNoInlineAttr, kMirTextAlwaysInlineAttr,
                    kMirTextNoOptimizeAttr, kMirTextNoSanitizeThreadAttr));
                break;
            }
        }
        // ★★★ THE SAME ABORT-INSTEAD-OF-REFUSAL AS `parseGlobal`, one construct
        // over. ✔MEASURED 2026-08-23 on `function %1 : bogus { … }`:
        //   `dss::MirBuilder fatal: addFunction: signature TypeId must be valid (FnSig)`
        // and exit 0xC0000409, from nothing more exotic than an unknown type name
        // (D-MIR-TEXT-INVALID-TYPE-REACHES-A-BUILDER-THAT-ABORTS).
        //
        // ★ RECOVERY IS A BALANCED-BRACE SKIP, not a bare `return`. The body is
        // still in the token stream, and leaving it there would feed `block %bN {`
        // lines to the MODULE loop, which would refuse each of them in turn —
        // burying the one diagnostic that matters under a cascade about a
        // construct that is perfectly well formed.
        if (!sig.valid()) { skipBracedBody(); return; }
        MirFuncId const f =
            builder_.addFunction(sig, SymbolId{sym}, binding, visibility, noInline,
                                 alwaysInline,    // TF-C81
                                 noOptimize,      // TF-C85
                                 noSanitizeThread);   // TF-C92
        // Text initfunc references use %f<MirFuncId.v>. Track in
        // parse order so deferred-resolution at finalize works even
        // when a global with `initfunc` precedes its target function.
        funcMap_[f.v] = f;
        // Bail on missing `{` — without the function body's opening
        // brace we have nothing to parse; continuing would cascade
        // every subsequent token as malformed.
        if (!expect(TokKind::LBrace)) return;
        // Two-pass: first scan all block headers (with their markers)
        // and create them in declaration order, so forward refs from
        // branch instructions resolve to blocks with the correct
        // marker. Then rewind via the Lexer's `setPos` and parse the
        // bodies.
        std::size_t const bodyStart = lex_.pos();
        scanBlockHeaders();
        lex_.setPos(bodyStart);
        while (true) {
            Tok pk = lex_.peek();
            if (pk.kind == TokKind::RBrace || pk.kind == TokKind::End) break;
            if (pk.kind != TokKind::Ident || pk.text != "block") {
                emitMalformed(std::format("expected 'block' inside function, got '{}'",
                    pk.text));
                lex_.take();
                continue;
            }
            parseBlock();
        }
        (void)expect(TokKind::RBrace);
    }

    // Consume a `{ … }` body without interpreting it, brace-balanced. The
    // recovery step for a construct whose HEADER was refused: the body is
    // well-formed text about a construct that no longer exists, and every token
    // of it would otherwise be re-offered to an outer loop that has no arm for it.
    void skipBracedBody() {
        if (lex_.peek().kind != TokKind::LBrace) return;
        int depth = 0;
        while (true) {
            Tok const t = lex_.take();
            if (t.kind == TokKind::End) return;
            if (t.kind == TokKind::LBrace) ++depth;
            else if (t.kind == TokKind::RBrace) { if (--depth == 0) return; }
        }
    }

    // First-pass scan: walk the function body tokens and CREATE every
    // block (with its declared marker) in declaration order. Doesn't
    // parse instruction bodies; just consumes balanced braces past
    // each block. Stops at the matching `}` for the enclosing function.
    void scanBlockHeaders() {
        int depth = 1;  // we're inside the function's `{`
        while (depth > 0) {
            Tok pk = lex_.peek();
            if (pk.kind == TokKind::End) break;
            if (pk.kind == TokKind::RBrace) { lex_.take(); --depth; continue; }
            if (pk.kind == TokKind::Ident && pk.text == "block" && depth == 1) {
                lex_.take();  // consume `block`
                std::uint32_t const slot = parsePercentValue();
                StructCfMarker marker = StructCfMarker::Linear;
                if (lex_.peek().kind == TokKind::LBracket) {
                    lex_.take();
                    Tok m = lex_.take();
                    // ⚠ FAIL LOUD, and this arm used to be SILENT. It read
                    // `if (auto mk = markerFromName(m.text); mk) marker = *mk;`
                    // with no `else`: an unrecognized marker spelling left
                    // `marker` at `Linear` and the parse SUCCEEDED, so a
                    // `.dssmir` carrying a typo'd — or simply NEWER — marker
                    // loaded clean with the block's structural role silently
                    // erased. That is the worst shape available here: the role
                    // is what the loop/if/switch consumers read, so the text
                    // round trip returned a module that DIFFERS from the one
                    // written and nothing said so. The refusal renders the
                    // accepted set off the same table the lookup uses, so it
                    // cannot go stale
                    // (D-MIR-TEXT-UNKNOWN-BLOCK-MARKER-SILENTLY-DEGRADED-TO-LINEAR).
                    if (auto mk = kMirTextMarkerTable.fromName(m.text);
                        mk.has_value()) {
                        marker = *mk;
                    } else {
                        emitUnknownName(std::format(
                            "unknown block marker '{}' — accepted: {}", m.text,
                            detail::renderAllowedList(
                                allNames(kMirTextMarkerTable))));
                    }
                    (void)expect(TokKind::RBracket);
                }
                // Create the block now with the correct marker. Body
                // tokens will be re-lexed in pass 2.
                if (blockMap_.find(slot) == blockMap_.end()) {
                    blockMap_[slot] = builder_.createBlock(marker);
                }
                // Skip through the block's `{ ... }` to the next
                // sibling block.
                if (lex_.peek().kind == TokKind::LBrace) {
                    lex_.take();
                    ++depth;  // entered block body
                }
                continue;
            }
            if (pk.kind == TokKind::LBrace) { lex_.take(); ++depth; continue; }
            lex_.take();  // skip any other token at this depth
        }
    }

    [[nodiscard]] MirBlockId ensureBlock(std::uint32_t slot,
                                         StructCfMarker marker) {
        auto it = blockMap_.find(slot);
        if (it != blockMap_.end()) return it->second;
        MirBlockId const b = builder_.createBlock(marker);
        blockMap_[slot] = b;
        return b;
    }

    [[nodiscard]] MirBlockId resolveBlockRef(std::uint32_t slot) {
        auto it = blockMap_.find(slot);
        if (it != blockMap_.end()) return it->second;
        // Reached only on MALFORMED input: a `br`/`condbr`/`switch` names a
        // block-slot that was not declared as a `block %bN` header.
        // `scanBlockHeaders` pre-creates every declared block, so a lookup miss
        // here means the reference is to a block that does not exist.
        //
        // ★★★ THE COMMENT THAT USED TO STAND HERE DELEGATED THE REFUSAL TO A PASS
        // THAT CANNOT RUN. It said the placeholder was made "so the builder doesn't
        // abort" and that "the verify-on-load pass will flag the orphan" — and both
        // halves are wrong in the same way. Verify-on-load runs AFTER
        // `MirBuilder::finish()`, and `closeFunction_` ABORTS THE PROCESS on any
        // block created but never filled, which is exactly what this placeholder
        // is. With no diagnostic emitted, `errors_` stayed false, `finalize()` did
        // NOT short-circuit, and the reader died in the builder instead of
        // returning a refusal. ✔MEASURED 2026-08-21 through the sibling switch arm,
        // whose identical shape produced `dss::MirBuilder fatal: block MirBlockId=1
        // has no terminator` and exit `0xc0000409`
        // (`D-MIR-TEXT-UNDECLARED-BRANCH-TARGET-DELEGATES-ITS-REFUSAL-TO-A-PASS-THAT-CANNOT-RUN`).
        //
        // ★ The placeholder still gets created — the parse is collect-all and the
        // caller needs a block id to hand the builder — but the diagnostic is what
        // makes it safe: `errors_` is now set, so `finalize()` discards the module
        // before `finish()` is ever called. Same discipline as the block-marker and
        // switch-default arms: refuse here, discard there, never abort.
        emitUnknownName(std::format(
            "branch target %b{} was never declared as a 'block %b{}' header in this "
            "function", slot, slot));
        MirBlockId const b = builder_.createBlock(StructCfMarker::Linear);
        blockMap_[slot] = b;
        return b;
    }

    void parseBlock() {
        (void)expectIdent("block");
        std::uint32_t const slot = parsePercentValue();
        // Consume optional marker brackets; the BLOCK was already
        // CREATED with this marker during scanBlockHeaders, so we
        // just need to advance past the syntactic marker here.
        if (lex_.peek().kind == TokKind::LBracket) {
            lex_.take();
            lex_.take();  // marker ident
            (void)expect(TokKind::RBracket);
        }
        auto it = blockMap_.find(slot);
        if (it == blockMap_.end()) {
            emitUnknownName(std::format("block %b{} not pre-declared", slot));
            return;
        }
        MirBlockId const b = it->second;
        // Bail BEFORE `beginBlock` — otherwise a missing `{` leaves
        // the builder with an Open-state block that finalize()'s
        // `errors_` short-circuit relies on to avoid `MirBuilder::
        // finish()`'s abort. Bailing first keeps the builder in a
        // clean state regardless of the finalize() path.
        if (!expect(TokKind::LBrace)) return;
        // ★★★ AND BAIL ON AN EARLIER ERROR TOO, BECAUSE finalize()'s GUARD IS IN
        // THE WRONG PLACE TO CATCH THIS ONE.
        //
        // ✔MEASURED 2026-08-19 (cycle P20) on the first `.dssir` text ever emitted
        // containing an `inlineasmgoto`: the process ABORTED with
        // `block MirBlockId=1 has no terminator`. Not in `finish()` — in
        // `beginBlock`, HERE, which refuses to open a block while the previous one
        // is unterminated.
        //
        // ⚠ THE REFUSAL SITE'S OWN COMMENT NAMES THE RULE IT WAS BREAKING —
        // *"a refusal that crashes is not a refusal"* — and it was right about the
        // principle and wrong about the coverage: `finalize()` short-circuits on
        // `errors_` and so never calls `finish()`, but an instruction refused
        // mid-block leaves that block unterminated and the NEXT `%bN {` never
        // reaches `finalize()` at all. The guard protected the last step of a walk
        // that dies two steps earlier.
        //
        // ★ Stopping here rather than sealing the block with a synthesized
        // `unreachable`: the module is already being discarded by `finalize()`, so
        // a synthetic terminator would exist only to satisfy a builder invariant
        // for a module nobody will read — and it would make a REFUSED parse and a
        // SUCCESSFUL one produce structurally similar builders, which is how the
        // discard path stops being obviously correct.
        if (errors_) return;
        builder_.beginBlock(b);
        while (true) {
            Tok pk = lex_.peek();
            if (pk.kind == TokKind::RBrace || pk.kind == TokKind::End) break;
            parseInstruction();
        }
        (void)expect(TokKind::RBrace);
    }

    [[nodiscard]] MirInstId resolveValue(std::uint32_t slot) {
        auto it = valueMap_.find(slot);
        if (it != valueMap_.end()) return it->second;
        emitUnknownName(std::format("unknown value handle '%v{}'", slot));
        return InvalidMirInst;
    }

    void parseInstruction() {
        // Two forms:
        //   `%vN = opcode : type (operands)`
        //   `terminator [operands]` (br/condbr/switch/return/unreachable)
        Tok first = lex_.peek();
        std::size_t const lineStart = first.off;
        std::uint32_t resultSlot = 0;
        if (first.kind == TokKind::Percent) {
            resultSlot = parsePercentValue();
            (void)expect(TokKind::Eq);
        }
        Tok mn = lex_.take();
        if (mn.kind != TokKind::Ident) {
            emitMalformed("expected opcode mnemonic"); return;
        }
        // Mnemonics may contain dots (e.g. icmp.eq). Re-glue.
        std::string mnemonic = mn.text;
        while (lex_.peek().kind == TokKind::Dot) {
            lex_.take();
            Tok part = lex_.take();
            mnemonic += '.';
            mnemonic += part.text;
        }
        auto opOpt = opcodeFromMnemonic(mnemonic);
        if (!opOpt.has_value()) {
            // D-TEXT-TIER-REFUSALS-NAME-NO-ACCEPTED-SET: the `.dssir` twin of
            // `hir_text.cpp`'s `parseOp` arm — the accepted set comes off the same
            // `opcodeInfo` walk the lookup just failed, so it cannot be narrower,
            // wider or staler than the check above it.
            emitMalformed(std::format("unknown opcode '{}' — accepted: {}",
                                      mnemonic, opcodeMnemonicsAccepted()));
            return;
        }
        MirOpcode const op = *opOpt;
        // Inline-asm P5: an asm block's payload indexes the module's
        // `MirAsmDescriptorPool`, and this text format has no syntax for the
        // descriptor — the template, the constraint list and the clobber list are
        // simply not in the `.dssir` grammar yet. Accepting the mnemonic would
        // build an instruction whose payload names a pool slot that does not
        // exist, and `MirBuilder::addInst` would ABORT the process on the way
        // there. Refuse it as a parse diagnostic instead: a refusal that crashes
        // is not a refusal (D-LIR-TEXT-PARSE-UNSEALED-BLOCK-ABORT's class).
        if (op == MirOpcode::InlineAsm || op == MirOpcode::InlineAsmGoto) {
            emitMalformed(std::format(
                "opcode '{}' cannot be read from MIR text: an inline-asm block "
                "carries a descriptor (template + constraints + clobbers) that "
                "this format does not yet spell", mnemonic));
            return;
        }
        TypeId resultType = InvalidType;
        if (lex_.peek().kind == TokKind::Colon) {
            lex_.take();
            resultType = parseType();
        }
        // Per-opcode operand parsing.
        switch (op) {
            case MirOpcode::Const: {
                if (!expect(TokKind::LParen)) return;
                MirLiteralValue lv = parseLiteral();
                (void)expect(TokKind::RParen);
                MirInstId const id = builder_.addConst(std::move(lv), resultType);
                if (resultSlot != 0) valueMap_[resultSlot] = id;
                break;
            }
            case MirOpcode::Arg: {
                if (!expect(TokKind::LParen)) return;
                Tok n = lex_.take();
                std::uint32_t const idx = parseNumber<std::uint32_t>(
                    n.text, "arg ordinal");
                // Optional `, position` (arg_payload.hpp); absent → position
                // defaults to the ordinal (the single-class golden form).
                std::uint32_t position = idx;
                if (lex_.peek().kind == TokKind::Comma) {
                    (void)lex_.take();
                    Tok p = lex_.take();
                    position = parseNumber<std::uint32_t>(p.text, "arg position");
                }
                (void)expect(TokKind::RParen);
                MirInstId const id = builder_.addArg(idx, resultType, position);
                if (resultSlot != 0) valueMap_[resultSlot] = id;
                break;
            }
            case MirOpcode::GlobalAddr: {
                if (!expect(TokKind::LParen)) return;
                std::uint32_t const sym = parsePercentValue();
                (void)expect(TokKind::RParen);
                MirInstId const id = builder_.addGlobalAddr(SymbolId{sym}, resultType);
                if (resultSlot != 0) valueMap_[resultSlot] = id;
                break;
            }
            case MirOpcode::IntrinsicCall: {
                // Emitter wrote: `(intrinsic <id>, %v1, %v2, ...)`.
                if (!expect(TokKind::LParen)) return;
                if (!expectIdent("intrinsic")) return;
                Tok idTok = lex_.take();
                std::uint32_t const intrinId = parseNumber<std::uint32_t>(
                    idTok.text, "intrinsic id");
                std::vector<MirInstId> operands;
                while (lex_.peek().kind == TokKind::Comma) {
                    lex_.take();
                    operands.push_back(resolveValue(parsePercentValue()));
                }
                (void)expect(TokKind::RParen);
                MirInstId const id = builder_.addInst(op, operands, resultType, intrinId);
                if (resultSlot != 0) valueMap_[resultSlot] = id;
                break;
            }
            case MirOpcode::Phi: {
                if (!expect(TokKind::LBracket)) return;
                std::vector<std::pair<std::uint32_t, std::uint32_t>> incs;
                while (true) {
                    Tok pk = lex_.peek();
                    if (pk.kind == TokKind::RBracket) break;
                    if (!incs.empty()) (void)expect(TokKind::Comma);
                    (void)expect(TokKind::LParen);
                    std::uint32_t const v = parsePercentValue();
                    (void)expect(TokKind::Comma);
                    std::uint32_t const p = parsePercentValue();
                    (void)expect(TokKind::RParen);
                    incs.emplace_back(v, p);
                }
                (void)expect(TokKind::RBracket);
                MirInstId const phi = builder_.addPhi(resultType);
                if (resultSlot != 0) valueMap_[resultSlot] = phi;
                pendingPhis_.push_back({phi, std::move(incs)});
                break;
            }
            case MirOpcode::Br: {
                std::uint32_t const target = parsePercentValue();
                MirBlockId const tBB = resolveBlockRef(target);
                builder_.addBr(tBB);
                break;
            }
            case MirOpcode::CondBr: {
                std::uint32_t const condSlot = parsePercentValue();
                std::uint32_t const t1 = parsePercentValue();
                std::uint32_t const t2 = parsePercentValue();
                MirInstId const cond = resolveValue(condSlot);
                MirBlockId const b1 = resolveBlockRef(t1);
                MirBlockId const b2 = resolveBlockRef(t2);
                builder_.addCondBr(cond, b1, b2);
                break;
            }
            case MirOpcode::Switch: {
                std::uint32_t const discSlot = parsePercentValue();
                MirInstId const disc = resolveValue(discSlot);
                if (!expect(TokKind::LBrace)) return;
                std::vector<std::pair<MirInstId, MirBlockId>> cases;
                MirBlockId defaultBB{};
                bool sawDefault = false;
                while (true) {
                    Tok pk = lex_.peek();
                    if (pk.kind == TokKind::RBrace) break;
                    if (!cases.empty() || sawDefault) (void)expect(TokKind::Comma);
                    Tok kw = lex_.take();
                    if (kw.kind == TokKind::Ident && kw.text == "case") {
                        std::uint32_t const caseVSlot = parsePercentValue();
                        (void)expect(TokKind::Arrow);
                        std::uint32_t const tgt = parsePercentValue();
                        MirInstId const cv = resolveValue(caseVSlot);
                        MirBlockId const tb = resolveBlockRef(tgt);
                        cases.emplace_back(cv, tb);
                    } else if (kw.kind == TokKind::Ident && kw.text == "default") {
                        (void)expect(TokKind::Arrow);
                        std::uint32_t const tgt = parsePercentValue();
                        defaultBB = resolveBlockRef(tgt);
                        sawDefault = true;
                    } else {
                        emitMalformed(std::format("expected 'case'/'default', got '{}'", kw.text));
                        lex_.take();
                    }
                }
                (void)expect(TokKind::RBrace);
                // ⚠ THE FALSE BRANCH USED TO BE AN UNNAMED DEFAULT — and the thing
                // it silently defaulted to was DROPPING A TERMINATOR. With no
                // `default -> %bN` arm this was a bare `if (sawDefault)` with no
                // `else`, so the `switch` instruction was never built, `errors_`
                // stayed false, and the block reached `finalize()` unterminated —
                // where `MirBuilder::finish()` ABORTS THE PROCESS. That is the class
                // this file already names two arms above: a refusal that crashes is
                // not a refusal (`D-LIR-TEXT-PARSE-UNSEALED-BLOCK-ABORT`). The
                // emitter always writes the default (it is `succs.back()`), so text
                // without one is hand-written or truncated — exactly the input a
                // reader owes a diagnostic
                // (D-MIR-TEXT-SWITCH-WITHOUT-DEFAULT-SILENTLY-DROPS-THE-TERMINATOR).
                if (!sawDefault) {
                    emitMalformed(std::format(
                        "switch on %v{} has no 'default -> %b<n>' arm; a MIR switch "
                        "terminator requires one (the emitter always writes it as "
                        "the last successor)", discSlot));
                    break;
                }
                builder_.addSwitch(disc, cases, defaultBB);
                break;
            }
            case MirOpcode::Return: {
                // FC7 C1c: parse EVERY return-piece operand (multi-piece struct
                // return). 0 → void `addReturn()`; N≥1 → `addReturnMulti` (which
                // covers the scalar 1-operand case identically to `addReturn(v)`).
                std::vector<MirInstId> vals;
                while (lex_.peek().kind == TokKind::Percent) {
                    std::uint32_t const v = parsePercentValue();
                    vals.push_back(resolveValue(v));
                }
                if (vals.empty()) {
                    builder_.addReturn();
                } else {
                    builder_.addReturnMulti(vals);
                }
                break;
            }
            case MirOpcode::Unreachable: {
                builder_.addUnreachable();
                break;
            }
            // ★★★ D-CSUBSET-COMPUTED-GOTO: `indirectbr %v<addr> { %b…, %b… }`.
            //
            // ✔MEASURED 2026-08-23: with no arm here this fell into `default:`,
            // which read ZERO operands and called `MirBuilder::addInst` — whose
            // FIRST guard refuses a TERMINATOR through the non-terminator entry
            // point. The process ABORTED with
            //   `dss::MirBuilder fatal: addInst: opcode 'indirectbr' is a
            //    terminator; use the terminator API (addBr/addCondBr/addSwitch/
            //    addReturn/addUnreachable)`
            // and exit 0xC0000409.
            //
            // ⚠ AND THE ORDER OF THE FIXES MATTERED, which is the part no arity
            // table could have told anyone. In text the WRITER produces, a
            // `blockaddress` always precedes the `indirectbr` that consumes it, so
            // the reader desynced on THAT instruction's unconsumed ` %bN` tail and
            // the recovery re-tokenized the `indirectbr` line into `%`, `v1`, `{`…
            // — the mnemonic was never looked up and the abort was unreachable
            // from writer output. Fixing `blockaddress` alone would have PROMOTED a
            // recoverable refusal into a process abort.
            case MirOpcode::IndirectBr: {
                std::uint32_t const addrSlot = parsePercentValue();
                MirInstId const addr = resolveValue(addrSlot);
                if (!expect(TokKind::LBrace)) return;
                std::vector<MirBlockId> targets;
                while (true) {
                    Tok pk = lex_.peek();
                    if (pk.kind == TokKind::RBrace || pk.kind == TokKind::End) break;
                    if (!targets.empty()) (void)expect(TokKind::Comma);
                    targets.push_back(resolveBlockRef(parsePercentValue()));
                }
                (void)expect(TokKind::RBrace);
                // `opcodeInfo(IndirectBr)` requires at least one successor, and
                // `addIndirectBr` is where that would be discovered — by an abort.
                // Refuse here instead: the same discipline the switch-default arm
                // above states, for the same reason.
                if (targets.empty()) {
                    emitMalformed(std::format(
                        "indirectbr on %v{} lists no target blocks; a computed goto "
                        "carries the full address-taken successor set, which is "
                        "never empty", addrSlot));
                    break;
                }
                if (errors_) break;
                builder_.addIndirectBr(addr, targets);
                break;
            }
            // D-CSUBSET-COMPUTED-GOTO: `%vN = blockaddress : <ptr> %b<target>`.
            // The payload IS the target block, so it goes through
            // `addBlockAddress` (and `resolveBlockRef`, which handles the forward
            // reference every computed goto has). The generic arm read no payload
            // at all and built a `blockaddress` naming block 0 — a DIFFERENT
            // block, silently — before choking on the leftover `%bN`.
            case MirOpcode::BlockAddress: {
                std::uint32_t const slot = parsePercentValue();
                MirBlockId const target = resolveBlockRef(slot);
                MirInstId const id = builder_.addBlockAddress(target, resultType);
                if (resultSlot != 0) valueMap_[resultSlot] = id;
                break;
            }
            // D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS:
            // `%vN = byvaluestackarg : <ptr> (%vK) size <bytes> exhaust <class>`.
            // Both payload fields were dropped by the generic arm (it looks only
            // for the literal keyword `payload`), and the words `size 24 exhaust
            // gpr` were then re-tokenized as the next instruction.
            case MirOpcode::ByValueStackArg: {
                std::vector<MirInstId> operands;
                if (lex_.peek().kind == TokKind::LParen) {
                    lex_.take();
                    while (true) {
                        Tok pk = lex_.peek();
                        if (pk.kind == TokKind::RParen || pk.kind == TokKind::End) break;
                        if (!operands.empty()) (void)expect(TokKind::Comma);
                        operands.push_back(resolveValue(parsePercentValue()));
                    }
                    (void)expect(TokKind::RParen);
                }
                if (!expectIdent("size")) return;
                Tok const szTok = lex_.take();
                std::uint32_t const bytes = parseNumber<std::uint32_t>(
                    szTok.text, "byvaluestackarg byte size");
                if (bytes > kByValueStackArgSizeMask) {
                    emitMalformed(std::format(
                        "byvaluestackarg size {} does not fit the {}-bit size field",
                        bytes, 32 - kByValueStackArgExhaustShift));
                }
                if (!expectIdent("exhaust")) return;
                Tok const clsTok = lex_.take();
                std::uint8_t const cls = orUnknownName(
                    kMirTextExhaustTable, clsTok.text, "byvaluestackarg exhaust class",
                    kByValueStackArgExhaustNone);
                if (errors_) break;
                MirInstId const id = builder_.addInst(
                    op, operands, resultType,
                    (bytes & kByValueStackArgSizeMask)
                        | (static_cast<std::uint32_t>(cls)
                           << kByValueStackArgExhaustShift));
                if (resultSlot != 0) valueMap_[resultSlot] = id;
                break;
            }
            case MirOpcode::SehTryBegin: {
                // c115 SEH: `seh_try_begin <region> %b<try> %b<filter>`.
                Tok rTok = lex_.take();
                std::uint32_t const region = parseNumber<std::uint32_t>(
                    rTok.text, "seh region id");
                std::uint32_t const t1 = parsePercentValue();
                std::uint32_t const t2 = parsePercentValue();
                builder_.addSehTryBegin(resolveBlockRef(t1), resolveBlockRef(t2),
                                        region);
                break;
            }
            case MirOpcode::SehFilterReturn: {
                // `seh_filter_return <region> %v<val> %b<handler>`.
                Tok rTok = lex_.take();
                std::uint32_t const region = parseNumber<std::uint32_t>(
                    rTok.text, "seh region id");
                std::uint32_t const vSlot = parsePercentValue();
                std::uint32_t const tgt   = parsePercentValue();
                builder_.addSehFilterReturn(resolveValue(vSlot),
                                            resolveBlockRef(tgt), region);
                break;
            }
            case MirOpcode::SehTryEnd: {
                // `seh_try_end <region>` — a payload-carrying 0-operand marker.
                Tok rTok = lex_.take();
                std::uint32_t const region = parseNumber<std::uint32_t>(
                    rTok.text, "seh region id");
                builder_.addInst(op, {}, InvalidType, region);
                break;
            }
            default: {
                // Generic: zero-or-more operands in parens.
                std::vector<MirInstId> operands;
                if (lex_.peek().kind == TokKind::LParen) {
                    lex_.take();
                    while (true) {
                        Tok pk = lex_.peek();
                        if (pk.kind == TokKind::RParen) break;
                        if (!operands.empty()) (void)expect(TokKind::Comma);
                        std::uint32_t const v = parsePercentValue();
                        operands.push_back(resolveValue(v));
                    }
                    (void)expect(TokKind::RParen);
                }
                std::uint32_t payload = 0;
                if (peekIdent("payload")) {
                    lex_.take();
                    Tok n = lex_.take();
                    payload = parseNumber<std::uint32_t>(n.text, "payload");
                }
                MirInstId const id = builder_.addInst(op, operands, resultType, payload);
                if (resultSlot != 0) valueMap_[resultSlot] = id;
                break;
            }
        }
        refuseUnconsumedOperandTail(mnemonic, lineStart);
    }

    // ★★★ THE CLASS GUARD FOR
    // D-MIR-TEXT-ROUND-TRIP-INCOMPLETE-FOR-OPERAND-CARRYING-FORMS.
    //
    // Three opcodes were fixed by giving each a reader arm, and three fixes leave
    // the CLASS open: the next opcode whose writer arm renders an operand or
    // payload tail its reader arm does not consume reopens it, with the same
    // signature — a SUCCESSFUL parse of a WRONG instruction, followed some tokens
    // later by a diagnostic about a line that was never the problem.
    //
    // This asks the one question that separates the two: after an instruction's
    // arm has run, is there anything left on its line? The emitter writes exactly
    // one instruction per line, so anything still there was rendered by a writer
    // arm and dropped by a reader arm. The refusal QUOTES the leftover, because
    // the diagnostic an author actually got before this existed named the NEXT
    // construct.
    //
    // ⓘ A closing `}` is excluded: it ends the block, the function or the module,
    // and hand-written text is allowed to put it on the instruction's line. Any
    // other same-line token is refused — which makes "one instruction per line"
    // a stated rule of the grammar rather than an accident of the emitter.
    void refuseUnconsumedOperandTail(std::string_view mnemonic, std::size_t lineStart) {
        Tok const nx = lex_.peek();
        if (nx.kind == TokKind::End || nx.kind == TokKind::RBrace) return;
        std::string_view const src = lex_.text();
        if (nx.off > src.size() || lineStart > nx.off) return;
        std::string_view const between = src.substr(lineStart, nx.off - lineStart);
        if (between.find('\n') != std::string_view::npos) return;
        // The leftover is whatever remains of the line from the next token on.
        std::string_view rest = src.substr(nx.off);
        if (auto const nl = rest.find('\n'); nl != std::string_view::npos) {
            rest = rest.substr(0, nl);
        }
        emitMalformed(std::format(
            "instruction '{}' left unparsed text on its line: '{}' — this reader "
            "consumed every operand and payload field it knows about and the "
            "writer rendered more, so the two directions of this format disagree "
            "about this opcode", mnemonic, rest));
    }

    [[nodiscard]] std::unique_ptr<MirParseResult> finalize() {
        // Resolve any pending `initfunc` globals whose target function
        // was declared after the global in the text.
        for (auto const& pg : pendingInitFuncGlobals_) {
            auto it = funcMap_.find(pg.initFuncSlot);
            if (it == funcMap_.end()) {
                emitUnknownName(std::format(
                    "global %{}'s initfunc %f{} references a function "
                    "that was never declared",
                    pg.sym.v, pg.initFuncSlot));
                continue;
            }
            builder_.addGlobal(pg.ty, pg.sym, UINT32_MAX, it->second,
                               SymbolBinding::Global, SymbolVisibility::Default,
                               /*isConst=*/false, MirThreadStorage::Shared);
        }
        // Resolve phi incomings now that all blocks + values are known.
        for (auto& pp : pendingPhis_) {
            for (auto const& [vSlot, pSlot] : pp.incomings) {
                MirInstId const v = resolveValue(vSlot);
                auto it = blockMap_.find(pSlot);
                MirBlockId const p = (it != blockMap_.end())
                    ? it->second
                    : ([&] { emitUnknownName(std::format("unknown block '%b{}'", pSlot));
                             return MirBlockId{}; })();
                if (!v.valid() || !p.valid()) continue;
                builder_.addPhiIncoming(pp.phi, MirPhiIncoming{v, p});
            }
        }
        std::size_t const errBefore = reporter_.errorCount();
        // `MirBuilder::finish()` aborts on contract violations (e.g.
        // a block was created but never `beginBlock`'d, or never
        // terminated). If parse errors have already occurred we
        // cannot trust the builder's invariants — return an empty
        // module rather than aborting the process, so the user sees
        // the parse diagnostics instead of a crash. Verify-on-load
        // is skipped because there's nothing meaningful to verify.
        if (errors_) {
            return std::make_unique<MirParseResult>(
                Mir{}, std::move(interner_), std::move(symbolNames_));
        }
        Mir module = std::move(builder_).finish();
        auto result = std::make_unique<MirParseResult>(
            std::move(module), std::move(interner_), std::move(symbolNames_));
        // Verify-on-load.
        MirVerifier verifier{result->mir, &result->interner};
        (void)verifier.verify(reporter_);
        result->ok = (reporter_.errorCount() == errBefore);
        return result;
    }
};

} // namespace

std::unique_ptr<MirParseResult> parseMir(std::string_view text,
                                         CompilationUnitId cuId,
                                         DiagnosticReporter& reporter) {
    Parser p{text, cuId, reporter};
    return p.run();
}

} // namespace dss
