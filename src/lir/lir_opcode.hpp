#pragma once

// Cycle-1 had a `TargetId` enum + per-target dispatch (`lirOpcodeInfo`
// / `lirIsTerminator`) baked into C++. Cycle-2 pivots to JSON-driven
// targets — the opcode universe lives in `TargetSchema` (loaded from
// `src/dss-config/targets/*.target.json`) and the dispatch is a
// `targetSchema.opcodeInfo(opcode)` lookup. This header survives only
// as the documentation pointer; consumers include
// `core/types/target_schema.hpp` for the descriptor types
// (`TargetOpcodeInfo` / `TargetResultRule` / `TargetSchema`).

#include "core/types/target_schema.hpp"
