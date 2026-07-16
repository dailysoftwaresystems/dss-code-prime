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

// ── type qualifiers (D-CSUBSET-VOLATILE-POINTEE / c27 · D-CSUBSET-QUAL-BITSET) ──

TypeId TypeInterner::qualified(TypeId inner, std::int64_t addBits) {
    if (!inner.valid()) return InvalidType;
    // STRIP → UNION → RE-INTERN — never a "return inner if already qualified"
    // early-out, which would silently DROP `addBits` when `inner` is already a
    // qualifier (e.g. `_Atomic` over `volatile` collapsing to merely volatile — a
    // loss-of-atomicity miscompile). Read the existing mask off `inner` (0 if it is
    // not a qualifier skin), OR in the new bits, and re-intern ONE skin over the
    // material type. Idempotent AND order-independent by construction:
    // `qualified(qualified(T,A),B) == qualified(T, A|B)`. `volatile (volatile T)`
    // ≡ `volatile T` (C 6.7.3p5) falls out — the union of equal masks is unchanged.
    std::int64_t const merged = qualifierBits(inner) | addBits;
    TypeId const base = materialId_(inner);
    if (merged == 0) return base;  // no codegen-affecting qualifier ⇒ no skin
    std::array<TypeId, 1> const ops{base};
    std::array<std::int64_t, 1> const sc{merged};
    return internContent(TypeKind::VolatileQual, {}, ops, sc, {});
}

TypeId TypeInterner::volatileQualified(TypeId inner) {
    return qualified(inner, static_cast<std::int64_t>(QualBit::Volatile));
}

TypeId TypeInterner::atomicQualified(TypeId inner) {
    return qualified(inner, static_cast<std::int64_t>(QualBit::Atomic));
}

TypeId TypeInterner::materialId_(TypeId id) const {
    // Strip the qualifier skin to the material type. `qualified` never nests (it
    // merges bits into ONE skin), so this is at most one level, but loop
    // defensively (cost: a handful of valid ids never exceed depth 1). Reads
    // `arena_` directly so `kind()`/`operands()` can call this without recursing
    // back into themselves.
    while (id.valid() && arena_.at(id).kind == TypeKind::VolatileQual) {
        id = operandPool_[arena_.at(id).operandStart];
    }
    return id;
}

TypeId TypeInterner::stripVolatile(TypeId id) const {
    return materialId_(id);
}

std::int64_t TypeInterner::qualifierBits(TypeId id) const {
    // RAW read of the qualifier mask in scalar slot 0 — NOT the transparent
    // `scalars()`, which redirects THROUGH the skin to the inner type's scalars.
    // Returns 0 for any NON-qualifier id (so callers can OR unconditionally).
    if (!id.valid()) return 0;
    TypeRecord const& rec = arena_.at(id);
    if (rec.kind != TypeKind::VolatileQual) return 0;
    // A real qualifier ALWAYS carries exactly one nonzero-mask scalar (the sole
    // producer `qualified` never mints a skin with a zero/absent mask — a zero mask
    // returns the material type, no skin). A VolatileQual with no scalar is a corrupt
    // invariant; FAIL LOUD rather than silently return 0 — a silent 0 would make
    // `isVolatileQualified`/`isAtomicQualified` false, DROPPING the qualifier, which
    // is the exact silent-miscompile class this refactor exists to prevent.
    if (rec.scalarCount == 0)
        latticeFatal("qualifierBits: VolatileQual record has no mask scalar");
    return scalarPool_[rec.scalarStart];
}

bool TypeInterner::isVolatileQualified(TypeId id) const {
    return (qualifierBits(id) & static_cast<std::int64_t>(QualBit::Volatile)) != 0;
}

bool TypeInterner::isAtomicQualified(TypeId id) const {
    return (qualifierBits(id) & static_cast<std::int64_t>(QualBit::Atomic)) != 0;
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
    // c27: a `volatile T[]` is still an incomplete array — strip the skin first
    // so the raw-kind check sees Array (the public `kind()` is transparent, but
    // this reads `arena_` directly).
    id = materialId_(id);
    if (arena_.at(id).kind != TypeKind::Array) return false;
    auto const sc = scalars(id);
    // Exactly -1 — a VLA's -2 sentinel is a DISTINCT, complete type (isVlaArray),
    // so FAM logic (layout contribution, `sizeof` ill-formedness) never matches it.
    return !sc.empty() && sc[0] == kIncompleteArrayLength;
}

TypeId TypeInterner::vlaArray(TypeId element) {
    // VLA C1a (D-CSUBSET-VLA): the exact incompleteArray mirror — a kind=Array with
    // the `kVlaLength` (-2) sentinel. Dedups by content, so every `int` VLA shares
    // one TypeId; the per-declaration runtime bound is held out-of-band.
    return array(element, kVlaLength);
}

bool TypeInterner::isVlaArray(TypeId id) const {
    // Robust to an INVALID id: this predicate is called on symbol/declared types
    // that may be `InvalidType` (a declarator whose type failed to resolve — e.g. a
    // rejected multi-dim VLA), so an unguarded `arena_.at(invalid)` would abort
    // ("TypeId out of range"). An invalid type is not a VLA.
    if (!id.valid()) return false;
    // Strip a `volatile`-qualifier skin first (mirror isIncompleteArray) so a
    // `volatile`-qualified VLA still reads as Array on the raw kind.
    id = materialId_(id);
    if (arena_.at(id).kind != TypeKind::Array) return false;
    auto const sc = scalars(id);
    return !sc.empty() && sc[0] == kVlaLength;
}

bool TypeInterner::typeContainsVla(TypeId id) const {
    // VLA C3 (D-CSUBSET-VLA): walk the array-element spine (`ops[0]`), testing
    // `isVlaArray` at each level. `int a[5][n]` = array(vlaArray(int),5): the top
    // is a fixed Array (not a VLA), but its element IS a VLA → true. A fully-fixed
    // `int[5][5]` = array(array(int,5),5): no level is a VLA → false (no over-fire).
    // A `volatile`-skin at any level is seen through by the transparent `kind()` /
    // `operands()` accessors (mirroring isVlaArray's own materialId_ strip).
    while (id.valid()) {
        if (isVlaArray(id)) return true;
        if (kind(id) != TypeKind::Array) return false;   // non-array base — stop
        auto const ops = operands(id);
        if (ops.empty()) return false;
        id = ops[0];
    }
    return false;
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
// c107 (D-FFI-DESCRIPTOR-UNION-OVERLAY): explicit field offsets also enter the
// content identity so an explicit-offset struct is DISTINCT from the same
// field-types laid out naturally, AND both the shipped `structs` entry and the
// bare typedef's inline `struct "X" { T @off }` — which carry identical offsets —
// collapse to one TypeId. The offset mix is GUARDED on non-empty so a struct with
// NO explicit offsets hashes byte-identically to the pre-c107 function — every
// existing composite keeps its EXACT declSiteKey (literally no churn), and only an
// offset-bearing struct gets the extra mix.
[[nodiscard]] std::uint64_t
contentDeclSiteKey(std::span<TypeId const> fields,
                   std::span<std::int64_t const> bitWidthScalars,
                   std::span<std::uint64_t const> fieldOffsets = {},
                   std::span<std::uint32_t const> fieldAligns = {},
                   bool packed = false) {
    std::uint64_t h = kFnvOffset;
    h = fnvMix(h, fields.size());
    for (TypeId f : fields) h = fnvMix(h, f.v);
    h = fnvMix(h, bitWidthScalars.size());
    for (std::int64_t s : bitWidthScalars) h = fnvMix(h, static_cast<std::uint64_t>(s));
    if (!fieldOffsets.empty()) {
        h = fnvMix(h, fieldOffsets.size());
        for (std::uint64_t o : fieldOffsets) h = fnvMix(h, o);
    }
    // D-CSUBSET-MEMBER-ALIGNAS: member-alignas overrides enter the content identity
    // so an align-bearing struct is DISTINCT from the same fields laid out with
    // natural alignment. GUARDED on non-empty (exactly like offsets above) so an
    // align-free struct hashes byte-identically to the pre-alignas function — every
    // existing composite keeps its EXACT declSiteKey (no churn); only an align-
    // bearing struct gets the extra mix.
    if (!fieldAligns.empty()) {
        h = fnvMix(h, fieldAligns.size());
        for (std::uint32_t a : fieldAligns) h = fnvMix(h, a);
    }
    // D-CSUBSET-PACKED: the whole-composite packed flag enters the content identity
    // so a packed struct is DISTINCT from the same fields laid out padded. GUARDED on
    // TRUE (mirrors the offsets/aligns guards) so an UNPACKED composite hashes
    // byte-identically to the pre-packed function — every existing composite keeps
    // its EXACT declSiteKey (zero churn / round-trip + goldens unaffected); only a
    // packed struct gets the extra mix.
    if (packed) h = fnvMix(h, std::uint64_t{1});
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
                                     bool packed,
                                     std::span<std::int64_t const> fieldBitWidths,
                                     std::span<std::uint64_t const> fieldOffsets,
                                     std::span<std::uint32_t const> fieldAligns) {
    TypeRecord const& rec = arena_.at(id);
    if (rec.kind != TypeKind::Struct && rec.kind != TypeKind::Union) {
        latticeFatal("completeComposite: TypeId is not a Struct/Union");
    }
    auto it = compositeFields_.find(id.v);
    if (it == compositeFields_.end()) {
        latticeFatal("completeComposite: TypeId was not forward-minted as a composite");
    }
    auto sc = encodeFieldBitWidths(fields.size(), fieldBitWidths);
    // c107: explicit offsets are ALL-fields-or-NONE (a partial set is a caller bug).
    if (!fieldOffsets.empty() && fieldOffsets.size() != fields.size()) {
        latticeFatal("completeComposite: explicit field offsets must cover every "
                     "field (all-or-none)");
    }
    // D-CSUBSET-MEMBER-ALIGNAS: member-alignas overrides are ALL-fields-or-NONE too
    // (mirrors the offsets rule — a partial set is a caller bug).
    if (!fieldAligns.empty() && fieldAligns.size() != fields.size()) {
        latticeFatal("completeComposite: explicit field aligns must cover every "
                     "field (all-or-none)");
    }
    // D-CSUBSET-MEMBER-ALIGNAS: explicit offsets place fields wholesale (overriding
    // alignment entirely), so combining member-alignas WITH explicit offsets on the
    // same struct is contradictory — a caller bug. Fail loud rather than silently
    // let one channel win (mirrors the offsets-vs-bitfields rejection at layout).
    if (!fieldAligns.empty() && !fieldOffsets.empty()) {
        latticeFatal("completeComposite: a struct cannot carry BOTH member-alignas "
                     "overrides and explicit field offsets (offsets override "
                     "alignment wholesale)");
    }
    // D-CSUBSET-PACKED: explicit offsets ALSO place fields wholesale, so `packed`
    // (which removes derived padding) is contradictory with an explicit-offset
    // struct — a caller bug. Fail loud (mirrors the aligns-vs-offsets guard above).
    // packed + member-alignas is LEGAL (alignas raises per-field via the layout
    // MAX-fold even under a packed baseline), so no guard against that pair.
    if (packed && !fieldOffsets.empty()) {
        latticeFatal("completeComposite: a struct cannot be BOTH packed and carry "
                     "explicit field offsets (offsets place fields wholesale, "
                     "overriding padding)");
    }
    if (it->second.complete) {
        // Idempotent for an IDENTICAL re-completion (a benign re-resolution); a
        // CONFLICTING re-completion is a caller bug — fail loud rather than
        // silently keep stale fields or corrupt a shared TypeId.
        bool same = it->second.fields.size() == fields.size()
                 && it->second.bitWidthScalars.size() == sc.size()
                 && it->second.fieldOffsets.size() == fieldOffsets.size()
                 && it->second.fieldAligns.size() == fieldAligns.size()
                 && it->second.packed == packed;
        for (std::size_t i = 0; same && i < fields.size(); ++i)
            if (it->second.fields[i].v != fields[i].v) same = false;
        for (std::size_t i = 0; same && i < sc.size(); ++i)
            if (it->second.bitWidthScalars[i] != sc[i]) same = false;
        for (std::size_t i = 0; same && i < fieldOffsets.size(); ++i)
            if (it->second.fieldOffsets[i] != fieldOffsets[i]) same = false;
        for (std::size_t i = 0; same && i < fieldAligns.size(); ++i)
            if (it->second.fieldAligns[i] != fieldAligns[i]) same = false;
        if (!same) {
            latticeFatal("completeComposite: composite re-completed with different "
                         "fields (double-complete / tag redecl)");
        }
        return;
    }
    it->second.fields.assign(fields.begin(), fields.end());
    it->second.bitWidthScalars = std::move(sc);
    it->second.fieldOffsets.assign(fieldOffsets.begin(), fieldOffsets.end());
    it->second.fieldAligns.assign(fieldAligns.begin(), fieldAligns.end());
    it->second.packed = packed;
    it->second.complete = true;
    ++poolGen_;   // the field view changed — invalidate any pre-completion span
}

bool TypeInterner::isIncompleteComposite(TypeId id) const {
    // c27: a `volatile struct S` is incomplete iff S is — strip the skin so the
    // raw-kind check + side-table key see the material composite.
    id = materialId_(id);
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
    completeComposite(id, fields, /*packed=*/false);
    return id;
}

TypeId TypeInterner::structType(std::string_view name, std::span<TypeId const> fields,
                                std::span<std::int64_t const> fieldBitWidths) {
    auto const sc = encodeFieldBitWidths(fields.size(), fieldBitWidths);
    TypeId const id = internComposite(TypeKind::Struct, name,
                                      contentDeclSiteKey(fields, sc));
    completeComposite(id, fields, /*packed=*/false, fieldBitWidths);
    return id;
}

TypeId TypeInterner::structType(std::string_view name, std::span<TypeId const> fields,
                                std::span<std::int64_t const> fieldBitWidths,
                                std::span<std::uint64_t const> fieldOffsets) {
    // c107 (D-FFI-DESCRIPTOR-UNION-OVERLAY): the offsets enter the content identity
    // (contentDeclSiteKey) so the shipped `structs` entry and the inline typedef
    // struct-text — both carrying identical offsets — collapse to one TypeId. An
    // empty offsets span routes exactly like the 3-arg overload (byte-identical).
    auto const sc = encodeFieldBitWidths(fields.size(), fieldBitWidths);
    TypeId const id = internComposite(TypeKind::Struct, name,
                                      contentDeclSiteKey(fields, sc, fieldOffsets));
    completeComposite(id, fields, /*packed=*/false, fieldBitWidths, fieldOffsets);
    return id;
}

TypeId TypeInterner::structType(std::string_view name, std::span<TypeId const> fields,
                                std::span<std::int64_t const> fieldBitWidths,
                                std::span<std::uint64_t const> fieldOffsets,
                                std::span<std::uint32_t const> fieldAligns) {
    // D-CSUBSET-MEMBER-ALIGNAS: the member-alignas overrides enter the content
    // identity (contentDeclSiteKey) so an align-bearing struct is DISTINCT from the
    // same field-types aligned naturally, AND two align-bearing structs with
    // identical (name, fields, widths, offsets, aligns) collapse to one TypeId. An
    // empty aligns span routes exactly like the 4-arg overload (byte-identical).
    auto const sc = encodeFieldBitWidths(fields.size(), fieldBitWidths);
    TypeId const id = internComposite(
        TypeKind::Struct, name,
        contentDeclSiteKey(fields, sc, fieldOffsets, fieldAligns));
    completeComposite(id, fields, /*packed=*/false, fieldBitWidths, fieldOffsets,
                      fieldAligns);
    return id;
}

bool TypeInterner::hasExplicitOffsets(TypeId id) const {
    id = materialId_(id);
    TypeKind const k = arena_.at(id).kind;
    if (k != TypeKind::Struct && k != TypeKind::Union) return false;
    auto it = compositeFields_.find(id.v);
    return it != compositeFields_.end() && !it->second.fieldOffsets.empty();
}

std::optional<std::uint64_t>
TypeInterner::explicitFieldOffset(TypeId id, std::size_t i) const {
    id = materialId_(id);
    auto it = compositeFields_.find(id.v);
    if (it == compositeFields_.end() || i >= it->second.fieldOffsets.size()) {
        return std::nullopt;
    }
    return it->second.fieldOffsets[i];
}

bool TypeInterner::hasExplicitAligns(TypeId id) const {
    id = materialId_(id);
    TypeKind const k = arena_.at(id).kind;
    if (k != TypeKind::Struct && k != TypeKind::Union) return false;
    auto it = compositeFields_.find(id.v);
    return it != compositeFields_.end() && !it->second.fieldAligns.empty();
}

std::uint32_t TypeInterner::explicitFieldAlign(TypeId id, std::size_t i) const {
    id = materialId_(id);
    auto it = compositeFields_.find(id.v);
    if (it == compositeFields_.end() || i >= it->second.fieldAligns.size()) {
        return 0;   // no override → natural alignment (the ordinary path)
    }
    return it->second.fieldAligns[i];
}

bool TypeInterner::isPacked(TypeId id) const {
    id = materialId_(id);
    TypeKind const k = arena_.at(id).kind;
    if (k != TypeKind::Struct && k != TypeKind::Union) return false;
    auto it = compositeFields_.find(id.v);
    return it != compositeFields_.end() && it->second.packed;
}

TypeId TypeInterner::unionType(std::string_view name, std::span<TypeId const> variants) {
    std::span<std::int64_t const> const noWidths{};
    TypeId const id = internComposite(TypeKind::Union, name,
                                      contentDeclSiteKey(variants, noWidths));
    completeComposite(id, variants, /*packed=*/false);
    return id;
}

TypeId TypeInterner::unionType(std::string_view name, std::span<TypeId const> variants,
                               std::span<std::int64_t const> fieldBitWidths) {
    auto const sc = encodeFieldBitWidths(variants.size(), fieldBitWidths);
    TypeId const id = internComposite(TypeKind::Union, name,
                                      contentDeclSiteKey(variants, sc));
    completeComposite(id, variants, /*packed=*/false, fieldBitWidths);
    return id;
}

TypeId TypeInterner::enumType(std::string_view name, TypeKind underlying) {
    // scalars=[(int)underlying]; no operands (enumerator symbols carry
    // the enum's TypeId individually as Variables; the enum type itself
    // is identified nominally by name + tagged with its underlying type).
    std::array<std::int64_t, 1> const sc{static_cast<std::int64_t>(underlying)};
    return internContent(TypeKind::Enum, {}, {}, sc, names_.intern(name));
}

TypeId TypeInterner::bitInt(std::int64_t widthBits, bool isSigned) {
    // scalars=[widthBits, signed?1:0]; no operands, no name. The interner dedups
    // on the scalar pair, so `_BitInt(N)` / `unsigned _BitInt(N)` each intern once.
    std::array<std::int64_t, 2> const sc{widthBits, isSigned ? std::int64_t{1}
                                                             : std::int64_t{0}};
    return internContent(TypeKind::BitInt, {}, {}, sc, {});
}

std::int64_t TypeInterner::bitIntWidth(TypeId id) const {
    if (kind(id) != TypeKind::BitInt) latticeFatal("bitIntWidth: TypeId is not a BitInt");
    auto const sc = scalars(id);
    if (sc.empty()) latticeFatal("bitIntWidth: BitInt has no width scalar");
    return sc[0];
}

bool TypeInterner::bitIntIsSigned(TypeId id) const {
    if (kind(id) != TypeKind::BitInt) latticeFatal("bitIntIsSigned: TypeId is not a BitInt");
    auto const sc = scalars(id);
    // scalars[1] is the signedness flag; a 1-scalar BitInt (shouldn't occur via the
    // builder) is treated as signed (the C default).
    return sc.size() < 2 || sc[1] != 0;
}

TypeKind TypeInterner::bitIntContainerKind(TypeId id) const {
    if (kind(id) != TypeKind::BitInt)
        latticeFatal("bitIntContainerKind: TypeId is not a BitInt");
    std::int64_t const n = bitIntWidth(id);
    bool const s = bitIntIsSigned(id);
    // Smallest native integer container holding N bits.
    if (n <= 8)  return s ? TypeKind::I8  : TypeKind::U8;
    if (n <= 16) return s ? TypeKind::I16 : TypeKind::U16;
    if (n <= 32) return s ? TypeKind::I32 : TypeKind::U32;
    if (n <= 64) return s ? TypeKind::I64 : TypeKind::U64;
    // D-CSUBSET-BITINT-C2-WIDE (M1): N>64 has NO single native container — it is a
    // MULTI-LIMB memory value. A "container kind" query for a wide `_BitInt` is a
    // scalar-path LEAK: every wide consumer works on i64 LIMBS (never the whole type as
    // a scalar), so reaching here means a wide value slipped into the scalar path. FAIL
    // LOUD (a crash), never the old silent `Void` sentinel that would flow to codegen
    // as a garbage-width op. The C2 by-address diverts keep this unreachable for wide.
    latticeFatal("bitIntContainerKind: _BitInt(N>64) has no native container — a wide "
                 "_BitInt is multi-limb (memory), reached by ADDRESS, never as a scalar "
                 "value; this query is a scalar-path leak (D-CSUBSET-BITINT-C2-WIDE)");
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

TypeKind TypeInterner::kind(TypeId id) const {
    // c27 VolatileQual transparency: report the MATERIAL kind so every structural
    // consumer (layout/arith/codegen/classification — ~128 sites) dispatches on
    // the underlying kind WITHOUT a per-site strip. The wrapper is observable only
    // via `isVolatileQualified` / `get()`. A non-VolatileQual id is unaffected.
    return arena_.at(materialId_(id)).kind;
}

GuardedSpan<TypeId> TypeInterner::operands(TypeId id) const {
    // c27: see through a VolatileQual skin to the material type's operands — a
    // `volatile T`'s "children" ARE T's children (e.g. layout of a
    // VolatileQual(Ptr<X>) reads [X] exactly like Ptr<X>). Resolve the material id
    // ONCE and use it for BOTH the record read AND the composite side-table key
    // (so `volatile struct S` redirects to S's fields). Idempotency-safe.
    TypeId const mid = materialId_(id);
    TypeRecord const& record = arena_.at(mid);
    // D-CSUBSET-SELF-REFERENTIAL-STRUCT: a nominal composite stores its fields in
    // the side-table, not the operand pool — redirect transparently so every
    // existing composite-operand consumer (layout/ABI/HIR-verifier/brace-init/
    // struct-copy/text/reintern/type-checker) is UNCHANGED. The side-table entry's
    // `fields` vector is pointer-stable; the GuardedSpan still rides `poolGen_`
    // (bumped on completion) so a span held across a later mutation aborts as usual.
    if (record.kind == TypeKind::Struct || record.kind == TypeKind::Union) {
        auto it = compositeFields_.find(mid.v);
        if (it != compositeFields_.end()) {
            return guard_<TypeId>({it->second.fields.data(), it->second.fields.size()});
        }
    }
    return guard_<TypeId>({operandPool_.data() + record.operandStart, record.operandCount});
}

GuardedSpan<std::int64_t> TypeInterner::scalars(TypeId id) const {
    // c27: transparent over VolatileQual (see `operands`). Material id once, used
    // for both the record and the composite side-table key.
    TypeId const mid = materialId_(id);
    TypeRecord const& record = arena_.at(mid);
    // Composite bit-field widths likewise live in the side-table (read by the
    // layout engine's "any bit-field?" test + `fieldBitWidth`); redirect to keep
    // those consumers unchanged. Enum/FnSig/array scalars stay in the pool.
    if (record.kind == TypeKind::Struct || record.kind == TypeKind::Union) {
        auto it = compositeFields_.find(mid.v);
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
    // FC17.9(e) (D-CSUBSET-LONG-DOUBLE): F64 < F80 < F128 — renumbered IN
    // LOCKSTEP with type_rules.hpp's floatRank (a divergence is silent
    // wrong UAC).
    switch (k) {
        case TypeKind::F16:  return 1;
        case TypeKind::F32:  return 2;
        case TypeKind::F64:  return 3;
        case TypeKind::F80:  return 4;
        case TypeKind::F128: return 5;
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
