#pragma once

#include "hir/hir.hpp"   // HirAttribute<T>

#include "hir/attributes/alignment_attr.hpp"
#include "hir/attributes/diagnostic_info.hpp"
#include "hir/attributes/ffi_metadata.hpp"
#include "hir/attributes/linkage_attr.hpp"
#include "hir/attributes/mutability_attr.hpp"
#include "hir/attributes/shader_intrinsic.hpp"
#include "hir/attributes/source_span.hpp"
#include "hir/attributes/transpile_hints.hpp"
#include "hir/attributes/volatile_attr.hpp"

// HIR attribute catalog (HR5). The standard per-node side-tables, each a named
// alias of `HirAttribute<T>` over the value structs in `attributes/`. A consumer
// includes this one header to bind any of them to a frozen `Hir`:
//
//     HirSourceMap spans{hir};
//     spans.set(node, HirSourceLoc{buffer, span});
//
// These are the side-tables plan §2.6 enumerates. They share the SP1
// `ArenaAttribute` substrate (sparse→dense auto-promotion, cross-module guard),
// so a module annotates without mutating its frozen nodes — exactly as
// `NodeAttribute<T>` annotates a `Tree`. The value structs carry no `Hir`
// dependency; only this catalog binds them to the arena.

namespace dss {

// Source provenance: which buffer + byte range each node lowered from. Populated
// by CST→HIR lowering (HR8); read by the verifier to locate diagnostics and by
// MIR/LIR/debug-info (plan 15) to trace generated code back to source.
using HirSourceMap = HirAttribute<HirSourceLoc>;

// FFI linkage/library metadata for extern nodes (HR4 shipped the value struct).
// Populated by FFI ingestion (plan 11).
using HirFfiMap = HirAttribute<FfiMetadata>;

// Native-declaration linkage (binding/visibility) for decls that carried a
// source linkage specifier (C `static` / `__attribute__`). Populated by CST→HIR
// lowering from the language's `linkageSpecifiers` facet; read by HIR→MIR
// lowering to stamp `MirFunc`/`MirGlobal` binding+visibility — the input the
// optimizer's DCE-protect predicate `isExternallyVisible()` consults.
using HirLinkageMap = HirAttribute<LinkageAttr>;

// Native-declaration mutability (const vs writable) for globals that carried a
// source CONST qualifier. Populated by CST→HIR lowering from the bound symbol's
// `SymbolRecord.isConst`; read by HIR→MIR lowering to stamp `MirGlobal.isConst`
// — the input the assembler's section selection consults to route an
// initialized global to read-only `.rodata` (const) vs writable `.data`.
using HirMutabilityMap = HirAttribute<MutabilityAttr>;

// C11/C23 6.7.5 (D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN): explicit `alignas(N)` /
// `alignas(T)` alignment for a Global or VarDecl that carried the specifier.
// Populated by CST→HIR lowering from the bound symbol's
// `SymbolRecord.explicitAlignment` (already validated by the semantic phase);
// read at HIR→MIR lowering to raise a global's data-item section alignment and
// a local's effective alloca alignment. Keyed on the DECLARATION node (like
// mutability, unlike the access-keyed volatile map).
using HirAlignmentMap = HirAttribute<AlignmentAttr>;

// c21 (D-CSUBSET-VOLATILE-QUALIFIER): per-ACCESS volatility for object Refs,
// struct/union MemberAccesses, and VarDecl/Global init stores whose object
// carried a source `volatile` qualifier. Populated by CST→HIR lowering from the
// bound symbol's / field's `SymbolRecord.isVolatile`; read by HIR→MIR lowering
// to OR `MirInstFlags::Volatile` onto that access's Load/Store so the optimizer
// cannot elide / cache / reorder it. Keyed on the ACCESS node (unlike the
// declaration-keyed mutability map) and flows to the Load/Store FLAG (unlike
// mutability's section selection) — genuinely new plumbing.
using HirVolatileMap = HirAttribute<VolatileAttr>;

// Shader stage / built-in / workgroup / binding data. Populated by shader-shape
// lowering (plan 17); read by SPIR-V codegen.
using HirShaderMap = HirAttribute<ShaderIntrinsic>;

// Per-node target-language preference hints. Populated by the source language's
// lowering; read by HIR→HIR transpilation (plan 10).
using HirTranspileMap = HirAttribute<TranspileHint>;

// Per-node recovery info for `HasError` nodes. Populated by lowering on broken
// paths (HR8); read by the verifier and the `.dsshir` text format (HR7).
using HirDiagnosticMap = HirAttribute<DiagnosticInfo>;

} // namespace dss
