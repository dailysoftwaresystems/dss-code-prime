#include "core/types/type_lattice/type_lattice.hpp"

#include "core/types/grammar_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dss {

namespace detail {
// D-TYPEINTERNER-OPERAND-SPAN-LIFETIME-GUARD: a GuardedSpan detected a read after
// the interner pool was mutated since the view was created (a heap-use-after-free
// the Windows-only non-ASan local gate cannot otherwise see). Loud, deterministic.
[[noreturn]] void typeInternerStaleSpan() {
    std::fputs("dss::TypeInterner fatal: stale operand/scalar span read — the "
               "interner pool was mutated by a later intern since this view was "
               "created; retaining operands()/scalars()/fnParams() across an "
               "intern is a heap-use-after-free "
               "(D-TYPEINTERNER-OPERAND-SPAN-LIFETIME-GUARD).\n",
               stderr);
    std::abort();
}
}  // namespace detail

namespace {

[[noreturn]] void latticeFatal(char const* what) {
    std::fputs("dss::TypeLattice fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

[[nodiscard]] bool sameParameters(std::vector<TypeParam> const& a,
                                  std::vector<TypeParam> const& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].name != b[i].name || a[i].kind != b[i].kind) return false;
    }
    return true;
}

// FNV-1a over a value's 8 bytes.
constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime  = 1099511628211ull;

inline std::uint64_t fnvMix(std::uint64_t h, std::uint64_t word) {
    for (int byte = 0; byte < 8; ++byte) {
        h ^= (word & 0xffu);
        h *= kFnvPrime;
        word >>= 8;
    }
    return h;
}

} // namespace

// ── TypeInterner ──────────────────────────────────────────────────────────

TypeInterner::TypeInterner(CompilationUnitId owner) : arena_(owner) {
    if (!owner.valid()) {
        latticeFatal("TypeInterner: owner CompilationUnitId is invalid — every "
                     "TypeId would be untagged and cross-CU isolation lost");
    }
}

// ── TypeInterner: canonicalization ────────────────────────────────────────

std::uint64_t TypeInterner::hashContent(TypeKind kind, TypeKindId extensionKind,
                                        std::span<TypeId const> operands,
                                        std::span<std::int64_t const> scalars,
                                        TypeNameId name) const {
    std::uint64_t h = kFnvOffset;
    h = fnvMix(h, static_cast<std::uint64_t>(kind));
    h = fnvMix(h, extensionKind.v);
    h = fnvMix(h, name.v);
    h = fnvMix(h, operands.size());
    for (TypeId operand : operands) h = fnvMix(h, operand.v);
    h = fnvMix(h, scalars.size());
    for (std::int64_t scalar : scalars) h = fnvMix(h, static_cast<std::uint64_t>(scalar));
    return h;
}

bool TypeInterner::equalContent(TypeId existing, TypeKind kind, TypeKindId extensionKind,
                                std::span<TypeId const> operands,
                                std::span<std::int64_t const> scalars,
                                TypeNameId name) const {
    TypeRecord const& record = arena_.at(existing);
    if (record.kind != kind || record.extensionKind != extensionKind || record.name != name) {
        return false;
    }
    if (record.operandCount != operands.size() || record.scalarCount != scalars.size()) {
        return false;
    }
    for (std::uint32_t i = 0; i < record.operandCount; ++i) {
        if (operandPool_[record.operandStart + i].v != operands[i].v) return false;
    }
    for (std::uint32_t i = 0; i < record.scalarCount; ++i) {
        if (scalarPool_[record.scalarStart + i] != scalars[i]) return false;
    }
    return true;
}

TypeId TypeInterner::internContent(TypeKind kind, TypeKindId extensionKind,
                                   std::span<TypeId const> operands,
                                   std::span<std::int64_t const> scalars,
                                   TypeNameId name) {
    // The record invariant: a valid extensionKind iff the kind is Extension.
    if ((kind == TypeKind::Extension) != extensionKind.valid()) {
        latticeFatal("TypeInterner: extensionKind is valid iff kind == Extension");
    }
    const std::uint64_t h = hashContent(kind, extensionKind, operands, scalars, name);
    auto const range = byHash_.equal_range(h);
    for (auto it = range.first; it != range.second; ++it) {
        if (equalContent(it->second, kind, extensionKind, operands, scalars, name)) {
            return it->second;
        }
    }
    TypeRecord record;
    record.kind          = kind;
    record.extensionKind = extensionKind;
    record.name          = name;
    record.operandStart  = static_cast<std::uint32_t>(operandPool_.size());
    record.operandCount  = static_cast<std::uint32_t>(operands.size());
    operandPool_.insert(operandPool_.end(), operands.begin(), operands.end());
    record.scalarStart   = static_cast<std::uint32_t>(scalarPool_.size());
    record.scalarCount   = static_cast<std::uint32_t>(scalars.size());
    scalarPool_.insert(scalarPool_.end(), scalars.begin(), scalars.end());
    // The pools were just appended to (and may have reallocated): bump the
    // generation so any GuardedSpan handed out before this intern aborts on its
    // next read (D-TYPEINTERNER-OPERAND-SPAN-LIFETIME-GUARD). Reached only on a
    // genuinely NEW type — the dedup path above returns before any mutation.
    ++poolGen_;

    const TypeId id = arena_.addNode(record);
    byHash_.insert({h, id});
    return id;
}

// ── TypeInterner: builders ────────────────────────────────────────────────

TypeId TypeInterner::primitive(TypeKind kind) {
    return internContent(kind, {}, {}, {}, {});
}

TypeId TypeInterner::vector(TypeId element, std::int64_t lanes) {
    std::array<TypeId, 1> const ops{element};
    std::array<std::int64_t, 1> const sc{lanes};
    return internContent(TypeKind::Vector, {}, ops, sc, {});
}

TypeId TypeInterner::matrix(TypeId element, std::int64_t rows, std::int64_t cols) {
    std::array<TypeId, 1> const ops{element};
    std::array<std::int64_t, 2> const sc{rows, cols};
    return internContent(TypeKind::Matrix, {}, ops, sc, {});
}

TypeId TypeInterner::pointer(TypeId pointee) {
    std::array<TypeId, 1> const ops{pointee};
    return internContent(TypeKind::Ptr, {}, ops, {}, {});
}

TypeId TypeInterner::reference(TypeId referent) {
    std::array<TypeId, 1> const ops{referent};
    return internContent(TypeKind::Ref, {}, ops, {}, {});
}

TypeId TypeInterner::nullable(TypeId inner) {
    std::array<TypeId, 1> const ops{inner};
    return internContent(TypeKind::Nullable, {}, ops, {}, {});
}

TypeId TypeInterner::optional(TypeId inner) {
    std::array<TypeId, 1> const ops{inner};
    return internContent(TypeKind::Optional, {}, ops, {}, {});
}

TypeId TypeInterner::array(TypeId element, std::int64_t length) {
    std::array<TypeId, 1> const ops{element};
    std::array<std::int64_t, 1> const sc{length};
    return internContent(TypeKind::Array, {}, ops, sc, {});
}

TypeId TypeInterner::slice(TypeId element) {
    std::array<TypeId, 1> const ops{element};
    return internContent(TypeKind::Slice, {}, ops, {}, {});
}

TypeId TypeInterner::incompleteArray(TypeId element) {
    return array(element, kIncompleteArrayLength);
}

bool TypeInterner::isIncompleteArray(TypeId id) const {
    if (arena_.at(id).kind != TypeKind::Array) return false;
    auto const sc = scalars(id);
    return !sc.empty() && sc[0] == kIncompleteArrayLength;
}

TypeId TypeInterner::tuple(std::span<TypeId const> elements) {
    return internContent(TypeKind::Tuple, {}, elements, {}, {});
}

namespace {
// FC8 bitfields (D-CSUBSET-BITFIELD): encode the per-field widths into the
// scalar form — (width + 1) per field, `kNotBitfield` → 0. Returns EMPTY
// when no field is a bitfield, so a bitfield-free composite carries empty
// scalars (no layout/codegen churn). Shared by struct + union.
[[nodiscard]] std::vector<std::int64_t>
encodeFieldBitWidths(std::size_t fieldCount,
                     std::span<std::int64_t const> fieldBitWidths) {
    bool anyBitfield = false;
    for (std::int64_t const w : fieldBitWidths) {
        if (w != kNotBitfield) { anyBitfield = true; break; }
    }
    if (!anyBitfield) return {};
    std::vector<std::int64_t> sc;
    sc.reserve(fieldCount);
    for (std::size_t i = 0; i < fieldCount; ++i) {
        std::int64_t const w =
            i < fieldBitWidths.size() ? fieldBitWidths[i] : kNotBitfield;
        sc.push_back(w == kNotBitfield ? std::int64_t{0} : w + 1);
    }
    return sc;
}

// D-CSUBSET-SELF-REFERENTIAL-STRUCT: derive a `declSiteKey` for the COMPLETE-AT-
// ONCE composite path (the 2-arg/3-arg structType/unionType overloads — shipped
// descriptors, reintern, text round-trip). It hashes the FIELD CONTENT (the field
// TypeIds + the bit-width scalars) so that, AMONG composites of the same (kind,
// name), two with identical fields collapse to one TypeId (canonicalization
// preserved) while same-name DIFFERENT-field composites stay distinct
// (StructIsNominalAndStructural). The top bit is set so a content-derived key can
// never collide with a decl-site key (which the semantic analyzer packs from
// 32-bit tree/node ids — always < 2^63).
[[nodiscard]] std::uint64_t
contentDeclSiteKey(std::span<TypeId const> fields,
                   std::span<std::int64_t const> bitWidthScalars) {
    std::uint64_t h = kFnvOffset;
    h = fnvMix(h, fields.size());
    for (TypeId f : fields) h = fnvMix(h, f.v);
    h = fnvMix(h, bitWidthScalars.size());
    for (std::int64_t s : bitWidthScalars) h = fnvMix(h, static_cast<std::uint64_t>(s));
    return h | (std::uint64_t{1} << 63);
}
} // namespace

// ── NOMINAL composites (D-CSUBSET-SELF-REFERENTIAL-STRUCT) ──

TypeId TypeInterner::internComposite(TypeKind kind, std::string_view name,
                                     std::uint64_t declSiteKey) {
    if (kind != TypeKind::Struct && kind != TypeKind::Union) {
        latticeFatal("internComposite: kind must be Struct or Union");
    }
    TypeNameId const nameId = names_.intern(name);
    // Dedup by NOMINAL identity (kind, name, declSiteKey). Hash all three; verify
    // a candidate against the record's kind+name AND the side-table's declSiteKey
    // so a hash collision can never alias two distinct composites ("one composite →
    // one TypeId", the byHash-dedup charge).
    std::uint64_t h = kFnvOffset;
    h = fnvMix(h, static_cast<std::uint64_t>(kind));
    h = fnvMix(h, nameId.v);
    h = fnvMix(h, declSiteKey);
    auto const range = byHash_.equal_range(h);
    for (auto it = range.first; it != range.second; ++it) {
        TypeRecord const& rec = arena_.at(it->second);
        if (rec.kind != kind || rec.name != nameId) continue;
        auto cf = compositeFields_.find(it->second.v);
        if (cf != compositeFields_.end() && cf->second.declSiteKey == declSiteKey) {
            return it->second;   // the existing forward record
        }
    }
    // Mint a fresh INCOMPLETE record. Operand/scalar ranges stay empty: fields live
    // in the side-table, never the operand pool (no back-patch).
    TypeRecord record;
    record.kind         = kind;
    record.name         = nameId;
    record.operandStart = static_cast<std::uint32_t>(operandPool_.size());
    record.operandCount = 0;
    record.scalarStart  = static_cast<std::uint32_t>(scalarPool_.size());
    record.scalarCount  = 0;
    const TypeId id = arena_.addNode(record);
    byHash_.insert({h, id});
    CompositeFields entry;
    entry.declSiteKey = declSiteKey;
    entry.complete    = false;
    compositeFields_.emplace(id.v, std::move(entry));
    ++poolGen_;   // a new composite view boundary (incomplete entry created)
    return id;
}

TypeId TypeInterner::forwardComposite(TypeKind kind, std::string_view name,
                                      std::uint64_t declSiteKey) {
    return internComposite(kind, name, declSiteKey);
}

void TypeInterner::completeComposite(TypeId id, std::span<TypeId const> fields,
                                     std::span<std::int64_t const> fieldBitWidths) {
    TypeRecord const& rec = arena_.at(id);
    if (rec.kind != TypeKind::Struct && rec.kind != TypeKind::Union) {
        latticeFatal("completeComposite: TypeId is not a Struct/Union");
    }
    auto it = compositeFields_.find(id.v);
    if (it == compositeFields_.end()) {
        latticeFatal("completeComposite: TypeId was not forward-minted as a composite");
    }
    auto sc = encodeFieldBitWidths(fields.size(), fieldBitWidths);
    if (it->second.complete) {
        // Idempotent for an IDENTICAL re-completion (a benign re-resolution); a
        // CONFLICTING re-completion is a caller bug — fail loud rather than
        // silently keep stale fields or corrupt a shared TypeId.
        bool same = it->second.fields.size() == fields.size()
                 && it->second.bitWidthScalars.size() == sc.size();
        for (std::size_t i = 0; same && i < fields.size(); ++i)
            if (it->second.fields[i].v != fields[i].v) same = false;
        for (std::size_t i = 0; same && i < sc.size(); ++i)
            if (it->second.bitWidthScalars[i] != sc[i]) same = false;
        if (!same) {
            latticeFatal("completeComposite: composite re-completed with different "
                         "fields (double-complete / tag redecl)");
        }
        return;
    }
    it->second.fields.assign(fields.begin(), fields.end());
    it->second.bitWidthScalars = std::move(sc);
    it->second.complete = true;
    ++poolGen_;   // the field view changed — invalidate any pre-completion span
}

bool TypeInterner::isIncompleteComposite(TypeId id) const {
    TypeKind const k = arena_.at(id).kind;
    if (k != TypeKind::Struct && k != TypeKind::Union) return false;
    auto it = compositeFields_.find(id.v);
    // No side-table entry ⇒ never forward-minted (e.g. a composite minted before
    // this mechanism) ⇒ treat as complete (its fields, if any, live in the pool).
    // An entry with complete=false is the genuine incomplete (forward-only) case.
    return it != compositeFields_.end() && !it->second.complete;
}

TypeId TypeInterner::structType(std::string_view name, std::span<TypeId const> fields) {
    std::span<std::int64_t const> const noWidths{};
    TypeId const id = internComposite(TypeKind::Struct, name,
                                      contentDeclSiteKey(fields, noWidths));
    completeComposite(id, fields, {});
    return id;
}

TypeId TypeInterner::structType(std::string_view name, std::span<TypeId const> fields,
                                std::span<std::int64_t const> fieldBitWidths) {
    auto const sc = encodeFieldBitWidths(fields.size(), fieldBitWidths);
    TypeId const id = internComposite(TypeKind::Struct, name,
                                      contentDeclSiteKey(fields, sc));
    completeComposite(id, fields, fieldBitWidths);
    return id;
}

TypeId TypeInterner::unionType(std::string_view name, std::span<TypeId const> variants) {
    std::span<std::int64_t const> const noWidths{};
    TypeId const id = internComposite(TypeKind::Union, name,
                                      contentDeclSiteKey(variants, noWidths));
    completeComposite(id, variants, {});
    return id;
}

TypeId TypeInterner::unionType(std::string_view name, std::span<TypeId const> variants,
                               std::span<std::int64_t const> fieldBitWidths) {
    auto const sc = encodeFieldBitWidths(variants.size(), fieldBitWidths);
    TypeId const id = internComposite(TypeKind::Union, name,
                                      contentDeclSiteKey(variants, sc));
    completeComposite(id, variants, fieldBitWidths);
    return id;
}

TypeId TypeInterner::enumType(std::string_view name, TypeKind underlying) {
    // scalars=[(int)underlying]; no operands (enumerator symbols carry
    // the enum's TypeId individually as Variables; the enum type itself
    // is identified nominally by name + tagged with its underlying type).
    std::array<std::int64_t, 1> const sc{static_cast<std::int64_t>(underlying)};
    return internContent(TypeKind::Enum, {}, {}, sc, names_.intern(name));
}

TypeId TypeInterner::fnSig(std::span<TypeId const> params, TypeId result, CallConv cc) {
    // 3-arg backward-compat path: non-variadic. Delegates to the 4-arg
    // overload with isVariadic=false. Pre-13.4 callers (30+ sites)
    // continue to intern the EXISTING shape (scalars=[(int)cc] only —
    // 1 slot) so legacy round-trip tests + cached TypeIds remain
    // bit-identical.
    return fnSig(params, result, cc, /*isVariadic=*/false);
}

TypeId TypeInterner::fnSig(std::span<TypeId const> params, TypeId result, CallConv cc,
                           bool isVariadic) {
    // operands = [result, params...] so the result is recoverable at a
    // fixed position. D-LANG-VARIADIC (step 13.4, 2026-06-02): scalars
    // encoding depends on isVariadic:
    //   non-variadic → scalars=[(int)cc]               (1 slot, legacy)
    //   variadic     → scalars=[(int)cc, isVariadic=1] (2 slots)
    // The 1-slot legacy form preserves bit-identical TypeIds for
    // pre-13.4 callers (30+ sites in semantic / hir / mir / lir / ffi
    // not yet ported to the 4-arg overload). The 2-slot form opts in
    // to variadic encoding only at sites that explicitly declare it
    // — c-subset's `...` paramList tail is the only declarer today.
    // Decoder `fnIsVariadic` handles both forms (false for legacy).
    std::vector<TypeId> ops;
    ops.reserve(params.size() + 1);
    ops.push_back(result);
    ops.insert(ops.end(), params.begin(), params.end());
    if (isVariadic) {
        std::array<std::int64_t, 2> const sc{
            static_cast<std::int64_t>(cc),
            std::int64_t{1}};
        return internContent(TypeKind::FnSig, {}, ops, sc, {});
    }
    std::array<std::int64_t, 1> const sc{static_cast<std::int64_t>(cc)};
    return internContent(TypeKind::FnSig, {}, ops, sc, {});
}

TypeId TypeInterner::extension(TypeKindId kind, std::string_view name,
                               std::span<TypeId const> typeArgs,
                               std::span<std::int64_t const> scalarArgs) {
    return internContent(TypeKind::Extension, kind, typeArgs, scalarArgs, names_.intern(name));
}

// ── TypeInterner: accessors ───────────────────────────────────────────────

GuardedSpan<TypeId> TypeInterner::operands(TypeId id) const {
    TypeRecord const& record = arena_.at(id);
    // D-CSUBSET-SELF-REFERENTIAL-STRUCT: a nominal composite stores its fields in
    // the side-table, not the operand pool — redirect transparently so every
    // existing composite-operand consumer (layout/ABI/HIR-verifier/brace-init/
    // struct-copy/text/reintern/type-checker) is UNCHANGED. The side-table entry's
    // `fields` vector is pointer-stable; the GuardedSpan still rides `poolGen_`
    // (bumped on completion) so a span held across a later mutation aborts as usual.
    if (record.kind == TypeKind::Struct || record.kind == TypeKind::Union) {
        auto it = compositeFields_.find(id.v);
        if (it != compositeFields_.end()) {
            return guard_<TypeId>({it->second.fields.data(), it->second.fields.size()});
        }
    }
    return guard_<TypeId>({operandPool_.data() + record.operandStart, record.operandCount});
}

GuardedSpan<std::int64_t> TypeInterner::scalars(TypeId id) const {
    TypeRecord const& record = arena_.at(id);
    // Composite bit-field widths likewise live in the side-table (read by the
    // layout engine's "any bit-field?" test + `fieldBitWidth`); redirect to keep
    // those consumers unchanged. Enum/FnSig/array scalars stay in the pool.
    if (record.kind == TypeKind::Struct || record.kind == TypeKind::Union) {
        auto it = compositeFields_.find(id.v);
        if (it != compositeFields_.end()) {
            return guard_<std::int64_t>(
                {it->second.bitWidthScalars.data(), it->second.bitWidthScalars.size()});
        }
    }
    return guard_<std::int64_t>({scalarPool_.data() + record.scalarStart, record.scalarCount});
}

std::string_view TypeInterner::name(TypeId id) const {
    // NOTE: the returned string_view points into the SEPARATE `names_` string
    // interner, NOT the operand/scalar pools — so it is deliberately OUT of the
    // GuardedSpan scope (D-TYPEINTERNER-OPERAND-SPAN-LIFETIME-GUARD covers
    // operands()/scalars()/fnParams()). Name views are consumed immediately and
    // have no historical dangling incident; if a names_-realloc hazard ever
    // surfaces, extend the same generation-guard pattern to a guarded string view.
    return names_.name(arena_.at(id).name);
}

std::optional<std::uint32_t>
TypeInterner::fieldBitWidth(TypeId structId, std::size_t fieldIndex) const {
    // FC8 bitfields: scalars[i] holds (width + 1) for a bitfield, 0 for an
    // ordinary field; an empty scalar pool (no bitfields in the struct) → all
    // ordinary. Decode back to the width (a zero-width bitfield → 0, not nullopt).
    auto const sc = scalars(structId);
    if (fieldIndex >= sc.size()) return std::nullopt;
    std::int64_t const enc = sc[fieldIndex];
    if (enc <= 0) return std::nullopt;
    return static_cast<std::uint32_t>(enc - 1);
}

TypeId TypeInterner::fnResult(TypeId id) const {
    if (kind(id) != TypeKind::FnSig) latticeFatal("fnResult: TypeId is not a FnSig");
    return operands(id)[0];   // operands = [result, params...]; always >= 1
}

GuardedSpan<TypeId> TypeInterner::fnParams(TypeId id) const {
    if (kind(id) != TypeKind::FnSig) latticeFatal("fnParams: TypeId is not a FnSig");
    return operands(id).subspan(1);
}

bool TypeInterner::fnIsVariadic(TypeId id) const {
    if (kind(id) != TypeKind::FnSig) latticeFatal("fnIsVariadic: TypeId is not a FnSig");
    auto const sc = scalars(id);
    // D-LANG-VARIADIC (step 13.4): every FnSig MUST carry at least
    // the cc scalar (post-13.4 audit-fold HIGH-1). Pre-13.4 FnSigs
    // encode scalars=[(int)cc] (1 slot); variadic FnSigs encode
    // scalars=[(int)cc, 1] (2 slots). A 0-slot FnSig has no cc —
    // structurally impossible via the public builder, fatal here to
    // pin the invariant against a future builder bypass.
    if (sc.empty()) latticeFatal("fnIsVariadic: FnSig has no cc scalar");
    return sc.size() >= 2 && sc[1] != 0;
}

namespace {

// Integer-rank (C99 §6.3.1.1). Bool < Char/I8/U8 < I16/U16 < I32/U32 < I64/U64
// < I128/U128. Same rank for signed/unsigned of the same width — the
// signedness tie-break lives in `commonType` below. Returns -1 for
// non-integer kinds.
[[nodiscard]] int integerRank(TypeKind k) noexcept {
    switch (k) {
        case TypeKind::Bool:                                   return 0;
        case TypeKind::I8:   case TypeKind::U8:
        case TypeKind::Char: case TypeKind::Byte:              return 1;
        case TypeKind::I16:  case TypeKind::U16:               return 2;
        case TypeKind::I32:  case TypeKind::U32:               return 3;
        case TypeKind::I64:  case TypeKind::U64:               return 4;
        case TypeKind::I128: case TypeKind::U128:              return 5;
        default: return -1;
    }
}

[[nodiscard]] int floatRank(TypeKind k) noexcept {
    switch (k) {
        case TypeKind::F16:  return 1;
        case TypeKind::F32:  return 2;
        case TypeKind::F64:  return 3;
        case TypeKind::F128: return 4;
        default: return -1;
    }
}

[[nodiscard]] bool isSignedInt(TypeKind k) noexcept {
    return k == TypeKind::I8  || k == TypeKind::I16 || k == TypeKind::I32
        || k == TypeKind::I64 || k == TypeKind::I128 || k == TypeKind::Char;
}

// Map a (rank, signed) pair to the canonical TypeKind. Used by commonType
// when synthesizing the result kind after integer promotion / cross-
// signedness tie-break.
[[nodiscard]] TypeKind integerKindAtRank(int rank, bool isSigned) noexcept {
    switch (rank) {
        case 0: return TypeKind::Bool;
        case 1: return isSigned ? TypeKind::I8   : TypeKind::U8;
        case 2: return isSigned ? TypeKind::I16  : TypeKind::U16;
        case 3: return isSigned ? TypeKind::I32  : TypeKind::U32;
        case 4: return isSigned ? TypeKind::I64  : TypeKind::U64;
        case 5: return isSigned ? TypeKind::I128 : TypeKind::U128;
        default: return TypeKind::Void;
    }
}

} // namespace

TypeId TypeInterner::commonType(TypeId a, TypeId b) {
    if (!a.valid() || !b.valid()) return InvalidType;
    TypeKind const ka = kind(a);
    TypeKind const kb = kind(b);
    // Same-type short-circuit applies ONLY when no integer promotion is
    // owed. C99 §6.3.1.8: integer promotion (rank-<int → int) applies
    // per-operand BEFORE the common-type computation, so `u16 + u16`
    // produces `i32`, not `u16`. Floats and ≥int integer ranks have
    // identity promotion and may short-circuit.
    if (a == b) {
        int const r = integerRank(ka);
        if (r < 0 || r >= 3) return a;
        // Else fall through to the promotion path below.
    }
    // Floating-point hierarchy wins over integer (per C99): if either is
    // float, promote both to the wider float.
    int const fa = floatRank(ka);
    int const fb = floatRank(kb);
    if (fa >= 0 || fb >= 0) {
        // If only one side is float, the other must promote to it; the
        // common type is the float side (or the wider float if both).
        if (fa >= 0 && fb < 0) return a;
        if (fb >= 0 && fa < 0) return b;
        // Both float: wider rank wins.
        return (fa >= fb) ? a : b;
    }
    // Integer side: apply integer promotions (rank < I32 → I32) and then
    // pick the wider rank, with signedness tie-break on equal width.
    int const ra = integerRank(ka);
    int const rb = integerRank(kb);
    if (ra < 0 || rb < 0) return InvalidType;  // not arithmetic
    // Integer promotion: anything narrower than I32 (rank<3) widens to I32
    // (signed). Bool and char follow the same rule.
    int const prA = ra < 3 ? 3 : ra;
    int const prB = rb < 3 ? 3 : rb;
    bool const sA = ra < 3 ? true : isSignedInt(ka);
    bool const sB = rb < 3 ? true : isSignedInt(kb);
    int const targetRank = std::max(prA, prB);
    bool signedness;
    if (prA == prB) {
        // Same width → unsigned wins (C99: same-rank signed/unsigned →
        // unsigned). Bool widening to I32 is signed by promotion.
        signedness = sA && sB;
    } else {
        // Different width → take the wider side's signedness.
        signedness = (prA > prB) ? sA : sB;
    }
    return primitive(integerKindAtRank(targetRank, signedness));
}

// ── TypeRegistry ──────────────────────────────────────────────────────────

TypeKindId TypeRegistry::registerExtension(std::string_view name, std::vector<TypeParam> parameters) {
    if (auto it = byName_.find(std::string(name)); it != byName_.end()) {
        // Idempotent for an identical re-declaration. A mismatched parameter
        // list is a genuine conflict (e.g. two schemas declaring the same
        // extension differently into one registry) — fail loud rather than
        // silently keep the first.
        ExtensionDescriptor const& existing = extensions_[it->second.v - kFirstExtensionKind];
        if (!sameParameters(existing.parameters, parameters)) {
            latticeFatal("TypeRegistry::registerExtension: extension re-declared "
                         "with a different parameter list");
        }
        return it->second;
    }
    const TypeKindId kind{nextKind_++};
    extensions_.push_back(ExtensionDescriptor{std::string(name), kind, std::move(parameters),
                                              sourceLanguage_});
    byName_.emplace(std::string(name), kind);
    return kind;
}

std::optional<TypeKindId> TypeRegistry::findExtension(std::string_view name) const {
    auto const it = byName_.find(std::string(name));
    if (it == byName_.end()) return std::nullopt;
    return it->second;
}

ExtensionDescriptor const& TypeRegistry::descriptor(TypeKindId kind) const {
    if (kind.v < kFirstExtensionKind) {
        latticeFatal("TypeRegistry::descriptor: kindId is a core kind, not an extension");
    }
    const std::size_t index = kind.v - kFirstExtensionKind;
    if (index >= extensions_.size()) {
        latticeFatal("TypeRegistry::descriptor: unknown extension kindId");
    }
    return extensions_[index];
}

// ── schema integration ────────────────────────────────────────────────────

void registerSchemaTypeExtensions(TypeRegistry& registry, GrammarSchema const& schema) {
    for (TypeExtensionDescriptor const& ext : schema.typeExtensions()) {
        registry.registerExtension(ext.name, ext.parameters);
    }
}

} // namespace dss
