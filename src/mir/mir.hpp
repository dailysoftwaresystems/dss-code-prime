#pragma once

#include "core/export.hpp"
#include "core/substrate/arena_attribute.hpp"
#include "core/substrate/arena_container.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_id.hpp"
#include "mir/mir_literal_pool.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

// `Mir` (ML1) — the frozen, immutable mid-level IR module: four dense arenas
// (instructions, basic blocks, functions, and module-level globals) all tagged
// by one `MirModuleId`, plus module-owned pools for the genuinely non-contiguous
// lists (operands, phi incomings, CFG successors) and the literal pool.
// Dogfoods the SP1 substrate exactly as `Hir` does. Built by `MirBuilder`,
// frozen by `finish()`.
//
// `Mir` satisfies the substrate `Arena` concept via its INSTRUCTION tier (the
// fused-value entity, and the most-annotated one), so `MirAttribute<T>` binds to
// instructions exactly as `HirAttribute<T>` binds to `Hir`. Block- and
// function-level side-tables bind through the sibling arenas via
// `MirBlockAttribute<T>` / `MirFuncAttribute<T>`.
//
// MIR is immutable once frozen; transforming passes (the optimizer) read an old
// module and BUILD a new one via `MirBuilder` rather than mutate in place — the
// same build-once-freeze discipline as `Tree`/`Hir`. This is why a block's
// instruction membership is a dense contiguous arena range: there is no in-place
// mid-block insertion to invalidate it.

namespace dss {

// Module-level alias-analysis polarity. Stamped by the HIR→MIR lowering
// from the source language's `SemanticConfig.PointerAliasingRules`.
// `Permissive` (the default) keeps the MIR-tier `mirMayAlias` predicate
// at the conservative `Maybe` for distinct primitive pointees;
// `StrictTBAA` opts into C-style strict-aliasing (Rule 6 returns `No`
// for `Ptr<I32>` vs `Ptr<I64>`, etc.).
//
// Pairs with `Mir.charTypesAliasAll()`: the two flags compose
// orthogonally (Rule 5 char-exception fires only when
// `charTypesAliasAll == true`). Closes
// `D-OPT-LOAD-ALIAS-ANALYSIS-STRICT-TBAA-WIRING` end-to-end.
enum class MirAliasingMode : std::uint8_t { Permissive, StrictTBAA };

class DSS_EXPORT Mir {
public:
    using InstArena   = substrate::ArenaContainer<detail::MirInst,   MirInstId,   MirModuleId>;
    using BlockArena  = substrate::ArenaContainer<detail::MirBlock,  MirBlockId,  MirModuleId>;
    using FuncArena   = substrate::ArenaContainer<detail::MirFunc,   MirFuncId,   MirModuleId>;
    using GlobalArena = substrate::ArenaContainer<detail::MirGlobal, MirGlobalId, MirModuleId>;

    // Substrate `Arena` concept surface: the instruction tier (so
    // `MirAttribute<T>` = `ArenaAttribute<Mir, T>` keys on `MirInstId`).
    using IdType  = MirInstId;
    using TagType = MirModuleId;

    // The empty module — the transient state before a builder hands over.
    Mir() noexcept = default;

    // The 10th + 11th args (`aliasingMode`, `charTypesAliasAll`) are
    // REQUIRED — no defaults — so a direct call site can't silently
    // produce a sound-but-wrong-shape module by omitting them.
    // `MirBuilder::finish` is the only intended producer and threads
    // both through explicitly.
    Mir(InstArena instArena, BlockArena blockArena, FuncArena funcArena,
        GlobalArena globalArena,
        std::vector<MirBlockId> instBlock, std::vector<MirInstId> operandPool,
        std::vector<MirPhiIncoming> phiPool, std::vector<MirBlockId> succPool,
        MirLiteralPool literalPool,
        MirAliasingMode aliasingMode,
        bool charTypesAliasAll) noexcept;

    // Move-only (the arenas are move-only — their cross-arena tags must stay
    // unique). Custom moves reset the source to a default-constructed observable
    // state (empty arenas, empty pools), the same fail-loud-on-use-after-move
    // discipline as `Hir` / `substrate::ArenaAttribute`.
    Mir(Mir const&)            = delete;
    Mir& operator=(Mir const&) = delete;
    Mir(Mir&&) noexcept;
    Mir& operator=(Mir&&) noexcept;

    // ── identity / introspection ──
    [[nodiscard]] MirModuleId id()         const noexcept { return instArena_.id(); }
    // `nodeCount` is the substrate `Arena`-concept surface (instruction tier);
    // prefer the domain name `instCount()` at call sites. These INCLUDE the
    // slot-0 sentinel — `moduleFuncCount()` is the sentinel-excluded count for
    // iteration. (Block/func tiers have no sentinel-excluded counter yet; ML2/ML3
    // iterate functions via `funcAt`, and blocks/insts via the func/block ranges.)
    [[nodiscard]] std::size_t nodeCount()  const noexcept { return instArena_.nodeCount(); }
    [[nodiscard]] bool        empty()      const noexcept { return instArena_.empty(); }
    [[nodiscard]] std::size_t instCount()  const noexcept { return nodeCount(); }
    [[nodiscard]] std::size_t blockCount() const noexcept { return blockArena_.nodeCount(); }
    [[nodiscard]] std::size_t funcCount()   const noexcept { return funcArena_.nodeCount(); }
    [[nodiscard]] std::size_t globalCount() const noexcept { return globalArena_.nodeCount(); }

    // Sibling arenas, for binding `MirBlockAttribute<T>` / `MirFuncAttribute<T>`
    // / `MirGlobalAttribute<T>`.
    [[nodiscard]] BlockArena  const& blockArena()  const noexcept { return blockArena_; }
    [[nodiscard]] FuncArena   const& funcArena()   const noexcept { return funcArena_; }
    [[nodiscard]] GlobalArena const& globalArena() const noexcept { return globalArena_; }

    // ── instruction accessors (bounds- + cross-module-checked via arena .at) ──
    [[nodiscard]] MirOpcode     instOpcode(MirInstId id)  const { return instArena_.at(id).opcode; }
    [[nodiscard]] MirInstFlags  instFlags(MirInstId id)   const { return instArena_.at(id).flags; }
    [[nodiscard]] TypeId        instType(MirInstId id)    const { return instArena_.at(id).typeId; }
    [[nodiscard]] std::uint32_t instPayload(MirInstId id) const { return instArena_.at(id).payload; }
    // The block that contains this instruction (O(1) reverse lookup, the MIR
    // analog of HIR's parent array — ML3's dominator/liveness passes start from
    // "which block defines value X").
    [[nodiscard]] MirBlockId instBlock(MirInstId id) const;
    // Value operands, in order. Aborts if the opcode is `Phi` (use `phiIncomings`)
    // or if the POD's operand range addresses past the operand pool.
    [[nodiscard]] std::span<MirInstId const>      instOperands(MirInstId id) const;
    // Phi incomings (value, predecessor-block) pairs. Aborts unless the opcode is
    // `Phi`, or if the range addresses past the phi pool.
    [[nodiscard]] std::span<MirPhiIncoming const> phiIncomings(MirInstId id) const;

    // Typed `payload` readers — each hides the per-opcode encoding (so consumers
    // never decode `payload` by hand) and aborts on a wrong-opcode id. Mirrors
    // HIR's typed payload accessors (varDeclSymbol, branchDepth, …).
    [[nodiscard]] std::uint32_t argIndex(MirInstId id) const;          // Arg
    [[nodiscard]] std::uint32_t constLiteralIndex(MirInstId id) const; // Const
    [[nodiscard]] SymbolId      globalAddrSymbol(MirInstId id) const;  // GlobalAddr
    [[nodiscard]] std::uint32_t intrinsicId(MirInstId id) const;       // IntrinsicCall
    [[nodiscard]] std::uint32_t returnPieceOrdinal(MirInstId id) const;// ReturnPiece

    // ── block accessors ──
    [[nodiscard]] StructCfMarker blockMarker(MirBlockId id) const { return blockArena_.at(id).marker; }
    // Narrow METADATA-ONLY mutation — the single exception to "MIR is
    // immutable once frozen". A `StructCfMarker` is verification metadata
    // DERIVED from the CFG (mir_struct_markers.hpp), not structure: every
    // producer (HIR→MIR lowering, CFG-mutating optimizer passes, the
    // cross-CU merge) re-stamps markers from the canonical derivation
    // AFTER `MirBuilder::finish()`, which requires a frozen-module setter.
    // Structural immutability (instruction/block/successor ranges, pools)
    // is untouched — only the marker byte may change through this.
    void setBlockMarker(MirBlockId id, StructCfMarker m) {
        blockArena_.mutableAt(id).marker = m;
    }
    [[nodiscard]] std::uint32_t  blockInstCount(MirBlockId id) const {
        return blockArena_.at(id).instCount;
    }
    // The owning function.
    [[nodiscard]] MirFuncId blockFunc(MirBlockId id) const;
    // The i-th instruction of the block (0-based), as a tagged MirInstId. Aborts
    // if `i` is past the block's instruction count.
    [[nodiscard]] MirInstId blockInstAt(MirBlockId id, std::uint32_t i) const;
    // The block's terminator — its last instruction. Aborts on an empty block (a
    // structural violation the ML3 verifier flags first; this is defense in depth).
    [[nodiscard]] MirInstId blockTerminator(MirBlockId id) const;
    // CFG successor blocks (from the succ pool). Convention: Br → [target];
    // CondBr → [ifTrue, ifFalse]; Switch → [case targets…, default]; Return /
    // Unreachable → empty. For a Switch, the case targets pair POSITIONALLY with
    // the terminator's operands: `instOperands(term)[0]` is the discriminant and
    // `instOperands(term)[1+i]` is the case constant whose target is
    // `blockSuccessors(block)[i]`; the final successor is the default.
    [[nodiscard]] std::span<MirBlockId const> blockSuccessors(MirBlockId id) const;

    // ── function accessors ──
    [[nodiscard]] TypeId        funcSignature(MirFuncId id) const { return funcArena_.at(id).signature; }
    [[nodiscard]] SymbolId      funcSymbol(MirFuncId id)    const {
        return SymbolId{funcArena_.at(id).symbol};
    }
    [[nodiscard]] std::uint32_t funcBlockCount(MirFuncId id) const {
        return funcArena_.at(id).blockCount;
    }
    // The i-th block of the function (0-based), as a tagged MirBlockId. Aborts if
    // `i` is past the function's block count.
    [[nodiscard]] MirBlockId funcBlockAt(MirFuncId id, std::uint32_t i) const;
    // The entry block (the function's first block). Aborts on a blockless function.
    [[nodiscard]] MirBlockId funcEntry(MirFuncId id) const;

    // D-OPT1-SYMBOL-BINDING-VISIBILITY-THREAD (step 13.6 OPT1 gate):
    // linkage attributes consumed by the optimizer's DCE pass and the
    // downstream link-tier emitter. `isExternallyVisible(binding,
    // visibility)` is the DCE-protect predicate.
    [[nodiscard]] SymbolBinding funcBinding(MirFuncId id) const {
        return funcArena_.at(id).binding;
    }
    [[nodiscard]] SymbolVisibility funcVisibility(MirFuncId id) const {
        return funcArena_.at(id).visibility;
    }

    // ── global accessors ──
    [[nodiscard]] TypeId   globalType(MirGlobalId id) const {
        return globalArena_.at(id).type;
    }
    [[nodiscard]] SymbolId globalSymbol(MirGlobalId id) const {
        return SymbolId{globalArena_.at(id).symbol};
    }
    // The constant initializer's `MirLiteralPool` index (`UINT32_MAX` when the
    // global has none — either zero-init or initialized by an init function).
    [[nodiscard]] std::uint32_t globalInitLiteralIndex(MirGlobalId id) const {
        return globalArena_.at(id).initLiteralIndex;
    }
    // The module-init function id that initializes this global at module load,
    // or `InvalidMirFunc` for constant-init or zero-init globals.
    [[nodiscard]] MirFuncId globalInitFunc(MirGlobalId id) const {
        return globalArena_.at(id).initFunc;
    }
    // D-OPT1-SYMBOL-BINDING-VISIBILITY-THREAD: same DCE-protect
    // discipline as `funcBinding` / `funcVisibility`. A global
    // with `isExternallyVisible(binding, visibility) == true` MUST
    // survive DCE / unused-symbol elimination.
    [[nodiscard]] SymbolBinding globalBinding(MirGlobalId id) const {
        return globalArena_.at(id).binding;
    }
    [[nodiscard]] SymbolVisibility globalVisibility(MirGlobalId id) const {
        return globalArena_.at(id).visibility;
    }
    // D-LK4-DATA-PRODUCER-MUTABLE-GLOBAL: true iff the source declared this
    // global `const`. The assembler's section selection routes an INITIALIZED
    // const global to read-only `.rodata` and a mutable one to writable
    // `.data`. Default `false` (mutable) — the conservative writable default.
    [[nodiscard]] bool globalIsConst(MirGlobalId id) const {
        return globalArena_.at(id).isConst;
    }

    // ── module-level iteration ──
    // The number of real functions and the i-th one (0-based; maps to arena slot
    // i+1, the slot-0 sentinel excluded).
    [[nodiscard]] std::size_t moduleFuncCount() const noexcept;
    [[nodiscard]] MirFuncId   funcAt(std::uint32_t i) const;
    // The number of real globals and the i-th one (0-based, slot-0 sentinel
    // excluded — same discipline as `moduleFuncCount` / `funcAt`).
    [[nodiscard]] std::size_t moduleGlobalCount() const noexcept;
    [[nodiscard]] MirGlobalId globalAt(std::uint32_t i) const;

    // ── literals ──
    [[nodiscard]] MirLiteralValue const& literalValue(std::uint32_t index) const {
        return literalPool_.at(index);
    }
    [[nodiscard]] MirLiteralPool const& literalPool() const noexcept { return literalPool_; }

    // ── module-level alias-analysis polarity ──
    // Read by CSE/LICM Load admission to thread the source language's
    // strict-aliasing opt-in into `mirMayAlias`. The rebuild substrate
    // (`mir_rebuild_helper`) propagates this mode to every pass's fresh
    // `MirBuilder` so a release pipeline doesn't silently downgrade
    // strict-TBAA to Permissive after the first rebuild.
    [[nodiscard]] MirAliasingMode aliasingMode() const noexcept { return aliasingMode_; }
    // Per-source-language C99 §6.5 ¶7 character-type exception flag.
    // Composes with `aliasingMode`: Rule 5 in `mirMayAlias` fires only
    // when this is `true`. Default `true` matches C/C++/Objective-C;
    // Rust / strict-typed DSLs declare `false`.
    [[nodiscard]] bool charTypesAliasAll() const noexcept { return charTypesAliasAll_; }

private:
    InstArena                   instArena_;
    BlockArena                  blockArena_;
    FuncArena                   funcArena_;
    GlobalArena                 globalArena_;
    // Parallel to the instruction arena (indexed by inst .v; slot 0 is the
    // sentinel): the block each instruction belongs to. The MIR analog of HIR's
    // `parentOf_` — kept out of the POD to hold the 24-byte scan density.
    std::vector<MirBlockId>     instBlock_;
    std::vector<MirInstId>      operandPool_;  // value operands (non-Phi insts)
    std::vector<MirPhiIncoming> phiPool_;      // Phi incoming (value, pred) pairs; inter-phi
                                               // ordering is unspecified (only each phi's own
                                               // contiguous slice is meaningful)
    std::vector<MirBlockId>     succPool_;      // terminator CFG successors
    MirLiteralPool              literalPool_;
    MirAliasingMode             aliasingMode_ = MirAliasingMode::Permissive;
    bool                        charTypesAliasAll_ = true;
};

// `MirAttribute<T>` — the HIR-style side-table over the instruction tier (the
// fused-value entity). The static_assert pins that `Mir` models the substrate
// `Arena` concept. `MirBlockAttribute<T>` / `MirFuncAttribute<T>` annotate the
// sibling tiers (e.g. ML3 dominator info per block) by binding to the exposed
// sub-arenas — no `Mir` reshape needed.
static_assert(substrate::Arena<Mir>,
              "Mir must satisfy substrate::Arena so MirAttribute<T> can bind to it");

template <class T>
using MirAttribute = substrate::ArenaAttribute<Mir, T>;
template <class T>
using MirBlockAttribute = substrate::ArenaAttribute<Mir::BlockArena, T>;
template <class T>
using MirGlobalAttribute = substrate::ArenaAttribute<Mir::GlobalArena, T>;
template <class T>
using MirFuncAttribute = substrate::ArenaAttribute<Mir::FuncArena, T>;

// ── MirBuilder ───────────────────────────────────────────────────────────────
//
// Single-use, move-only. Block CREATION is separated from block FILLING so a
// terminator can reference a forward block (an `if`'s condbr targets then/else
// blocks that are filled later):
//   addFunction → createBlock×N → beginBlock(b)→inst*/terminator → beginBlock(c)…
// A block's instructions are appended contiguously (its `instStart` is fixed when
// `beginBlock` opens it), so only one block fills at a time and it must be
// terminated before the next opens. Every created block must be filled +
// terminated before its function closes. Phi incomings may reference values/blocks
// emitted later (loop back-edges), so they are collected per-phi and flushed into
// the phi pool at `finish`.
class DSS_EXPORT MirBuilder {
public:
    explicit MirBuilder();
    explicit MirBuilder(MirModuleId tag);

    // Move-only. Default moves suffice: a builder is single-use by contract
    // (`finish()` consumes it), so a moved-from builder is only ever destroyed.
    MirBuilder(MirBuilder const&)            = delete;
    MirBuilder& operator=(MirBuilder const&) = delete;
    MirBuilder(MirBuilder&&) noexcept        = default;
    MirBuilder& operator=(MirBuilder&&) noexcept = default;

    [[nodiscard]] MirModuleId id() const noexcept { return moduleId_; }

    // ── module-level alias-analysis polarity ──
    // Stamp the module-level alias-analysis polarity. Intended to be
    // called at HIR→MIR lowering time from the source language's
    // `SemanticConfig.PointerAliasingRules`. Default is `Permissive`
    // + `charTypesAliasAll=true` (sound out of the box; every CSE/LICM
    // Load admission stays conservative). Callable at any time before
    // `finish()` (last value wins); the optimizer rebuild substrate
    // (`mir_rebuild_helper`) also calls these on its own builders to
    // propagate the mode through every pipeline pass.
    void setAliasingMode(MirAliasingMode mode) noexcept { aliasingMode_ = mode; }
    [[nodiscard]] MirAliasingMode aliasingMode() const noexcept { return aliasingMode_; }
    void setCharTypesAliasAll(bool v) noexcept { charTypesAliasAll_ = v; }
    [[nodiscard]] bool charTypesAliasAll() const noexcept { return charTypesAliasAll_; }

    // ── function / block lifecycle ──
    // Open a function. Closes any open function first (which requires its current
    // block be terminated and the function have ≥1 block). `signature` is the
    // FnSig TypeId; `symbol` the declared SymbolId.
    MirFuncId addFunction(TypeId signature, SymbolId symbol,
                          SymbolBinding    binding    = SymbolBinding::Global,
                          SymbolVisibility visibility = SymbolVisibility::Default);

    // ── literal pool ──
    // Append `value` to the module's literal pool and return the index.
    // Used by `addGlobal` to record a constant initializer without emitting
    // a `Const` instruction (since globals don't live in any function body).
    [[nodiscard]] std::uint32_t literalPoolAdd(MirLiteralValue value);

    // ── global storage ──
    // Append a module-level global to the globals arena. Module-level only — has
    // no lifecycle dependency on the open function / block; can be called at any
    // point before `finish()`. Initialization shape (mutually exclusive):
    //   - `initLiteralIndex != UINT32_MAX` and `initFunc == InvalidMirFunc`:
    //     constant initializer; the literal at that index is the initial state.
    //   - `initLiteralIndex == UINT32_MAX` and `initFunc.valid()`: a synthesized
    //     module-init function writes the initial state at module load.
    //   - both unset: zero-init (C-style file-scope default).
    // Aborts on a no-symbol or no-type call; aborts on the both-set combination.
    MirGlobalId addGlobal(TypeId type, SymbolId symbol,
                          std::uint32_t initLiteralIndex = UINT32_MAX,
                          MirFuncId     initFunc         = {},
                          SymbolBinding    binding       = SymbolBinding::Global,
                          SymbolVisibility visibility    = SymbolVisibility::Default,
                          bool             isConst       = false);

    // Reserve a basic block in the current function WITHOUT opening it, returning
    // its id so terminators can target it before it is filled (forward branches).
    // The first block created is the function's entry block. The block must later
    // be opened with `beginBlock` and given a terminator before the function closes.
    MirBlockId createBlock(StructCfMarker marker = StructCfMarker::Linear);

    // Open a previously-created block for instruction emission (fixing its
    // instruction range to start here). Requires the current open block, if any,
    // be terminated. A block may be opened exactly once. Subsequent addInst/
    // addPhi/terminator calls append to this block.
    void beginBlock(MirBlockId block);

    // Set/override a builder-produced block's structured-CF marker. ML2 lowering
    // stamps these; defaults to `Linear`.
    void setBlockMarker(MirBlockId block, StructCfMarker marker);

    // ── value-producing instructions (appended to the current block) ──
    // Generic computation. The opcode must be a non-terminator, non-Phi opcode;
    // operands and `resultType` are checked against `opcodeInfo(opcode)`
    // (operand count bounds + result-type rule). Returns the instruction id,
    // which IS its SSA value id.
    MirInstId addInst(MirOpcode opcode, std::span<MirInstId const> operands,
                      TypeId resultType = InvalidType, std::uint32_t payload = 0,
                      MirInstFlags flags = MirInstFlags::None);

    // Value-origin conveniences (fused model). Each is a leaf value-producer.
    MirInstId addArg(std::uint32_t paramIndex, TypeId type, MirInstFlags flags = MirInstFlags::None);
    MirInstId addConst(MirLiteralValue value, TypeId type, MirInstFlags flags = MirInstFlags::None);
    MirInstId addGlobalAddr(SymbolId symbol, TypeId type, MirInstFlags flags = MirInstFlags::None);
    // FC7 C1c (D-FC7-SYSV-STRUCT-RETURN-IN-REGS): the k-th register piece of a
    // struct-returning `call` (≥1 — piece 0 is the call's own result). `ordinal`
    // is the PER-CLASS return-register index; `pieceType` is the piece's register
    // type (I64/F64). The `call` operand anchors ordering + value-numbering.
    MirInstId addReturnPiece(MirInstId call, std::uint32_t ordinal, TypeId pieceType,
                             MirInstFlags flags = MirInstFlags::None);
    // FC7 C3 (AAPCS64/Apple x8 sret). Callee-side entry read of the indirect-
    // result register (the incoming result-storage pointer); `pointerType` is the
    // pointer-to-result type. (The caller side needs NO builder: the sret pointer
    // is a normal prepended Call operand routed to the IRR by the
    // `call_payload::kIndirectResultBit` flag — the IRR-reroute design.)
    MirInstId addReadIndirectResult(TypeId pointerType,
                                    MirInstFlags flags = MirInstFlags::None);

    // D5.6: aggregate field/element read + write (first-class, no memory).
    // `path` is the field-index chain (length 1 for direct read of a top-
    // level field; >1 for nested). `indexType` is the interner's i32
    // TypeId (MirBuilder doesn't carry a TypeInterner — the caller, who
    // does, provides it). Result type is the element's type for
    // ExtractValue, the aggregate's type for InsertValue.
    MirInstId addExtractValue(MirInstId aggregate, std::span<std::uint32_t const> path,
                              TypeId resultType, TypeId indexType,
                              MirInstFlags flags = MirInstFlags::None);
    MirInstId addInsertValue(MirInstId aggregate, MirInstId value,
                             std::span<std::uint32_t const> path,
                             TypeId resultType, TypeId indexType,
                             MirInstFlags flags = MirInstFlags::None);

    // Phi at the current block. By convention phis sit at the block head (a
    // verifier-checked rule in ML3, not enforced here). Incomings may be supplied
    // now and/or appended later via `addPhiIncoming` (loop back-edges); all are
    // flushed to the phi pool at `finish`.
    MirInstId addPhi(TypeId resultType, std::span<MirPhiIncoming const> incomings = {},
                     MirInstFlags flags = MirInstFlags::None);
    // Append one incoming edge to a previously-created Phi. Aborts if `phi` is
    // not a Phi this builder produced.
    void addPhiIncoming(MirInstId phi, MirPhiIncoming incoming);

    // ── terminators (each seals the current block) ──
    MirInstId addBr(MirBlockId target);
    MirInstId addCondBr(MirInstId cond, MirBlockId ifTrue, MirBlockId ifFalse);
    // Switch over `discriminant`; each case is a (constant value, target block)
    // pair; `defaultTarget` is taken when no case matches. Successors are recorded
    // as [case targets…, default].
    MirInstId addSwitch(MirInstId discriminant,
                        std::span<std::pair<MirInstId, MirBlockId> const> cases,
                        MirBlockId defaultTarget);
    MirInstId addReturn(std::optional<MirInstId> value = std::nullopt);
    // FC7 C1c: a by-value struct/union returned IN REGISTERS yields N eightbyte
    // pieces (SysV ≤16B → 2). Each `values[i]` is a piece value (I64/F64); the
    // callconv pass moves piece i into its per-class return register.
    MirInstId addReturnMulti(std::span<MirInstId const> values);
    MirInstId addUnreachable();

    // ── introspection (read-only on the in-progress build) ──
    // True iff the currently-open block has been sealed (i.e. one of the
    // terminator builders has run on it). Lowerings that synthesize an
    // implicit terminator (e.g. ML2's implicit-void-return on a `void f() {}`
    // body that didn't write its own return) read this to decide whether to
    // emit one. Returns false when no block is open.
    [[nodiscard]] bool openBlockHasTerminator() const noexcept;
    // True iff `block` is in the `Created` state — reserved by `createBlock`
    // but not yet opened with `beginBlock`. Lowerings recovering from an
    // inner-error path use this to find forward-created blocks they never
    // reached (the MirBuilder's "every created block must be filled +
    // terminated by finish()" invariant otherwise aborts).
    [[nodiscard]] bool isBlockUnopened(MirBlockId block) const noexcept;
    // The currently-open block (or `InvalidMirBlock` when none is open OR the
    // last-opened block has already been sealed by a terminator). Phi-
    // insertion lowerings (Ternary, LogicalAnd/Or) need to know which block
    // an expression's value was computed in so the phi can record the right
    // predecessor; a sub-expression's recursion may have advanced past the
    // initial block before returning. Strict: a post-terminator/pre-next-
    // `beginBlock` read returns invalid rather than the just-sealed id, so a
    // caller that forgets to capture BEFORE its own terminator gets an obvious
    // failure instead of a silently wrong phi predecessor.
    [[nodiscard]] MirBlockId currentlyOpenBlock() const noexcept {
        if (!openBlock_.valid()) return MirBlockId{};
        return blockState_[openBlock_.v] == BlockState::Open
                   ? openBlock_ : MirBlockId{};
    }

    // Freeze. Flushes pending phi incomings, validates the open function/block are
    // closed, and hands over the immutable module. Single-use; consumes *this.
    [[nodiscard]] Mir finish() &&;

    // Monotonic MirModuleId allocator (mirrors HirBuilder::nextModuleId). Ids
    // start at 1; 0 is the InvalidMirModule sentinel. Process-global atomic;
    // aborts on uint32 exhaustion rather than recycling id 0 (which would stamp
    // arenaTag 0 and silently defeat the cross-module guard).
    [[nodiscard]] static MirModuleId nextModuleId() noexcept;

private:
    // Append `pod` (+ its value operands) to the current open block. Increments
    // the block's instCount. `terminates` seals the block. Returns the new id.
    // D5.6: shared helper for the ExtractValue / InsertValue builders.
    // Interns each `path` entry as a Const-i32 MirInst and pushes the
    // resulting MirInstId onto `operands` in source order. Caller
    // supplies `indexType` because MirBuilder doesn't carry a
    // TypeInterner.
    void appendIndexPathOperands_(std::vector<MirInstId>& operands,
                                  std::span<std::uint32_t const> path,
                                  TypeId indexType);

    MirInstId appendInst_(detail::MirInst pod, std::span<MirInstId const> operands,
                          bool terminates);
    // Record CFG successors for the just-sealed `terminator` into the succ pool
    // and the current block's succ range (validating the count against the
    // terminator's opcodeInfo() successor arity).
    void recordSuccessors_(MirOpcode terminator, std::span<MirBlockId const> succs);
    // Validate an operand/successor id belongs to this module (untagged literals
    // pass — test ergonomics), aborting loud on a foreign-module id.
    void checkSameModule_(std::uint32_t arenaTag, char const* what) const;
    void closeBlock_();     // requires the open block (if any) be terminated
    void closeFunction_();  // closes the block, requires every block filled + ≥1 block

    // Build-time lifecycle of a created block.
    enum class BlockState : std::uint8_t { Created, Open, Sealed };

    MirModuleId moduleId_;
    substrate::ArenaBuilder<detail::MirInst,   MirInstId,   MirModuleId> instArena_;
    substrate::ArenaBuilder<detail::MirBlock,  MirBlockId,  MirModuleId> blockArena_;
    substrate::ArenaBuilder<detail::MirFunc,   MirFuncId,   MirModuleId> funcArena_;
    substrate::ArenaBuilder<detail::MirGlobal, MirGlobalId, MirModuleId> globalArena_;
    std::vector<MirBlockId>     instBlock_;    // grows in lockstep with the inst arena
    std::vector<MirInstId>      operandPool_;
    std::vector<MirPhiIncoming> phiPool_;
    std::vector<MirBlockId>     succPool_;
    MirLiteralPool              literalPool_;
    MirAliasingMode             aliasingMode_ = MirAliasingMode::Permissive;
    bool                        charTypesAliasAll_ = true;

    // Per-phi pending incomings, keyed by the phi instruction's slot (.v),
    // flushed into `phiPool_` (contiguously per phi) at `finish`.
    std::unordered_map<std::uint32_t, std::vector<MirPhiIncoming>> pendingPhi_;
    // Per-block build state, indexed by block .v (slot 0 is the sentinel). Lets
    // `closeFunction_` verify every created block was filled + terminated.
    std::vector<BlockState> blockState_;

    MirFuncId  openFunc_;   // invalid ⇒ no open function
    MirBlockId openBlock_;  // invalid ⇒ no open block
};

} // namespace dss
