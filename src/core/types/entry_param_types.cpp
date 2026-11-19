#include "core/types/entry_param_types.hpp"

namespace dss {

namespace {

// A pointee that classifies a one-level pointer: an 8-bit unit, a 16-bit unsigned, or an opaque object.
[[nodiscard]] std::optional<EntryParamShape> shapeOfPointerTo(TypeInterner const& intern, TypeId pointee) {
    switch (intern.kind(pointee)) {
    case TypeKind::Char:
    case TypeKind::I8:
        return EntryParamShape::PtrChar;
    case TypeKind::U16:
        return EntryParamShape::PtrU16;
    case TypeKind::Void:
    case TypeKind::Struct:
    case TypeKind::Union:
        return EntryParamShape::PtrOpaque;
    case TypeKind::Ptr: {
        // A pointer to a pointer: only to a pointer to an 8-bit unit or a 16-bit unsigned (an argument vector).
        switch (intern.kind(intern.operands(pointee)[0])) {
        case TypeKind::Char:
        case TypeKind::I8:
            return EntryParamShape::PtrPtrChar;
        case TypeKind::U16:
            return EntryParamShape::PtrPtrU16;
        default:
            return std::nullopt;
        }
    }
    default:
        return std::nullopt;
    }
}

}  // namespace

std::optional<EntryParamShape> classifyEntryParam(TypeInterner const& intern, TypeId type) {
    switch (intern.kind(type)) {
    case TypeKind::I32:
        return EntryParamShape::I32;
    case TypeKind::Ptr:
        return shapeOfPointerTo(intern, intern.operands(type)[0]);
    default:
        return std::nullopt;
    }
}

TypeId typeOfEntryParamShape(TypeInterner& intern, EntryParamShape shape) {
    switch (shape) {
    case EntryParamShape::I32:        return intern.primitive(TypeKind::I32);
    case EntryParamShape::PtrPtrChar: return intern.pointer(intern.pointer(intern.primitive(TypeKind::I8)));
    case EntryParamShape::PtrPtrU16:  return intern.pointer(intern.pointer(intern.primitive(TypeKind::U16)));
    case EntryParamShape::PtrChar:    return intern.pointer(intern.primitive(TypeKind::I8));
    case EntryParamShape::PtrU16:     return intern.pointer(intern.primitive(TypeKind::U16));
    case EntryParamShape::PtrOpaque:  return intern.pointer(intern.primitive(TypeKind::Void));
    case EntryParamShape::None:       break;
    }
    return InvalidType;   // the loader refuses a `none` shape: no verb row holds one
}

}  // namespace dss
