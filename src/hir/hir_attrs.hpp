#pragma once

#include "hir/hir.hpp"   // HirAttribute<T>

#include "hir/attributes/diagnostic_info.hpp"
#include "hir/attributes/ffi_metadata.hpp"
#include "hir/attributes/shader_intrinsic.hpp"
#include "hir/attributes/source_span.hpp"
#include "hir/attributes/transpile_hints.hpp"

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
