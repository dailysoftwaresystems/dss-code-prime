#pragma once

// TLS C1 (D-CSUBSET-THREAD-LOCAL): declaration thread-storage side-table
// value. Attached per-node via `HirAttribute<ThreadLocalAttr>` to a NATIVE
// declaration HIR node (Global / ExternGlobal) whose bound symbol carried a
// source-level thread-storage specifier — C11/C23 `_Thread_local int g;` →
// `isThreadLocal = true`. Populated by CST→HIR lowering from the bound
// symbol's `SymbolRecord.isThreadLocal` (set by the semantic phase from the
// language's `linkageSpecifiers` `{threadStorage:true}` entries); read at
// HIR→MIR lowering and stamped onto the `MirGlobal.isThreadLocal` bit (and
// `ExternImport.isThreadLocal` for extern data), which the assembler's
// section selection routes to the thread-template sections (`.tdata` /
// `.tbss`) and the format walkers lay out as the per-thread image (slices
// B/C of the arc).
//
// The exact `MutabilityAttr` discipline (that struct's comment is the
// authority): its own side-table because thread storage is NOT linkage — a
// file-scope `thread_local int g;` keeps external linkage — and NOT
// mutability (`const thread_local` is both read-only AND per-thread; the
// address varies per thread, so the section choice must consult THIS axis
// before isConst).
//
// A declaration node with NO attribute defaults to `isThreadLocal = false` —
// ordinary process-shared storage, the conservative default (absence can
// only lose per-thread-ness for a symbol the semantic phase never marked,
// and the Pass-2 validator makes an unmarked thread_local impossible). The
// side-table stays sparse: only thread-local decls are annotated.

namespace dss {

struct ThreadLocalAttr {
    bool isThreadLocal = false;
};

} // namespace dss
