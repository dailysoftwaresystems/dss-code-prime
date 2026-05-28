#pragma once

#include <cstdint>

// Shader-intrinsic side-table value (HR5). Attached per-node via
// `HirAttribute<ShaderIntrinsic>` (aliased `HirShaderMap` in hir_attrs.hpp) to
// nodes that carry shader-stage semantics — entry-point functions, interface
// variables, and bound resources. It records exactly the data SPIR-V lowering
// (`17-shader-gpu-plan`) turns into decorations/execution modes; it does NOT
// describe an operation (a `WorkgroupBarrier` / `TextureSample` is a registered
// EXTENSION HirKind, not a flag here).
//
// Population lands with shader-shape lowering (plan 17); HR5 establishes the
// home + the field shapes the backend needs. No `Hir` dependency — consumers
// bind it as `HirAttribute<ShaderIntrinsic>`. The `ShaderUsable` HirFlag
// (hir_node.hpp) gates the HR6 shader-restriction verifier; this side-table
// carries the richer per-node detail that doesn't fit a flag bit.

namespace dss {

// Pipeline stage an entry-point function runs at. Maps to SPIR-V's
// `ExecutionModel` (the concrete model word is plan 17's mapping job, not HR5's).
// `None` = the node is shader-usable but is not itself a stage entry point.
enum class ShaderStage : std::uint8_t {
    None = 0,
    Vertex,
    Fragment,
    Compute,
    Geometry,
    TessControl,
    TessEval,
};

// Built-in interface variable a node maps to (SPIR-V `OpDecorate BuiltIn`).
// `None` = an ordinary user-declared variable (decorated by `location` instead).
enum class ShaderBuiltin : std::uint8_t {
    None = 0,
    Position,            // gl_Position           (vertex out)
    PointSize,
    VertexIndex,         // gl_VertexIndex        (vertex in)
    InstanceIndex,
    FragCoord,           // gl_FragCoord          (fragment in)
    FragDepth,           // gl_FragDepth          (fragment out)
    FrontFacing,
    GlobalInvocationId,  // gl_GlobalInvocationID (compute)
    LocalInvocationId,
    WorkgroupId,
    NumWorkgroups,
};

// Compute-stage local workgroup dimensions (SPIR-V `OpExecutionMode LocalSize`).
// Meaningful only when `stage == Compute`; the {1,1,1} default is the scalar
// fallback.
struct ShaderWorkgroupSize {
    std::uint32_t x = 1;
    std::uint32_t y = 1;
    std::uint32_t z = 1;
};

// Descriptor binding of a bound resource (SPIR-V `OpDecorate DescriptorSet` +
// `OpDecorate Binding`). Meaningful only for resource-typed nodes (samplers,
// textures, UAVs, uniform/storage buffers).
struct ShaderResourceBinding {
    std::uint32_t set     = 0;
    std::uint32_t binding = 0;
};

// Sentinel for an unset interface `location` (0 is a valid location index).
inline constexpr std::uint32_t kUnsetShaderLocation = 0xFFFFFFFFu;

struct ShaderIntrinsic {
    // Entry-point stage (drives `OpEntryPoint`); `None` for non-entry nodes.
    ShaderStage stage = ShaderStage::None;

    // Built-in mapping for an interface variable; `None` for user variables.
    ShaderBuiltin builtin = ShaderBuiltin::None;

    // `OpExecutionMode LocalSize` operands — read only for a `Compute` entry.
    ShaderWorkgroupSize workgroup;

    // `OpDecorate DescriptorSet`/`Binding` operands — read only for resources.
    ShaderResourceBinding binding;

    // `OpDecorate Location` operand for a user interface variable;
    // `kUnsetShaderLocation` when this node is not a located interface variable.
    std::uint32_t location = kUnsetShaderLocation;
};

} // namespace dss
