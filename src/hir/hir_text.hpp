#pragma once

#include "core/export.hpp"
#include "core/types/declared_qualification.hpp"    // DeclaredQualification (parseTypeFromText's qualifier out-param)
#include "core/types/named_type_binding.hpp"       // NamedTypeBinding (parseTypeFromText aliases)
#include "core/types/strong_ids.hpp"               // CompilationUnitId
#include "core/types/type_lattice/type_interner.hpp" // TypeInterner (by-value in the parse result)
#include "hir/hir.hpp"                              // Hir
#include "hir/hir_attrs.hpp"                        // the five HirAttribute<T> side-table aliases
#include "hir/hir_inline_asm.hpp"                   // HirInlineAsmPool (inline-asm descriptors)
#include "hir/hir_literal_pool.hpp"                 // HirLiteralPool (literal values)

#include <cstdint>
#include <memory>
#include <string>
#include <span>
#include <string_view>
#include <vector>

// HIR text format `.dsshir` (HR7) — a round-trippable, human-readable serialization
// of a frozen `Hir`. The format is the debug + test-fixture surface for the whole
// HIR pipeline: `emitHir` renders a module to text, `parseHir` rebuilds the module
// (re-interning types, re-registering extension kinds/ops/intrinsics, repopulating
// the side-tables) and runs `HirVerifier` on load. The contract is byte-identical
// round-trip: `emitHir(parseHir(emitHir(h)))` reproduces the same bytes (plan §5).
//
// What lives where (and why the API takes a context):
//   - The node tree, every node's `typeId`, `flags`, payload, and the three
//     extension registries are IN the `Hir`, so they always round-trip.
//   - Types are CU-ephemeral `TypeId`s; the text renders them STRUCTURALLY
//     (`i64`, `ptr<i64>`, `fn(i32)->void`), so emit/parse both need a
//     `TypeInterner` to decode / re-intern — exactly as `HirVerifier` takes one.
//     A COMPOSITE (struct / union) is the one exception: it is DEFINED ONCE in
//     the `types` preamble section and every use names it (`type <H>`) — see the
//     v5 note below.
//   - Symbol names are NOT in the `Hir` (a node carries only an opaque `SymbolId`);
//     they are first-class file content sourced from an injected name table on
//     emit, and reconstructed into `HirParseResult::symbolNames` on parse so a
//     re-emit reproduces them. Absent a name table, the emitter writes a stable
//     synthetic handle (`%s7`) and the file is still self-contained + round-trips.
//   - Literal VALUES live in a `HirLiteralPool` OUTSIDE the `Hir` (the node
//     carries only its pool index in `payload`). Like the side-tables, emit
//     takes the pool by pointer and parse hands a rebuilt one back. When a pool
//     is supplied, a `Literal` renders its VALUE inline (`lit int 42 : i32`,
//     `lit str "hi" : arr<char, 3>`) so the value round-trips; absent a pool it
//     falls back to the bare index form (`lit #<index> : <type>`) — still
//     self-contained and re-parseable, just value-less (used by hand-built test
//     modules that have no pool).
//   - The five per-node side-tables (source-loc / ffi / shader / transpile / diag)
//     live OUTSIDE the `Hir`, so emit takes them by pointer and parse hands them
//     back, bound to the rebuilt module.
//   - The FORMAT VERSION and the PRODUCER REVISION head the file (`dsshir 4`,
//     `producer "…"`). Both are MANDATORY on read: a file that opens any other
//     way is refused, never guessed at. The version is a constant of this build
//     (`kHirTextFormatVersion`); the producer is supplied by the caller, because
//     `hir` cannot see who is compiling.
//   - BUFFER NAMES (`buffers { buf 3 "main.c" }`) resolve the `@loc(buf N, …)`
//     handles a source-map emission writes. Like symbol names, they are not in
//     the `Hir`; unlike symbol names, they are not derivable from it at all —
//     `BufferId` is a process-global counter, so a file WITHOUT this section has
//     spans no outside reader can attribute to a file. ⚠ The `N` in both places
//     is an ARTIFACT-LOCAL HANDLE the emitter assigns, never the `BufferId` —
//     see `HirTextBufferName::buffer`.
//
// ★★ THE SELF-CONTAINMENT RULE, STATED ONCE. A reader holding ONLY the bytes
// must be able to reconstruct every type and every name the module references,
// with no side channel. That is why every type TRAVELS (a composite's fields,
// offsets, alignments, bit widths and packing are all in the file, never an
// id into a compiler's memory), why names travel in `symbols`, and why buffers
// travel in `buffers`.
//
// ★★★ v5: EACH COMPOSITE IS DEFINED ONCE, AND EVERY USE REFERENCES IT.
//
//     types {
//       type 1 = struct "Node" {i32, ptr<type 1>}
//     }
//     …  param %3 : ptr<type 1>
//
// Until v4 a composite was spelled STRUCTURALLY AT EVERY USE — its whole field
// list inline, through every pointer — so the artifact's size was (type
// mentions) × (the reachable type graph). ✔MEASURED on sqlite (P68 round 7,
// lane `cr`): 1.18 MB of the amalgamation took 275.9 G instructions and died
// `std::bad_alloc` in `--emit-hir`, where the whole COMPILE of the same file is
// 3.23 G. Every reference IR measured (clang's `%struct.A = type {…}`, gcc's
// `@N` tree-dump handles, MSVC's CodeView type indices) defines a composite once
// and refers to it by handle; this format now does the same, so a module with
// N mentions of a composite whose graph has G nodes spells O(N + G) type nodes
// (`hirTextTypeNodesSpelledTake` counts them).
//
// ⚠⚠ THE HANDLE IS PER-COMPOSITE AND ARTIFACT-GLOBAL, NEVER PER-SPELLING. It is
// minted at the composite's first REFERENCE, in text order, so it is a function
// of the module alone; it is the same device `symbols`/`%N` and `buffers`/`buf N`
// already use for the file's other CU-ephemeral identities, and the parser keys
// its forward mint on it, so handle H ⇒ exactly one TypeId per file — two
// composites that happen to share a name and a field list (two scopes' `struct
// S`) stay two types. A CYCLE is therefore an ordinary case rather than a
// special one: `ptr<type 1>` inside the definition of `type 1` is a reference
// like any other. The v3 `rec <H>` back-reference is SUBSUMED in a module and
// refused there; it survives only in a STANDALONE type text
// (`parseTypeFromText`), which has no table to reference.
// ⓘ `opaque` marks an INCOMPLETE composite (no field list at all) and is an
// ordinary table entry: `type 2 = struct "FILE" opaque`.

namespace dss {

class DiagnosticReporter;
class TypeRegistry;

// ── The format version, and why it is a NUMBER on the artifact's face ─────────
//
// Every `.dsshir` opens with `dsshir <N>` and `parseHir` REFUSES any `N` this
// build does not understand (`H_TextVersionMismatch`, Error, no recovery). The
// refusal is the whole point of the field: this format GROWS while the language
// surface grows, and a reader that cannot tell it changed reads the new bytes
// under the old grammar and derives a confident wrong answer. A refusal costs a
// rebuild; a silent misread costs a true statement about the wrong program.
//
// ⚠ THE VERSION IS BUMPED BY ANY CHANGE A v(N-1) READER WOULD MISREAD, which
// includes ADDING a required construct — not only removing or re-spelling one.
// v2 added the mandatory `producer` header line and the optional `buffers`
// preamble section; a v1 reader meeting a v2 file would take `producer` for the
// start of the module and fail somewhere unhelpful, so the bump is not
// optional.
// v3 added the `rec <H>` composite marker and the `rec <H>` type head (the
// back-reference that lets a CYCLIC composite be spelled at all). A v2 reader
// meeting `struct "Node" rec 1 {i32, ptr<rec 1>}` would refuse the marker as a
// stray token — which is the correct outcome and exactly why the number moves:
// the alternative is a reader that skips what it does not recognise and rebuilds
// a `Node` with no `next`.
// v4 added the `rmw` expression.
// v5 moved every composite into the `types` section (defined once, referenced as
// `type <H>`), gave a composite definition a spelling for EVERY layout channel
// the interner carries (whole-composite `aligned N`, the `pack N` cap, per-field
// `bits N`, and `~N` on a union member, which v4 dropped), and spelled a
// value-less return `return void`: a v4 reader meeting `return` followed by the
// next statement's `@loc` took that statement for the return's VALUE — ✔MEASURED
// on sqlite's `test/speedtest1.c`, whose `if (c) return;` then-arms made
// `--emit-hir` refuse its own artifact (P68 round 8, lane `ht`).
// v6 spelled an enumeration's FIXED underlying type, `enum "E" fixed i64 "long"`
// (P68 round 12, lane `cs`): `enum E : long` and `enum E` are different types
// (C23 6.2.7p1), and v5 wrote both as `enum "E" : i64`. A v5 reader meeting the
// v6 spelling reads `enum "E"` and then meets `fixed` with no rule for it.
inline constexpr std::uint32_t kHirTextFormatVersion = 6;

// ── kHirTextMaxNodeDepth — THE FORMAT'S DECLARED NESTING LIMIT ────────────────
//
// ★★★ THE NESTING LIMIT SURVIVES AS A COUNTER, IT IS NO LONGER THE HOST STACK
// (D-COMPILER-INPUT-PROPORTIONAL-RECURSION-RESIDUE-UNCONVERTED-AND-UNCAPPED, the
// operator's ruling of 2026-09-02). The node walk on both sides of this codec
// used to be host recursion with no cap of any kind, so "how deep may a module
// be" was answered by whichever thread happened to be running the emitter —
// ✔MEASURED 2026-09-08: `dsscp --emit-hir` died of STATUS_STACK_OVERFLOW
// (0xC00000FD) with ZERO bytes on stderr and no diagnostic on a flat
// `x +1 +1 …` chain of 2000 terms, while `--compile` took the same file at rc 0.
// The walk is now an explicit heap work stack, and the limit it lost is
// reinstated HERE, as a number, in the header where the format's other declared
// constants live.
//
// ⚠ IT IS A LIMIT OF THE FORMAT, NOT OF THE WRITER, AND THAT IS WHY IT IS PUBLIC.
// A writer that spelled a module deeper than its own reader accepts would ship an
// artifact that cannot be read back — a round trip that fails at the consumer
// rather than here. Both halves read this one constant, so the two cannot drift.
//
// WHY THIS VALUE, stated rather than assumed:
//   * FROM BELOW — it must exceed the deepest HIR the front end can hand the
//     writer, or `--emit-hir` would refuse programs `--compile` compiles. The c
//     grammar declares `parser.maxExpressionDepth` 16384 (its own semantic cap on
//     nesting), and ✔MEASURED 2026-09-08 a flat 8000-term chain — which does NOT
//     count against that cap, because a left-associative chain climbs
//     iteratively — compiles at rc 0 and yields an ~8000-deep HIR. This bound is
//     16x the declared cap and 32x that measured chain.
//   * FROM ABOVE — the walk's cost is now HEAP, so the bound is what makes that
//     heap finite. At most three live tasks per level at ~56 bytes each puts the
//     emitter's worst case at the bound near 44 MiB, which is bounded and
//     reportable, where an uncapped walk is neither.
// A module past it is REFUSED BY NAME with an Error diagnostic and the `?` poison
// token — never a truncation that would read back as a smaller, valid program.
inline constexpr std::uint32_t kHirTextMaxNodeDepth = 262144;

// ── HirTextBufferName ─────────────────────────────────────────────────────────
//
// One buffer → source-name binding, as it travels in the file's `buffers`
// preamble section. A node's `@loc(buf 3, 120..131)` names buffer 3; without
// this section a reader holding ONLY the text cannot say which file that is,
// because `BufferId` is a process-global monotonic counter with no meaning
// outside the process that minted it. Emitting the pair is what turns a span
// from "node 37" into "main.c, bytes 120..131".
//
// ⚠⚠ `buffer` MEANS DIFFERENT THINGS ON THE TWO SIDES, AND THAT ASYMMETRY IS THE
// WHOLE RENUMBERING. On EMIT the caller fills it with the real `BufferId.v` it
// holds — it has nothing else — and the emitter maps that to an ARTIFACT-LOCAL
// HANDLE (1..N over the buffers this module uses, by sorted id) which is what
// the bytes carry, here and in every `@loc`. On PARSE it comes back as that
// HANDLE, because the handle IS the rebuilt `BufferId` — the same device
// `symbols`/`%N` already uses for symbols. So emit → parse → emit is the
// identity and no process-global counter ever reaches the file; feeding a parsed
// vector straight back into `HirTextContext` is correct, and is what the
// round-trip tests do.
struct DSS_EXPORT HirTextBufferName {
    std::uint32_t buffer = 0;   // emit: the caller's BufferId.v · parse: the artifact handle
    std::string   name;         // the buffer's own `SourceBuffer::name()`

    // ★★ NON-ZERO ⇒ THIS BUFFER IS THE PREPROCESSOR'S SYNTHESIZED TEXT, AND ITS
    // OFFSETS BELONG TO NO FILE ANYONE CAN OPEN. The value is the MAIN ORIGIN
    // buffer the synthesis started from. Zero ⇒ an ordinary source buffer whose
    // offsets index the named file directly.
    //
    // ⚠⚠ WITHOUT THIS FIELD THE SECTION WOULD BE A CONFIDENT LIE, and the reason
    // is the one that makes this defect class survive review: a synthesized
    // buffer is CONSTRUCTED WITH THE MAIN SOURCE'S NAME. So `buf 23 "main.c"`
    // beside `@loc(buf 23, 443..473)` reads as "bytes 443..473 of main.c" — a
    // plausible offset into a real file, shifted by the predefine prologue and
    // by one line per `-D`, and by a whole header's worth for anything spliced
    // in from an `#include`. ✔MEASURED through the CLI on a 140-byte source:
    // the spans landed at offsets 443..621. A reader indexing the file with them
    // would state a property "at" text that is not the text it read.
    //
    // ⓘ A language with no preprocess pass produces no synthesized buffer, so
    // every entry is zero there and the spans index the file directly.
    std::uint32_t synthesizedMainOrigin = 0;
};

// ── HirTextContext ────────────────────────────────────────────────────────────
//
// Non-owning enrichment for the emitter, mirroring `HirVerifier`'s optional
// injections (it must not outlive the objects it points at). Every field is
// optional; a fully-null context still produces a complete, self-contained,
// re-parseable file (synthetic symbol handles, and `?` types with a warning when
// the interner is missing).
struct DSS_EXPORT HirTextContext {
    // Decodes each node's `TypeId` into structural text. The real pipeline always
    // supplies the interner the semantic phase produced. Its `owner()` must match
    // the CU the module's `TypeId`s were interned against (the cross-arena guard
    // enforces this on first decode).
    TypeInterner const* interner = nullptr;

    // SymbolId.v → human name. Index 0 is the invalid-symbol slot; an id past the
    // end (or an empty entry) falls back to the synthetic `%s<v>` handle. A
    // production caller fills this from the CU's symbol table; a unit test may
    // leave it null.
    std::vector<std::string> const* symbolNames = nullptr;

    // Decoded literal values, indexed by a `Literal` node's `payload`. When set,
    // each literal renders its value inline; null ⇒ the bare `#<index>` form.
    // The lowering supplies `&CstToHirResult::literalPool`.
    HirLiteralPool const* literalPool = nullptr;

    // Inline-asm P5: the descriptors an `InlineAsm` node's `payload` HANDLE
    // names. The lowering supplies `&CstToHirResult::inlineAsmPool`.
    // !! NULL IS NOT A HARMLESS DEGRADATION HERE, unlike `literalPool`. A `lit`
    // with no pool still renders as `lit #3` and is recognisably a literal; an
    // `inline_asm` with no pool would render as a BARE BARRIER, which is a
    // different program. The writer therefore emits an explicit `#<handle>`
    // form AND reports a diagnostic rather than degrading quietly.
    HirInlineAsmPool const* inlineAsmPool = nullptr;

    // THE COMPILER REVISION THAT PRODUCED THIS ARTIFACT, written verbatim into
    // the mandatory `producer "<…>"` header line.
    //
    // ★ IT IS AN INPUT, NOT A CONSTANT, AND THAT IS FORCED BOTH WAYS. `hir` sits
    // BELOW `program` in the layering, so it cannot reach the build stamp
    // (`program/dss_build_stamp.hpp`) that names this compiler; and it must not,
    // because the identity of the producer is the DRIVER's fact, not the
    // format's. Making it an input also keeps the checked-in goldens stable —
    // they emit with no producer, so a commit does not rewrite five fixtures.
    //
    // EMPTY renders as `producer ""`, which is a POSITIVE statement — *"this
    // file is not attributed to a compiler revision"* — and never an absent
    // field: absence is malformed on read. Every artifact the shipped driver
    // writes carries a real stamp (`--emit-hir` supplies `dss::runtime::
    // kBuildStamp`); the empty form is for hand-built modules and unit tests.
    std::string_view producer{};

    // BufferId.v → source name, for the `buffers` preamble section. Null or
    // empty ⇒ the section is omitted, and any `@loc(buf N, …)` the side-table
    // below produces names a buffer the reader cannot resolve. Supply it
    // whenever `sourceMap` is supplied. Entries are emitted SORTED BY BUFFER ID
    // regardless of the order given, so the emission is deterministic for a
    // caller whose own collection order is not (see the determinism contract on
    // `emitHir`).
    std::vector<HirTextBufferName> const* bufferNames = nullptr;

    // The five side-tables to serialize. Null = nothing of that kind is emitted.
    HirSourceMap     const* sourceMap     = nullptr;
    HirFfiMap        const* ffiMap        = nullptr;
    HirShaderMap     const* shaderMap     = nullptr;
    HirTranspileMap  const* transpileMap  = nullptr;
    HirDiagnosticMap const* diagnosticMap = nullptr;
};

// Serialize `hir` to canonical `.dsshir` text. Pure function of (hir, ctx): the
// canonical-format rules apply unconditionally so the output is always ready to
// round-trip. Internal-consistency problems (a typed node with no interner to
// decode it) are reported into `reporter` (Warning) and rendered as `?`; the call
// never aborts and never throws.
//
// ★ DETERMINISM IS A CONTRACT, not an observed property: equal `(hir, ctx)` ⇒
// byte-identical output, on every host and every run. Nothing here reads a
// clock, a path, an address or an unordered container's iteration order —
// symbol handles are assigned in PRE-ORDER of first encounter, and the one
// caller-supplied collection whose order is not the caller's business
// (`bufferNames`) is sorted here rather than trusted. A caller that needs the
// emission to move when the compiler moves puts that in `ctx.producer`, where
// it is visible, rather than getting it by accident.
[[nodiscard]] DSS_EXPORT std::string emitHir(Hir const& hir, HirTextContext const& ctx,
                                             DiagnosticReporter& reporter);

// ── hirTextTypeNodesSpelledTake — THE WRITER'S TYPE-SPELLING WORK, COUNTED ────
//
// The number of type NODES `emitHir` has rendered on this thread since the last
// call (each primitive, each `ptr<…>`, each `type <H>` reference, each field of a
// `types` definition counts once), and resets it. It is the instrument that
// states the v5 property as a number: a module with N mentions of a composite
// whose reachable graph has G nodes spells O(N + G) nodes — doubling N adds the
// mentions' own cost and nothing of G — where v4 spelled O(N × G).
// Deterministic, host- and load-independent, so a pin COUNTS and never times.
// Per-THREAD, because the driver's per-CU pool may emit several modules at once
// (the `mirDomSlotsSweptTake` precedent).
[[nodiscard]] DSS_EXPORT std::uint64_t hirTextTypeNodesSpelledTake() noexcept;

// ── renderHirKindInventory ────────────────────────────────────────────────────
//
// This build's HIR node-kind inventory, as machine-readable text: every core
// `HirKind` it can emit, whether the writer spells it in EXPRESSION position and
// with which `.dsshir` keyword, whether it must carry a resolved type, and
// whether it carries a symbol id — plus the statement-keyword vocabulary and the
// extension-kind base. `producer` heads the output for the same reason it heads
// an artifact: an inventory nobody can attribute to a compiler revision cannot
// be compared against anything.
//
// ★★ IT LIVES HERE, BESIDE THE WRITER, AND THAT IS THE WHOLE VALUE. Every field
// is projected from the SAME tables the emitter and the parser dispatch on
// (`kHirKindTable`, `exprKwForKind`/`isExprKind`, `kHirTextStmtKwTable`,
// `requiresValidType`), so the published inventory cannot describe a compiler
// other than the one that would write the file. An inventory maintained beside
// the CLI would be a hand-kept second copy — and a coverage table that has
// drifted from the real node set is worse than none, because a consumer trusts
// it to be exhaustive.
//
// ⓘ ARITY IS NOT REPORTED, and the omission is deliberate rather than pending:
// it is not a static property of a kind in this IR. A `Call` has one callee plus
// N arguments, a `Block` has N statements, a `ConstructAggregate` has one slot
// per field. Publishing a number would mean publishing a wrong one for every
// variadic kind, so the inventory publishes the SHAPE FACTS that are constant
// (position, typed-ness, symbol-carrying) and leaves child counts to the text
// itself, which spells them.
[[nodiscard]] DSS_EXPORT std::string renderHirKindInventory(std::string_view producer);

// ── HirParseResult ────────────────────────────────────────────────────────────
//
// The product of a parse. Heap-allocated and returned by `unique_ptr` because the
// five side-table maps bind to `&hir` (an `ArenaAttribute` holds a raw arena
// pointer): a stable heap address keeps those pointers valid, which a value-moved
// struct could not guarantee. Access the module as `result->hir`.
//
// `interner` owns the types re-interned from the text; pass `&result->interner`
// back into a follow-up `emitHir` (with `&result->symbolNames`) to reproduce the
// source bytes. The maps are populated from the text's inline `@…` annotations.
struct DSS_EXPORT HirParseResult {
    Hir                      hir;
    TypeInterner             interner;
    std::vector<std::string> symbolNames;   // SymbolId.v → name; slot 0 unused

    // The `producer "<…>"` header line's value, verbatim. EMPTY means the file
    // said `producer ""` (an unattributed artifact) — it never means the field
    // was missing, because a missing field is a fatal parse error. Feed it back
    // into `HirTextContext::producer` to reproduce the source bytes.
    std::string              producer;
    // The `buffers` preamble, in file order. Empty when the section was absent.
    // Feed it back into `HirTextContext::bufferNames` to reproduce the bytes.
    std::vector<HirTextBufferName> bufferNames;

    HirSourceMap     sourceMap;
    HirFfiMap        ffiMap;
    HirShaderMap     shaderMap;
    HirTranspileMap  transpileMap;
    HirDiagnosticMap diagnosticMap;
    // Literal values rebuilt from the text's inline `lit <value>` forms (empty
    // when the source used the bare `#<index>` form). Indexed by `payload`.
    HirLiteralPool   literalPool;
    // Inline-asm P5: descriptors rebuilt from the text's inline `inline_asm ...`
    // form (empty when the source used the bare `#<handle>` form). Indexed by an
    // `InlineAsm` node's `payload`, 1-based.
    HirInlineAsmPool inlineAsmPool;

    // True iff neither the parse nor the verify-on-load pass emitted an
    // Error-severity diagnostic (computed by delta on the reporter, so prior
    // diagnostics don't taint the verdict).
    bool ok = false;

    // Members init in declaration order: `hir` first, so the maps bind to the
    // already-constructed module. Non-copyable/movable (the maps' arena pointer);
    // it only ever lives behind the returned `unique_ptr`.
    HirParseResult(Hir h, TypeInterner ti, std::vector<std::string> names)
        : hir(std::move(h)), interner(std::move(ti)), symbolNames(std::move(names)),
          sourceMap(hir), ffiMap(hir), shaderMap(hir), transpileMap(hir),
          diagnosticMap(hir) {}

    HirParseResult(HirParseResult const&)            = delete;
    HirParseResult& operator=(HirParseResult const&) = delete;
    HirParseResult(HirParseResult&&)                 = delete;
    HirParseResult& operator=(HirParseResult&&)      = delete;
};

// Parse `.dsshir` text into a frozen module + its side-tables, then run
// `HirVerifier` on the result (verify-on-load). All parse and verify diagnostics
// go to `reporter`; `result->ok` reflects whether any were Error-severity. Types
// are re-interned into a fresh interner tagged with `cuId`. Collect-all: a
// malformed token is reported and parsing recovers to the next construct rather
// than aborting. On an unrecoverable header error the returned module is empty.
[[nodiscard]] DSS_EXPORT std::unique_ptr<HirParseResult> parseHir(
    std::string_view text, CompilationUnitId cuId, DiagnosticReporter& reporter);

// ── parseTypeFromText ─────────────────────────────────────────────────────────
//
// Parse a STANDALONE hir-text TYPE string (e.g. `"fn(ptr<char>) -> i32"`,
// `"ptr<char>"`, `"i32"`, `"void"`) into a `TypeId`, interning the result into
// the CALLER-PROVIDED `interner` (+ `typeReg` for `ext<>` extension kinds). The
// caller's interner/registry is used DIRECTLY — the produced TypeId belongs to
// `interner` (its `owner()` CU), ready to drop into that CU's IR.
//
// This drives the SAME grammar production as the hir-text module parser's `type`
// rule (`parseType`): there is EXACTLY ONE type-text decoder in the codebase, so
// the standalone path can never drift from what the module parser accepts.
//
// Returns `InvalidType` and emits at least one Error-severity diagnostic into
// `reporter` on malformed or unknown type text; it never returns a partially
// constructed type silently. Trailing tokens after a complete type are reported
// (the input must be a single type, nothing more).
//
// ⚠ A STANDALONE TYPE TEXT IS SELF-CONTAINED, AND THAT IS WHY IT KEEPS THE INLINE
// COMPOSITE FORMS a `.dsshir` module no longer uses: `struct "N" {…}`, `union
// "N" {…}`, `struct "N" opaque`, and `rec <H>` for a cycle. A module's
// `type <H>` reference names an entry of that module's `types` section, and a
// standalone text has no such section, so `type <H>` is REFUSED here by name
// rather than read as an unknown identifier.
//
// c82 (D-FFI-DESCRIPTOR-VA-LIST-TYPE): `namedTypes` is an optional set of
// caller-supplied NAME → TypeId bindings (core/types/named_type_binding.hpp)
// consulted for a bare identifier that is not a builtin type keyword (checked
// AFTER every structural keyword, so a binding can never shadow
// `ptr`/`struct`/`fn`/...). It lets a caller resolve an ABI-defined alias
// whose concrete type only the caller knows — the shipped-descriptor reader
// threads the semantic tier's per-CC `va_list` here, so a descriptor can
// spell C's `vfprintf(FILE*, const char*, va_list)` neutrally while the param
// lands the ABI-exact TypeId (SysV `__va_list_tag[1]` / AAPCS64 `__va_list`
// struct / Win64 `char*`). Content-blind and generic: nothing here knows what
// the names mean. An empty span is byte-identical to the pre-c82 behavior.
//
// ★★ P44 (item (a) of D-C23-REDECL-QUALIFIER-AXIS-HAS-THREE-UNCLAIMED-SOURCES) —
// `outQual` IS HOW A DESCRIPTOR SIGNATURE SPELLS A QUALIFIER, AND IT IS A
// SEPARATE RETURN BECAUSE IT HAS TO BE. `const` and `restrict` are deliberately
// NOT interned (type_interner.hpp: `const` never affects codegen or layout), so
// they CANNOT ride the TypeId this function returns — a shipped `printf` row
// spelled `fn(ptr<const<char>>, ...) -> i32` interns byte-identically to
// `fn(ptr<char>, ...) -> i32` and always will. The qualification travels here
// instead, in the same `DeclaredQualification` the semantic declarator walk
// produces, so the C23 oracle can compare a user prototype against a corpus row
// on the one axis type identity is blind to.
//
// ⚠ THE SPELLING IS TYPE-TEXT-ONLY, AND THAT IS FAIL-LOUD RATHER THAN A
// LIMITATION. A `const<…>` inside a HIR MODULE dump is a malformed type: the
// printer cannot emit it back (the TypeId does not carry the fact), so accepting
// it on the module path would make a round-trip silently drop a qualifier. The
// standalone type-text path always tracks the claim; `outQual` only decides
// whether the caller is handed it.
//
// `nullptr` (the default) is byte-identical to the pre-P44 behavior.
[[nodiscard]] DSS_EXPORT TypeId
parseTypeFromText(std::string_view typeText, TypeInterner& interner,
                  TypeRegistry& typeReg, DiagnosticReporter& reporter,
                  std::span<NamedTypeBinding const> namedTypes = {},
                  DeclaredQualification* outQual = nullptr);

} // namespace dss
