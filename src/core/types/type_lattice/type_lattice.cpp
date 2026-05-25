#include "core/types/type_lattice/type_lattice.hpp"

#include "core/types/grammar_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace dss {

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

TypeId TypeInterner::tuple(std::span<TypeId const> elements) {
    return internContent(TypeKind::Tuple, {}, elements, {}, {});
}

TypeId TypeInterner::structType(std::string_view name, std::span<TypeId const> fields) {
    return internContent(TypeKind::Struct, {}, fields, {}, names_.intern(name));
}

TypeId TypeInterner::unionType(std::string_view name, std::span<TypeId const> variants) {
    return internContent(TypeKind::Union, {}, variants, {}, names_.intern(name));
}

TypeId TypeInterner::fnSig(std::span<TypeId const> params, TypeId result, CallConv cc) {
    // operands = [result, params...] so the result is recoverable at a fixed
    // position; scalars = [(int)cc].
    std::vector<TypeId> ops;
    ops.reserve(params.size() + 1);
    ops.push_back(result);
    ops.insert(ops.end(), params.begin(), params.end());
    std::array<std::int64_t, 1> const sc{static_cast<std::int64_t>(cc)};
    return internContent(TypeKind::FnSig, {}, ops, sc, {});
}

TypeId TypeInterner::extension(TypeKindId kind, std::string_view name,
                               std::span<TypeId const> typeArgs,
                               std::span<std::int64_t const> scalarArgs) {
    return internContent(TypeKind::Extension, kind, typeArgs, scalarArgs, names_.intern(name));
}

// ── TypeInterner: accessors ───────────────────────────────────────────────

std::span<TypeId const> TypeInterner::operands(TypeId id) const {
    TypeRecord const& record = arena_.at(id);
    return {operandPool_.data() + record.operandStart, record.operandCount};
}

std::span<std::int64_t const> TypeInterner::scalars(TypeId id) const {
    TypeRecord const& record = arena_.at(id);
    return {scalarPool_.data() + record.scalarStart, record.scalarCount};
}

std::string_view TypeInterner::name(TypeId id) const {
    return names_.name(arena_.at(id).name);
}

TypeId TypeInterner::fnResult(TypeId id) const {
    if (kind(id) != TypeKind::FnSig) latticeFatal("fnResult: TypeId is not a FnSig");
    return operands(id)[0];   // operands = [result, params...]; always >= 1
}

std::span<TypeId const> TypeInterner::fnParams(TypeId id) const {
    if (kind(id) != TypeKind::FnSig) latticeFatal("fnParams: TypeId is not a FnSig");
    return operands(id).subspan(1);
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
