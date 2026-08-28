#include "hir/hir_text.hpp"

#include "core/types/config_key_vocabulary.hpp"  // renderAllowedList (orMalformed projects the table)
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/enum_name_table.hpp"  // EnumNameTable / allNames — the ONE owner of each text spelling set
#include "core/types/parse_diagnostic.hpp"
#include "core/types/semantic_config.hpp"  // BuiltinLowering / kBuiltinLoweringTable — the `builtincall` payload's closed set
#include "core/types/source_span.hpp"
#include "core/types/target_schema.hpp"  // callConvName / kCallConvTable
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "hir/attributes/diagnostic_info.hpp"
#include "hir/attributes/ffi_metadata.hpp"
#include "hir/attributes/shader_intrinsic.hpp"
#include "hir/attributes/source_span.hpp"
#include "hir/attributes/transpile_hints.hpp"
#include "hir/hir_intrinsic_registry.hpp"
#include "hir/hir_kind_registry.hpp"
#include "hir/hir_op.hpp"
#include "hir/hir_op_registry.hpp"
#include "hir/hir_verifier.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// `.dsshir` text format (HR7) — see hir_text.hpp for the contract. Grammar at a
// glance (whitespace-insignificant; LF only; canonical formatting below):
//
//   file       := "dsshir" INT preamble module
//   preamble   := ext_kinds? ext_ops? intrinsics? symbols?   (non-empty sections only)
//   module     := "module" flags? STR "{" decl* "}"
//   decl       := function | global | type_decl | extern_function
//               | extern_global | import_group | ext_node | error
//   stmt       := block | if | while | do | for | switch
//               | break | continue | return | expr | var | assign
//               | unreachable | ext_node | error
//   expr       := lit | ref | call | intrinsic | binop | unop | cast | member
//               | index | swizzle | construct | ternary | logical_and
//               | logical_or | sizeof | alignof | addressof | deref | typeref
//               | ext_node | error
//   type       := bool|i8..|u8..|f16..|char|byte|void | ptr<T> | ref<T>
//               | nullable<T> | optional<T> | slice<T> | vec<T,N> | mat<T,R,C>
//               | arr<T,N> | tuple<T,...> | struct STR {T,...} | union STR {T,...}
//               | fn(T,...) -> T [cc NAME] | ext STR (T,...) [N,...] | invalid
//
// Symbols are positional handles (`%1`) bound to a name in the `symbols` preamble;
// the handle number IS the rebuilt SymbolId.v, so the body and preamble agree by
// construction. Types render structurally (CU-ephemeral TypeId.v never appears).
// Extension kinds/ops/intrinsics are referenced by name and re-registered from the
// preamble. Side-tables attach inline (`@loc(...)`, `@ffi(...)`, …) before a node;
// the lone cross-node reference (DiagnosticInfo.origin) is a pre-order node index.

namespace dss {

namespace {

// ── shared name tables (small fixed enums local to the grammar) ──────────────
//
// ★★★ D-TEXT-TIER-READERS-KEEP-HAND-WRITTEN-FROMNAME-IF-CHAINS. Every vocabulary
// below used to exist TWICE in this file: a `…Name(e)` switch for the writer and
// a `…FromName(s)` if-chain for the reader, spelling the same set in the same
// order. Two owners of one fact, and the failure they produce is not a wrong
// message — it is a BROKEN ROUND TRIP IN ONE DIRECTION. `.dsshir` is a
// write-then-read surface: rename a spelling in the writer's switch and the
// emitter starts producing text its own reader refuses; rename it in the reader
// and a hand-written `.dsshir` stops loading. Both halves compile, both halves
// look right beside each other, and no round-trip test that writes its own input
// can see either one, because such a test only ever feeds the reader names the
// writer just emitted.
//
// Each vocabulary is now ONE `EnumNameTable` and both directions project from
// it, so the two cannot disagree — the `lir_reg.hpp` /
// `D-LIR-REG-CLASS-SPELLINGS-HAD-A-SECOND-OWNER` shape.
//
// ★ WHY THE TABLES LIVE HERE AND NOT BESIDE THEIR ENUMS. These spellings are the
// `.dsshir` TEXT SURFACE, not a property of the enum: ✔MEASURED with
// `grep -rn '"tess_control"' src/` (and the same for `"guard_clause"`,
// `"substituted"`, `"strong"`) — every one resolves to this file only, twice
// each, which were exactly the two owners. Nothing in config declares them and
// no other tier reads them. `kCallConvTable` is the deliberate contrast and the
// reason the rule is not "always put the table beside the enum": a calling
// convention is spelled in `.target.json`, so the CONFIG owns that vocabulary
// and the table belongs in `target_schema.hpp` where the loader can reach it.
// Putting a serialization vocabulary into `hir/attributes/*.hpp` would push the
// text format's surface syntax into the semantic model that every HIR consumer
// includes, and would add an include edge for a set with one reader.
//
// ★ THE TWO COMPILE-TIME GUARDS EACH TABLE CARRIES, and what each one catches:
//   * `DSS_CHECK_KEY_VOCABULARY(allNames(kTable))` walks every row and refuses
//     an EMPTY or DUPLICATE spelling. A duplicate makes the second row
//     unreachable from `fromName`, so a value would write a name that reads back
//     as a DIFFERENT value — a silent round-trip miscompile, not a diagnostic.
//   * a `-Werror=switch` backstop over the enum in the `…Name` projection. A new
//     enumerator with no table row would otherwise take `EnumNameTable::name`'s
//     row-0 fall-back and be written out wearing the SENTINEL's spelling; the
//     switch makes it a BUILD failure instead of a wrong `.dsshir` file.
//
// CallConv name mapping previously hand-rolled here (and in
// mir_text.cpp) — duplication caught in the 2026-06-02 cross-
// codebase audit. Call sites now use `callConvName(cc)` /
// `callConvFromName(s)` directly from the single source of truth
// (`kCallConvTable` in target_schema.hpp alongside the other 5
// enum name tables). 2nd-order simplifier fold dropped the
// indirection wrappers per "no follow ups" — the wrappers' only
// remaining value was diff-minimalism, which the explicit call-
// site migration removes.

// The ONE spelling of the marker `appendLiteralValue` writes in place of a value
// this format cannot serialize, and `parseLiteralValue` refuses by name. It lives
// here rather than as a literal at each end for the reason the whole file has been
// converging on: a write-then-read surface with two copies of one spelling breaks
// in ONE direction, with both halves compiling and the suite green.
inline constexpr std::string_view kHirTextUnspelledAggregateTag = "unspelled_aggregate";

// ─────────────────────────────────────────────────────────────────────────────
// D-TEXT-TIER-REFUSALS-NAME-NO-ACCEPTED-SET — the keyword vocabularies
// ─────────────────────────────────────────────────────────────────────────────
//
// ★★★ THESE FIVE TABLES ARE MINTED FROM THE PARSE ARMS, and that is the reason
// this row could not be closed the way its sibling was.
// `D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET` is about a
// message that DRIFTED from a table it was supposed to project; here there was
// no table at all. Every one of these arms was already FAIL-LOUD — it refused an
// unknown spelling — and every one of them named NO ACCEPTED SET, so an author
// with a typo learned that their word was wrong and never what this reader would
// have taken.
//
// ⚠ THE TABLE IS THE DISPATCH KEY, NOT A LIST BESIDE IT. Each `if (kw == "…")`
// ladder became a `switch` over the table's enum, so `-Werror=switch` refuses a
// new row that nobody dispatched and the row is the ONLY place the spelling is
// written. A list beside a ladder would have been a second owner on day one —
// the exact shape cycle P23 measured three times, where a shipped message
// disagreed with the parse code it described.
//
// ⓘ `HirTextAttrKind`, `HirTextForClause`, `HirTextExprKw` and `HirTextStmtKw`
// are TEXT-SURFACE vocabularies with no pipeline verb behind them: they name
// productions of this grammar (an `@`-attribute kind, a `for` clause role, the
// keyword that opens a node line). Where a pipeline verb DOES exist the table is
// keyed on it directly — `kHirTextFlagTable` on `HirFlags`, the six attribute
// tables on `FfiLinkage`/`ShaderStage`/… — and `parseOp` projects `HirOpKind`
// through `opName`, minting nothing.

// The node-flag spellings, read by BOTH directions: `parseFlags` resolves a name
// through it and `flagsStr` renders through it. Before this, the writer's `add()`
// ladder and the reader's `if/else` ladder were two independent copies of one
// four-word vocabulary.
inline constexpr EnumNameTable<HirFlags, 4> kHirTextFlagTable{{{
    { HirFlags::HasError,     "err"    },
    { HirFlags::Synthetic,    "syn"    },
    { HirFlags::ShaderUsable, "shader" },
    { HirFlags::HostUsable,   "host"   },
}}};
DSS_CHECK_ENUM_NAME_TABLE(kHirTextFlagTable);
DSS_CHECK_KEY_VOCABULARY(allNames(kHirTextFlagTable));

// The `@`-attribute kinds. One row per side-table `applyAttrs` can populate.
enum class HirTextAttrKind : std::uint8_t { Loc, Ffi, Shader, Transpile, Diag };
inline constexpr EnumNameTable<HirTextAttrKind, 5> kHirTextAttrKindTable{{{
    { HirTextAttrKind::Loc,       "loc"       },
    { HirTextAttrKind::Ffi,       "ffi"       },
    { HirTextAttrKind::Shader,    "shader"    },
    { HirTextAttrKind::Transpile, "transpile" },
    { HirTextAttrKind::Diag,      "diag"      },
}}};
DSS_CHECK_ENUM_NAME_TABLE(kHirTextAttrKindTable);
DSS_CHECK_KEY_VOCABULARY(allNames(kHirTextAttrKindTable));

// The `for { … }` clause roles.
enum class HirTextForClause : std::uint8_t { Init, Cond, Update, Body };
inline constexpr EnumNameTable<HirTextForClause, 4> kHirTextForClauseTable{{{
    { HirTextForClause::Init,   "init"   },
    { HirTextForClause::Cond,   "cond"   },
    { HirTextForClause::Update, "update" },
    { HirTextForClause::Body,   "body"   },
}}};
DSS_CHECK_ENUM_NAME_TABLE(kHirTextForClauseTable);
DSS_CHECK_KEY_VOCABULARY(allNames(kHirTextForClauseTable));

// The keyword that OPENS an expression node line. One row per `parseExprInner`
// arm, and it is also what `parseNode` routes on.
//
// ★★★ THIS TABLE REPLACED A THIRD COPY OF THE SET, AND THE THIRD COPY WAS
// ALREADY WRONG. `parseExprInner`'s `if` ladder listed ten keywords, its
// `kTypedExprs` array listed thirteen more, and `isExprKeyword` — the ROUTER that
// decides whether a line is an expression at all — retyped TWENTY of the
// twenty-three. ✔MEASURED 2026-08-23: the three it omitted are `va_start`,
// `va_arg` and `va_end`, all three of which `emitNodeLine` writes via
// `typedCall`. So a `.dsshir` containing a variadic-access node routed to
// `parseStmtInner` and came back `unknown statement` — a write-only spelling of
// exactly the class D-MIR-TEXT-ROUND-TRIP-INCOMPLETE-FOR-OPERAND-CARRYING-FORMS
// names, produced by the retyped-set defect
// D-TEXT-TIER-REFUSALS-NAME-NO-ACCEPTED-SET names. One table, and the router,
// the dispatch and the refusal all read it.
//
// ⚠⚠ AND IT WAS STILL SHORT BY TWO AFTER THAT FIX. ✔MEASURED 2026-08-23:
// `emitExpr` wrote `builtincall` and `labeladdr`, and NEITHER string appeared
// anywhere else in this file — the writer produced text the reader refused, the
// same write-only shape the `va_*` repair had just closed for three siblings.
// Adding the two rows is the smaller half of the fix; the larger half is that
// the writer no longer spells anything itself (see `exprKwForKind`), so a
// spelling with no row here is now UNWRITABLE rather than merely untested.
enum class HirTextExprKw : std::uint8_t {
    Lit, Ref, Call, Intrinsic, BuiltinCall, BinOp, UnOp, Member, Swizzle,
    TypeRef, Seq, Cast, Index, Construct, Ternary, LogicalAnd, LogicalOr,
    SizeOf, AlignOf, AddressOf, Deref, LabelAddr, VaStart, VaArg, VaEnd,

    Count_  // keep last — counts the members; deliberately UNLISTED below
};
inline constexpr EnumNameTable<HirTextExprKw, 25> kHirTextExprKwTable{{{
    { HirTextExprKw::Lit,        "lit"         },
    { HirTextExprKw::Ref,        "ref"         },
    { HirTextExprKw::Call,       "call"        },
    { HirTextExprKw::Intrinsic,  "intrinsic"   },
    { HirTextExprKw::BuiltinCall,"builtincall" },
    { HirTextExprKw::BinOp,      "binop"       },
    { HirTextExprKw::UnOp,       "unop"        },
    { HirTextExprKw::Member,     "member"      },
    { HirTextExprKw::Swizzle,    "swizzle"     },
    { HirTextExprKw::TypeRef,    "typeref"     },
    { HirTextExprKw::Seq,        "seq"         },
    { HirTextExprKw::Cast,       "cast"        },
    { HirTextExprKw::Index,      "index"       },
    { HirTextExprKw::Construct,  "construct"   },
    { HirTextExprKw::Ternary,    "ternary"     },
    { HirTextExprKw::LogicalAnd, "logical_and" },
    { HirTextExprKw::LogicalOr,  "logical_or"  },
    { HirTextExprKw::SizeOf,     "sizeof"      },
    { HirTextExprKw::AlignOf,    "alignof"     },
    { HirTextExprKw::AddressOf,  "addressof"   },
    { HirTextExprKw::Deref,      "deref"       },
    { HirTextExprKw::LabelAddr,  "labeladdr"   },
    { HirTextExprKw::VaStart,    "va_start"    },
    { HirTextExprKw::VaArg,      "va_arg"      },
    { HirTextExprKw::VaEnd,      "va_end"      },
}}};
DSS_CHECK_ENUM_NAME_TABLE(kHirTextExprKwTable);
DSS_CHECK_KEY_VOCABULARY(allNames(kHirTextExprKwTable));

// ★★★ THE COMPLETENESS CHECK WELL-FORMEDNESS CANNOT GIVE, and it is the reason
// `Count_` exists on an enum that is otherwise a pure spelling set.
// `DSS_CHECK_ENUM_NAME_TABLE` proves the rows are non-empty and pairwise
// distinct; it cannot prove there are ENOUGH of them, because the row count is
// the hand-written template argument. An enumerator added without a row leaves
// `EnumNameTable::name()` on its row-0 fallback — so the writer would spell a
// brand-new node `lit`, the reader would faithfully read `lit`, and the round
// trip would be byte-identical for the WRONG NODE. The pair of asserts pins the
// two halves: the rows cover ordinals `[0, Count_)`, and there are exactly
// `Count_` of them.
// (The same argument `kHirKindTable` makes one file over, arrived at from the
// serialization side rather than the diagnostic side.)
static_assert(kHirTextExprKwTable.rows.size()
                  == static_cast<std::size_t>(HirTextExprKw::Count_),
              "kHirTextExprKwTable must carry exactly one row per keyword");
static_assert([] {
    for (std::size_t i = 0; i < static_cast<std::size_t>(HirTextExprKw::Count_); ++i)
        if (kHirTextExprKwTable.nameOrEmpty(static_cast<HirTextExprKw>(i)).empty())
            return false;
    return true;
}(), "every HirTextExprKw ordinal must have a row (Count_ stays unlisted)");

// The keyword that OPENS a statement / declaration node line — everything
// `parseNode` does NOT route to `parseExprInner`. Same treatment, same reason:
// the ladder's final `malformed` named no accepted set at all.
enum class HirTextStmtKw : std::uint8_t {
    Block, If, SehTry, While, Do, For, Switch, Case, Default, Label, Goto,
    Break, Continue, Return, Unreachable, InlineAsm, Assign, Expr, Var, Global,
    Param, Function, ExternFunction, ExternGlobal, ImportGroup, TypeDecl,
    ExtNode, Error,

    Count_  // keep last — counts the members; deliberately UNLISTED below
};
inline constexpr EnumNameTable<HirTextStmtKw, 28> kHirTextStmtKwTable{{{
    { HirTextStmtKw::Block,          "block"            },
    { HirTextStmtKw::If,             "if"               },
    { HirTextStmtKw::SehTry,         "seh_try"          },
    { HirTextStmtKw::While,          "while"            },
    { HirTextStmtKw::Do,             "do"               },
    { HirTextStmtKw::For,            "for"              },
    { HirTextStmtKw::Switch,         "switch"           },
    { HirTextStmtKw::Case,           "case"             },
    { HirTextStmtKw::Default,        "default"          },
    { HirTextStmtKw::Label,          "label"            },
    { HirTextStmtKw::Goto,           "goto"             },
    { HirTextStmtKw::Break,          "break"            },
    { HirTextStmtKw::Continue,       "continue"         },
    { HirTextStmtKw::Return,         "return"           },
    { HirTextStmtKw::Unreachable,    "unreachable"      },
    { HirTextStmtKw::InlineAsm,      "inline_asm"       },
    { HirTextStmtKw::Assign,         "assign"           },
    { HirTextStmtKw::Expr,           "expr"             },
    { HirTextStmtKw::Var,            "var"              },
    { HirTextStmtKw::Global,         "global"           },
    { HirTextStmtKw::Param,          "param"            },
    { HirTextStmtKw::Function,       "function"         },
    { HirTextStmtKw::ExternFunction, "extern_function"  },
    { HirTextStmtKw::ExternGlobal,   "extern_global"    },
    { HirTextStmtKw::ImportGroup,    "import_group"     },
    { HirTextStmtKw::TypeDecl,       "type_decl"        },
    { HirTextStmtKw::ExtNode,        "ext_node"         },
    { HirTextStmtKw::Error,          "error"            },
}}};
DSS_CHECK_ENUM_NAME_TABLE(kHirTextStmtKwTable);
DSS_CHECK_KEY_VOCABULARY(allNames(kHirTextStmtKwTable));
// The statement half of the same completeness argument — see the expression
// table's note for why the two asserts are not redundant with each other.
static_assert(kHirTextStmtKwTable.rows.size()
                  == static_cast<std::size_t>(HirTextStmtKw::Count_),
              "kHirTextStmtKwTable must carry exactly one row per keyword");
static_assert([] {
    for (std::size_t i = 0; i < static_cast<std::size_t>(HirTextStmtKw::Count_); ++i)
        if (kHirTextStmtKwTable.nameOrEmpty(static_cast<HirTextStmtKw>(i)).empty())
            return false;
    return true;
}(), "every HirTextStmtKw ordinal must have a row (Count_ stays unlisted)");

// ── D-TEXT-TIER-REFUSALS-NAME-NO-ACCEPTED-SET: the TYPE-keyword position ────
//
// The STRUCTURAL keywords `parseType` dispatches on, beside the primitive names
// `kHirTextPrimTable` already owns. `unknown type 'foo'` named neither.
//
// ⚠⚠ AND THIS ONE IS ON A SHIPPED PATH, which is why it is a SET and not a
// dispatch conversion. ✔MEASURED 2026-08-23: `parseTypeFromText` drives this
// production from 9 call sites — 7 in `ffi/shipped_lib_descriptor.cpp`, 2 in
// `analysis/semantic/semantic_analyzer.cpp` — decoding FFI-descriptor and
// builtin-signature type strings. The arms are structurally unlike each other,
// several share a body, `unsigned` is a PREFIX rather than a head keyword, and
// the caller-supplied `namedTypes_` bindings are a deliberate LAST resort after
// every keyword. Reshaping that ladder to buy `-Werror=switch` would trade a
// message-quality gain for a change in the shipped decoder's control flow.
//
// ★ WHAT REPLACES THE COMPILE-TIME PAIRING GUARD: a FEED-BACK PIN. Every
// spelling this list advertises is driven back through `parseTypeFromText` and
// must not come back as `unknown type`, so the list cannot advertise a keyword
// the ladder does not handle, and cannot go stale when one is added without it.
// ⚠ `opaque` and `packed` are NOT here, and the feed-back pin is what said so.
// The first draft of this list carried `opaque`; `EveryAdvertisedHirTypeKeywordIs// RecognizedByTheReader` reddened immediately, because `opaque` is a MODIFIER
// inside `struct "N" opaque`, not a head keyword — advertising it would have
// told an FFI-descriptor author to write a type this decoder refuses. That is the
// exact drift this row is about, caught inside one cycle of writing the list.
inline constexpr std::array<std::string_view, 21> kHirTextTypeKeywords{
    "invalid", "ptr", "ref", "nullable", "optional", "slice", "complex",
    "volatile", "atomic", "fnptr", "vec", "mat", "arr", "tuple", "struct",
    "union", "enum", "fn", "ext", "_BitInt", "unsigned",
};
DSS_CHECK_KEY_VOCABULARY(kHirTextTypeKeywords);


// ★★ `diagnosticCodeName`'s answer for an ordinal its switch has no arm for.
//
// That switch (`parse_diagnostic.cpp`) has NO `default:` and covers every
// enumerator, so `-Werror=switch` — on for this whole build — makes it grow an
// arm with EVERY new code. That is what turns this one string into an
// "is this ordinal allocated" predicate rather than a second list to maintain,
// and it is the same source `scripts/check-diagnostic-codes/check-diagnostic-codes.py`
// reads for the allocation gate: the enum itself
// (D-MIR-TEXT-DIAG-CODE-CAST-IS-UNVALIDATED).
inline constexpr std::string_view kUnallocatedDiagnosticCodeName = "Unknown";

// The `.dsshir` bare-keyword type spellings. A deliberate SUBSET of `TypeKind`:
// the aggregate / pointer / function kinds have their own bracketed syntax
// (`ptr<T>`, `arr<T,N>`, `fn(...) -> T`) and are never spelled as a keyword, so
// this table is NOT total over the enum and must not be read as if it were.
//
// ⚠ THEREFORE `nameOrEmpty`, NEVER `name`. `EnumNameTable::name` falls back to
// `rows[0].second`, which here is `"bool"` — so a `Struct` reaching the writer
// would be emitted as the keyword `bool` and read back as a bool. Empty is the
// answer the caller already tests for (`primName(k).empty()` is how the type
// writer decides to take the structural path), and it is the reason this table
// gets no `-Werror=switch` backstop: totality over `TypeKind` is not the
// property, agreement between the two directions is.
inline constexpr EnumNameTable<TypeKind, 20> kHirTextPrimTable{{{
    { TypeKind::Bool, "bool" },
    { TypeKind::I8,   "i8"   }, { TypeKind::I16,  "i16"  },
    { TypeKind::I32,  "i32"  }, { TypeKind::I64,  "i64"  },
    { TypeKind::I128, "i128" },
    { TypeKind::U8,   "u8"   }, { TypeKind::U16,  "u16"  },
    { TypeKind::U32,  "u32"  }, { TypeKind::U64,  "u64"  },
    { TypeKind::U128, "u128" },
    { TypeKind::F16,  "f16"  }, { TypeKind::F32,  "f32"  },
    { TypeKind::F64,  "f64"  }, { TypeKind::F80,  "f80"  },
    { TypeKind::F128, "f128" },
    { TypeKind::Char, "char" }, { TypeKind::Byte, "byte" },
    { TypeKind::Void, "void" },
    { TypeKind::NullptrT, "nullptr_t" },  // C23 (debug-dump only)
    // ⚠ `Complex` is deliberately ABSENT and the absence is load-bearing:
    // `complex` is a WRAP-1 keyword (`complex<f64>`), decoded further down
    // parseType, and `primFromName` is consulted FIRST. A row here would make
    // the bare keyword `complex` resolve to a prim and the `<elem>` operand
    // would never be read. The comment at that decode site records the same
    // fact from the other side.
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kHirTextPrimTable);
DSS_CHECK_KEY_VOCABULARY(allNames(kHirTextPrimTable));

[[nodiscard]] std::string_view primName(TypeKind k) noexcept {
    return kHirTextPrimTable.nameOrEmpty(k);
}
[[nodiscard]] std::optional<TypeKind> primFromName(std::string_view s) noexcept {
    return kHirTextPrimTable.fromName(s);
}

// The LITERAL-CORE position, which is a wider set than the type-keyword position.
//
// ★ THE TWIN OF `kMirTextLiteralCoreTable`, ROW FOR ROW AND SPELLING FOR
// SPELLING. A literal's `core` may be a STRUCTURAL kind — `hir_literal_pool.hpp`
// states the contract: `core` ∈ {Struct, Union, Array} is the aggregate arm — and
// `kHirTextPrimTable` deliberately omits those, because a row there would make
// the bare keyword `struct` resolve as a prim and swallow the `"Name" { … }` that
// the TYPE grammar expects after it. So this position needs its own table rather
// than rows added over there, exactly as the MIR tier concluded.
//
// ⚠ Reached ONLY from the aggregate literal's per-field core
// (D-HIR-TEXT-WRITER-DROPS-THE-AGGREGATE-LITERAL-ARM); a top-level literal's core
// is recomputed from its type annotation by `literalCoreFor` and never spelled.
inline constexpr EnumNameTable<TypeKind, 6> kHirTextLiteralCoreTable{{{
    { TypeKind::Struct, "struct" },
    { TypeKind::Union,  "union"  },
    { TypeKind::Array,  "array"  },
    { TypeKind::Ptr,    "ptr"    },
    { TypeKind::Ref,    "ref"    },
    { TypeKind::Enum,   "enum"   },
}}};
DSS_CHECK_ENUM_NAME_TABLE(kHirTextLiteralCoreTable);
DSS_CHECK_KEY_VOCABULARY(allNames(kHirTextLiteralCoreTable));

// ⚠ `nameOrEmpty` on BOTH tables, never `name`: neither lists an invalid
// sentinel, so the row-0 fall-back would render an unspelled kind as `bool` /
// `struct` — a wrong answer that reads as a legitimate declaration.
[[nodiscard]] std::string_view literalCoreName(TypeKind k) noexcept {
    std::string_view const p = kHirTextPrimTable.nameOrEmpty(k);
    return p.empty() ? kHirTextLiteralCoreTable.nameOrEmpty(k) : p;
}

[[nodiscard]] std::optional<TypeKind> literalCoreFromName(std::string_view s) noexcept {
    if (auto const k = kHirTextPrimTable.fromName(s); k.has_value()) return k;
    return kHirTextLiteralCoreTable.fromName(s);
}

// The accepted set for that position, projected off BOTH owning tables so the
// sentence cannot be narrower, wider or staler than the lookup above it.
[[nodiscard]] std::string literalCoreAccepted() {
    std::string out{detail::renderAllowedList(allNames(kHirTextPrimTable))};
    out += ", ";
    out += detail::renderAllowedList(allNames(kHirTextLiteralCoreTable));
    return out;
}

// The accepted set at a type position: the primitive names plus the structural
// keywords, projected off both owners.
[[nodiscard]] std::string typeKeywordsAccepted() {
    std::string out{detail::renderAllowedList(allNames(kHirTextPrimTable))};
    out += ", ";
    out += detail::renderAllowedList(kHirTextTypeKeywords);
    return out;
}

[[nodiscard]] std::optional<HirOpKind> coreOpFromName(std::string_view s) noexcept {
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(HirOpKind::Count_); ++i) {
        auto const op = static_cast<HirOpKind>(i);
        if (opName(op) == s) return op;
    }
    return std::nullopt;
}

// The accepted set for `parseOp`, projected off the SAME walk `coreOpFromName`
// performs — `opName` over `[0, HirOpKind::Count_)`. No table is minted here and
// none could be: `hir_op.hpp` already owns the spellings, and a copy of them at
// this tier would be the second owner this row exists to remove
// (D-TEXT-TIER-REFUSALS-NAME-NO-ACCEPTED-SET).
//
// ⓘ Built on demand rather than as a constant: it is reached only on a refusal,
// and materializing every core-operator spelling on every parse to serve a
// diagnostic that usually never fires is the wrong trade.
[[nodiscard]] std::string coreOpAccepted() {
    std::vector<std::string_view> names;
    names.reserve(static_cast<std::size_t>(HirOpKind::Count_));
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(HirOpKind::Count_); ++i) {
        names.push_back(opName(static_cast<HirOpKind>(i)));
    }
    return detail::renderAllowedList(names);
}

// The six ATTRIBUTE vocabularies. Each table lists its `None`/`Default`
// sentinel as ROW 0, which is what makes `EnumNameTable::name`'s row-0
// fall-back reproduce the exact string the hand-written switches returned for an
// out-of-range value — so `name()` is correct here, and `nameOrEmpty` (which the
// prim table above needs) would be a behaviour change.
inline constexpr EnumNameTable<FfiLinkage, 3> kHirTextFfiLinkageTable{{{
    { FfiLinkage::Strong, "strong" },
    { FfiLinkage::Weak,   "weak"   },
    { FfiLinkage::Common, "common" },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kHirTextFfiLinkageTable);
DSS_CHECK_KEY_VOCABULARY(allNames(kHirTextFfiLinkageTable));

[[nodiscard]] std::string_view ffiLinkageName(FfiLinkage l) noexcept {
    // The `-Werror=switch` backstop: it owns no spelling, and a new enumerator
    // with no table row fails the BUILD instead of being written as `strong`.
    switch (l) {
        case FfiLinkage::Strong: case FfiLinkage::Weak: case FfiLinkage::Common:
            break;
    }
    return kHirTextFfiLinkageTable.name(l);
}

inline constexpr EnumNameTable<FfiVisibility, 3> kHirTextFfiVisibilityTable{{{
    { FfiVisibility::Default,   "default"   },
    { FfiVisibility::Hidden,    "hidden"    },
    { FfiVisibility::Protected, "protected" },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kHirTextFfiVisibilityTable);
DSS_CHECK_KEY_VOCABULARY(allNames(kHirTextFfiVisibilityTable));

[[nodiscard]] std::string_view ffiVisName(FfiVisibility v) noexcept {
    switch (v) {
        case FfiVisibility::Default: case FfiVisibility::Hidden:
        case FfiVisibility::Protected:
            break;
    }
    return kHirTextFfiVisibilityTable.name(v);
}

inline constexpr EnumNameTable<ShaderStage, 7> kHirTextShaderStageTable{{{
    { ShaderStage::None,        "none"         },
    { ShaderStage::Vertex,      "vertex"       },
    { ShaderStage::Fragment,    "fragment"     },
    { ShaderStage::Compute,     "compute"      },
    { ShaderStage::Geometry,    "geometry"     },
    { ShaderStage::TessControl, "tess_control" },
    { ShaderStage::TessEval,    "tess_eval"    },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kHirTextShaderStageTable);
DSS_CHECK_KEY_VOCABULARY(allNames(kHirTextShaderStageTable));

[[nodiscard]] std::string_view stageName(ShaderStage s) noexcept {
    switch (s) {
        case ShaderStage::None: case ShaderStage::Vertex:
        case ShaderStage::Fragment: case ShaderStage::Compute:
        case ShaderStage::Geometry: case ShaderStage::TessControl:
        case ShaderStage::TessEval:
            break;
    }
    return kHirTextShaderStageTable.name(s);
}

inline constexpr EnumNameTable<ShaderBuiltin, 12> kHirTextShaderBuiltinTable{{{
    { ShaderBuiltin::None,               "none"                 },
    { ShaderBuiltin::Position,           "position"             },
    { ShaderBuiltin::PointSize,          "point_size"           },
    { ShaderBuiltin::VertexIndex,        "vertex_index"         },
    { ShaderBuiltin::InstanceIndex,      "instance_index"       },
    { ShaderBuiltin::FragCoord,          "frag_coord"           },
    { ShaderBuiltin::FragDepth,          "frag_depth"           },
    { ShaderBuiltin::FrontFacing,        "front_facing"         },
    { ShaderBuiltin::GlobalInvocationId, "global_invocation_id" },
    { ShaderBuiltin::LocalInvocationId,  "local_invocation_id"  },
    { ShaderBuiltin::WorkgroupId,        "workgroup_id"         },
    { ShaderBuiltin::NumWorkgroups,      "num_workgroups"       },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kHirTextShaderBuiltinTable);
DSS_CHECK_KEY_VOCABULARY(allNames(kHirTextShaderBuiltinTable));

[[nodiscard]] std::string_view builtinName(ShaderBuiltin b) noexcept {
    switch (b) {
        case ShaderBuiltin::None: case ShaderBuiltin::Position:
        case ShaderBuiltin::PointSize: case ShaderBuiltin::VertexIndex:
        case ShaderBuiltin::InstanceIndex: case ShaderBuiltin::FragCoord:
        case ShaderBuiltin::FragDepth: case ShaderBuiltin::FrontFacing:
        case ShaderBuiltin::GlobalInvocationId:
        case ShaderBuiltin::LocalInvocationId: case ShaderBuiltin::WorkgroupId:
        case ShaderBuiltin::NumWorkgroups:
            break;
    }
    return kHirTextShaderBuiltinTable.name(b);
}

inline constexpr EnumNameTable<TranspileIdiom, 6> kHirTextTranspileIdiomTable{{{
    { TranspileIdiom::Default,     "default"      },
    { TranspileIdiom::EarlyReturn, "early_return" },
    { TranspileIdiom::GuardClause, "guard_clause" },
    { TranspileIdiom::TernaryExpr, "ternary_expr" },
    { TranspileIdiom::RangeFor,    "range_for"    },
    { TranspileIdiom::WhileLoop,   "while_loop"   },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kHirTextTranspileIdiomTable);
DSS_CHECK_KEY_VOCABULARY(allNames(kHirTextTranspileIdiomTable));

[[nodiscard]] std::string_view idiomName(TranspileIdiom i) noexcept {
    switch (i) {
        case TranspileIdiom::Default: case TranspileIdiom::EarlyReturn:
        case TranspileIdiom::GuardClause: case TranspileIdiom::TernaryExpr:
        case TranspileIdiom::RangeFor: case TranspileIdiom::WhileLoop:
            break;
    }
    return kHirTextTranspileIdiomTable.name(i);
}

inline constexpr EnumNameTable<HirRecovery, 4> kHirTextRecoveryTable{{{
    { HirRecovery::None,        "none"        },
    { HirRecovery::Substituted, "substituted" },
    { HirRecovery::Dropped,     "dropped"     },
    { HirRecovery::Synthesized, "synthesized" },
}}};

// Well-formedness of the table itself: no empty spelling, no duplicate
// spelling, no duplicate ENUMERATOR. An under-filled table is legal C++ and
// would make "" a resolving spelling; see D-CORE-ENUM-NAME-TABLE-HAS-NO-WELL-FORMEDNESS-PREDICATE.
DSS_CHECK_ENUM_NAME_TABLE(kHirTextRecoveryTable);
DSS_CHECK_KEY_VOCABULARY(allNames(kHirTextRecoveryTable));

[[nodiscard]] std::string_view recoveryName(HirRecovery r) noexcept {
    switch (r) {
        case HirRecovery::None: case HirRecovery::Substituted:
        case HirRecovery::Dropped: case HirRecovery::Synthesized:
            break;
    }
    return kHirTextRecoveryTable.name(r);
}

// ── D-HIR-TEXT-WRITER-SPELLS-KEYWORDS-THE-READER-HAS-NO-ROW-FOR ─────────────
//
// ★★★ THE ONE BINDING BETWEEN A CORE `HirKind` AND THE `.dsshir` KEYWORD THAT
// SPELLS IT, AND EVERY DIRECTION READS IT. The writer projects a kind to its
// spelling through this (it never writes a keyword string of its own any more),
// the ROUTER below is DEFINED as "this kind has a spelling here", and the reader
// dispatches on the same `HirTextExprKw` through a `default:`-less switch. So
// the loop closes at COMPILE TIME in all three places:
//   a spelling ⇒ an enumerator ⇒ a table row ⇒ a reader arm.
//
// ⚠⚠ THAT LOOP WAS OPEN, AND IT HAD LEAKED THREE TIMES IN ONE FILE.
// ✔MEASURED 2026-08-23: the writer emitted `builtincall` and `labeladdr`, which
// appeared NOWHERE else — `emitHir` produced text `parseHir` refused by name.
// A sibling repair the same day had closed the identical hole for `va_start` /
// `va_arg` / `va_end` by adding table rows, which fixed those three spellings
// and left the MECHANISM that produced them untouched: the writer still spelled
// its own keywords, so the next node kind could re-open it. It re-opened
// immediately in a THIRD place — the router below listed neither `LabelAddressOf`
// NOR the three `va_*` kinds it had just taught the reader, so a variadic node
// in statement position was routed to the statement writer and degraded to
// `error` with a diagnostic that blamed the node rather than the router.
//
// ★ NO `default:`, DELIBERATELY. `-Werror=switch` (on for this whole build) is
// what makes a NEW `HirKind` stop the build here until its author says whether
// it is an expression and, if so, which keyword spells it. That is the pairing
// the `default:`-carrying writer switches could not have: an unhandled kind
// rendered `error` at RUN time, which is a silently smaller program.
[[nodiscard]] constexpr std::optional<HirTextExprKw>
exprKwForKind(HirKind k) noexcept {
    switch (k) {
        case HirKind::Literal:            return HirTextExprKw::Lit;
        case HirKind::Ref:                return HirTextExprKw::Ref;
        case HirKind::Call:               return HirTextExprKw::Call;
        case HirKind::IntrinsicCall:      return HirTextExprKw::Intrinsic;
        case HirKind::BuiltinCall:        return HirTextExprKw::BuiltinCall;
        case HirKind::BinaryOp:           return HirTextExprKw::BinOp;
        case HirKind::UnaryOp:            return HirTextExprKw::UnOp;
        case HirKind::MemberAccess:       return HirTextExprKw::Member;
        case HirKind::Swizzle:            return HirTextExprKw::Swizzle;
        case HirKind::TypeRef:            return HirTextExprKw::TypeRef;
        case HirKind::SeqExpr:            return HirTextExprKw::Seq;
        case HirKind::Cast:               return HirTextExprKw::Cast;
        case HirKind::Index:              return HirTextExprKw::Index;
        case HirKind::ConstructAggregate: return HirTextExprKw::Construct;
        case HirKind::Ternary:            return HirTextExprKw::Ternary;
        case HirKind::LogicalAnd:         return HirTextExprKw::LogicalAnd;
        case HirKind::LogicalOr:          return HirTextExprKw::LogicalOr;
        case HirKind::SizeOf:             return HirTextExprKw::SizeOf;
        case HirKind::AlignOf:            return HirTextExprKw::AlignOf;
        case HirKind::AddressOf:          return HirTextExprKw::AddressOf;
        case HirKind::Deref:              return HirTextExprKw::Deref;
        case HirKind::LabelAddressOf:     return HirTextExprKw::LabelAddr;
        case HirKind::VaStart:            return HirTextExprKw::VaStart;
        case HirKind::VaArg:              return HirTextExprKw::VaArg;
        case HirKind::VaEnd:              return HirTextExprKw::VaEnd;

        // ── everything that is NOT written in expression position ──
        // `Error` and `Extension` are the deliberate subtlety: they DO render
        // inline inside an expression, but their keywords (`error` / `ext_node`)
        // are STATEMENT keywords with an inline form, owned by the statement
        // table. Pairing them here would advertise two owners for one spelling.
        case HirKind::Module:      case HirKind::Function:
        case HirKind::Global:      case HirKind::TypeDecl:
        case HirKind::ExternFunction: case HirKind::ExternGlobal:
        case HirKind::ImportGroup: case HirKind::Block:
        case HirKind::IfStmt:      case HirKind::WhileStmt:
        case HirKind::DoWhileStmt: case HirKind::ForStmt:
        case HirKind::SwitchStmt:  case HirKind::CaseArm:
        case HirKind::BreakStmt:   case HirKind::ContinueStmt:
        case HirKind::ReturnStmt:  case HirKind::ExprStmt:
        case HirKind::VarDecl:     case HirKind::AssignStmt:
        case HirKind::GotoStmt:    case HirKind::LabelStmt:
        case HirKind::IndirectGotoStmt: case HirKind::SehTryExcept:
        case HirKind::InlineAsm:   case HirKind::Unreachable:
        case HirKind::Error:       case HirKind::Extension:
        case HirKind::Count_:
            return std::nullopt;
    }
    return std::nullopt;
}

// The writer's router, and no longer a hand-retyped second copy of the set
// above: a kind is written in expression position EXACTLY WHEN this file can
// spell it as one. The two cannot disagree because there is only one list.
[[nodiscard]] bool isExprKind(HirKind k) noexcept {
    return exprKwForKind(k).has_value();
}

// The statement half of the same discipline. There is deliberately NO
// `HirKind`→keyword function on this side: two kinds spell two keywords each —
// a `VarDecl` is `var` or `param` by POSITION, a `CaseArm` is `case` or
// `default` by CONTENT — so a kind-keyed map would have to pick one and be
// wrong at the other site. What this projection still buys is the property the
// finding was about: every statement keyword the writer emits is a ROW of the
// table the reader dispatches on, so a write-only spelling cannot be typed here
// either.
[[nodiscard]] constexpr std::string_view stmtKw(HirTextStmtKw k) noexcept {
    return kHirTextStmtKwTable.name(k);
}

// Does this node carry a symbol id in its payload? (Used by the symbol pre-pass.)
[[nodiscard]] bool carriesSymbol(HirKind k) noexcept {
    switch (k) {
        case HirKind::Ref: case HirKind::VarDecl: case HirKind::Function:
        case HirKind::Global: case HirKind::TypeDecl: case HirKind::ExternFunction:
        case HirKind::ExternGlobal:
            return true;
        default: return false;
    }
}

[[nodiscard]] std::string quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

// ── Emitter ──────────────────────────────────────────────────────────────────

class Emitter {
public:
    Emitter(Hir const& hir, HirTextContext const& ctx, DiagnosticReporter& reporter)
        : hir_(hir), ctx_(ctx), reporter_(reporter) {}

    [[nodiscard]] std::string run() {
        if (!hir_.root().valid()) { out_ += "dsshir 1\n"; return std::move(out_); }
        prepass(hir_.root());

        out_ += "dsshir 1\n";
        emitExtKinds();
        emitExtOps();
        emitIntrinsics();
        emitSymbols();

        HirNodeId const root = hir_.root();
        if (hir_.kind(root) != HirKind::Module) {
            // The grammar's top level is a module; a non-Module root can't be
            // spelled. Fail loud-but-recoverable (a diagnostic, not an abort).
            report("root node is not a Module — cannot serialize", DiagnosticSeverity::Error);
            out_ += "module \"\" {\n}\n";
            return std::move(out_);
        }
        out_ += "module";
        out_ += flagsStr(hir_.flags(root));
        out_ += ' ';
        out_ += quote(hir_.sourceLanguage());
        out_ += " {\n";
        for (HirNodeId d : hir_.moduleDecls(root)) emitNodeLine(d, 1);
        out_ += "}\n";
        return std::move(out_);
    }

private:
    Hir const&            hir_;
    HirTextContext const& ctx_;
    DiagnosticReporter&   reporter_;
    std::string           out_;

    std::unordered_map<std::uint32_t, std::uint32_t> preIndex_;   // HirNodeId.v -> pre-order index
    std::unordered_map<std::uint32_t, std::uint32_t> symHandle_;  // SymbolId.v  -> handle (1..N)
    std::vector<std::uint32_t>                       symOrder_;   // SymbolId.v in first-encounter order
    bool internerWarned_ = false;

    // Emitter-side diagnostic. Defaults to Error: the cases that reach here
    // (non-Module root, an Extension/IntrinsicCall payload the registry doesn't
    // resolve, an unprintable type) emit a `?`/`error` token that CANNOT be
    // re-parsed — the output is not round-trippable, so `reporter.hasErrors()`
    // must report it. The lone documented exception (a deliberately-absent
    // interner, the `?`-types degraded mode) passes Warning explicitly.
    // ★★ THE SEVERITY IS A REQUIRED ARGUMENT. It defaulted to Error here while
    // the sibling `mir_text.cpp` helper defaulted to Warning, so the SAME class of
    // writer-side drop was loud in one text tier and a warning in the other, on
    // two files that describe themselves as twins
    // (D-MIR-TEXT-ROUND-TRIP-INCOMPLETE-FOR-OPERAND-CARRYING-FORMS).
    //
    // ⓘ Deleting the default rather than picking one: both severities are right
    // for different sites. A value this format cannot render is an Error (the text
    // will not read back); a lossy dump the CALLER asked for by supplying no
    // interner is a Warning, said once. A default makes that choice by omission,
    // which is how the two tiers came to disagree without either one deciding.
    void report(std::string detail, DiagnosticSeverity sev) {
        ParseDiagnostic d;
        d.code = DiagnosticCode::H_TextMalformed;
        d.severity = sev;
        d.actual = std::move(detail);
        reporter_.report(std::move(d));
    }

    // Single children-order pre-order DFS: assigns each node a stable pre-order
    // index (for DiagnosticInfo.origin references) and collects referenced
    // SymbolIds in first-encounter order (their handles). Must visit children in
    // the same order `emitNodeLine` does (children() order) so indices align.
    void prepass(HirNodeId id) {
        std::uint32_t const idx = static_cast<std::uint32_t>(preIndex_.size());
        preIndex_.emplace(id.v, idx);
        if (carriesSymbol(hir_.kind(id))) {
            std::uint32_t const sv = hir_.payload(id);
            if (!symHandle_.contains(sv)) {
                symOrder_.push_back(sv);
                symHandle_.emplace(sv, static_cast<std::uint32_t>(symOrder_.size()));
            }
        }
        for (HirNodeId c : hir_.children(id)) prepass(c);
    }

    [[nodiscard]] std::string indent(int n) const { return std::string(static_cast<std::size_t>(n) * 2, ' '); }

    // ⓘ RENDERS THROUGH `kHirTextFlagTable`, the same rows `parseFlags` resolves
    // against. This was a four-line `add("err")` ladder and its reader was a
    // four-line `if (n == "err")` ladder — two independent copies of one
    // vocabulary, which is the shape this file has been converting throughout
    // (D-TEXT-TIER-REFUSALS-NAME-NO-ACCEPTED-SET). The declaration ORDER of the
    // table is the emission order, so existing text is byte-unchanged.
    [[nodiscard]] std::string flagsStr(HirFlags f) const {
        if (!any(f)) return {};
        std::string out = " [";
        bool first = true;
        for (auto const& [bit, name] : kHirTextFlagTable.rows) {
            if (!has(f, bit)) continue;
            if (!first) out += ',';
            out += name;
            first = false;
        }
        out += ']';
        return out;
    }

    [[nodiscard]] std::uint32_t handleOf(std::uint32_t symv) const {
        auto it = symHandle_.find(symv);
        return it == symHandle_.end() ? 0u : it->second;
    }

    // ── type printer (structural; nominal types by interned name) ────────────
    void appendType(TypeId t) {
        if (!t.valid()) { out_ += "invalid"; return; }
        if (ctx_.interner == nullptr) {
            if (!internerWarned_) {
                report("no TypeInterner supplied; types render as '?', and text containing "
                       "'?' is refused on the way back in",
                       DiagnosticSeverity::Warning);
                internerWarned_ = true;
            }
            out_ += '?';
            return;
        }
        TypeInterner const& in = *ctx_.interner;
        // D-CSUBSET-QUAL-BITSET (M1): a qualifier skin (`volatile` / `_Atomic`) is
        // TRANSPARENT to in.kind()/operands()/scalars(), so it must be spelled from
        // the RAW `isVolatileQualified`/`isAtomicQualified` predicates BEFORE the kind
        // switch — otherwise the switch sees THROUGH to the material kind and the
        // qualifier silently DROPS in text (a shipped `_Atomic int` typedef would
        // reintern as plain `int` — the exact loss-of-atomicity this codec closes).
        // Emit nested keyword wrappers over the material type; `parseType` merges the
        // bits back into ONE skin on reintern (`atomic<volatile<T>>` → bits{V,A}), so
        // the round-trip is identity. `atomic` outermost is the canonical order (the
        // bitset is order-independent — this only fixes a deterministic spelling).
        if (in.isVolatileQualified(t) || in.isAtomicQualified(t)) {
            bool const atom = in.isAtomicQualified(t);
            bool const vol  = in.isVolatileQualified(t);
            if (atom) out_ += "atomic<";
            if (vol)  out_ += "volatile<";
            appendType(in.stripVolatile(t));   // the material type (skin stripped)
            if (vol)  out_ += '>';
            if (atom) out_ += '>';
            return;
        }
        auto args = [&](std::span<TypeId const> ops) {
            bool first = true;
            for (TypeId o : ops) { if (!first) out_ += ", "; appendType(o); first = false; }
        };
        switch (in.kind(t)) {
            case TypeKind::Ptr:      out_ += "ptr<";      appendType(in.operands(t)[0]); out_ += '>'; return;
            case TypeKind::Ref:      out_ += "ref<";      appendType(in.operands(t)[0]); out_ += '>'; return;
            case TypeKind::Nullable: out_ += "nullable<"; appendType(in.operands(t)[0]); out_ += '>'; return;
            case TypeKind::Optional: out_ += "optional<"; appendType(in.operands(t)[0]); out_ += '>'; return;
            case TypeKind::Slice:    out_ += "slice<";    appendType(in.operands(t)[0]); out_ += '>'; return;
            case TypeKind::FnPtr:    out_ += "fnptr<";    appendType(in.operands(t)[0]); out_ += '>'; return;
            case TypeKind::Vector:
                out_ += "vec<"; appendType(in.operands(t)[0]);
                out_ += std::format(", {}>", in.scalars(t)[0]); return;
            case TypeKind::Matrix:
                out_ += "mat<"; appendType(in.operands(t)[0]);
                out_ += std::format(", {}, {}>", in.scalars(t)[0], in.scalars(t)[1]); return;
            // C99 _Complex (D-CSUBSET-COMPLEX, M1): `complex<elem>` — the bidirectional
            // twin of parseType's `complex` keyword. So a `double complex` typedef /
            // the `__builtin_complex` signature spells a genuine Complex type through
            // the shipped-lib text codec.
            case TypeKind::Complex:
                out_ += "complex<"; appendType(in.operands(t)[0]); out_ += '>'; return;
            case TypeKind::Array:
                out_ += "arr<"; appendType(in.operands(t)[0]);
                out_ += std::format(", {}>", in.scalars(t)[0]); return;
            case TypeKind::Tuple:  out_ += "tuple<"; args(in.operands(t)); out_ += '>'; return;
            case TypeKind::Struct: {
                out_ += "struct "; out_ += quote(in.name(t));
                // D-FFI-OPAQUE-TAG-HAS-NO-SPELLING: an INCOMPLETE composite has no
                // field list at all, and it must not be spelled `{}` -- that is a
                // LEGAL COMPLETE zero-field struct (see `isIncompleteComposite`), so
                // emitting braces here would reintern an opaque tag as a COMPLETE
                // zero-byte type. Exactly the silent-drop shape the ` packed` marker
                // below exists to prevent, and the marker follows its precedent: a
                // bare keyword after the name. No braces follow `opaque`.
                if (in.isIncompleteComposite(t)) { out_ += " opaque"; return; }
                // D-CSUBSET-PACKED: emit a ` packed` marker for a packed struct so the
                // whole-composite packed flag round-trips (else it would reintern
                // UNPACKED — a silent ABI drop across the text boundary). May combine
                // with the `~<align>` per-field markers (a packed struct with an
                // alignas member); never with `@<off>` (packed excludes explicit offsets).
                if (in.isPacked(t)) out_ += " packed";
                out_ += " {";
                // c107 (D-FFI-DESCRIPTOR-UNION-OVERLAY): emit `@<off>` per field for
                // an explicit-offset struct so the text round-trips (reintern +
                // canonicalization depend on the offsets surviving serialization).
                // D-CSUBSET-MEMBER-ALIGNAS: emit `~<align>` per field for a member-
                // alignas struct (the two channels are mutually exclusive — a struct
                // carries offsets XOR aligns). The `~` marker never collides with the
                // offset `@`; both round-trip through parseType below.
                if (in.hasExplicitOffsets(t)) {
                    auto const ops = in.operands(t);
                    bool first = true;
                    for (std::size_t i = 0; i < ops.size(); ++i) {
                        if (!first) out_ += ", ";
                        appendType(ops[i]);
                        auto const off = in.explicitFieldOffset(t, i);
                        out_ += std::format(" @{}", off ? *off : 0);
                        first = false;
                    }
                } else if (in.hasExplicitAligns(t)) {
                    auto const ops = in.operands(t);
                    bool first = true;
                    for (std::size_t i = 0; i < ops.size(); ++i) {
                        if (!first) out_ += ", ";
                        appendType(ops[i]);
                        out_ += std::format(" ~{}", in.explicitFieldAlign(t, i));
                        first = false;
                    }
                } else {
                    args(in.operands(t));
                }
                out_ += '}'; return;
            }
            case TypeKind::Union: {
                out_ += "union ";  out_ += quote(in.name(t));
                if (in.isPacked(t)) out_ += " packed";   // D-CSUBSET-PACKED (see struct)
                out_ += " {"; args(in.operands(t)); out_ += '}'; return;
            }
            // D5.5: enum is nominal-by-name; underlying TypeKind lives in
            // scalars[0]. Round-trip the underlying explicitly when it
            // diverges from the default I32 (`enum "E" : u8`); omit the
            // suffix when underlying = I32 to keep the common form
            // readable. Parser defaults to I32 when the suffix is
            // absent — emission elision MUST stay in lockstep with the
            // parser default for round-trip correctness.
            //
            // ★★ THE UNDERLYING KIND IS SPELLED BY NAME, NOT BY ITS ORDINAL. This
            // arm wrote `std::to_string(sc[0])` — the raw `TypeKind` integer — and
            // the reader cast it back, which contradicts the rule `core_type.hpp`
            // states at each of its three "appended so every pre-existing kind keeps
            // its integer value" notes: NO TypeKind ordinal is serialized. The
            // `wfloat` literal arm in this same file had already written the reason
            // out — serialize "a STABLE semantic discriminator, NOT the
            // version-fragile TypeKind ordinal". The name comes off
            // `kHirTextPrimTable`, the table the rest of this grammar already reads
            // in both directions
            // (D-TEXT-TIER-ENUM-UNDERLYING-SERIALIZED-AS-A-TYPEKIND-ORDINAL).
            case TypeKind::Enum: {
                out_ += "enum ";
                out_ += quote(in.name(t));
                auto sc = in.scalars(t);
                if (!sc.empty() && static_cast<TypeKind>(sc[0]) != TypeKind::I32) {
                    std::string_view const n = primName(static_cast<TypeKind>(sc[0]));
                    if (n.empty()) {
                        // No keyword for it ⇒ say so and emit NOTHING rather than a
                        // number the reader would have to guess at. The text then
                        // reads back as the plain `enum "N"` default, which is a
                        // stated loss instead of a silent re-point.
                        // Error, by this emitter's DEFAULT and for its stated
                        // reason: output that does not read back as the module it
                        // came from is not round-trippable, and `reporter
                        // .hasErrors()` is how a caller learns that. The one
                        // documented Warning exception here is the deliberately
                        // absent interner, which is a MODE, not a loss.
                        report(std::format(
                            "enum '{}' has an underlying TypeKind (ordinal {}) this "
                            "format has no keyword for; the underlying type is NOT "
                            "rendered and the text reads back as the I32 default",
                            in.name(t), sc[0]), DiagnosticSeverity::Error);
                    } else {
                        out_ += " : ";
                        out_ += n;
                    }
                }
                return;
            }
            case TypeKind::FnSig: {
                out_ += "fn(";
                auto const ps = in.fnParams(t);
                args(ps);
                // Emit the variadic marker so a variadic FnSig round-trips through
                // text (the scalars[1] flag would otherwise be lost on reparse).
                if (in.fnIsVariadic(t)) { out_ += ps.empty() ? "..." : ", ..."; }
                out_ += ") -> ";
                appendType(in.fnResult(t));
                auto sc = in.scalars(t);
                if (!sc.empty()) {
                    auto const cc = static_cast<CallConv>(sc[0]);
                    if (cc != CallConv::CcSysV) { out_ += " cc "; out_ += callConvName(cc); }
                }
                return;
            }
            case TypeKind::Extension: {
                out_ += "ext "; out_ += quote(in.name(t)); out_ += " (";
                args(in.operands(t)); out_ += ')';
                auto sc = in.scalars(t);
                if (!sc.empty()) {
                    out_ += " [";
                    bool first = true;
                    for (auto s : sc) { if (!first) out_ += ", "; out_ += std::format("{}", s); first = false; }
                    out_ += ']';
                }
                return;
            }
            case TypeKind::BitInt: {  // C23 _BitInt(N) (D-CSUBSET-BITINT) — debug dump
                if (!in.bitIntIsSigned(t)) out_ += "unsigned ";
                out_ += "_BitInt(";
                out_ += std::to_string(in.bitIntWidth(t));
                out_ += ')';
                return;
            }
            default: { // primitives (and any unexpected kind)
                std::string_view const p = primName(in.kind(t));
                if (!p.empty()) out_ += p;
                else { report("unprintable type kind", DiagnosticSeverity::Error); out_ += '?'; }
                // D-LANG-TYPE-IDENTITY-VOCABULARY: emit the vocabulary tag when the
                // primitive carries one, so the text round-trip preserves IDENTITY
                // and not merely representation. Anonymous primitives (every core
                // whose C spelling is its own representation) print exactly as
                // before — zero churn for existing dumps.
                std::string_view const vocab = in.vocabularyName(t);
                if (!vocab.empty()) {
                    out_ += " \"";
                    out_ += vocab;
                    out_ += '"';
                }
                return;
            }
        }
    }

    // ── inline attribute prefix (for expression-position nodes) ──────────────
    [[nodiscard]] std::string attrsInline(HirNodeId id) {
        std::string s;
        forEachAttr(id, [&](std::string const& a) { s += a; s += ' '; });
        return s;
    }
    void emitAttrsBlock(HirNodeId id, int ind) {
        forEachAttr(id, [&](std::string const& a) { out_ += indent(ind); out_ += a; out_ += '\n'; });
    }

    template <class F> void forEachAttr(HirNodeId id, F&& sink) {
        if (ctx_.sourceMap) if (auto const* v = ctx_.sourceMap->tryGet(id)) sink(fmtLoc(*v));
        if (ctx_.ffiMap) if (auto const* v = ctx_.ffiMap->tryGet(id)) sink(fmtFfi(*v));
        if (ctx_.shaderMap) if (auto const* v = ctx_.shaderMap->tryGet(id)) sink(fmtShader(*v));
        if (ctx_.transpileMap) if (auto const* v = ctx_.transpileMap->tryGet(id)) sink(fmtTranspile(*v));
        if (ctx_.diagnosticMap) if (auto const* v = ctx_.diagnosticMap->tryGet(id)) sink(fmtDiag(*v));
    }

    [[nodiscard]] static std::string fmtLoc(HirSourceLoc const& v) {
        return std::format("@loc(buf {}, {}..{})", v.buffer.v, v.span.start(), v.span.end());
    }
    [[nodiscard]] static std::string fmtFfi(FfiMetadata const& v) {
        std::string s = "@ffi(";
        bool first = true;
        auto field = [&](std::string part) { if (!first) s += ", "; s += std::move(part); first = false; };
        if (!v.mangledName.empty()) field("name " + quote(v.mangledName));
        field(std::string("link ") + std::string(ffiLinkageName(v.linkage)));
        field(std::string("vis ") + std::string(ffiVisName(v.visibility)));
        if (!v.importLibrary.empty()) field("lib " + quote(v.importLibrary));
        if (!v.soname.empty()) field("soname " + quote(v.soname));
        // c156 (D-LK-ELF-SYMBOL-VERSIONING): the required ELF symbol version —
        // conditional (unversioned externs stay byte-identical), mirroring soname.
        if (!v.version.empty()) field("version " + quote(v.version));
        s += ')';
        return s;
    }
    [[nodiscard]] static std::string fmtShader(ShaderIntrinsic const& v) {
        std::string s = "@shader(";
        bool first = true;
        auto field = [&](std::string part) { if (!first) s += ", "; s += std::move(part); first = false; };
        field(std::string("stage ") + std::string(stageName(v.stage)));
        if (v.builtin != ShaderBuiltin::None) field(std::string("builtin ") + std::string(builtinName(v.builtin)));
        if (v.workgroup.x != 1 || v.workgroup.y != 1 || v.workgroup.z != 1)
            field(std::format("wg {} {} {}", v.workgroup.x, v.workgroup.y, v.workgroup.z));
        if (v.binding.set != 0 || v.binding.binding != 0)
            field(std::format("binding {}:{}", v.binding.set, v.binding.binding));
        if (v.location != kUnsetShaderLocation) field(std::format("loc {}", v.location));
        s += ')';
        return s;
    }
    [[nodiscard]] static std::string fmtTranspile(TranspileHint const& v) {
        std::string s = "@transpile(";
        bool first = true;
        auto field = [&](std::string part) { if (!first) s += ", "; s += std::move(part); first = false; };
        if (!v.targetLanguage.empty()) field("target " + quote(v.targetLanguage));
        if (!v.overrideKind.empty()) field("override " + quote(v.overrideKind));
        if (v.idiom != TranspileIdiom::Default) field(std::string("idiom ") + std::string(idiomName(v.idiom)));
        s += ')';
        return s;
    }
    [[nodiscard]] std::string fmtDiag(DiagnosticInfo const& v) {
        std::string s = "@diag(";
        // code as decimal value: the forward `diagnosticCodeName` switch is the
        // single source of truth for the enum; a parallel name->code reverse table
        // would be a second thing to keep in sync (and rot). The numeric value is
        // stable and round-trips exactly.
        s += std::format("code {}", static_cast<std::uint32_t>(v.code));
        if (v.recovery != HirRecovery::None) { s += ", recovery "; s += recoveryName(v.recovery); }
        if (v.origin.valid()) {
            auto it = preIndex_.find(v.origin.v);
            if (it != preIndex_.end()) s += std::format(", origin {}", it->second);
        }
        if (!v.detail.empty()) { s += ", detail "; s += quote(v.detail); }
        s += ')';
        return s;
    }

    // ── per-node emission ────────────────────────────────────────────────────

    // A node on its own line(s) at `ind`. Used for decls, statements, and the
    // Extension/Error wildcards. Expression-kind nodes are emitted inline instead.
    void emitNodeLine(HirNodeId id, int ind) {
        if (isExprKind(hir_.kind(id))) {
            emitAttrsBlock(id, ind);
            out_ += indent(ind);
            emitExpr(id);
            out_ += '\n';
            return;
        }
        emitStmtLike(id, ind);
    }

    void emitStmtLike(HirNodeId id, int ind) {
        emitAttrsBlock(id, ind);
        out_ += indent(ind);
        HirFlags const f = hir_.flags(id);
        switch (hir_.kind(id)) {
            case HirKind::Module:
                report("nested Module is not representable", DiagnosticSeverity::Error);
                out_ += stmtKw(HirTextStmtKw::Error); out_ += '\n'; return;
            case HirKind::Function: {
                out_ += stmtKw(HirTextStmtKw::Function); out_ += flagsStr(f);
                out_ += std::format(" %{} : ", handleOf(hir_.payload(id)));
                appendType(hir_.functionSignature(id));
                out_ += " {\n";
                for (HirNodeId p : hir_.functionParams(id)) emitParam(p, ind + 1);
                emitNodeLine(hir_.functionBody(id), ind + 1);
                out_ += indent(ind); out_ += "}\n";
                return;
            }
            case HirKind::ExternFunction: {
                out_ += stmtKw(HirTextStmtKw::ExternFunction); out_ += flagsStr(f);
                out_ += std::format(" %{}", handleOf(hir_.payload(id)));
                if (hir_.externFunctionSignature(id).valid()) { out_ += " : "; appendType(hir_.externFunctionSignature(id)); }
                out_ += " {\n";
                for (HirNodeId p : hir_.externFunctionParams(id)) emitParam(p, ind + 1);
                out_ += indent(ind); out_ += "}\n";
                return;
            }
            case HirKind::Global: {
                out_ += stmtKw(HirTextStmtKw::Global); out_ += flagsStr(f);
                out_ += std::format(" %{} : ", handleOf(hir_.payload(id)));
                appendType(hir_.globalType(id));
                if (auto init = hir_.globalInit(id)) { out_ += " = "; emitExpr(*init); }
                out_ += '\n';
                return;
            }
            case HirKind::TypeDecl:
                out_ += stmtKw(HirTextStmtKw::TypeDecl); out_ += flagsStr(f);
                out_ += std::format(" %{} : ", handleOf(hir_.payload(id)));
                appendType(hir_.typeDeclType(id)); out_ += '\n';
                return;
            case HirKind::ExternGlobal:
                out_ += stmtKw(HirTextStmtKw::ExternGlobal); out_ += flagsStr(f);
                out_ += std::format(" %{}", handleOf(hir_.payload(id)));
                if (hir_.externGlobalType(id).valid()) { out_ += " : "; appendType(hir_.externGlobalType(id)); }
                out_ += '\n';
                return;
            case HirKind::ImportGroup:
                out_ += stmtKw(HirTextStmtKw::ImportGroup); out_ += flagsStr(f); out_ += " {\n";
                for (HirNodeId m : hir_.importGroupMembers(id)) emitNodeLine(m, ind + 1);
                out_ += indent(ind); out_ += "}\n";
                return;
            case HirKind::Block:
                out_ += stmtKw(HirTextStmtKw::Block); out_ += flagsStr(f); out_ += " {\n";
                for (HirNodeId s : hir_.children(id)) emitNodeLine(s, ind + 1);
                out_ += indent(ind); out_ += "}\n";
                return;
            case HirKind::IfStmt: {
                out_ += stmtKw(HirTextStmtKw::If); out_ += flagsStr(f); out_ += " (";
                emitExpr(hir_.ifCondition(id)); out_ += ")\n";
                emitNodeLine(hir_.ifThen(id), ind + 1);
                if (auto e = hir_.ifElse(id)) { out_ += indent(ind); out_ += "else\n"; emitNodeLine(*e, ind + 1); }
                return;
            }
            case HirKind::SehTryExcept:
                // c115 SEH: `seh_try` <tryBody> `seh_except (` filter `)` <handler>.
                out_ += stmtKw(HirTextStmtKw::SehTry); out_ += flagsStr(f); out_ += '\n';
                emitNodeLine(hir_.sehTryBody(id), ind + 1);
                out_ += indent(ind); out_ += "seh_except (";
                emitExpr(hir_.sehTryFilter(id)); out_ += ")\n";
                emitNodeLine(hir_.sehTryHandler(id), ind + 1);
                return;
            case HirKind::WhileStmt:
                out_ += stmtKw(HirTextStmtKw::While); out_ += flagsStr(f); out_ += " (";
                emitExpr(*hir_.loopCondition(id)); out_ += ")\n";
                emitNodeLine(hir_.loopBody(id), ind + 1);
                return;
            case HirKind::DoWhileStmt:
                out_ += stmtKw(HirTextStmtKw::Do); out_ += flagsStr(f); out_ += "\n";
                emitNodeLine(hir_.loopBody(id), ind + 1);
                out_ += indent(ind); out_ += "while ("; emitExpr(*hir_.loopCondition(id)); out_ += ")\n";
                return;
            case HirKind::ForStmt: {
                out_ += stmtKw(HirTextStmtKw::For); out_ += flagsStr(f); out_ += " {\n";
                if (auto i = hir_.forInit(id))   { out_ += indent(ind + 1); out_ += "init:\n";   emitNodeLine(*i, ind + 2); }
                if (auto c = hir_.loopCondition(id)) { out_ += indent(ind + 1); out_ += "cond:\n"; emitNodeLine(*c, ind + 2); }
                if (auto u = hir_.forUpdate(id)) { out_ += indent(ind + 1); out_ += "update:\n"; emitNodeLine(*u, ind + 2); }
                out_ += indent(ind + 1); out_ += "body:\n"; emitNodeLine(hir_.loopBody(id), ind + 2);
                out_ += indent(ind); out_ += "}\n";
                return;
            }
            case HirKind::SwitchStmt: {
                // c60 (Design I-A): `switch (disc) { body: <block> case v L<ord> ...
                // default L<ord> }`. The body Block carries the case/default markers
                // inline; the dispatch arms map each case value to its marker ordinal.
                out_ += stmtKw(HirTextStmtKw::Switch); out_ += flagsStr(f); out_ += " (";
                emitExpr(hir_.switchDiscriminant(id)); out_ += ") {\n";
                out_ += indent(ind + 1); out_ += "body:\n";
                emitNodeLine(hir_.switchBody(id), ind + 2);
                for (HirNodeId arm : hir_.switchArms(id)) emitCaseArm(arm, ind + 1);
                out_ += indent(ind); out_ += "}\n";
                return;
            }
            case HirKind::BreakStmt: {
                out_ += stmtKw(HirTextStmtKw::Break); out_ += flagsStr(f);
                if (hir_.branchDepth(id) != 0) out_ += std::format(" {}", hir_.branchDepth(id));
                out_ += '\n'; return;
            }
            case HirKind::ContinueStmt: {
                out_ += stmtKw(HirTextStmtKw::Continue); out_ += flagsStr(f);
                if (hir_.branchDepth(id) != 0) out_ += std::format(" {}", hir_.branchDepth(id));
                out_ += '\n'; return;
            }
            case HirKind::ReturnStmt:
                out_ += stmtKw(HirTextStmtKw::Return); out_ += flagsStr(f);
                if (auto v = hir_.returnValue(id)) { out_ += ' '; emitExpr(*v); }
                out_ += '\n'; return;
            case HirKind::ExprStmt:
                out_ += stmtKw(HirTextStmtKw::Expr); out_ += flagsStr(f); out_ += ' '; emitExpr(hir_.exprStmtExpr(id)); out_ += '\n';
                return;
            case HirKind::VarDecl:
                out_ += stmtKw(HirTextStmtKw::Var); out_ += flagsStr(f);
                out_ += std::format(" %{} : ", handleOf(hir_.payload(id)));
                appendType(hir_.varDeclType(id));
                if (auto init = hir_.varDeclInit(id)) { out_ += " = "; emitExpr(*init); }
                out_ += '\n'; return;
            case HirKind::AssignStmt:
                out_ += stmtKw(HirTextStmtKw::Assign); out_ += flagsStr(f); out_ += ' ';
                emitExpr(hir_.assignTarget(id)); out_ += " = "; emitExpr(hir_.assignValue(id)); out_ += '\n';
                return;
            case HirKind::Unreachable:
                out_ += stmtKw(HirTextStmtKw::Unreachable); out_ += flagsStr(f); out_ += '\n'; return;
            case HirKind::GotoStmt:
                out_ += stmtKw(HirTextStmtKw::Goto); out_ += flagsStr(f);
                out_ += std::format(" L{}", hir_.labelOrdinal(id)); out_ += '\n'; return;
            case HirKind::LabelStmt:
                out_ += stmtKw(HirTextStmtKw::Label); out_ += flagsStr(f);
                out_ += std::format(" L{}:\n", hir_.labelOrdinal(id));
                emitNodeLine(hir_.labelBody(id), ind + 1);
                return;
            case HirKind::IndirectGotoStmt:
                out_ += stmtKw(HirTextStmtKw::Goto); out_ += flagsStr(f); out_ += " *";
                emitExpr(hir_.indirectGotoTarget(id)); out_ += '\n'; return;
            case HirKind::InlineAsm:
                emitInlineAsm(id, f); return;
            case HirKind::Error: case HirKind::Extension:
                emitExtOrError(id, /*inlineForm=*/false, ind); out_ += '\n'; return;

            // ── every EXPRESSION kind, plus the two arms that render elsewhere ──
            // ★ `emitNodeLine` routes expression kinds to `emitExpr` before this
            // switch runs, and `CaseArm` renders through `emitCaseArm` from its
            // SwitchStmt parent, so reaching any of these is a mis-route rather
            // than an unhandled node. They are listed rather than left to a
            // `default:` for the reason the expression switch lists its
            // complement: a `default:` absorbs a NEW `HirKind` in silence and
            // degrades it to `error`, and `-Werror=switch` is the only thing that
            // makes the two directions grow together.
            case HirKind::Literal:     case HirKind::Ref:
            case HirKind::Call:        case HirKind::IntrinsicCall:
            case HirKind::BuiltinCall: case HirKind::BinaryOp:
            case HirKind::UnaryOp:     case HirKind::Cast:
            case HirKind::MemberAccess: case HirKind::Index:
            case HirKind::Swizzle:     case HirKind::ConstructAggregate:
            case HirKind::Ternary:     case HirKind::LogicalAnd:
            case HirKind::LogicalOr:   case HirKind::SizeOf:
            case HirKind::AlignOf:     case HirKind::AddressOf:
            case HirKind::Deref:       case HirKind::SeqExpr:
            case HirKind::LabelAddressOf: case HirKind::VaStart:
            case HirKind::VaArg:       case HirKind::VaEnd:
            case HirKind::TypeRef:     case HirKind::CaseArm:
            case HirKind::Count_:
                report(std::format("unexpected node kind '{}' in statement position",
                                   hirKindName(hir_.kind(id))),
                       DiagnosticSeverity::Error);
                out_ += stmtKw(HirTextStmtKw::Error); out_ += '\n'; return;
        }
        // Unreachable: the switch above is total over `HirKind`.
        report("statement node carries a kind outside the core enum",
               DiagnosticSeverity::Error);
        out_ += stmtKw(HirTextStmtKw::Error); out_ += '\n';
    }

    // -- inline-asm P5 (D-CSUBSET-INLINE-ASM-OPERANDS) -----------------------
    //
    // TWO FORMS, because the node has two. `payload == kNoInlineAsmDescriptor`
    // is the bare barrier and renders EXACTLY as it always has - a lone
    // `inline_asm` keyword line - so every pre-P5 golden is byte-identical.
    // A descriptor renders as:
    //
    //   inline_asm "TEMPLATE" [{ [extended] [goto] [mem] [cc]
    //                          [outputs <N> operands ( <opnd> , ... )]
    //                          [clobbers ( "r" , ... )]
    //                          [labels ( <label> , ... )] }]
    //   <opnd>  := "constraint" [ [name] ] [<spells>] [class <N>] [pin "reg"]
    //              [operand_kind <name>] -> <expr>
    //   <label> := L<n> [<spells>]
    //   <spells>:= spells ( "%0" , ... )
    //
    // * `spells` RENDERS A CARRIED FACT, NOT A DERIVED ONE, which is why it is
    // in the text at all. An operand's `%N`/`%[name]` forms and a label's
    // `%lN`/`%l[name]` forms are minted ONCE at the front end from the
    // language's declared lexemes; a reader that recomputed them here would be
    // a second owner of the numbering, and the numbering is exactly what this
    // arc moved to one owner. A field that is not rendered is a field the round
    // trip silently drops, so omitting them would put the drop back.
    //
    // * THE POOL HANDLE IS NOT PRINTED, and that is what keeps the round trip
    // stable: the descriptor is rendered INLINE, the parser re-adds it and gets
    // a fresh handle in tree order, and re-emitting yields the same bytes. The
    // `lit <value>` form works exactly this way and for exactly this reason.
    // !! With NO pool supplied the handle CANNOT be resolved, so the `#<handle>`
    // fallback is emitted AND a diagnostic is reported - the literal pool's
    // out-of-range arm, never the silent degradation its no-pool arm takes: a
    // `lit` without its value still says `lit`, whereas an asm statement
    // rendered without its operands would read as a bare barrier, which is a
    // DIFFERENT PROGRAM.
    void emitInlineAsm(HirNodeId id, HirFlags f) {
        std::uint32_t const handle = hir_.payload(id);
        out_ += stmtKw(HirTextStmtKw::InlineAsm);
        out_ += flagsStr(f);
        if (handle == kNoInlineAsmDescriptor) { out_ += '\n'; return; }
        if (ctx_.inlineAsmPool == nullptr || !ctx_.inlineAsmPool->contains(handle)) {
            report("inline-asm descriptor handle does not resolve against the "
                   "supplied pool - rendering the opaque handle form; the "
                   "operands, clobbers and labels are NOT in this output",
                   DiagnosticSeverity::Error);
            out_ += std::format(" #{}\n", handle);
            return;
        }
        auto const& d = ctx_.inlineAsmPool->at(handle);
        out_ += ' ';
        out_ += quote(d.templateText);
        // !! THE DESCRIPTOR TAIL IS BRACED, AND THE BRACES ARE NOT DECORATION.
        // The flags are bare keywords and one of them is `goto`, which is ALSO
        // a statement keyword -- and this format's lexer is newline-blind. So
        //     inline_asm "nop"
        //     goto L1
        // would parse the NEXT STATEMENT's `goto` as this asm's goto flag and
        // then choke on `L1`. Bracing the tail makes its extent explicit and
        // kills that whole collision class rather than renaming one keyword out
        // of the way (the next keyword added would re-open it).
        // The braces are omitted when there is nothing to put in them, so the
        // commonest form -- a plain basic template -- stays a one-token tail.
        auto const kids = hir_.children(id);
        // !! THIS PREDICATE ENUMERATES EVERY FIELD, and a new field left out of
        // it is not a cosmetic miss: with nothing else set, the whole brace
        // group is skipped and that field is dropped from the text with no
        // report. `labelSpellings` is listed beside `labelOrdinals` rather than
        // assumed to travel with it precisely so the enumeration stays literal.
        // (An operand's `spellings` live INSIDE an operand, so `operands` is
        // already the condition that renders them.)
        bool const anyTail = d.isExtended || d.isGoto || d.clobbersMemory
                             || d.clobbersConditionCodes || !d.operands.empty()
                             || !d.clobbers.empty() || !d.labelOrdinals.empty()
                             || !d.labelSpellings.empty();
        if (!anyTail) { out_ += '\n'; return; }
        // One operand's / one label's spelling list, or nothing at all when it
        // has none (a language whose sigil role is declared `null`).
        auto const emitSpells = [&](std::vector<std::string> const& spellings) {
            if (spellings.empty()) return;
            out_ += " spells (";
            for (std::size_t i = 0; i < spellings.size(); ++i) {
                out_ += (i == 0 ? " " : ", ");
                out_ += quote(spellings[i]);
            }
            out_ += " )";
        };
        out_ += " {";
        if (d.isExtended)             out_ += " extended";
        if (d.isGoto)                 out_ += " goto";
        if (d.clobbersMemory)         out_ += " mem";
        if (d.clobbersConditionCodes) out_ += " cc";
        if (!d.operands.empty()) {
            out_ += std::format(" outputs {} operands (", d.outputCount);
            for (std::size_t i = 0; i < d.operands.size(); ++i) {
                auto const& op = d.operands[i];
                out_ += (i == 0 ? " " : ", ");
                out_ += quote(op.constraint.raw);
                if (!op.symbolicName.empty()) {
                    out_ += " ["; out_ += op.symbolicName; out_ += ']';
                }
                emitSpells(op.spellings);
                if (op.regClassResolved)
                    out_ += std::format(" class {}", op.regClass);
                if (!op.fixedRegister.empty()) {
                    out_ += " pin "; out_ += quote(op.fixedRegister);
                }
                // ── D-HIR-TEXT-INLINE-ASM-OPERAND-KIND-DROPPED-IN-TRANSIT ──
                //
                // ★★★ THE THIRD ARM OF THE RESOLUTION, AND IT MUST TRAVEL OR THE
                // ROUND TRIP RE-CREATES THE VERY DEFECT THE PIPELINE JUST FIXED.
                // A constraint letter binds one of THREE things
                // (`TargetAsmConstraint::binds`), and the two lines above carry
                // only two of them. A form-bound letter — `"m"` → `membase`,
                // `"i"` → `imm32` — sets `operandKindResolved` and NOTHING else,
                // so a descriptor written without this clause reads back with
                // `!regClassResolved && !operandKindResolved`: byte-identical to
                // *"no target was in scope"*. `hir_to_mir` then refuses the
                // operand saying the letter *"was never bound to a processor"* —
                // a refusal whose stated reason is FALSE, which sends the reader
                // to fix a config that is already correct. That is
                // D-ASM-MEMORY-CONSTRAINT-REFUSED-DESPITE-BEING-DECLARED,
                // reproduced one tier over by a serialization gap.
                //
                // ★ SPELLED BY NAME, NOT BY ORDINAL, and that is a deliberate
                // departure from the `class {}` line above it. `OperandKindFilter`
                // is an OPEN-ENDED enum by its own docblock (*"future Imm8/Imm16/
                // Imm64 join as distinct filters"*), so an inserted enumerator
                // silently retargets every ordinal a stored `.dsshir` ever wrote,
                // and an out-of-range ordinal has no failure arm at all. A name
                // has one, and it is the table the whole pipeline already reads
                // (`kOperandKindFilterTable`) — no new vocabulary.
                // ⚠ `nameOrEmpty`, never `name()`: `OperandKindFilter::Reg` is 0,
                // so `name()`'s row-0 fallback would render an out-of-range value
                // as `reg` — the plausible wrong answer instead of the loud one.
                if (op.operandKindResolved) {
                    auto const kind = static_cast<OperandKindFilter>(op.operandKind);
                    std::string_view const spelling =
                        kOperandKindFilterTable.nameOrEmpty(kind);
                    if (spelling.empty()) {
                        report(std::format(
                                   "inline-asm operand carries operand-kind ordinal "
                                   "{}, which no `OperandKindFilter` row spells - it "
                                   "cannot be written to `.dsshir`; accepted: {}",
                                   op.operandKind,
                                   detail::renderAllowedList(
                                       allNames(kOperandKindFilterTable))),
                               DiagnosticSeverity::Error);
                    } else {
                        out_ += " operand_kind "; out_ += spelling;
                    }
                }
                out_ += " -> ";
                // The operand's VALUE is child i - the descriptor and the child
                // list are index-aligned by construction, and `HirVerifier`'s
                // `checkInlineAsm` is what keeps them so.
                if (i < kids.size()) { emitExpr(kids[i]); }
                else {
                    report("inline-asm descriptor declares more operands than the "
                           "node has children", DiagnosticSeverity::Error);
                    out_ += stmtKw(HirTextStmtKw::Error);
                }
            }
            out_ += " )";
        }
        if (!d.clobbers.empty()) {
            out_ += " clobbers (";
            for (std::size_t i = 0; i < d.clobbers.size(); ++i) {
                out_ += (i == 0 ? " " : ", ");
                out_ += quote(d.clobbers[i]);
            }
            out_ += " )";
        }
        if (!d.labelOrdinals.empty() || !d.labelSpellings.empty()) {
            // !! THE SPELLINGS RIDE ON THEIR OWN ORDINAL RATHER THAN IN A
            // PARALLEL SECTION, and that is the same argument the descriptor
            // itself makes: two sibling lists in the text can be edited out of
            // step and the reader cannot tell, whereas an inner group attached
            // to `L<n>` makes the alignment structural. A count DISAGREEMENT is
            // still possible (a hand-written dump), which is why
            // `HirVerifier::checkInlineAsm` asserts the sizes rather than this
            // writer assuming them: the writer walks the LONGER list so a
            // mismatch is rendered and reported, never silently truncated.
            std::size_t const n =
                std::max(d.labelOrdinals.size(), d.labelSpellings.size());
            if (d.labelOrdinals.size() != d.labelSpellings.size()) {
                report("inline-asm descriptor carries a different number of "
                       "`asm goto` label ordinals and spelling groups - the two "
                       "are index-aligned by contract", DiagnosticSeverity::Error);
            }
            out_ += " labels (";
            for (std::size_t i = 0; i < n; ++i) {
                out_ += (i == 0 ? " " : ", ");
                if (i < d.labelOrdinals.size())
                    out_ += std::format("L{}", d.labelOrdinals[i]);
                else
                    out_ += "error";
                if (i < d.labelSpellings.size()) emitSpells(d.labelSpellings[i]);
            }
            out_ += " )";
        }
        out_ += " }\n";
    }

    // A parameter VarDecl inside a (extern)function body.
    void emitParam(HirNodeId id, int ind) {
        emitAttrsBlock(id, ind);
        out_ += indent(ind);
        out_ += stmtKw(HirTextStmtKw::Param); out_ += flagsStr(hir_.flags(id));
        out_ += std::format(" %{} : ", handleOf(hir_.payload(id)));
        appendType(hir_.varDeclType(id));
        if (auto init = hir_.varDeclInit(id)) { out_ += " = "; emitExpr(*init); }
        out_ += '\n';
    }

    // c60 (Design I-A): a dispatch entry — `case <value> L<ord>` / `default L<ord>`.
    // The arm carries no body (the body lives on the SwitchStmt); `L<ord>` is the
    // ordinal of the case's synthetic LabelStmt marker inside that body.
    void emitCaseArm(HirNodeId id, int ind) {
        emitAttrsBlock(id, ind);
        out_ += indent(ind);
        if (hir_.caseArmIsDefault(id)) {
            out_ += stmtKw(HirTextStmtKw::Default); out_ += flagsStr(hir_.flags(id));
        } else {
            out_ += stmtKw(HirTextStmtKw::Case); out_ += flagsStr(hir_.flags(id)); out_ += ' ';
            emitExpr(*hir_.caseArmValue(id));
        }
        out_ += std::format(" L{}\n", hir_.caseArmLabelOrdinal(id));
    }

    // The `ext_node`/`error` wildcard form. Inline form (no indent/newline, for
    // expression position) vs block form (own line); both share the body shape.
    void emitExtOrError(HirNodeId id, bool inlineForm, int ind) {
        HirFlags const f = hir_.flags(id);
        if (hir_.kind(id) == HirKind::Extension) {
            std::string_view name = "?";
            std::uint32_t const p = hir_.payload(id);
            if (p >= kFirstHirExtensionKind) name = hir_.registry().descriptor(HirKindId{p}).name();
            else report("Extension node payload is not an extension kind id",
                        DiagnosticSeverity::Error);
            out_ += stmtKw(HirTextStmtKw::ExtNode); out_ += flagsStr(f); out_ += ' '; out_ += quote(name);
        } else {
            out_ += stmtKw(HirTextStmtKw::Error); out_ += flagsStr(f);
        }
        if (hir_.typeId(id).valid()) { out_ += " : "; appendType(hir_.typeId(id)); }
        auto kids = hir_.children(id);
        if (!kids.empty()) {
            out_ += " {\n";
            int const childInd = inlineForm ? ind : ind + 1;
            for (HirNodeId c : kids) emitNodeLine(c, childInd);
            out_ += indent(inlineForm ? 0 : ind); out_ += "}";
        }
    }

    // An expression, inline (no leading indent / trailing newline). Children in
    // comma-separated parens.
    void emitExpr(HirNodeId id) {
        out_ += attrsInline(id);
        HirFlags const f = hir_.flags(id);
        auto operands = [&](std::span<HirNodeId const> kids) {
            out_ += '(';
            bool first = true;
            for (HirNodeId k : kids) { if (!first) out_ += ", "; emitExpr(k); first = false; }
            out_ += ')';
        };
        // ★★★ THE KEYWORD IS NOT A PARAMETER ANY MORE — IT IS PROJECTED FROM THE
        // NODE'S OWN KIND through `exprKwForKind`, so no arm can write a spelling
        // that is not this format's spelling for the kind it is rendering, and no
        // arm can write a spelling the reader has no row for. That is the whole
        // repair for `builtincall` / `labeladdr`: adding the two rows fixed two
        // spellings, and taking the string away from the writer is what stops the
        // next one being born. `header` therefore takes NOTHING.
        // ⚠ A kind with no pairing REPORTS rather than emitting a blank keyword —
        // an empty spelling would render `" : i64"`, which reads as a syntax
        // error at some later reader instead of a defect here.
        auto header = [&]() {
            if (auto const kw = exprKwForKind(hir_.kind(id)))
                out_ += kHirTextExprKwTable.name(*kw);
            else
                report("expression node kind has no `.dsshir` keyword", DiagnosticSeverity::Error);
            out_ += flagsStr(f);
        };
        auto typed = [&]() { header(); out_ += " : "; appendType(hir_.typeId(id)); };
        // The `kw : type (operands...)` family — the parser's mirror is its
        // `typedCall` arm; the two now share the spelling table, not a convention.
        auto typedCall = [&]() { typed(); out_ += ' '; operands(hir_.children(id)); };
        switch (hir_.kind(id)) {
            case HirKind::Literal: {
                header(); out_ += ' ';
                std::uint32_t const idx = hir_.payload(id);
                // Inline the VALUE when a pool is supplied (faithful round-trip);
                // else the bare index form (value-less, for pool-less modules).
                if (ctx_.literalPool && idx < ctx_.literalPool->size()) {
                    appendLiteralValue(ctx_.literalPool->at(idx));
                } else {
                    // A pool IS supplied but the index is past it ⇒ a lowering
                    // bug (a Literal pointing past its pool). Warn instead of
                    // silently dropping the value into the index fallback.
                    if (ctx_.literalPool)
                        report(std::format("literal #{} is out of range of the pool (size {})",
                                           idx, ctx_.literalPool->size()),
                               DiagnosticSeverity::Error);
                    out_ += std::format("#{}", idx);
                }
                out_ += " : "; appendType(hir_.typeId(id)); return;
            }
            case HirKind::Ref:
                header(); out_ += std::format(" %{} : ", handleOf(hir_.payload(id))); appendType(hir_.typeId(id)); return;
            case HirKind::IntrinsicCall: {
                header(); out_ += ' ';
                std::uint32_t const p = hir_.payload(id);
                if (hir_.intrinsicRegistry().contains(HirIntrinsicId{p}))
                    out_ += quote(hir_.intrinsicRegistry().descriptor(HirIntrinsicId{p}).name());
                else { report("IntrinsicCall payload is not a registered intrinsic",
                              DiagnosticSeverity::Error); out_ += "\"?\""; }
                out_ += " : "; appendType(hir_.typeId(id)); out_ += ' '; operands(hir_.children(id)); return;
            }
            case HirKind::BuiltinCall: {
                // c103 (D-CSUBSET-INTRINSIC-UMULH): `builtincall #<lowering> : <type>
                // (<operands>)` — the BuiltinLowering payload prints numerically.
                header(); out_ += ' ';
                out_ += std::format("#{}", hir_.payload(id));
                out_ += " : "; appendType(hir_.typeId(id)); out_ += ' ';
                operands(hir_.children(id)); return;
            }
            case HirKind::BinaryOp: case HirKind::UnaryOp: {
                header();
                out_ += ' '; appendOpName(hir_.payload(id));
                out_ += " : "; appendType(hir_.typeId(id)); out_ += ' '; operands(hir_.children(id)); return;
            }
            case HirKind::MemberAccess:
                header(); out_ += std::format(" #{} : ", hir_.payload(id)); appendType(hir_.typeId(id));
                out_ += ' '; operands(hir_.children(id)); return;
            case HirKind::Swizzle:
                header(); out_ += std::format(" #{} : ", hir_.payload(id)); appendType(hir_.typeId(id));
                out_ += ' '; operands(hir_.children(id)); return;
            case HirKind::Call:               typedCall(); return;
            case HirKind::Cast:               typedCall(); return;
            case HirKind::Index:              typedCall(); return;
            case HirKind::ConstructAggregate: typedCall(); return;
            case HirKind::Ternary:            typedCall(); return;
            case HirKind::LogicalAnd:         typedCall(); return;
            case HirKind::LogicalOr:          typedCall(); return;
            case HirKind::SizeOf:             typedCall(); return;
            case HirKind::AlignOf:            typedCall(); return;
            case HirKind::VaStart:            typedCall(); return;
            case HirKind::VaArg:              typedCall(); return;
            case HirKind::VaEnd:              typedCall(); return;
            case HirKind::AddressOf:          typedCall(); return;
            case HirKind::Deref:              typedCall(); return;
            case HirKind::LabelAddressOf:
                // D-CSUBSET-COMPUTED-GOTO: `&&label` leaf — render the target label
                // ordinal + type (no operands), mirroring goto/label's `L{ord}` form.
                header();
                out_ += std::format(" L{} : ", hir_.labelAddressOrdinal(id));
                appendType(hir_.typeId(id)); return;
            case HirKind::SeqExpr: {
                // `seq : type { <stmt-lines> yield <resultExpr> }` — the
                // statement children render as normal statement lines; the
                // result (last child) is the yielded value. Mirrors the
                // inline-brace form `error`/`ext_node` use.
                typed(); out_ += " {\n";
                for (HirNodeId s : hir_.seqExprStmts(id)) emitNodeLine(s, 1);
                out_ += indent(1); out_ += "yield "; emitExpr(hir_.seqExprResult(id));
                out_ += "\n}"; return;
            }
            case HirKind::TypeRef:    typed(); return;
            case HirKind::Error: case HirKind::Extension:
                emitExtOrError(id, /*inlineForm=*/true, 0); return;

            // ── NOT an expression: reached only when a caller mis-routes ──
            // ★ SPELLED OUT RATHER THAN LEFT TO `default:`, and that is the
            // second half of the pairing `exprKwForKind` starts. A `default:`
            // here absorbs a NEW `HirKind` silently and renders it `error` — a
            // smaller program with a diagnostic that blames the node instead of
            // this switch. Without one, `-Werror=switch` stops the build until
            // the new kind is classified.
            case HirKind::Module:      case HirKind::Function:
            case HirKind::Global:      case HirKind::TypeDecl:
            case HirKind::ExternFunction: case HirKind::ExternGlobal:
            case HirKind::ImportGroup: case HirKind::Block:
            case HirKind::IfStmt:      case HirKind::WhileStmt:
            case HirKind::DoWhileStmt: case HirKind::ForStmt:
            case HirKind::SwitchStmt:  case HirKind::CaseArm:
            case HirKind::BreakStmt:   case HirKind::ContinueStmt:
            case HirKind::ReturnStmt:  case HirKind::ExprStmt:
            case HirKind::VarDecl:     case HirKind::AssignStmt:
            case HirKind::GotoStmt:    case HirKind::LabelStmt:
            case HirKind::IndirectGotoStmt: case HirKind::SehTryExcept:
            case HirKind::InlineAsm:   case HirKind::Unreachable:
            case HirKind::Count_:
                report(std::format("unexpected node kind '{}' in expression position",
                                   hirKindName(hir_.kind(id))),
                       DiagnosticSeverity::Error);
                out_ += stmtKw(HirTextStmtKw::Error); return;
        }
        // Unreachable: the switch above is total over `HirKind` and an extension
        // node arrives as `HirKind::Extension`, which has an arm.
        report("expression node carries a kind outside the core enum",
               DiagnosticSeverity::Error);
        out_ += stmtKw(HirTextStmtKw::Error);
    }

    // A pool literal's inline value, tagged by variant arm so the parser
    // reconstructs the exact arm: `none` / `bool true` / `int -7` / `uint 42` /
    // `float 3.14` / `str "hi"`. Floats use std::format's shortest round-trip
    // repr (to_chars); strings use the same escaped-quote form as symbol names.
    void appendLiteralValue(HirLiteralValue const& v) {
        if (std::holds_alternative<std::monostate>(v.value)) { out_ += "none"; return; }
        if (auto const* b = std::get_if<bool>(&v.value)) {
            out_ += *b ? "bool true" : "bool false"; return;
        }
        if (auto const* i = std::get_if<std::int64_t>(&v.value)) {
            out_ += std::format("int {}", *i); return;
        }
        if (auto const* u = std::get_if<std::uint64_t>(&v.value)) {
            out_ += std::format("uint {}", *u); return;
        }
        if (auto const* d = std::get_if<double>(&v.value)) {
            // std::format renders non-finite doubles as `inf`/`-inf`/`nan`, which
            // takeFloat() accepts back — so they round-trip too (e.g. synthetic
            // HIR from constant folding).
            out_ += "float "; out_ += std::format("{}", *d); return;
        }
        if (auto const* s = std::get_if<std::string>(&v.value)) {
            out_ += "str "; out_ += quote(*s); return;
        }
        if (auto const* a = std::get_if<HirAddressValue>(&v.value)) {
            // c43 address constant: `addr <base> <byteOffset>` (pointeeType is
            // fold-transient and not round-tripped — invalid on the parsed value).
            out_ += std::format("addr {} {}", a->base, a->byteOffset); return;
        }
        if (auto const* bi = std::get_if<BitIntValue>(&v.value)) {
            // C4b (I5) `_BitInt` value: `bitint <width> <signed 0|1> <nLimbs> <limb…>`
            // (little-endian decimal limbs) — a lossless round-trip of the host
            // bit-precise value through the `.dsshir`/`.dssir` text format.
            out_ += std::format("bitint {} {} {}", bi->width(),
                                bi->isSigned() ? 1 : 0, bi->limbs().size());
            for (std::uint64_t l : bi->limbs()) out_ += std::format(" {}", l);
            return;
        }
        if (auto const* wf = std::get_if<WideFloatValue>(&v.value)) {
            // LD-3 (D-CSUBSET-LONG-DOUBLE-CONSTFOLD-PRECISION) folded F80/F128 value:
            // `wfloat <bits> <hi> <lo>` — the format bit-width (80|128, a STABLE
            // semantic discriminator, NOT the version-fragile TypeKind ordinal) plus
            // the pack() bit pattern, read back via WideFloatValue::fromPacked (a
            // lossless bit-exact round-trip; pack ∘ fromPacked is identity, pinned in
            // test_wide_float_value).
            WideFloatValue::Packed const p = wf->pack();
            int const bits = (wf->kind() == TypeKind::F128) ? 128 : 80;
            out_ += std::format("wfloat {} {} {}", bits, p.hi, p.lo);
            return;
        }
        // ★★★ THE ARM THAT USED TO FALL OFF THE END. Nine of `HirLiteralValue`'s
        // TEN variant arms were spelled above; the tenth — `HirAggregateValue`,
        // the D5.3 folded struct/union/array constant — had no spelling at all,
        // and with no final arm here it rendered NOTHING: `lit ` followed straight
        // by the type annotation, the whole constant dropped, no diagnostic on
        // either side. Cycle P23 made that LOUD (a named marker the reader refused
        // by name); what was still missing was the CAPABILITY
        // (D-HIR-TEXT-WRITER-DROPS-THE-AGGREGATE-LITERAL-ARM).
        //
        // ★★ THE SPELLING IS THE MIR TIER'S, `agg { … }`, AND THAT IS THE WHOLE
        // POINT OF THE ROW. The two text tiers of one compiler disagreeing about
        // what is serializable was the defect; inventing a second syntax for the
        // same folded constant would have kept the disagreement and merely made it
        // symmetrical. `mir_text.cpp`'s `appendLiteral` already writes `agg {`,
        // comma-separated fields, `}` — and its comment for the shared `bitint`
        // arm states the rule this follows: the two pools hold the SAME host
        // value, so a second syntax for it would be a second owner of one
        // serialization.
        //
        // ⚠ EACH FIELD CARRIES ITS OWN `: <core>`, WHICH THE TOP-LEVEL VALUE DOES
        // NOT. A top-level literal's `core` is RECOMPUTED by the reader from the
        // node's type annotation (`literalCoreFor`), and a nested field has no type
        // annotation — there is nothing to recompute it from. Without the per-field
        // core every element of a folded aggregate would come back `Void`: a
        // round trip that is byte-stable on re-emit and LOSSY in the pool, which
        // is the failure mode hardest to see from the text alone.
        if (auto const* agg = std::get_if<HirAggregateValue>(&v.value)) {
            out_ += "agg {";
            bool first = true;
            for (HirLiteralValue const& f : agg->fields) {
                if (!first) out_ += ", ";
                appendLiteralValue(f);
                out_ += " : ";
                // ⚠ `literalCoreName`, NOT `primName`: a NESTED aggregate field's
                // own core is `Struct`/`Union`/`Array`, which the type-keyword
                // table deliberately omits. ✔MEASURED while writing the nesting
                // pin — a `primName`-only writer rendered the inner aggregate's
                // core as `?` and the round trip refused its own output.
                std::string_view const core = literalCoreName(f.core);
                if (core.empty()) {
                    // Same discipline as the enum-underlying arm: name the kind
                    // that has no spelling rather than emitting a plausible one.
                    report(std::format(
                        "aggregate literal field has core TypeKind ordinal {}, "
                        "which this format has no spelling for; rendered as '?', "
                        "which the reader REFUSES — accepted: {}",
                        static_cast<std::uint32_t>(f.core), literalCoreAccepted()),
                        DiagnosticSeverity::Error);
                    out_ += '?';
                } else {
                    out_ += core;
                }
                first = false;
            }
            out_ += '}';
            return;
        }
        // ★ THE MARKER STAYS, AND IT IS NOT DEAD CODE. Every arm above returns, so
        // reaching here means `HirLiteralValue` grew an arm this writer does not
        // spell. The `static_assert` below FAILS THE BUILD on exactly that, so this
        // is the runtime backstop for the window in which someone silences the
        // assert by bumping the count without writing the arm — which is the one
        // edit that turns a compile error back into a silent drop.
        //
        // ONE TOKEN AND IDENTIFIER-SHAPED, for the reason the MIR emitter's
        // `<asm-descriptor-unspelled>` marker states FROM MEASUREMENT: the parser's
        // recovery re-tokenizes what follows a refusal, and prose in the output can
        // hand it a real keyword. Identifier-shaped (not `<…>`, which this lexer
        // splits into seven tokens) so it arrives at `parseLiteralValue`'s tag
        // dispatch as ONE token and is refused THERE, by name, with the reason.
        report(std::format(
            "this format has no spelling for this literal value; rendered as '{}', "
            "which the reader REFUSES", kHirTextUnspelledAggregateTag),
            DiagnosticSeverity::Error);
        out_ += kHirTextUnspelledAggregateTag;
    }

    // The count is the guard the chain above cannot give itself: every arm returns,
    // so "did I cover them all" is not something the compiler checks. Adding an arm
    // to `HirLiteralValue::value` now FAILS THE BUILD here, naming this writer,
    // instead of serializing the new value as nothing.
    static_assert(std::variant_size_v<decltype(HirLiteralValue::value)> == 10,
                  "HirLiteralValue gained (or lost) a variant arm — add its "
                  "spelling to appendLiteralValue AND its tag to "
                  "parseLiteralValue, then update this count");

    void appendOpName(std::uint32_t payload) {
        if (isCoreOp(payload)) out_ += opName(decodeCoreOp(payload));
        else { out_ += "ext "; out_ += quote(hir_.opRegistry().descriptor(decodeExtOp(payload)).name()); }
    }

    // ── preamble sections ────────────────────────────────────────────────────
    void emitExtKinds() {
        auto ext = hir_.registry().extensions();
        if (ext.empty()) return;
        out_ += "ext_kinds {\n";
        for (auto const& d : ext) {
            out_ += "  "; out_ += quote(d.name());
            out_ += " lang "; out_ += quote(d.sourceLanguage()); out_ += '\n';
        }
        out_ += "}\n";
    }
    void emitExtOps() {
        auto ext = hir_.opRegistry().extensions();
        if (ext.empty()) return;
        out_ += "ext_ops {\n";
        for (auto const& d : ext) {
            out_ += "  "; out_ += quote(d.name());
            out_ += (d.arity() == HirOpArity::Binary) ? " binary" : " unary";
            out_ += " lang "; out_ += quote(d.sourceLanguage()); out_ += '\n';
        }
        out_ += "}\n";
    }
    void emitIntrinsics() {
        auto ins = hir_.intrinsicRegistry().intrinsics();
        if (ins.empty()) return;
        out_ += "intrinsics {\n";
        for (auto const& d : ins) {
            out_ += "  "; out_ += quote(d.name());
            out_ += " lang "; out_ += quote(d.sourceLanguage()); out_ += '\n';
        }
        out_ += "}\n";
    }
    void emitSymbols() {
        if (symOrder_.empty()) return;
        out_ += "symbols {\n";
        for (std::uint32_t symv : symOrder_) {
            std::string_view name;
            if (ctx_.symbolNames && symv < ctx_.symbolNames->size()) name = (*ctx_.symbolNames)[symv];
            out_ += std::format("  %{} ", handleOf(symv));
            out_ += quote(name); out_ += '\n';
        }
        out_ += "}\n";
    }
};

} // namespace

std::string emitHir(Hir const& hir, HirTextContext const& ctx, DiagnosticReporter& reporter) {
    return Emitter{hir, ctx, reporter}.run();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Parser
// ─────────────────────────────────────────────────────────────────────────────

namespace {

enum class Tk : std::uint8_t {
    Eof, Unknown, Ident, Int, Float, Str,
    LBrace, RBrace, LParen, RParen, LAngle, RAngle, LBrack, RBrack,
    Colon, Comma, Percent, Hash, Equal, Arrow, Minus, DotDot, Ellipsis, At, Tilde,
};

struct Tok {
    Tk          kind = Tk::Eof;
    std::string text;          // ident/str content (unescaped) or int/float digits
    std::uint64_t num = 0;     // for Int
    double        dnum = 0;    // for Float
    bool          overflow = false;  // Int: digits exceeded uint64 (→ malformed)
    std::uint32_t off = 0;     // byte offset (diagnostics)
};

class Lexer {
public:
    explicit Lexer(std::string_view src) : s_(src) { advance(); }

    [[nodiscard]] Tok const& peek() const { return cur_; }
    Tok take() { Tok t = std::move(cur_); advance(); return t; }

private:
    std::string_view s_;
    std::size_t      p_ = 0;
    Tok              cur_;

    void advance() {
        skipTrivia();
        cur_ = Tok{};
        cur_.off = static_cast<std::uint32_t>(p_);
        if (p_ >= s_.size()) { cur_.kind = Tk::Eof; return; }
        char const c = s_[p_];
        if (isIdentStart(c)) { lexIdent(); return; }
        if (isDigit(c))      { lexInt(); return; }
        if (c == '"')        { lexStr(); return; }
        // punctuation
        ++p_;
        switch (c) {
            case '{': cur_.kind = Tk::LBrace; return;
            case '}': cur_.kind = Tk::RBrace; return;
            case '(': cur_.kind = Tk::LParen; return;
            case ')': cur_.kind = Tk::RParen; return;
            case '<': cur_.kind = Tk::LAngle; return;
            case '>': cur_.kind = Tk::RAngle; return;
            case '[': cur_.kind = Tk::LBrack; return;
            case ']': cur_.kind = Tk::RBrack; return;
            case ':': cur_.kind = Tk::Colon; return;
            case ',': cur_.kind = Tk::Comma; return;
            case '%': cur_.kind = Tk::Percent; return;
            case '#': cur_.kind = Tk::Hash; return;
            case '@': cur_.kind = Tk::At; return;
            case '~': cur_.kind = Tk::Tilde; return;
            case '=': cur_.kind = Tk::Equal; return;
            case '-':
                if (p_ < s_.size() && s_[p_] == '>') { ++p_; cur_.kind = Tk::Arrow; }
                else cur_.kind = Tk::Minus;
                return;
            case '.':
                if (p_ < s_.size() && s_[p_] == '.') {
                    ++p_;   // consumed the 2nd dot
                    // Three dots `...` = the variadic marker in a `fn(...)` type; two
                    // dots `..` stays DotDot (span ranges). Distinct lexical contexts.
                    if (p_ < s_.size() && s_[p_] == '.') { ++p_; cur_.kind = Tk::Ellipsis; return; }
                    cur_.kind = Tk::DotDot; return;
                }
                cur_.kind = Tk::Unknown; return;   // stray '.' — distinct from EOF so it can't truncate the parse
            default: cur_.kind = Tk::Unknown; return;  // unknown byte; the parser reports + recovers, never mistakes it for EOF
        }
    }

    static bool isIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
    static bool isIdentCont(char c) { return isIdentStart(c) || isDigit(c) || c == '.'; }
    static bool isDigit(char c) { return c >= '0' && c <= '9'; }

    void skipTrivia() {
        for (;;) {
            while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\n' || s_[p_] == '\r')) ++p_;
            if (p_ + 1 < s_.size() && s_[p_] == '/' && s_[p_ + 1] == '/') {
                p_ += 2;
                while (p_ < s_.size() && s_[p_] != '\n') ++p_;
                continue;
            }
            break;
        }
    }
    void lexIdent() {
        cur_.kind = Tk::Ident;
        std::size_t const start = p_;
        // identifiers may carry '.' (e.g. intrinsic-like keywords aren't used, but
        // type/builtin names stay quoted, so '.' here is harmless and unused).
        while (p_ < s_.size() && isIdentCont(s_[p_])) ++p_;
        cur_.text.assign(s_.substr(start, p_ - start));
    }
    void lexInt() {
        std::size_t const start = p_;
        std::uint64_t v = 0;
        bool overflow = false;
        while (p_ < s_.size() && isDigit(s_[p_])) {
            std::uint64_t const d = static_cast<std::uint64_t>(s_[p_] - '0');
            if (v > (std::numeric_limits<std::uint64_t>::max() - d) / 10) overflow = true;
            v = v * 10 + d;
            ++p_;
        }
        // Float: a fractional part (`.` + digit) and/or an exponent (`e`/`E`)
        // following the integer digits promote this to a Float token. (A bare
        // trailing `.` is NOT consumed — that's the `..` range or a stray dot.)
        bool isFloat = false;
        if (p_ + 1 < s_.size() && s_[p_] == '.' && isDigit(s_[p_ + 1])) {
            isFloat = true;
            p_ += 2;
            while (p_ < s_.size() && isDigit(s_[p_])) ++p_;
        }
        if (p_ < s_.size() && (s_[p_] == 'e' || s_[p_] == 'E')) {
            std::size_t q = p_ + 1;
            if (q < s_.size() && (s_[q] == '+' || s_[q] == '-')) ++q;
            if (q < s_.size() && isDigit(s_[q])) {
                isFloat = true;
                p_ = q + 1;
                while (p_ < s_.size() && isDigit(s_[p_])) ++p_;
            }
        }
        cur_.text.assign(s_.substr(start, p_ - start));
        if (isFloat) {
            cur_.kind = Tk::Float;
            char* end = nullptr;
            cur_.dnum = std::strtod(cur_.text.c_str(), &end);  // exact for to_chars output
            (void)end;
        } else {
            cur_.kind = Tk::Int;
            cur_.num  = v;
            cur_.overflow = overflow;   // surfaced by takeInt → H_TextMalformed
        }
    }
    void lexStr() {
        cur_.kind = Tk::Str;
        ++p_;  // opening quote
        std::string out;
        while (p_ < s_.size() && s_[p_] != '"') {
            char c = s_[p_++];
            if (c == '\\' && p_ < s_.size()) c = s_[p_++];  // \" and \\ (and any escaped char)
            out += c;
        }
        if (p_ < s_.size()) ++p_;  // closing quote
        cur_.text = std::move(out);
    }
};

// Attributes parsed ahead of a node (the node id is known only after building).
struct PendingAttrs {
    std::optional<HirSourceLoc>  loc;
    std::optional<FfiMetadata>   ffi;
    std::optional<ShaderIntrinsic> shader;
    std::optional<TranspileHint> transpile;
    std::optional<DiagnosticInfo> diag;
    bool          diagHasOrigin = false;
    std::uint32_t diagOriginPre = 0;   // pre-order index of the origin node
};

class Parser {
public:
    // Module path: the Parser OWNS a fresh interner/registry (tagged with `cuId`),
    // and the `interner_`/`typeReg_` references bind to them. `parseHir` later
    // moves the owned interner out of `ownedInterner_` into the HirParseResult.
    Parser(std::string_view text, CompilationUnitId cuId, DiagnosticReporter& reporter)
        : ownedInterner_(std::in_place, cuId), ownedTypeReg_(std::in_place),
          interner_(*ownedInterner_), typeReg_(*ownedTypeReg_),
          lex_(text), reporter_(reporter) {}

    // Type-text path (`parseTypeFromText`): the caller OWNS the interner/registry,
    // so the owned storage stays empty and the references bind to the externals —
    // the produced TypeId interns into the caller's CU. The same `parseType`
    // production runs unchanged; only where it interns differs.
    Parser(std::string_view text, TypeInterner& extInterner, TypeRegistry& extTypeReg,
           DiagnosticReporter& reporter)
        : interner_(extInterner), typeReg_(extTypeReg), lex_(text), reporter_(reporter) {}

    // c82 (D-FFI-DESCRIPTOR-VA-LIST-TYPE): caller-supplied NAME → TypeId
    // bindings for the type-text path (see parseTypeFromText's doc). Consulted
    // by `parseType`'s identifier fallback — AFTER every structural keyword —
    // so a binding can never shadow the grammar. The span's storage outlives
    // the parse (the caller holds it across the parseTypeFromText call).
    void setNamedTypes(std::span<NamedTypeBinding const> namedTypes) {
        namedTypes_ = namedTypes;
    }

    HirBuilder              builder_;
    // Owned only on the module path (empty on the type-text path); the references
    // below are the single point of use for every production.
    std::optional<TypeInterner> ownedInterner_;
    std::optional<TypeRegistry> ownedTypeReg_;
    TypeInterner&           interner_;
    TypeRegistry&           typeReg_;
    std::vector<std::string> symbolNames_;     // SymbolId.v -> name; slot 0 unused

    std::vector<std::pair<HirNodeId, HirSourceLoc>>   pLoc_;
    std::vector<std::pair<HirNodeId, FfiMetadata>>    pFfi_;
    std::vector<std::pair<HirNodeId, ShaderIntrinsic>> pShader_;
    std::vector<std::pair<HirNodeId, TranspileHint>>  pTranspile_;
    struct DiagPend { HirNodeId node; DiagnosticInfo info; bool hasOrigin; std::uint32_t originPre; };
    std::vector<DiagPend> pDiag_;
    std::vector<HirNodeId> indexToId_;          // pre-order index -> built node
    HirLiteralPool        pLiterals_;           // values from inline `lit <value>` forms
    // Inline-asm P5: descriptors rebuilt from the inline `inline_asm ...` form.
    // Handles are re-minted IN TREE ORDER by `parseInlineAsm`, which is what
    // makes emit(parse(emit(h))) byte-identical without printing the handle.
    HirInlineAsmPool      pInlineAsm_;

    // Parse the whole file. Returns the module root (invalid on a fatal header
    // error). Populates the builder, interner, side-table pending lists.
    [[nodiscard]] HirNodeId parse() {
        // header: "dsshir" INT
        if (!acceptKeyword("dsshir")) { malformed("expected 'dsshir' header"); return InvalidHirNode; }
        if (lex_.peek().kind != Tk::Int) { malformed("expected format version"); return InvalidHirNode; }
        std::uint64_t const version = lex_.take().num;
        if (version != 1) {
            ParseDiagnostic d; d.code = DiagnosticCode::H_TextVersionMismatch;
            d.severity = DiagnosticSeverity::Error;
            d.actual = std::format("dsshir format version {} (this build understands 1)", version);
            reporter_.report(std::move(d));
            return InvalidHirNode;
        }
        parsePreamble();
        return parseModule();
    }

    // Entry for `parseTypeFromText`: decode a STANDALONE type string via the same
    // `parseType` production used by the module parser, then require the input to
    // be exhausted (no second type, no stray tokens). The grammar itself is the
    // private `parseType` below — this is only the single-type framing.
    [[nodiscard]] TypeId parseTypeFromTextEntry() {
        TypeId const t = parseType();
        if (!peekIs(Tk::Eof)) malformed("unexpected trailing tokens after type");
        return t;
    }

private:
    Lexer               lex_;
    DiagnosticReporter& reporter_;
    std::unordered_map<std::string, HirKindId>      extKindByName_;
    std::unordered_map<std::string, HirOpId>        extOpByName_;
    std::unordered_map<std::string, HirIntrinsicId> intrinsicByName_;
    // c82: caller-supplied identifier→TypeId aliases (type-text path only;
    // empty on the module path). Storage owned by the caller.
    std::span<NamedTypeBinding const> namedTypes_;
    std::uint32_t preCounter_ = 0;

    // ── token helpers ────────────────────────────────────────────────────────
    [[nodiscard]] Tk peekKind() const { return lex_.peek().kind; }
    [[nodiscard]] bool peekIs(Tk k) const { return lex_.peek().kind == k; }
    [[nodiscard]] bool peekKeyword(std::string_view kw) const {
        return lex_.peek().kind == Tk::Ident && lex_.peek().text == kw;
    }
    bool accept(Tk k) { if (peekIs(k)) { lex_.take(); return true; } return false; }
    bool acceptKeyword(std::string_view kw) { if (peekKeyword(kw)) { lex_.take(); return true; } return false; }
    void expect(Tk k, char const* what) { if (!accept(k)) malformed(std::format("expected {}", what)); }
    [[nodiscard]] std::string takeIdent() {
        if (peekIs(Tk::Ident)) return lex_.take().text;
        malformed("expected identifier"); return {};
    }
    // c60 (Design I-A): parse a `L<ordinal>` label reference (the form goto/label/
    // labeladdr render and the switch dispatch arms use). One `Ident` token whose
    // text is `L` followed by decimal digits.
    [[nodiscard]] std::uint32_t parseLabelOrdinal() {
        if (!peekIs(Tk::Ident)) { malformed("expected a label ordinal 'L<n>'"); return 0; }
        std::string const t = lex_.take().text;
        if (t.size() < 2 || t[0] != 'L') {
            malformed(std::format("expected a label ordinal 'L<n>', got '{}'", t));
            return 0;
        }
        std::uint32_t ord = 0;
        for (std::size_t i = 1; i < t.size(); ++i) {
            if (t[i] < '0' || t[i] > '9') {
                malformed(std::format("malformed label ordinal '{}'", t));
                return 0;
            }
            ord = ord * 10u + static_cast<std::uint32_t>(t[i] - '0');
        }
        return ord;
    }
    [[nodiscard]] std::string takeStr() {
        if (peekIs(Tk::Str)) return lex_.take().text;
        malformed("expected string literal"); return {};
    }
    [[nodiscard]] std::uint64_t takeInt() {
        if (peekIs(Tk::Int)) {
            Tok t = lex_.take();
            if (t.overflow) malformed(std::format("integer literal '{}' exceeds 64 bits", t.text));
            return t.num;
        }
        malformed("expected integer"); return 0;
    }
    // A float value accepts Float (`3.14`), Int (`42` — a whole-valued double
    // std::format rendered without a point), and the `inf`/`nan` idents
    // std::format emits for non-finite doubles (so synthetic HIR round-trips).
    [[nodiscard]] double takeFloat() {
        if (peekIs(Tk::Float)) return lex_.take().dnum;
        if (peekIs(Tk::Int))   return static_cast<double>(lex_.take().num);
        if (peekIs(Tk::Ident)) {
            std::string const& t = lex_.peek().text;
            if (t == "inf") { lex_.take(); return std::numeric_limits<double>::infinity(); }
            if (t == "nan") { lex_.take(); return std::numeric_limits<double>::quiet_NaN(); }
        }
        malformed("expected float"); return 0.0;
    }
    // Parse a tagged inline literal value (the `lit <tag> <value>` form).
    [[nodiscard]] HirLiteralValue parseLiteralValue() {
        HirLiteralValue v;
        std::string const tag = takeIdent();
        if (tag == "none") { /* monostate */ }
        else if (tag == "bool") {
            std::string const b = takeIdent();
            if (b == "true")       v.value = true;
            else if (b == "false") v.value = false;
            else malformed(std::format("expected 'true' or 'false', got '{}'", b));
        }
        else if (tag == "int")  {
            bool n = accept(Tk::Minus);
            std::uint64_t u = takeInt();
            // Negate in the unsigned domain (forming -INT64_MIN as a signed op
            // is UB); the two's-complement cast back is well-defined in C++20.
            v.value = n ? static_cast<std::int64_t>(0u - u) : static_cast<std::int64_t>(u);
        }
        else if (tag == "uint") { v.value = takeInt(); }
        else if (tag == "float"){ bool n = accept(Tk::Minus); double d = takeFloat();
                                  v.value = n ? -d : d; }
        else if (tag == "str")  { v.value = takeStr(); }
        else if (tag == "addr") {
            // c43 address constant: `addr <base> <byteOffset>` (signed offset).
            HirAddressValue a;
            a.base = static_cast<std::uint32_t>(takeInt());
            bool const n = accept(Tk::Minus);
            std::uint64_t const off = takeInt();
            a.byteOffset = n ? static_cast<std::int64_t>(0u - off)
                             : static_cast<std::int64_t>(off);
            v.value = std::move(a);
        }
        else if (tag == "bitint") {
            // C4b (I5): `bitint <width> <signed 0|1> <nLimbs> <limb…>` — the inverse
            // of appendLiteralValue's serialization (the BitIntValue ctor re-wraps).
            std::uint64_t const width  = takeInt();
            std::uint64_t const sgn    = takeInt();
            std::uint64_t const nLimbs = takeInt();
            std::vector<std::uint64_t> limbs;
            limbs.reserve(static_cast<std::size_t>(nLimbs));
            for (std::uint64_t i = 0; i < nLimbs; ++i) limbs.push_back(takeInt());
            v.value = BitIntValue(std::move(limbs),
                                  static_cast<std::uint32_t>(width), sgn != 0);
        }
        else if (tag == "wfloat") {
            // LD-3: `wfloat <bits> <hi> <lo>` — the inverse of appendLiteralValue's
            // pack() serialization; the bit-width (80|128) selects the F80 vs F128
            // unpack layout via WideFloatValue::fromPacked.
            std::uint64_t const bits = takeInt();
            std::uint64_t const hi   = takeInt();
            std::uint64_t const lo   = takeInt();
            TypeKind const k = (bits == 128) ? TypeKind::F128 : TypeKind::F80;
            v.value = WideFloatValue::fromPacked(lo, hi, k);
        }
        else if (tag == "agg") {
            // D-HIR-TEXT-WRITER-DROPS-THE-AGGREGATE-LITERAL-ARM: the inverse of
            // `appendLiteralValue`'s aggregate arm, and the SAME syntax
            // `mir_text.cpp`'s `parseLiteral` reads — `agg { <field>, … }`, each
            // field a tagged value followed by `: <core>`.
            //
            // ⚠ THE PER-FIELD `: <core>` IS PARSED HERE AND NOWHERE ELSE. The
            // top-level `literalCoreFor` recomputation works off the node's type
            // annotation, which a nested field does not have; dropping the core
            // here would make every element of a folded aggregate come back
            // `Void` while the re-emitted text still matched byte for byte.
            expect(Tk::LBrace, "'{'");
            HirAggregateValue agg;
            while (!peekIs(Tk::RBrace) && !peekIs(Tk::Eof)) {
                if (!agg.fields.empty() && !accept(Tk::Comma)) break;
                if (peekIs(Tk::RBrace)) break;
                HirLiteralValue f = parseLiteralValue();
                expect(Tk::Colon, "':'");
                std::string const core = takeIdent();
                // Resolved through BOTH owning tables, and refused by name with
                // the union as the accepted set — the twin of `mir_text.cpp`'s
                // `literalCoreFromName` / `literalCoreAccepted` pair.
                if (auto const k = literalCoreFromName(core); k.has_value()) {
                    f.core = *k;
                } else {
                    malformed(std::format(
                        "unknown aggregate literal field core '{}' — accepted: {}",
                        core, literalCoreAccepted()));
                }
                agg.fields.push_back(std::move(f));
            }
            expect(Tk::RBrace, "'}'");
            v.value = std::move(agg);
        }
        else if (tag == kHirTextUnspelledAggregateTag) {
            // The writer's own marker for a value it could not serialize. Refused
            // BY NAME with the reason, rather than falling into the generic
            // "unknown tag" arm: the author of this text did not typo a tag, they
            // hit a stated limit of the format, and a reader that cannot tell those
            // apart sends them looking for a spelling error that is not there.
            //
            // ⓘ It no longer means "aggregate" — the aggregate arm above has a real
            // spelling now. It means the writer met a `HirLiteralValue` arm it does
            // not render, which is a defect in the WRITER rather than a limit of
            // the format, and the sentence says so.
            malformed(std::format(
                "'{}' marks a literal value the .dsshir writer had no spelling for "
                "— the value was NOT serialized, so this text cannot be read back "
                "into the module it came from. Every arm of HirLiteralValue is "
                "supposed to have a spelling; reaching this marker means one was "
                "added to the variant without one",
                kHirTextUnspelledAggregateTag));
        }
        else malformed(std::format("unknown literal value tag '{}'", tag));
        return v;
    }
    // The pool `core` for a parsed value: a string literal's element core is the
    // ELEMENT of its `Array<core,N+1>` node type — Char for a narrow `"…"`, but
    // U16/U32/U8 for a C11/C23 6.4.5 wide/UTF literal (`u"…"`/`U"…"`/`u8"…"`), so it
    // is read off the array element, NOT hardcoded (a hardcode would re-emit a wide
    // literal as Char and silently mis-round-trip its width). A bool is Bool;
    // monostate is Void; everything else mirrors the node's resolved type kind.
    [[nodiscard]] TypeKind literalCoreFor(TypeId t, HirLiteralValue const& v) const {
        if (std::holds_alternative<std::string>(v.value)) {
            if (t.valid() && interner_.kind(t) == TypeKind::Array) {
                if (auto const ops = interner_.operands(t); !ops.empty() && ops[0].valid())
                    return interner_.kind(ops[0]);
            }
            return TypeKind::Char;   // untyped / non-array fallback (narrow default)
        }
        if (std::holds_alternative<bool>(v.value))          return TypeKind::Bool;
        if (std::holds_alternative<std::monostate>(v.value)) return TypeKind::Void;
        return t.valid() ? interner_.kind(t) : TypeKind::Void;
    }
    void malformed(std::string detail) {
        ParseDiagnostic d; d.code = DiagnosticCode::H_TextMalformed;
        d.severity = DiagnosticSeverity::Error;
        d.span = SourceSpan::empty(lex_.peek().off);
        d.actual = std::move(detail);
        reporter_.report(std::move(d));
    }
    void unknownName(std::string detail) {
        ParseDiagnostic d; d.code = DiagnosticCode::H_TextUnknownName;
        d.severity = DiagnosticSeverity::Error;
        d.span = SourceSpan::empty(lex_.peek().off);
        d.actual = std::move(detail);
        reporter_.report(std::move(d));
    }
    // Resolve a parsed enum name against the vocabulary's OWN table; report (not
    // silently default) on a miss so an unrecognized token can never coerce to a
    // wrong value and silently diverge on re-emit. Mirrors parseOp's
    // unknown-operator handling.
    //
    // ★ THE TABLE IS THE PARAMETER, NOT AN ALREADY-RESOLVED `std::optional`.
    // This used to take the result of a hand-written `…FromName` if-chain and
    // report `unknown {what} '{name}'` — a refusal that named NO ACCEPTED SET AT
    // ALL, so an author holding a `.dsshir` with a typo'd attribute learned only
    // that their name was wrong, never what the reader would have taken. Passing
    // the TABLE makes the lookup and the advertised set come off the same rows,
    // so the message cannot be narrower, wider or staler than the check: there
    // is no second copy of the set to go stale
    // (D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET).
    template <class E, std::size_t N>
    [[nodiscard]] E orMalformed(EnumNameTable<E, N> const& table,
                                std::string_view name, char const* what, E dflt) {
        if (auto const v = table.fromName(name)) return *v;
        malformed(std::format("unknown {} '{}' — accepted: {}", what, name,
                              detail::renderAllowedList(allNames(table))));
        return dflt;
    }
    void recordIndex(std::uint32_t idx, HirNodeId id) {
        if (idx >= indexToId_.size()) indexToId_.resize(idx + 1);
        indexToId_[idx] = id;
    }

    // ── preamble ───────────────────────────────────────────────────────────
    void parsePreamble() {
        for (;;) {
            if (acceptKeyword("ext_kinds")) parseExtKinds();
            else if (acceptKeyword("ext_ops")) parseExtOps();
            else if (acceptKeyword("intrinsics")) parseIntrinsics();
            else if (acceptKeyword("symbols")) parseSymbols();
            else break;
        }
    }
    void parseExtKinds() {
        expect(Tk::LBrace, "'{'");
        while (!peekIs(Tk::RBrace) && !peekIs(Tk::Eof)) {
            std::uint32_t const off = cursorOff();
            std::string name = takeStr();
            if (!acceptKeyword("lang")) malformed("expected 'lang'");
            std::string lang = takeStr();
            HirKindId id = builder_.registry().registerExtension(name, lang);
            extKindByName_.emplace(std::move(name), id);
            if (cursorOff() == off) lex_.take();  // progress guard
        }
        expect(Tk::RBrace, "'}'");
    }
    void parseExtOps() {
        expect(Tk::LBrace, "'{'");
        while (!peekIs(Tk::RBrace) && !peekIs(Tk::Eof)) {
            std::uint32_t const off = cursorOff();
            std::string name = takeStr();
            HirOpArity arity = HirOpArity::Binary;
            if (acceptKeyword("binary")) arity = HirOpArity::Binary;
            else if (acceptKeyword("unary")) arity = HirOpArity::Unary;
            else malformed("expected 'binary' or 'unary'");
            if (!acceptKeyword("lang")) malformed("expected 'lang'");
            std::string lang = takeStr();
            HirOpId id = builder_.opRegistry().registerExtension(name, arity, lang);
            extOpByName_.emplace(std::move(name), id);
            if (cursorOff() == off) lex_.take();  // progress guard
        }
        expect(Tk::RBrace, "'}'");
    }
    void parseIntrinsics() {
        expect(Tk::LBrace, "'{'");
        while (!peekIs(Tk::RBrace) && !peekIs(Tk::Eof)) {
            std::uint32_t const off = cursorOff();
            std::string name = takeStr();
            if (!acceptKeyword("lang")) malformed("expected 'lang'");
            std::string lang = takeStr();
            HirIntrinsicId id = builder_.intrinsicRegistry().registerIntrinsic(name, lang);
            intrinsicByName_.emplace(std::move(name), id);
            if (cursorOff() == off) lex_.take();  // progress guard
        }
        expect(Tk::RBrace, "'}'");
    }
    void parseSymbols() {
        expect(Tk::LBrace, "'{'");
        while (!peekIs(Tk::RBrace) && !peekIs(Tk::Eof)) {
            std::uint32_t const off = cursorOff();
            expect(Tk::Percent, "'%'");
            std::uint64_t const handle = takeInt();
            std::string name = takeStr();
            if (handle >= symbolNames_.size()) symbolNames_.resize(handle + 1);
            symbolNames_[handle] = std::move(name);
            if (cursorOff() == off) lex_.take();  // progress guard
        }
        expect(Tk::RBrace, "'}'");
    }

    // ── module ───────────────────────────────────────────────────────────────
    [[nodiscard]] HirNodeId parseModule() {
        if (!acceptKeyword("module")) { malformed("expected 'module'"); return InvalidHirNode; }
        std::uint32_t const idx = preCounter_++;
        HirFlags const flags = parseFlags();
        std::string lang = takeStr();
        builder_.setSourceLanguage(std::move(lang));  // frozen into Hir at finish()
        expect(Tk::LBrace, "'{'");
        std::vector<HirNodeId> decls;
        while (!peekIs(Tk::RBrace) && !peekIs(Tk::Eof)) {
            std::uint32_t const off = cursorOff();
            decls.push_back(parseNode());
            if (cursorOff() == off) lex_.take();  // progress guard
        }
        expect(Tk::RBrace, "'}'");
        HirNodeId const root = builder_.makeModule(decls, flags);
        recordIndex(idx, root);
        return root;
    }

    // ── flags / attrs ──────────────────────────────────────────────────────
    // D-TEXT-TIER-REFUSALS-NAME-NO-ACCEPTED-SET: `unknown flag 'foo'` told an
    // author their word was wrong and never what the four accepted words are.
    // Resolved through `kHirTextFlagTable`, which `flagsStr` also renders from,
    // so the refusal's list and the writer's spellings are the same rows.
    //
    // ⓘ `HirFlags::None` as the miss value is inert by construction: `|=` of the
    // zero bit changes nothing, and `orMalformed` has already set the parse's
    // error state, so no flag is silently invented.
    [[nodiscard]] HirFlags parseFlags() {
        HirFlags f = HirFlags::None;
        if (!accept(Tk::LBrack)) return f;
        while (!peekIs(Tk::RBrack) && !peekIs(Tk::Eof)) {
            std::string n = takeIdent();
            f |= orMalformed(kHirTextFlagTable, n, "node flag", HirFlags::None);
            if (!accept(Tk::Comma)) break;
        }
        expect(Tk::RBrack, "']'");
        return f;
    }

    // The `@`-attribute dispatch, keyed on `kHirTextAttrKindTable` so the refusal
    // names the accepted set and `-Werror=switch` refuses a table row nobody
    // handles. ⓘ `skipToRParen` still runs on a miss — the parse is collect-all
    // and the unrecognized attribute's body has to be stepped over — but the
    // decision to skip it is now made by a lookup failure rather than by falling
    // off the end of a ladder.
    [[nodiscard]] PendingAttrs parseAttrs() {
        PendingAttrs a;
        while (accept(Tk::At)) {
            std::string kind = takeIdent();
            expect(Tk::LParen, "'('");
            auto const which = kHirTextAttrKindTable.fromName(kind);
            if (!which.has_value()) {
                malformed(std::format(
                    "unknown attribute '@{}' — accepted: {}", kind,
                    detail::renderAllowedList(allNames(kHirTextAttrKindTable))));
                skipToRParen();
            } else {
                switch (*which) {
                    case HirTextAttrKind::Loc:       a.loc = parseLoc();             break;
                    case HirTextAttrKind::Ffi:       a.ffi = parseFfi();             break;
                    case HirTextAttrKind::Shader:    a.shader = parseShader();       break;
                    case HirTextAttrKind::Transpile: a.transpile = parseTranspile(); break;
                    case HirTextAttrKind::Diag:      parseDiag(a);                   break;
                }
            }
            expect(Tk::RParen, "')'");
        }
        return a;
    }
    void skipToRParen() { while (!peekIs(Tk::RParen) && !peekIs(Tk::Eof)) lex_.take(); }

    [[nodiscard]] HirSourceLoc parseLoc() {
        if (!acceptKeyword("buf")) malformed("expected 'buf'");
        std::uint32_t buf = static_cast<std::uint32_t>(takeInt());
        expect(Tk::Comma, "','");
        std::uint32_t s = static_cast<std::uint32_t>(takeInt());
        expect(Tk::DotDot, "'..'");
        std::uint32_t e = static_cast<std::uint32_t>(takeInt());
        return HirSourceLoc{BufferId{buf}, SourceSpan::of(s, e)};
    }
    [[nodiscard]] FfiMetadata parseFfi() {
        FfiMetadata m;
        for (;;) {
            if (acceptKeyword("name")) m.mangledName = takeStr();
            else if (acceptKeyword("link")) { std::string n = takeIdent(); m.linkage = orMalformed(kHirTextFfiLinkageTable, n, "ffi linkage", FfiLinkage::Strong); }
            else if (acceptKeyword("vis")) { std::string n = takeIdent(); m.visibility = orMalformed(kHirTextFfiVisibilityTable, n, "ffi visibility", FfiVisibility::Default); }
            else if (acceptKeyword("lib")) m.importLibrary = takeStr();
            else if (acceptKeyword("soname")) m.soname = takeStr();
            else if (acceptKeyword("version")) m.version = takeStr();  // c156
            else break;
            if (!accept(Tk::Comma)) break;
        }
        return m;
    }
    [[nodiscard]] ShaderIntrinsic parseShader() {
        ShaderIntrinsic m;
        for (;;) {
            if (acceptKeyword("stage")) { std::string n = takeIdent(); m.stage = orMalformed(kHirTextShaderStageTable, n, "shader stage", ShaderStage::None); }
            else if (acceptKeyword("builtin")) { std::string n = takeIdent(); m.builtin = orMalformed(kHirTextShaderBuiltinTable, n, "shader builtin", ShaderBuiltin::None); }
            else if (acceptKeyword("wg")) {
                m.workgroup.x = static_cast<std::uint32_t>(takeInt());
                m.workgroup.y = static_cast<std::uint32_t>(takeInt());
                m.workgroup.z = static_cast<std::uint32_t>(takeInt());
            } else if (acceptKeyword("binding")) {
                m.binding.set = static_cast<std::uint32_t>(takeInt());
                expect(Tk::Colon, "':'");
                m.binding.binding = static_cast<std::uint32_t>(takeInt());
            } else if (acceptKeyword("loc")) {
                m.location = static_cast<std::uint32_t>(takeInt());
            } else break;
            if (!accept(Tk::Comma)) break;
        }
        return m;
    }
    [[nodiscard]] TranspileHint parseTranspile() {
        TranspileHint m;
        for (;;) {
            if (acceptKeyword("target")) m.targetLanguage = takeStr();
            else if (acceptKeyword("override")) m.overrideKind = takeStr();
            else if (acceptKeyword("idiom")) { std::string n = takeIdent(); m.idiom = orMalformed(kHirTextTranspileIdiomTable, n, "transpile idiom", TranspileIdiom::Default); }
            else break;
            if (!accept(Tk::Comma)) break;
        }
        return m;
    }
    void parseDiag(PendingAttrs& a) {
        DiagnosticInfo info;
        for (;;) {
            // ★★★ THE CAST USED TO BE THE WHOLE OF IT: an arbitrary integer went
            // straight to `DiagnosticCode` with no validation whatsoever, so a
            // `.dsshir` could name `@diag(code 47000)` and the parse SUCCEEDED
            // with a code that has never been allocated
            // (D-MIR-TEXT-DIAG-CODE-CAST-IS-UNVALIDATED).
            //
            // ★ `DiagnosticCode` is a single flat ordinal space whose every value
            // is an OPERATOR-VISIBLE published identity — it renders as
            // `error[D0029]`, it appears in docs and in `expected.json` fixtures —
            // which is exactly why it has an allocation gate
            // (`scripts/check-diagnostic-codes/check-diagnostic-codes.py`). This
            // cast walked straight past that gate, and the value it minted would
            // render under `diagnosticCodePrefix`'s letter for the high nibble as
            // if it were a real code.
            //
            // ⚠ THE REFUSAL RESOLVES THROUGH THE SAME SOURCE THE GATE READS. The
            // gate reads the ENUM rather than a hand-maintained list, on the
            // stated grounds that anything asking a human to keep a second list in
            // sync has the failure mode it exists to catch. `diagnosticCodeName`
            // is that enum projected through a `default:`-less switch, so
            // `-Werror=switch` keeps it total and the two cannot disagree.
            if (acceptKeyword("code")) {
                std::uint64_t const raw = takeInt();
                auto const code = static_cast<DiagnosticCode>(
                    static_cast<std::uint16_t>(raw));
                if (raw > 0xFFFFu) {
                    malformed(std::format(
                        "diagnostic code {} does not fit DiagnosticCode's 16-bit "
                        "ordinal space", raw));
                } else if (diagnosticCodeName(code) == kUnallocatedDiagnosticCodeName) {
                    malformed(std::format(
                        "diagnostic code {} (0x{:04X}) has never been allocated — a "
                        "DiagnosticCode is a published identity (it renders as "
                        "'{}'), so a code this build does not define cannot be "
                        "named here",
                        raw, raw, diagnosticCodePrefix(code)));
                } else {
                    info.code = code;
                }
            }
            else if (acceptKeyword("recovery")) { std::string n = takeIdent(); info.recovery = orMalformed(kHirTextRecoveryTable, n, "diag recovery", HirRecovery::None); }
            else if (acceptKeyword("origin")) { a.diagHasOrigin = true; a.diagOriginPre = static_cast<std::uint32_t>(takeInt()); }
            else if (acceptKeyword("detail")) info.detail = takeStr();
            else break;
            if (!accept(Tk::Comma)) break;
        }
        a.diag = std::move(info);
    }
    void applyAttrs(HirNodeId id, PendingAttrs&& a) {
        if (a.loc) pLoc_.push_back({id, *a.loc});
        if (a.ffi) pFfi_.push_back({id, std::move(*a.ffi)});
        if (a.shader) pShader_.push_back({id, *a.shader});
        if (a.transpile) pTranspile_.push_back({id, std::move(*a.transpile)});
        if (a.diag) pDiag_.push_back({id, std::move(*a.diag), a.diagHasOrigin, a.diagOriginPre});
    }

    // ── node dispatch ─────────────────────────────────────────────────────────

    // Byte offset of the current token — the basis of every list-loop progress
    // guard: if an iteration leaves the cursor unmoved, a malformed/stuck token
    // is force-skipped so collect-all parsing can never spin.
    [[nodiscard]] std::uint32_t cursorOff() const { return lex_.peek().off; }

    // ⚠ THIS WAS A HAND-RETYPED COPY OF THE EXPRESSION KEYWORD SET, AND IT WAS
    // ALREADY SHORT BY THREE. It listed twenty of the twenty-three keywords
    // `parseExprInner` handles, omitting `va_start` / `va_arg` / `va_end` — all
    // three of which the writer emits — so a `.dsshir` carrying a variadic-access
    // node was routed to `parseStmtInner` and refused as an unknown statement. The
    // router now asks the same table the dispatch and the refusal read.
    [[nodiscard]] static bool isExprKeyword(std::string_view kw) {
        return kHirTextExprKwTable.fromName(kw).has_value();
    }

    // Parse any node (decl/stmt/expr/wildcard). Handles attrs + pre-order index.
    HirNodeId parseNode() {
        PendingAttrs attrs = parseAttrs();
        std::uint32_t const idx = preCounter_++;
        std::string_view kw = peekIs(Tk::Ident) ? std::string_view{lex_.peek().text} : std::string_view{};
        HirNodeId id = isExprKeyword(kw) ? parseExprInner() : parseStmtInner();
        recordIndex(idx, id);
        applyAttrs(id, std::move(attrs));
        return id;
    }

    [[nodiscard]] std::vector<HirNodeId> parseParenOperands() {
        std::vector<HirNodeId> kids;
        expect(Tk::LParen, "'('");
        while (!peekIs(Tk::RParen) && !peekIs(Tk::Eof)) {
            kids.push_back(parseNode());
            if (!accept(Tk::Comma)) break;
        }
        expect(Tk::RParen, "')'");
        return kids;
    }
    [[nodiscard]] std::vector<HirNodeId> parseBraceNodes() {
        std::vector<HirNodeId> kids;
        expect(Tk::LBrace, "'{'");
        while (!peekIs(Tk::RBrace) && !peekIs(Tk::Eof)) {
            std::uint32_t const off = cursorOff();
            kids.push_back(parseNode());
            if (cursorOff() == off) lex_.take();  // progress guard
        }
        expect(Tk::RBrace, "'}'");
        return kids;
    }

    [[nodiscard]] std::uint32_t parseSymHandle() {
        expect(Tk::Percent, "'%'");
        std::uint32_t const h = static_cast<std::uint32_t>(takeInt());
        if (h == 0 || h >= symbolNames_.size())
            unknownName(std::format("symbol handle %{} not declared in 'symbols'", h));
        return h;
    }
    [[nodiscard]] TypeId parseTypeAnnot() { expect(Tk::Colon, "':'"); return parseType(); }

    // ── expressions ──────────────────────────────────────────────────────────
    HirNodeId parseExprInner() {
        std::string kw = takeIdent();
        HirFlags flags = parseFlags();
        auto const which = kHirTextExprKwTable.fromName(kw);
        if (!which.has_value()) {
            // ⓘ UNREACHABLE THROUGH `parseNode`, AND DELIBERATELY KEPT. The router
            // (`isExprKeyword`) and this dispatch now consult the SAME table, so a
            // keyword that got here resolved there — which is the property that
            // fixed the `va_*` routing hole in the first place. It stays because
            // `parseExprInner` is a production, not a private helper of one
            // caller, and a production that can be reached with an unknown keyword
            // must say what it accepts rather than fall through building an Error
            // node in silence (D-TEXT-TIER-REFUSALS-NAME-NO-ACCEPTED-SET).
            malformed(std::format("unknown expression '{}' — accepted: {}", kw,
                                  detail::renderAllowedList(allNames(kHirTextExprKwTable))));
            return builder_.addLeaf(HirKind::Error, InvalidType, 0, flags);
        }
        // The `kw : type (operands...)` family — mirror of the emitter's
        // `typedCall`. Payload is always 0; arity is enforced later by the
        // verifier.
        auto typedCall = [&](HirKind kind) {
            TypeId t = parseTypeAnnot();
            auto ops = parseParenOperands();
            return builder_.addParent(kind, ops, t, 0, flags);
        };
        // ⓘ `-Werror=switch` IS THE PAIRING GUARD. A row added to
        // `kHirTextExprKwTable` with no arm here fails the build, which is the
        // property the three-way `if`-ladder + array + router arrangement could
        // not have: adding a keyword to one of them and not the others compiled
        // fine and shipped a spelling that only worked in one direction.
        switch (*which) {
            case HirTextExprKw::Lit: {
                if (accept(Tk::Hash)) {   // bare index form: `lit #N : type` (no pool)
                    std::uint32_t const i = static_cast<std::uint32_t>(takeInt());
                    TypeId t = parseTypeAnnot();
                    return builder_.makeLiteral(t, i, flags);
                }
                // value form: `lit <tagged-value> : type` — rebuild the pool entry.
                HirLiteralValue v = parseLiteralValue();
                TypeId t = parseTypeAnnot();
                v.core = literalCoreFor(t, v);
                std::uint32_t const idx = pLiterals_.add(std::move(v));
                return builder_.makeLiteral(t, idx, flags);
            }
            case HirTextExprKw::Ref: {
                std::uint32_t h = parseSymHandle(); TypeId t = parseTypeAnnot();
                return builder_.makeRef(t, h, flags);
            }
            case HirTextExprKw::Call: {
                TypeId t = parseTypeAnnot(); auto kids = parseParenOperands();
                if (kids.empty()) { malformed("call needs a callee"); return builder_.addLeaf(HirKind::Error, t, 0, flags); }
                HirNodeId callee = kids.front();
                std::vector<HirNodeId> args(kids.begin() + 1, kids.end());
                return builder_.makeCall(callee, args, t, flags);
            }
            case HirTextExprKw::BuiltinCall: {
                // c103 (D-CSUBSET-INTRINSIC-UMULH): `builtincall #<lowering> :
                // <type> (<operands>)` — the mirror of the writer's arm. The
                // writer had one and this did not, so `emitHir` produced text
                // `parseHir` refused by name
                // (D-HIR-TEXT-WRITER-SPELLS-KEYWORDS-THE-READER-HAS-NO-ROW-FOR).
                //
                // ★ THE ORDINAL IS VALIDATED, NOT CAST. `BuiltinLowering` is a
                // closed set whose members are named by CONFIG (`grammar_schema_
                // json.cpp` resolves `"lowering"` through the same table), so an
                // ordinal outside it is not a lowering at all — and `hir_to_mir`
                // maps the payload straight onto a `MirOpcode`. The same argument
                // this file already makes for `@diag(code N)`
                // (D-MIR-TEXT-DIAG-CODE-CAST-IS-UNVALIDATED), applied to the
                // other unvalidated ordinal in the format.
                // ⚠ `None` (0) is DELIBERATELY unlisted in that table — it is the
                // "no lowering" sentinel — so it renders empty here and is
                // refused, which is right: a `BuiltinCall` node exists precisely
                // because the builtin HAS a lowering.
                expect(Tk::Hash, "'#' before a builtincall lowering ordinal");
                std::uint64_t const raw = takeInt();
                TypeId t = parseTypeAnnot();
                auto kids = parseParenOperands();
                if (raw > 0xFFFFu
                    || builtinLoweringName(
                           static_cast<BuiltinLowering>(
                               static_cast<std::uint16_t>(raw))).empty()) {
                    malformed(std::format(
                        "builtincall lowering ordinal {} names no BuiltinLowering "
                        "this build defines - accepted: {}", raw,
                        detail::renderAllowedList(allNames(kBuiltinLoweringTable))));
                    return builder_.addLeaf(HirKind::Error, t, 0, flags);
                }
                return builder_.addParent(HirKind::BuiltinCall, kids, t,
                                          static_cast<std::uint32_t>(raw), flags);
            }
            case HirTextExprKw::LabelAddr: {
                // D-CSUBSET-COMPUTED-GOTO: `labeladdr L<n> : <type>` — a LEAF
                // whose payload is the target label's per-function ordinal, the
                // same namespace `goto`/`label` use, read by the same
                // `parseLabelOrdinal` production so the three cannot disagree.
                std::uint32_t const ord = parseLabelOrdinal();
                TypeId t = parseTypeAnnot();
                return builder_.makeLabelAddressOf(ord, t, flags);
            }
            case HirTextExprKw::Intrinsic: {
                std::string name = takeStr(); TypeId t = parseTypeAnnot();
                auto kids = parseParenOperands();
                auto it = intrinsicByName_.find(name);
                std::uint32_t iid = 0;
                if (it != intrinsicByName_.end()) iid = it->second.v;
                else unknownName(std::format("intrinsic \"{}\" not declared", name));
                return builder_.makeIntrinsicCall(iid, kids, t, flags);
            }
            case HirTextExprKw::BinOp:
            case HirTextExprKw::UnOp: {
                std::uint32_t payload = 0; (void)parseOp(payload);
                TypeId t = parseTypeAnnot(); auto kids = parseParenOperands();
                return builder_.addParent(
                    (*which == HirTextExprKw::BinOp) ? HirKind::BinaryOp : HirKind::UnaryOp,
                    kids, t, payload, flags);
            }
            case HirTextExprKw::Member: {
                expect(Tk::Hash, "'#'"); std::uint32_t fi = static_cast<std::uint32_t>(takeInt());
                TypeId t = parseTypeAnnot(); auto k = parseParenOperands();
                return builder_.addParent(HirKind::MemberAccess, k, t, fi, flags);
            }
            case HirTextExprKw::Swizzle: {
                expect(Tk::Hash, "'#'"); std::uint32_t m = static_cast<std::uint32_t>(takeInt());
                TypeId t = parseTypeAnnot(); auto k = parseParenOperands();
                return builder_.addParent(HirKind::Swizzle, k, t, m, flags);
            }
            case HirTextExprKw::TypeRef: {
                TypeId t = parseTypeAnnot(); return builder_.makeTypeRef(t, flags);
            }
            case HirTextExprKw::Seq: {
                // seq : type { <stmt-lines> yield <resultExpr> }
                TypeId t = parseTypeAnnot();
                expect(Tk::LBrace, "'{'");
                std::vector<HirNodeId> stmts;
                while (!peekKeyword("yield") && !peekIs(Tk::RBrace) && !peekIs(Tk::Eof)) {
                    std::uint32_t const off = cursorOff();
                    stmts.push_back(parseNode());
                    if (cursorOff() == off) lex_.take();   // progress guard
                }
                if (!acceptKeyword("yield")) malformed("seq body needs a 'yield <expr>'");
                HirNodeId result = parseNode();
                expect(Tk::RBrace, "'}'");
                return builder_.makeSeqExpr(stmts, result, t, flags);
            }
            case HirTextExprKw::Cast:       return typedCall(HirKind::Cast);
            case HirTextExprKw::Index:      return typedCall(HirKind::Index);
            case HirTextExprKw::Construct:  return typedCall(HirKind::ConstructAggregate);
            case HirTextExprKw::Ternary:    return typedCall(HirKind::Ternary);
            case HirTextExprKw::LogicalAnd: return typedCall(HirKind::LogicalAnd);
            case HirTextExprKw::LogicalOr:  return typedCall(HirKind::LogicalOr);
            case HirTextExprKw::SizeOf:     return typedCall(HirKind::SizeOf);
            case HirTextExprKw::AlignOf:    return typedCall(HirKind::AlignOf);
            case HirTextExprKw::AddressOf:  return typedCall(HirKind::AddressOf);
            case HirTextExprKw::Deref:      return typedCall(HirKind::Deref);
            case HirTextExprKw::VaStart:    return typedCall(HirKind::VaStart);
            case HirTextExprKw::VaArg:      return typedCall(HirKind::VaArg);
            case HirTextExprKw::VaEnd:      return typedCall(HirKind::VaEnd);
            // The row-count sentinel. It is UNLISTED in the table, so `fromName`
            // cannot produce it and this arm cannot be reached from any input —
            // it exists because `-Werror=switch` is the pairing guard, and a
            // `default:` bought to silence one sentinel would disable that guard
            // for every real keyword added afterwards.
            case HirTextExprKw::Count_: break;
        }
        // Unreachable: `-Werror=switch` proves the switch is total over the enum,
        // and `fromName` cannot return a value outside it.
        malformed(std::format("unknown expression '{}'", kw));
        return builder_.addLeaf(HirKind::Error, InvalidType, 0, flags);
    }

    // Parse an op name into a node payload (core HirOpKind or registered HirOpId).
    void parseOp(std::uint32_t& payload) {
        if (acceptKeyword("ext")) {
            std::string name = takeStr();
            auto it = extOpByName_.find(name);
            if (it != extOpByName_.end()) payload = it->second.v;
            else { unknownName(std::format("operator \"{}\" not declared", name)); payload = 0; }
            return;
        }
        std::string name = takeIdent();
        if (auto op = coreOpFromName(name)) payload = encodeOp(*op);
        else {
            // D-TEXT-TIER-REFUSALS-NAME-NO-ACCEPTED-SET: the accepted set comes
            // off the same `opName` walk the lookup just failed, so it cannot be
            // narrower, wider or staler than the check above it.
            malformed(std::format("unknown operator '{}' — accepted: {}", name,
                                  coreOpAccepted()));
            payload = 0;
        }
    }

    // ── statements / decls / wildcards ────────────────────────────────────────
    HirNodeId parseStmtInner() {
        if (!peekIs(Tk::Ident)) { malformed("expected a statement"); return builder_.addLeaf(HirKind::Error); }
        std::string kw = takeIdent();
        HirFlags flags = parseFlags();
        auto const which = kHirTextStmtKwTable.fromName(kw);
        if (!which.has_value()) {
            // D-TEXT-TIER-REFUSALS-NAME-NO-ACCEPTED-SET: this said only
            // `unknown statement 'foo'`, which is the least useful thing a reader
            // can say about a closed keyword set it is holding.
            //
            // ★ THE SET IS THE UNION, AND THAT IS NOT PADDING. `parseNode` routes
            // on `isExprKeyword` FIRST, so a keyword reaching here is one BOTH
            // tables missed — the accepted set at this position is every node
            // keyword, not just the statement half. Naming only the statement set
            // would send an author who typo'd `addressof` looking for a statement
            // that was never what they wanted.
            malformed(std::format(
                "unknown node keyword '{}' — accepted statements: {}; accepted "
                "expressions: {}", kw,
                detail::renderAllowedList(allNames(kHirTextStmtKwTable)),
                detail::renderAllowedList(allNames(kHirTextExprKwTable))));
            return builder_.addLeaf(HirKind::Error, InvalidType, 0, flags);
        }
        // ⓘ `-Werror=switch` is the pairing guard: a row in
        // `kHirTextStmtKwTable` with no arm here fails the build.
        switch (*which) {
            case HirTextStmtKw::Block: { auto k = parseBraceNodes(); return builder_.makeBlock(k, flags); }
            case HirTextStmtKw::If: {
                expect(Tk::LParen, "'('"); HirNodeId cond = parseNode(); expect(Tk::RParen, "')'");
                HirNodeId then = parseNode();
                std::optional<HirNodeId> els;
                if (acceptKeyword("else")) els = parseNode();
                return builder_.makeIfStmt(cond, then, els, flags);
            }
            case HirTextStmtKw::SehTry: {
                // c115 SEH round-trip: `seh_try` <tryBody> `seh_except (` filter `)`
                // <handler> — mirrors the writer arm exactly.
                HirNodeId tryBody = parseNode();
                if (!acceptKeyword("seh_except")) malformed("expected 'seh_except'");
                expect(Tk::LParen, "'('"); HirNodeId filter = parseNode(); expect(Tk::RParen, "')'");
                HirNodeId handler = parseNode();
                return builder_.makeSehTryExcept(tryBody, filter, handler, flags);
            }
            case HirTextStmtKw::While: {
                expect(Tk::LParen, "'('"); HirNodeId cond = parseNode(); expect(Tk::RParen, "')'");
                HirNodeId body = parseNode();
                return builder_.makeWhileStmt(cond, body, flags);
            }
            case HirTextStmtKw::Do: {
                HirNodeId body = parseNode();
                if (!acceptKeyword("while")) malformed("expected 'while'");
                expect(Tk::LParen, "'('"); HirNodeId cond = parseNode(); expect(Tk::RParen, "')'");
                return builder_.makeDoWhileStmt(body, cond, flags);
            }
            case HirTextStmtKw::For: return parseFor(flags);
            case HirTextStmtKw::Switch: {
                // c60 (Design I-A): `switch (disc) { body: <block> case v L<ord> ...
                // default L<ord> }` — the body Block then the dispatch arms.
                expect(Tk::LParen, "'('"); HirNodeId disc = parseNode(); expect(Tk::RParen, "')'");
                expect(Tk::LBrace, "'{'");
                if (!acceptKeyword("body")) malformed("expected 'body:' in switch");
                expect(Tk::Colon, "':'");
                HirNodeId body = parseNode();
                std::vector<HirNodeId> arms;
                while (!peekIs(Tk::RBrace) && !peekIs(Tk::Eof)) {
                    std::uint32_t const off = cursorOff();
                    arms.push_back(parseNode());
                    if (cursorOff() == off) lex_.take();
                }
                expect(Tk::RBrace, "'}'");
                return builder_.makeSwitchStmt(disc, body, arms, flags);
            }
            case HirTextStmtKw::Case: {
                HirNodeId v = parseNode(); std::uint32_t ord = parseLabelOrdinal();
                return builder_.makeCaseArm(v, ord, flags);
            }
            case HirTextStmtKw::Default: {
                std::uint32_t ord = parseLabelOrdinal();
                return builder_.makeCaseArm(std::nullopt, ord, flags);
            }
            // c60 (Design I-A): `label L<ord>:` <body> and `goto L<ord>` / `goto *expr`
            // (the switch body's case markers render as `label` statements, so the
            // round-trip parser must read them).
            case HirTextStmtKw::Label: {
                std::uint32_t ord = parseLabelOrdinal();
                expect(Tk::Colon, "':'");
                HirNodeId body = parseNode();
                return builder_.makeLabelStmt(ord, body, flags);
            }
            case HirTextStmtKw::Goto: {   // `goto L<ord>` (the plain label form)
                std::uint32_t ord = parseLabelOrdinal();
                return builder_.makeGotoStmt(ord, flags);
            }
            case HirTextStmtKw::Break: {
                std::uint32_t d = peekIs(Tk::Int) ? static_cast<std::uint32_t>(takeInt()) : 0u;
                return builder_.makeBreak(d, flags);
            }
            case HirTextStmtKw::Continue: {
                std::uint32_t d = peekIs(Tk::Int) ? static_cast<std::uint32_t>(takeInt()) : 0u;
                return builder_.makeContinue(d, flags);
            }
            // FC17.9(i) (D-CSUBSET-INLINE-ASM): the empty-template asm barrier — a bare
            // `inline_asm` leaf (mirrors the writer arm; no payload in cycle-1).
            case HirTextStmtKw::InlineAsm: return parseInlineAsm(flags);
            case HirTextStmtKw::Return: {
                // A return value may carry inline attributes (`return @loc(...) expr`).
                // A value-less `return` is always block-terminal (nothing may follow
                // it — checkBlockTermination), so a leading `@` here unambiguously
                // introduces an attributed value, never the next statement's attrs.
                if (peekIs(Tk::At) || startsExpr()) { HirNodeId v = parseNode(); return builder_.makeReturn(v, flags); }
                return builder_.makeReturn(std::nullopt, flags);
            }
            case HirTextStmtKw::Expr: { HirNodeId e = parseNode(); return builder_.makeExprStmt(e, flags); }
            case HirTextStmtKw::Var:
            case HirTextStmtKw::Param: return parseVarLike(flags);
            case HirTextStmtKw::Assign: {
                HirNodeId tgt = parseNode(); expect(Tk::Equal, "'='"); HirNodeId val = parseNode();
                return builder_.makeAssignStmt(tgt, val, flags);
            }
            case HirTextStmtKw::Unreachable:
                return builder_.addLeaf(HirKind::Unreachable, InvalidType, 0, flags);
            case HirTextStmtKw::Function:       return parseFunction(flags);
            case HirTextStmtKw::ExternFunction: return parseExternFunction(flags);
            case HirTextStmtKw::Global: {
                std::uint32_t sym = parseSymHandle(); TypeId t = parseTypeAnnot();
                std::optional<HirNodeId> init;
                if (accept(Tk::Equal)) init = parseNode();
                return builder_.makeGlobal(t, sym, init, flags);
            }
            case HirTextStmtKw::TypeDecl: {
                std::uint32_t sym = parseSymHandle(); TypeId t = parseTypeAnnot();
                return builder_.makeTypeDecl(t, sym, flags);
            }
            case HirTextStmtKw::ExternGlobal: {
                std::uint32_t sym = parseSymHandle();
                TypeId t = accept(Tk::Colon) ? parseType() : InvalidType;
                return builder_.makeExternGlobal(t, sym, flags);
            }
            case HirTextStmtKw::ImportGroup: { auto m = parseBraceNodes(); return builder_.makeImportGroup(m, flags); }
            case HirTextStmtKw::ExtNode: return parseExtNode(flags);
            case HirTextStmtKw::Error:   return parseErrorNode(flags);
            // Unlisted sentinel — see the expression switch's arm for why it is
            // an arm rather than a `default:`.
            case HirTextStmtKw::Count_: break;
        }
        // Unreachable: `-Werror=switch` proves the switch is total over the enum,
        // and `fromName` cannot return a value outside it.
        malformed(std::format("unknown statement '{}'", kw));
        return builder_.addLeaf(HirKind::Error, InvalidType, 0, flags);
    }

    [[nodiscard]] bool startsExpr() {
        if (!peekIs(Tk::Ident)) return false;
        return isExprKeyword(lex_.peek().text);
    }

    // Inline-asm P5: the inverse of `emitInlineAsm`. Three forms, matching the
    // writer exactly:
    //   `inline_asm`                 - the bare barrier (payload 0, no children)
    //   `inline_asm #<n>`            - the opaque handle the writer falls back to
    //                                  when it had no pool; carried through
    //                                  VERBATIM so a pool-less round trip is
    //                                  still byte-identical
    //   `inline_asm "tmpl" ...`      - a full descriptor, re-added to THIS parse's
    //                                  pool (handle re-minted in tree order)
    //
    // * SECTION ORDER IS FIXED AND THE PARSE IS ORDER-DEPENDENT, deliberately:
    // the writer emits one canonical order, so accepting a permuted input would
    // admit text `emitHir` can never produce and quietly break the
    // emit(parse(emit)) identity the format's contract rests on.
    [[nodiscard]] HirNodeId parseInlineAsm(HirFlags flags) {
        if (accept(Tk::Hash)) {
            auto const raw = takeInt();
            return builder_.addLeaf(HirKind::InlineAsm, InvalidType,
                                    static_cast<std::uint32_t>(raw), flags);
        }
        if (!peekIs(Tk::Str)) {
            return builder_.addLeaf(HirKind::InlineAsm, InvalidType,
                                    kNoInlineAsmDescriptor, flags);
        }
        HirInlineAsmDescriptor d;
        d.templateText = takeStr();
        std::vector<HirNodeId> children;
        // No brace => a bare template with no flags and no sections. See the
        // writer for why the tail is braced at all (`goto` is a statement
        // keyword and this lexer is newline-blind).
        if (!accept(Tk::LBrace)) {
            std::uint32_t const bare = pInlineAsm_.add(std::move(d));
            return builder_.addLeaf(HirKind::InlineAsm, InvalidType, bare, flags);
        }
        if (acceptKeyword("extended")) d.isExtended             = true;
        if (acceptKeyword("goto"))     d.isGoto                 = true;
        if (acceptKeyword("mem"))      d.clobbersMemory         = true;
        if (acceptKeyword("cc"))       d.clobbersConditionCodes = true;

        // The inverse of the writer's `emitSpells`. Absent group => an empty
        // spelling list, which is the same state the writer renders as nothing:
        // the round trip is closed in both directions, and a language whose
        // sigil role is `null` stays distinguishable from one whose spellings
        // were dropped only because `HirVerifier` asserts the label sizes.
        auto const parseSpells = [&]() -> std::vector<std::string> {
            std::vector<std::string> out;
            if (!acceptKeyword("spells")) return out;
            expect(Tk::LParen, "'('");
            do { out.push_back(takeStr()); } while (accept(Tk::Comma));
            expect(Tk::RParen, "')'");
            return out;
        };

        if (acceptKeyword("outputs")) {
            d.outputCount = static_cast<std::uint32_t>(takeInt());
            if (!acceptKeyword("operands")) {
                malformed("expected 'operands (' after 'outputs <n>'");
                return builder_.addLeaf(HirKind::InlineAsm, InvalidType,
                                        kNoInlineAsmDescriptor, flags);
            }
            expect(Tk::LParen, "'('");
            do {
                HirInlineAsmOperand op;
                // The RAW constraint is what the source wrote; re-splitting it
                // through the SAME `parseAsmConstraint` the front end used is
                // what keeps the modifier flags from becoming a second source of
                // truth that a hand-edited `.dsshir` could contradict.
                auto const parsed = parseAsmConstraint(takeStr());
                if (!parsed.ok())
                    malformed("inline-asm operand constraint does not parse: "
                              + std::string{asmConstraintDefectDescription(parsed.defect)});
                op.constraint = parsed.value;
                op.isOutput   = d.operands.size() < d.outputCount;
                if (accept(Tk::LBrack)) {
                    op.symbolicName = takeIdent();
                    expect(Tk::RBrack, "']'");
                }
                op.spellings = parseSpells();
                // ── D-HIR-TEXT-INLINE-ASM-REGISTER-CLASS-ORDINAL-IS-UNVALIDATED ──
                //
                // ⚠ THE ORDINAL IS VALIDATED, NOT CAST — the same hole
                // D-MIR-TEXT-DIAG-CODE-CAST-IS-UNVALIDATED names one production
                // over, found while adding the sibling clause below. This read
                // `static_cast<std::uint8_t>(takeInt())` straight onto the wire,
                // so `class 200` loaded CLEAN and handed every consumer a
                // `TargetRegClass` with no row — and `bindAsmOperand`'s only
                // guard is `cls == None`, which a garbage ordinal passes. The
                // ordinal STAYS the wire form (that is the writer's shape and a
                // spelling change would break stored goldens); what changes is
                // that a value the enum does not define is now refused by name.
                if (acceptKeyword("class")) {
                    std::uint64_t const raw = takeInt();
                    if (raw > 0xFFu
                        || kTargetRegClassTable
                               .nameOrEmpty(static_cast<TargetRegClass>(
                                   static_cast<std::uint8_t>(raw)))
                               .empty()) {
                        malformed(std::format(
                            "inline-asm operand register class {} names no "
                            "TargetRegClass this build defines - accepted "
                            "ordinals spell: {}", raw,
                            detail::renderAllowedList(
                                allNames(kTargetRegClassTable))));
                    } else {
                        op.regClassResolved = true;
                        op.regClass = static_cast<std::uint8_t>(raw);
                    }
                }
                if (acceptKeyword("pin")) op.fixedRegister = takeStr();
                // The inverse of the writer's `operand_kind` clause. ABSENT means
                // "the letter did not resolve to a form", which is a real state
                // (`binds` names exactly one arm) and NOT the same as a dropped
                // field — which is precisely why the writer emits the clause at
                // all (D-HIR-TEXT-INLINE-ASM-OPERAND-KIND-DROPPED-IN-TRANSIT).
                // ★ AN UNKNOWN SPELLING IS REFUSED, NAMING THE ACCEPTED SET —
                // the same treatment the `class` ordinal just above now gets,
                // arrived at from the two opposite directions the format uses.
                if (acceptKeyword("operand_kind")) {
                    std::string const kind = takeIdent();
                    if (auto const which = operandKindFilterFromName(kind)) {
                        op.operandKindResolved = true;
                        op.operandKind = static_cast<std::uint8_t>(*which);
                    } else {
                        malformed(std::format(
                            "unknown inline-asm operand kind '{}' - accepted: {}",
                            kind,
                            detail::renderAllowedList(
                                allNames(kOperandKindFilterTable))));
                    }
                }
                expect(Tk::Arrow, "'->' before an inline-asm operand value");
                children.push_back(parseNode());
                d.operands.push_back(std::move(op));
            } while (accept(Tk::Comma));
            expect(Tk::RParen, "')'");
        }
        if (acceptKeyword("clobbers")) {
            expect(Tk::LParen, "'('");
            do { d.clobbers.push_back(takeStr()); } while (accept(Tk::Comma));
            expect(Tk::RParen, "')'");
        }
        if (acceptKeyword("labels")) {
            expect(Tk::LParen, "'('");
            // Ordinal and spellings are pushed TOGETHER, so the two lists this
            // parse produces are index-aligned by construction and no input can
            // make them drift — the same lockstep the lowering uses.
            do {
                d.labelOrdinals.push_back(parseLabelOrdinal());
                d.labelSpellings.push_back(parseSpells());
            } while (accept(Tk::Comma));
            expect(Tk::RParen, "')'");
        }
        expect(Tk::RBrace, "'}' closing the inline-asm descriptor");
        std::uint32_t const handle = pInlineAsm_.add(std::move(d));
        return builder_.addParent(HirKind::InlineAsm, children, InvalidType,
                                  handle, flags);
    }

    HirNodeId parseVarLike(HirFlags flags) {
        std::uint32_t sym = parseSymHandle();
        TypeId t = parseTypeAnnot();
        std::optional<HirNodeId> init;
        if (accept(Tk::Equal)) init = parseNode();
        return builder_.makeVarDecl(t, sym, init, flags);
    }

    HirNodeId parseFor(HirFlags flags) {
        expect(Tk::LBrace, "'{'");
        std::optional<HirNodeId> init, cond, update, body;
        while (!peekIs(Tk::RBrace) && !peekIs(Tk::Eof)) {
            std::uint32_t const off = cursorOff();
            std::string role = takeIdent();
            expect(Tk::Colon, "':'");
            HirNodeId n = parseNode();
            // D-TEXT-TIER-REFUSALS-NAME-NO-ACCEPTED-SET: keyed on
            // `kHirTextForClauseTable`, so the refusal names the four roles and a
            // new row cannot be added without a dispatch arm (`-Werror=switch`).
            auto const which = kHirTextForClauseTable.fromName(role);
            if (!which.has_value()) {
                malformed(std::format(
                    "unknown for-clause '{}' — accepted: {}", role,
                    detail::renderAllowedList(allNames(kHirTextForClauseTable))));
            } else {
                switch (*which) {
                    case HirTextForClause::Init:   init   = n; break;
                    case HirTextForClause::Cond:   cond   = n; break;
                    case HirTextForClause::Update: update = n; break;
                    case HirTextForClause::Body:   body   = n; break;
                }
            }
            if (cursorOff() == off) lex_.take();  // progress guard
        }
        expect(Tk::RBrace, "'}'");
        if (!body) { malformed("for is missing a body"); body = builder_.addLeaf(HirKind::Error); }
        return builder_.makeForStmt(init, cond, update, *body, flags);
    }

    HirNodeId parseFunction(HirFlags flags) {
        std::uint32_t sym = parseSymHandle();
        TypeId sig = parseTypeAnnot();
        auto kids = parseBraceNodes();
        if (kids.empty()) { malformed("function has no body"); return builder_.addLeaf(HirKind::Error, sig, sym, flags); }
        HirNodeId body = kids.back();
        std::vector<HirNodeId> params(kids.begin(), kids.end() - 1);
        return builder_.makeFunction(sig, sym, params, body, flags);
    }
    HirNodeId parseExternFunction(HirFlags flags) {
        std::uint32_t sym = parseSymHandle();
        TypeId sig = accept(Tk::Colon) ? parseType() : InvalidType;
        auto params = parseBraceNodes();
        return builder_.makeExternFunction(sig, sym, params, flags);
    }
    HirNodeId parseExtNode(HirFlags flags) {
        std::string name = takeStr();
        auto it = extKindByName_.find(name);
        std::uint32_t payload = 0;
        if (it != extKindByName_.end()) payload = it->second.v;
        else unknownName(std::format("extension kind \"{}\" not declared", name));
        TypeId t = accept(Tk::Colon) ? parseType() : InvalidType;
        if (peekIs(Tk::LBrace)) { auto kids = parseBraceNodes(); return builder_.addParent(HirKind::Extension, kids, t, payload, flags); }
        return builder_.addLeaf(HirKind::Extension, t, payload, flags);
    }
    HirNodeId parseErrorNode(HirFlags flags) {
        TypeId t = accept(Tk::Colon) ? parseType() : InvalidType;
        if (peekIs(Tk::LBrace)) { auto kids = parseBraceNodes(); return builder_.addParent(HirKind::Error, kids, t, 0, flags); }
        return builder_.addLeaf(HirKind::Error, t, 0, flags);
    }

    // ── types ─────────────────────────────────────────────────────────────────
    [[nodiscard]] std::vector<TypeId> parseTypeListUntil(Tk close) {
        std::vector<TypeId> ts;
        while (!peekIs(close) && !peekIs(Tk::Eof)) {
            ts.push_back(parseType());
            if (!accept(Tk::Comma)) break;
        }
        return ts;
    }
    [[nodiscard]] TypeId parseType() {
        if (!peekIs(Tk::Ident)) { malformed("expected a type"); return InvalidType; }
        std::string kw = lex_.take().text;
        if (kw == "invalid") return InvalidType;
        if (auto p = primFromName(kw)) {
            // D-LANG-TYPE-IDENTITY-VOCABULARY: an OPTIONAL quoted VOCABULARY TAG
            // after the core (`u64 "unsigned long long"`, the `struct "N"` naming
            // precedent). The bare core spells the ANONYMOUS representative of
            // that representation — which is what `int`/`unsigned`/`short`/`char`
            // are, so every existing spelling is unchanged. A type whose C
            // identity is distinct from its representation (`long`, `long long`,
            // `long double` and their unsigned forms) MUST carry the tag here or
            // the text round-trip silently re-collapses it onto the anonymous
            // type — and an FFI descriptor's pointer parameter would reject the
            // very C type it models (`ptr<u64>` vs a user's `unsigned long long*`).
            if (peekIs(Tk::Str)) return interner_.primitive(*p, lex_.take().text);
            return interner_.primitive(*p);
        }
        auto wrap1 = [&](TypeId (TypeInterner::*fn)(TypeId)) -> TypeId {
            expect(Tk::LAngle, "'<'"); TypeId e = parseType(); expect(Tk::RAngle, "'>'"); return (interner_.*fn)(e);
        };
        if (kw == "ptr") return wrap1(&TypeInterner::pointer);
        if (kw == "ref") return wrap1(&TypeInterner::reference);
        if (kw == "nullable") return wrap1(&TypeInterner::nullable);
        if (kw == "optional") return wrap1(&TypeInterner::optional);
        if (kw == "slice") return wrap1(&TypeInterner::slice);
        // C99 _Complex (D-CSUBSET-COMPLEX, M1): `complex<elem>` reinterns via the
        // single-operand `complex` builder — the appendType twin. Placed among the
        // wrap1 keywords (the `signature` decode of `__builtin_complex`'s
        // `complex<f64>` result routes here). primFromName has no "complex" entry, so
        // the check above fell through to here.
        if (kw == "complex") return wrap1(&TypeInterner::complex);
        // D-CSUBSET-QUAL-BITSET (M1): the bidirectional twin of appendType's qualifier
        // spelling. `volatile<T>`/`atomic<T>` reintern via volatileQualified/
        // atomicQualified, which STRIP→UNION→re-intern ONE skin — so a nested
        // `atomic<volatile<T>>` merges to bits{Volatile,Atomic} (order-independent),
        // and a shipped `atomic<i32>` typedef (stdatomic.json's atomic_int) genuinely
        // carries the Atomic bit. Closes the pre-existing volatile-drops-in-text gap too.
        if (kw == "volatile") return wrap1(&TypeInterner::volatileQualified);
        if (kw == "atomic") return wrap1(&TypeInterner::atomicQualified);
        // ★★ C23 `_BitInt(N)` / `unsigned _BitInt(N)` — THE INVERSE OF
        // `appendType`'s BitInt ARM, WHICH HAD NONE.
        //
        // The writer has rendered `unsigned _BitInt(N)` since the `_BitInt` arc
        // landed and this reader had no such keyword, so the spelling was
        // WRITE-ONLY on a format whose own header states a byte-identical
        // round-trip contract
        // (D-MIR-TEXT-ROUND-TRIP-INCOMPLETE-FOR-OPERAND-CARRYING-FORMS part (d)).
        //
        // ⚠ AND THIS HALF IS NOT A DEBUG SURFACE. `emitHir`/`parseHir` have no
        // callers in `src/` outside this TU, but `parseTypeFromText` drives THIS
        // production — ✔MEASURED 2026-08-23: 9 call sites, 7 in
        // `ffi/shipped_lib_descriptor.cpp` and 2 in
        // `analysis/semantic/semantic_analyzer.cpp` — so before this arm existed
        // no shipped FFI descriptor could name a bit-precise type at all.
        //
        // The width goes to `TypeInterner::bitInt`, which owns the legality of the
        // value; a text reader re-deciding it would be a second owner of one fact.
        if (kw == "_BitInt" || kw == "unsigned") {
            bool const isSigned = (kw != "unsigned");
            if (!isSigned && !acceptKeyword("_BitInt")) {
                malformed("expected '_BitInt' after 'unsigned' — 'unsigned' is not "
                          "a type keyword on its own in this format (an unsigned "
                          "primitive is spelled by its own name, e.g. 'u32')");
                return InvalidType;
            }
            expect(Tk::LParen, "'('");
            auto const width = static_cast<std::int64_t>(takeInt());
            expect(Tk::RParen, "')'");
            return interner_.bitInt(width, isSigned);
        }
        if (kw == "fnptr") { expect(Tk::LAngle, "'<'"); (void)parseType(); expect(Tk::RAngle, "'>'");
            malformed("fnptr<> is not constructible in this interner"); return InvalidType; }
        if (kw == "vec") { expect(Tk::LAngle, "'<'"); TypeId e = parseType(); expect(Tk::Comma, "','");
            std::int64_t n = static_cast<std::int64_t>(takeInt()); expect(Tk::RAngle, "'>'");
            return interner_.vector(e, n); }
        if (kw == "mat") { expect(Tk::LAngle, "'<'"); TypeId e = parseType(); expect(Tk::Comma, "','");
            std::int64_t r = static_cast<std::int64_t>(takeInt()); expect(Tk::Comma, "','");
            std::int64_t c = static_cast<std::int64_t>(takeInt()); expect(Tk::RAngle, "'>'");
            return interner_.matrix(e, r, c); }
        if (kw == "arr") { expect(Tk::LAngle, "'<'"); TypeId e = parseType(); expect(Tk::Comma, "','");
            std::int64_t n = static_cast<std::int64_t>(takeInt()); expect(Tk::RAngle, "'>'");
            return interner_.array(e, n); }
        if (kw == "tuple") { expect(Tk::LAngle, "'<'"); auto ts = parseTypeListUntil(Tk::RAngle); expect(Tk::RAngle, "'>'");
            return interner_.tuple(ts); }
        if (kw == "struct") { std::string name = takeStr();
            // D-FFI-OPAQUE-TAG-HAS-NO-SPELLING: the appendType twin. `opaque` marks an
            // INCOMPLETE composite and is TERMINAL -- no `{}` follows, because an
            // incomplete type has no field list (as distinct from `{}`, which is a
            // legal COMPLETE zero-field struct). `declSiteKey` is a FIXED 0 here, not a
            // decl-site id: every textual mention of `struct "FILE" opaque` must name
            // ONE type, which is the same canonicalization the complete-at-once path
            // gets by deriving its key from field content. The semantic analyzer's
            // self-referential path still passes a real decl-site key.
            if (acceptKeyword("opaque"))
                return interner_.forwardComposite(TypeKind::Struct, name, 0);
            // D-CSUBSET-PACKED: an optional ` packed` marker after the name (before the
            // `{`) round-trips the whole-composite packed flag. Routed through
            // forwardComposite + completeComposite below (the structType convenience
            // overloads don't carry packed).
            bool const packed = acceptKeyword("packed");
            expect(Tk::LBrace, "'{'");
            // c107 (D-FFI-DESCRIPTOR-UNION-OVERLAY): each field is a type optionally
            // followed by `@<byteOffset>` (an explicit overlapping layout).
            // D-CSUBSET-MEMBER-ALIGNAS: OR by `~<align>` (a member-alignas override).
            // Each marker is all-or-none, and the two are MUTUALLY EXCLUSIVE (a struct
            // carries offsets XOR aligns) — a mix is malformed. A field-local loop
            // (NOT parseTypeListUntil) so `@`/`~` are consumed here, never by node-
            // attribute logic (types are never parsed where those tokens are also
            // legal, so no ambiguity).
            std::vector<TypeId>        ts;
            std::vector<std::uint64_t> offs;
            std::vector<std::uint32_t> aligns;
            std::size_t                nWithOff   = 0;
            std::size_t                nWithAlign = 0;
            while (!peekIs(Tk::RBrace) && !peekIs(Tk::Eof)) {
                ts.push_back(parseType());
                if (accept(Tk::At)) {
                    offs.push_back(takeInt()); aligns.push_back(0); ++nWithOff;
                } else if (accept(Tk::Tilde)) {
                    aligns.push_back(static_cast<std::uint32_t>(takeInt()));
                    offs.push_back(0); ++nWithAlign;
                } else {
                    offs.push_back(0); aligns.push_back(0);
                }
                if (!accept(Tk::Comma)) break;
            }
            expect(Tk::RBrace, "'}'");
            if (nWithOff != 0 && nWithAlign != 0) {
                malformed("struct fields cannot mix explicit offsets (@) and "
                          "member aligns (~)");
                return InvalidType;
            }
            std::span<std::int64_t const> const noWidths{};
            // D-CSUBSET-PACKED: a packed struct routes through forwardComposite +
            // completeComposite (structType doesn't carry packed). A content-derived
            // declSiteKey keeps identical packed spellings canonical; bit 62 marks the
            // packed hir-text key space (distinct from contentDeclSiteKey's bit 63) so a
            // packed `S` and a non-packed `S` never collapse. packed + explicit offsets
            // is impossible (completeComposite rejects the pair) → malformed here.
            auto internPacked = [&](std::span<std::uint32_t const> al) -> TypeId {
                std::uint64_t key = 1469598103934665603ull;
                for (char ch : name)
                    key = (key ^ static_cast<std::uint8_t>(ch)) * 1099511628211ull;
                for (TypeId f : ts) key = (key ^ f.v) * 1099511628211ull;
                // F-4 symmetry: fold the member aligns into the forward key,
                // mirroring contentDeclSiteKey (type_lattice.cpp) which includes
                // fieldAligns. An empty span (internPacked({})) → empty loop → key
                // BYTE-IDENTICAL to before (zero churn for align-free packed structs);
                // two same-name+fields packed structs differing ONLY in member aligns
                // now get DISTINCT forward keys instead of colliding on the forward id.
                for (std::uint32_t a : al) key = (key ^ a) * 1099511628211ull;
                key |= (std::uint64_t{1} << 62);
                TypeId const fwd =
                    interner_.forwardComposite(TypeKind::Struct, name, key);
                std::span<std::uint64_t const> const noOffs{};
                interner_.completeComposite(fwd, ts, /*packed=*/true, noWidths,
                                            noOffs, al);
                return fwd;
            };
            if (nWithOff == 0 && nWithAlign == 0) {
                if (packed) return internPacked({});
                return interner_.structType(name, ts);
            }
            if (nWithAlign != 0) {
                if (nWithAlign != ts.size()) {
                    malformed("struct member aligns must be all-or-none");
                    return InvalidType;
                }
                if (packed) return internPacked(aligns);
                std::span<std::uint64_t const> const noOffs{};
                return interner_.structType(name, ts, noWidths, noOffs, aligns);
            }
            if (packed) {
                malformed("a packed struct cannot carry explicit field offsets (@)");
                return InvalidType;
            }
            if (nWithOff != ts.size()) {
                malformed("struct field offsets must be all-or-none");
                return InvalidType;
            }
            return interner_.structType(name, ts, noWidths, offs); }
        if (kw == "union") { std::string name = takeStr();
            bool const packed = acceptKeyword("packed");   // D-CSUBSET-PACKED
            expect(Tk::LBrace, "'{'");
            auto ts = parseTypeListUntil(Tk::RBrace); expect(Tk::RBrace, "'}'");
            if (!packed) return interner_.unionType(name, ts);
            std::uint64_t key = 1469598103934665603ull;
            for (char ch : name)
                key = (key ^ static_cast<std::uint8_t>(ch)) * 1099511628211ull;
            for (TypeId f : ts) key = (key ^ f.v) * 1099511628211ull;
            key |= (std::uint64_t{1} << 62);
            TypeId const fwd = interner_.forwardComposite(TypeKind::Union, name, key);
            interner_.completeComposite(fwd, ts, /*packed=*/true);
            return fwd; }
        // D5.5: `enum "Name"` with optional `: <underlyingOrdinal>`. Enumerator
        // names live in the SemanticModel symbol table, not the type record;
        // only the nominal name + underlying TypeKind round-trip here.
        if (kw == "enum") {
            std::string name = takeStr();
            TypeKind underlying = TypeKind::I32;
            if (accept(Tk::Colon)) {
                // ⚠ FAIL LOUD. This read an ordinal and kept `I32` when it fell
                // outside `Count_` — a SUCCESSFUL parse of `enum "E" : 9999` that
                // returned an enumeration with the WRONG underlying type and no
                // diagnostic, on a decoder `parseTypeFromText` puts on the shipped
                // FFI-descriptor and builtin-signature paths. The range check was
                // the tell: it is the half of a validation whose other half — the
                // refusal — was never written, and an in-range ordinal naming a
                // NON-integer kind slipped through it untouched. `orMalformed`
                // projects the accepted set off the same table the lookup uses
                // (D-TEXT-TIER-ENUM-UNDERLYING-SERIALIZED-AS-A-TYPEKIND-ORDINAL).
                //
                // ⓘ WHETHER THE NAMED KIND IS AN INTEGER IS NOT DECIDED HERE. The
                // semantic tier already owns that rule (`S_InvalidEnumUnderlyingType`,
                // c.lang.json's `enumUnderlyingType`); re-deciding it in the
                // text reader would be a second owner of one fact, which is the
                // defect this whole file has been closing.
                std::string const n = takeIdent();
                underlying = orMalformed(kHirTextPrimTable, n,
                                         "enum underlying type", TypeKind::I32);
            }
            return interner_.enumType(name, underlying); }
        if (kw == "fn") {
            // Param list, handled here (not via parseTypeListUntil) so a trailing
            // `...` variadic marker is accepted as the last "param" instead of
            // tripping parseType's "expected a type". Shapes: `fn()`, `fn(i32)`,
            // `fn(ptr<char>, i32, ...)` (variadic), `fn(...)` (variadic, no fixed).
            expect(Tk::LParen, "'('");
            std::vector<TypeId> params;
            bool isVariadic = false;
            while (!peekIs(Tk::RParen) && !peekIs(Tk::Eof)) {
                if (accept(Tk::Ellipsis)) { isVariadic = true; break; }   // `...` only valid trailing
                params.push_back(parseType());
                if (!accept(Tk::Comma)) break;
            }
            expect(Tk::RParen, "')'");
            expect(Tk::Arrow, "'->'"); TypeId result = parseType();
            CallConv cc = CallConv::CcSysV;
            if (acceptKeyword("cc")) { std::string n = takeIdent(); cc = orMalformed(kCallConvTable, n, "calling convention", CallConv::CcSysV); }
            return interner_.fnSig(params, result, cc, isVariadic);
        }
        if (kw == "ext") {
            std::string name = takeStr();
            expect(Tk::LParen, "'('"); auto args = parseTypeListUntil(Tk::RParen); expect(Tk::RParen, "')'");
            std::vector<std::int64_t> scalars;
            if (accept(Tk::LBrack)) {
                while (!peekIs(Tk::RBrack) && !peekIs(Tk::Eof)) {
                    bool neg = accept(Tk::Minus);
                    std::int64_t v = static_cast<std::int64_t>(takeInt());
                    scalars.push_back(neg ? -v : v);
                    if (!accept(Tk::Comma)) break;
                }
                expect(Tk::RBrack, "']'");
            }
            TypeKindId kindId = typeReg_.registerExtension(name, {});
            return interner_.extension(kindId, name, args, scalars);
        }
        // c82 (D-FFI-DESCRIPTOR-VA-LIST-TYPE): caller-supplied named-type
        // bindings — the LAST resort before the unknown-type reject, so a
        // binding can never shadow a primitive or a structural keyword. A
        // bound but INVALID TypeId falls through to the reject (never a
        // silent InvalidType success).
        for (NamedTypeBinding const& nb : namedTypes_) {
            if (nb.name == kw && nb.type.valid()) return nb.type;
        }
        // D-TEXT-TIER-REFUSALS-NAME-NO-ACCEPTED-SET: this named nothing at all —
        // on the production that decodes every shipped FFI descriptor's type
        // strings, where the author reading the message is writing a `.json`
        // descriptor and has no other way to learn what the decoder takes.
        malformed(std::format("unknown type '{}' — accepted: {}", kw,
                              typeKeywordsAccepted()));
        return InvalidType;
    }
};

} // namespace

std::unique_ptr<HirParseResult> parseHir(std::string_view text, CompilationUnitId cuId,
                                         DiagnosticReporter& reporter) {
    std::size_t const errBefore = reporter.errorCount();
    Parser parser{text, cuId, reporter};
    HirNodeId const root = parser.parse();

    if (!root.valid()) {
        // Header/fatal failure: hand back an empty module (built from the same
        // builder, so finish()'s root-provenance check passes) so the result is
        // still a well-formed object the caller can inspect.
        HirNodeId const empty = parser.builder_.makeModule({});
        auto res = std::make_unique<HirParseResult>(
            std::move(parser.builder_).finish(empty),
            std::move(*parser.ownedInterner_), std::move(parser.symbolNames_));
        res->ok = false;
        return res;
    }

    Hir hir = std::move(parser.builder_).finish(root);
    auto res = std::make_unique<HirParseResult>(
        std::move(hir), std::move(*parser.ownedInterner_), std::move(parser.symbolNames_));

    // Hand back the rebuilt literal pool (empty if the source used `#index`).
    res->literalPool = std::move(parser.pLiterals_);
    res->inlineAsmPool = std::move(parser.pInlineAsm_);

    // Apply the collected side-table annotations to the frozen module's maps.
    for (auto& [id, v] : parser.pLoc_)       res->sourceMap.set(id, v);
    for (auto& [id, v] : parser.pFfi_)        res->ffiMap.set(id, std::move(v));
    for (auto& [id, v] : parser.pShader_)     res->shaderMap.set(id, v);
    for (auto& [id, v] : parser.pTranspile_)  res->transpileMap.set(id, std::move(v));
    for (auto& d : parser.pDiag_) {
        DiagnosticInfo info = std::move(d.info);
        if (d.hasOrigin && d.originPre < parser.indexToId_.size()
            && parser.indexToId_[d.originPre].valid()) {
            info.origin = parser.indexToId_[d.originPre];
        }
        res->diagnosticMap.set(d.node, std::move(info));
    }

    // Verify-on-load: the round-trip is only clean if the rebuilt module verifies.
    HirVerifier verifier{res->hir, &res->sourceMap, &res->interner,
                         &res->inlineAsmPool};
    (void)verifier.verify(reporter);

    res->ok = reporter.errorCount() == errBefore;
    return res;
}

TypeId parseTypeFromText(std::string_view typeText, TypeInterner& interner,
                         TypeRegistry& typeReg, DiagnosticReporter& reporter,
                         std::span<NamedTypeBinding const> namedTypes) {
    std::size_t const errBefore = reporter.errorCount();

    // Reuse the ONE type-grammar decoder: drive the module parser's `parseType`
    // production over `typeText`, interning into the caller's interner/registry.
    Parser parser{typeText, interner, typeReg, reporter};
    parser.setNamedTypes(namedTypes);   // c82: caller-supplied identifier aliases
    TypeId const ty = parser.parseTypeFromTextEntry();

    // A standalone type string must be exactly one type. Trailing tokens (e.g.
    // `"i32 i32"`) are malformed — `parseTypeFromTextEntry` reports them.
    //
    // Fail loud, never partial: if ANY error was emitted while decoding (a
    // truncated `"fn(ptr<"`, an unknown keyword, leftover tokens), the text did
    // not name a well-formed type — return InvalidType rather than let a
    // half-built type escape as if it were valid.
    if (reporter.errorCount() != errBefore) return InvalidType;
    return ty;
}

} // namespace dss