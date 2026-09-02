#include "core/types/type_lattice/type_layout.hpp"

#include "core/types/type_lattice/core_type.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <span>

namespace dss {

namespace {

// D-CSUBSET-UINT128-TYPE (TF-C94): the wide-integer accessors' fail-loud exit —
// a byte-for-byte mirror of `latticeFatal` (type_lattice.cpp), which is the
// backstop `TypeInterner::bitIntWidth`/`bitIntIsSigned` already use for the same
// precondition class. Reaching here means a caller asked for the width/signedness
// of a type that is NOT multi-limb, i.e. a facade site that skipped its `isWideInt`
// gate — an engine invariant break, never user input. Abort rather than return a
// guessed 64/128: a wrong width sizes the limb loop wrong and writes past (or short
// of) the slot, which is silent memory corruption, the one outcome worse than a crash.
[[noreturn]] void wideIntFatal(char const* what) {
    std::fputs("dss::type_layout fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

// Pointer byte width under a data model (the one OS-dependent layout dimension —
// every other scalar's width is already baked into its TypeKind by FC3).
[[nodiscard]] std::uint64_t pointerBytes(DataModel dm) noexcept {
    switch (dm) {
        case DataModel::Lp64:
        case DataModel::Llp64: return 8;
        case DataModel::Ilp32: return 4;
    }
    return 8;  // unreachable for a valid model
}

// The alignment of a scalar/pointer of `size` bytes under the params. The bounded
// natural-alignment rule: align = min(size, maxAlignment). Both operands are
// powers of two (scalar sizes ∈ {1,2,4,8,16}; maxAlignment is loader-validated
// pow2), so the min is a power of two — `ofRuntimePow2` is exact.
[[nodiscard]] Alignment scalarAlign(std::uint64_t size,
                                    AggregateLayoutParams params) noexcept {
    // `ScalarAlignmentRule::Natural` is the only rule today; a future non-natural
    // ABI adds a member here (an `Explicit` per-primitive table), never a
    // target-name branch.
    std::uint64_t const a =
        std::min<std::uint64_t>(size == 0 ? 1 : size, params.maxAlignment);
    return Alignment::ofRuntimePow2(static_cast<std::uint32_t>(a == 0 ? 1 : a));
}

// The stricter (larger) of two alignments.
[[nodiscard]] Alignment maxAlign(Alignment a, Alignment b) noexcept {
    return a.bytes() >= b.bytes() ? a : b;
}

// D-CSUBSET-COMPOSITE-ALIGNED (TF-C73): the SEED alignment a composite's own layout
// starts from — its WHOLE-COMPOSITE explicit alignment request
// (`__attribute__((aligned(N)))` on a struct/union DEFINITION), or 1 when it has
// none. Seeding is what realizes C 6.7.5's MAX rule BY CONSTRUCTION: every
// subsequent fold in the Struct/Union arms is `maxAlign(out.align, …)`, which can
// only RAISE, so the aggregate ends at max(natural, requested) and a request WEAKER
// than natural is automatically a no-op (clang: `struct {char;int;}
// __attribute__((aligned(2)))` stays align 4 / size 8) — with no comparison branch
// to get backwards, and no interaction to re-derive per arm.
//
// It composes with `packed` rather than fighting it: packed lowers the per-FIELD
// baseline to 1 (removing inter-field padding), this raises the AGGREGATE's own
// alignment, so `packed, aligned(16)` lands at clang's sizeof 16 — offsets packed
// tight, total rounded up to 16 — and NOT at packed's bare 5.
//
// AGNOSTIC: `explicitCompositeAlign` is language/target-neutral interned data and
// the cap is the `Alignment` newtype's own domain, not a hardcoded ABI number.
// Returns nullopt when the stored value is not a representable alignment — an
// upstream bug (`completeComposite` rejects it at the sink) — so layout fails LOUD
// rather than silently mis-aligning, mirroring the member-alignas `fromBytes` reject.
[[nodiscard]] std::optional<Alignment>
compositeSeedAlign(TypeInterner const& interner, TypeId id) noexcept {
    std::uint32_t const req = interner.explicitCompositeAlign(id);
    if (req == 0) return Alignment::of<1>();   // no request → the unchanged path
    return Alignment::fromBytes(req);
}

// ── The ONE member-alignment CLAMP (D-CSUBSET-PACKED-BITFIELD-INTERACTION) ──
//
// The exact DUAL of `compositeSeedAlign` above: that one RAISES the aggregate's
// own alignment, this one LOWERS each MEMBER's baseline. `packed` and
// `#pragma pack(N)` are the two channels that do the lowering, and EVERY
// member-alignment site in this engine now funnels through here — the Struct
// arm's `effectiveAlign`, the Union arm's, and BOTH bit-field packers (via
// `bitfieldPackerEffectiveAlign` for an ordinary field, and directly for a
// bit-field's storage-unit alignment).
//
// TF-C97: before this cycle the clamp was written TWICE — once in each
// non-bit-field arm — and the two bit-field packers had NEITHER copy, so
// `#pragma pack(N)` was silently IGNORED for any struct containing a bit-field.
// MEASURED vs `/usr/bin/clang -arch arm64`: `#pragma pack(4) struct { u64 a;
// unsigned b:1; }` came out 16/8 where clang says 12/4, and `pack(2) struct
// { unsigned a; u64 b:40; }` came out 16/8 where clang says 10/2. ONE helper is
// the point, not a tidiness preference: a fourth copy is a fourth chance to miss
// a site, and a missed site here is a silent ABI miscompile, not a crash.
//
// `packed` WINS over a surrounding `pack(N)` (gcc agrees: an explicit
// `__attribute__((packed))` is never weakened by a wider pragma cap), which
// falls out of returning the packed baseline FIRST and only clamping the
// natural one. A cap WEAKER than the member's natural alignment is a no-op, so
// `pack(8)` over a `char` is inert — by construction, with no comparison branch
// to get backwards.
//
// Returns nullopt when a stored cap is not a representable alignment. The value
// is validated a power of two in [1, 256] at the pragma AND again at
// `completeComposite`, so an unrepresentable one reaching here is an upstream
// bug — fail LOUD rather than silently lay the composite out uncapped.
//
// AGNOSTIC: both inputs are interned per-composite data; no target/format/
// language identity is consulted.
[[nodiscard]] std::optional<Alignment>
clampedBaselineAlign(bool packed, std::uint32_t packCap, Alignment natural) noexcept {
    if (packed) return Alignment::of<1>();
    if (packCap == 0 || packCap >= natural.bytes()) return natural;
    return Alignment::fromBytes(packCap);
}

// Is the declared bit-field strategy one the engine actually realizes? A
// declared-but-unbuilt strategy (e.g. a future ABI value) — and `None` (not
// declared at all) — fail loud at the consumer rather than silently using a
// wrong rule (the `aggregate_abi::aggregateAbiImplemented` precedent). The
// realized set grows with each new arm in `layoutStructBitfields*` below.
[[nodiscard]] constexpr bool
bitFieldStrategyRealized(BitFieldStrategy s) noexcept {
    switch (s) {
        case BitFieldStrategy::GnuPacked:
        case BitFieldStrategy::MsvcStraddle:
            return true;
        case BitFieldStrategy::None:
            return false;
    }
    return false;
}

// ── Per-strategy struct bit-field packers (D-CSUBSET-BITFIELD-ABI-EXACT) ──
//
// Each returns the fully-populated `out` (offsets, bitFields[], align, size) for
// a struct that CONTAINS a bit-field, or nullopt on a fail-loud condition (a
// non-integer bit-field, a malformed FAM, an out-of-scope field type). The
// Struct arm dispatches to one of these by SWITCHING on the strategy enum — the
// only place the per-ABI rule is selected, and NEVER on a target/format name.
//
// Both packers share the front matter the Struct arm prepared (`out.align`
// seeded to 1, `out.fieldOffsets` reserved). They fill `out.bitFields` (one
// `BitFieldPlacement` per field; `unitBytes == 0` marks an ordinary field or a
// zero-width break) + `out.fieldOffsets` (one byte offset per field) + the
// running `out.align` + the final `out.size`.

// D-CSUBSET-MEMBER-ALIGNAS: field `i`'s EFFECTIVE alignment inside a bit-field
// packer — the clamped baseline (`clampedBaselineAlign`) folded with the field's
// explicit `alignas` override under MAX semantics. This is the SAME two-step the
// non-bitfield struct/union arm's `effectiveAlign` lambda applies, hoisted here so
// BOTH bit-field packers honor it on their ORDINARY (non-bit-field) fields (a
// bit-field FIELD itself can't carry alignas — the semantic phase rejects it).
//
// `alignas` on a non-bit-field member of a bit-field-bearing struct is legal C11
// 6.7.5 and RAISES that field's (and thus the struct's) alignment — it must never be
// silently dropped; a `pack(N)` cap LOWERS it, and alignas still wins per-field over
// the cap via the MAX-fold. TF-C97 MEASURED: the cap half of this is what places `a`
// at offset 4 (not 8) in clang's `#pragma pack(4) struct { unsigned b:3; u64 a; }`.
//
// Returns `nullopt` when a stored cap or override is not a power of two in [1, 256]
// (an upstream pragma-/alignas-semantics bug — fail loud rather than silently
// mis-pad, mirroring the non-bit-field path's `Alignment::fromBytes` reject).
[[nodiscard]] std::optional<Alignment>
bitfieldPackerEffectiveAlign(TypeInterner const& interner, TypeId id, std::size_t i,
                             bool packed, std::uint32_t packCap, Alignment natural) {
    auto const baseline = clampedBaselineAlign(packed, packCap, natural);
    if (!baseline) return std::nullopt;
    if (!interner.hasExplicitAligns(id)) return baseline;
    std::uint32_t const ovr = interner.explicitFieldAlign(id, i);
    if (ovr == 0) return baseline;   // no override on this field
    auto const a = Alignment::fromBytes(ovr);
    if (!a) return std::nullopt;
    return maxAlign(*baseline, *a);
}

// GnuPacked: SysV/Itanium/GNU/AAPCS64/Apple little-endian. Bits flow LSB-first
// through a single absolute `bitCursor`; a field's allocation unit is its
// declared-type size; a straddle bumps to the next unit of that type; a
// zero-width unnamed field forces the cursor to its type's unit boundary;
// different-typed adjacent bit-fields may SHARE a unit. Struct size = the bits
// actually consumed, rounded up to the struct alignment.
[[nodiscard]] std::optional<StructLayout>
layoutStructBitfieldsGnuPacked(TypeId id, std::span<TypeId const> fields,
                               TypeInterner const& interner,
                               AggregateLayoutParams params, DataModel dm,
                               StructLayout out) {
    out.bitFields.assign(fields.size(), BitFieldPlacement{});
    // TF-C97 (D-CSUBSET-PACKED-BITFIELD-INTERACTION): the member-alignment cap this
    // composite was defined under — read ONCE here, then fed through
    // `clampedBaselineAlign` at every alignment site below.
    //
    // ★ D-CSUBSET-PACKED-BITFIELD-INTERACTION: `packed` REACHES THIS PACKER NOW. It
    // used to be always-false here (the Struct arm's belt refused packed + bit-fields
    // before dispatch) and was threaded through so the packer would be correct BY
    // CONSTRUCTION when the belt was lifted. The belt is lifted; the threading is what
    // made that a deletion instead of a new algorithm, and the correctness it bought
    // is now MEASURED rather than asserted (see the Struct arm's note for the seven
    // gcc/clang-pinned shapes).
    bool const          packed  = interner.isPacked(id);
    std::uint32_t const packCap = interner.maxFieldAlign(id);
    // The GNU straddle rule is CONDITIONAL on there being no member-alignment cap.
    // MEASURED (`/usr/bin/clang -arch arm64`): `struct { unsigned char a; unsigned
    // b:31; unsigned char c; }` is sizeof 12 bare but sizeof 8 under `#pragma
    // pack(4)` — AND ALSO 8 under `pack(16)`, a cap that clamps nothing. So it is
    // the mere PRESENCE of a cap, not its value, that turns the bump off: bits then
    // flow straight through the unit boundary instead of skipping to the next unit.
    // This mirrors clang's `AllowPadding = MaxFieldAlignment.isZero()`
    // (ItaniumRecordLayoutBuilder) and gcc's `maximum_field_alignment == 0` gate on
    // the whole PCC_BITFIELD_TYPE_MATTERS block — the two independent
    // implementations of the ABI this strategy names.
    //
    // ⚠ What this gate BUYS on a field that DOES flow through a unit boundary is
    // fail-loud, not a laid-out straddler: such a field trips the representability
    // guard further down and is REFUSED. That is the whole point — with the bump left
    // on, `pack(2) struct { unsigned a; u64 b:40; }` silently computes sizeof 14 where
    // clang says 10, a wrong ABI nobody is told about. Cases where no field straddles
    // are unaffected by this gate and get a fully correct layout from the alignment
    // clamp alone; that is the majority, including every shape in the macOS SDK's
    // pack+bit-field closure and every gcc/clang-pinned `packed` shape below.
    //
    // ★ `packed` SUPPRESSES THE BUMP FOR THE SAME REASON A CAP DOES, and that is not
    // an analogy — it is why `__attribute__((packed))` and `#pragma pack(1)` come out
    // byte-identical on every shape measured (D-CSUBSET-PACKED-BITFIELD-INTERACTION).
    // Both spellings set the member baseline to 1 through `clampedBaselineAlign` and
    // both land here as "no padding to the next unit", which is exactly gcc's and
    // clang's own rule.
    bool const allowStraddlePadding = !packed && packCap == 0;
    std::uint64_t bitCursor = 0;   // absolute bits from the struct start
    for (std::size_t i = 0; i < fields.size(); ++i) {
        TypeId const f  = fields[i];
        auto const    bw = interner.fieldBitWidth(id, i);
        if (!bw.has_value()) {
            // Ordinary field (incl. a FAM): close any open bit-unit by
            // rounding the cursor up to a byte, then to the field's align.
            if (interner.isIncompleteArray(f)) {
                if (i + 1 != fields.size()) return std::nullopt;
                auto const fops = interner.operands(f);
                if (fops.empty()) return std::nullopt;
                auto const elem = computeLayout(fops[0], interner, params, dm);
                if (!elem) return std::nullopt;
                // A FAM can carry `alignas` (`alignas(16) int fam[];`): raise its
                // effective alignment exactly as the non-bit-field path does.
                auto const ea = bitfieldPackerEffectiveAlign(
                    interner, id, i, packed, packCap, elem->align);
                if (!ea) return std::nullopt;
                std::uint64_t const fo = ea->alignUp((bitCursor + 7) / 8);
                out.fieldOffsets.push_back(fo);
                out.align = maxAlign(out.align, *ea);
                out.hasFlexibleArrayMember = true;
                bitCursor = fo * 8;   // no size contribution
                continue;
            }
            auto const fl = computeLayout(f, interner, params, dm);
            if (!fl) return std::nullopt;
            // Fold the cap + a member `alignas` override into the ordinary field's
            // alignment (D-CSUBSET-MEMBER-ALIGNAS) — a bit-field-bearing struct's
            // non-bit-field member may be over-aligned OR capped; the offset AND the
            // struct align must honor both.
            auto const ea = bitfieldPackerEffectiveAlign(
                interner, id, i, packed, packCap, fl->align);
            if (!ea) return std::nullopt;
            std::uint64_t const fo = ea->alignUp((bitCursor + 7) / 8);
            out.fieldOffsets.push_back(fo);
            out.align = maxAlign(out.align, *ea);
            bitCursor = (fo + fl->size) * 8;
            continue;
        }
        // Bit-field: the allocation unit is its declared type's size.
        auto const fl = computeLayout(f, interner, params, dm);
        if (!fl || fl->size == 0) return std::nullopt;   // non-int bitfield → fail loud
        // A bit-field's storage unit contributes its declared type's alignment to the
        // struct — CLAMPED by the cap, exactly like an ordinary field's. MEASURED:
        // clang's `#pragma pack(4) struct { char c; u64 b:1; }` is _Alignof 4 (the
        // capped u64), not 8 (uncapped) and not 1 (dropped).
        auto const unitAlign = clampedBaselineAlign(packed, packCap, fl->align);
        if (!unitAlign) return std::nullopt;
        std::uint64_t const unitBits = fl->size * 8;
        std::uint32_t const w        = *bw;
        if (w == 0) {
            // D-CSUBSET-ZERO-WIDTH-BITFIELD-ALIGNMENT.
            //
            // THE BREAK IS UNCONDITIONAL AND UNCAPPED, and stays exactly as it was:
            // the cursor is forced to the DECLARED TYPE's full unit boundary whatever
            // `#pragma pack` says (✔MEASURED: `pack(1) {char c; u64 :0; char d;}` still
            // breaks to bit 64). Only the ALIGNMENT contribution is per-ABI.
            //
            // ⚠ THE ALIGNMENT ANSWER SPLITS *WITHIN* gnu_packed, which is why it is a
            // config key and not a strategy. Same struct, same strategy (✔MEASURED,
            // gcc 13.3.0 + clang 18.1.3 agreeing PER TARGET, and Apple clang 21.0.0 on
            // the physical macOS host for the darwin rows):
            //     `{char c; unsigned :0; char d;}`
            //     x86_64-linux · arm64-apple · x86_64-apple · riscv64 · ppc64le -> 5/1
            //     aarch64-linux · arm-linux-gnueabihf ......................... -> 8/4
            // Before this key dss computed 8/4 for ALL of them: correct on the aarch64
            // ELF formats, a SILENT LAYOUT MISCOMPILE on the x86_64 ELF and every
            // Mach-O one. ⚠ Note what that means for the obvious "fix": simply dropping
            // the fold — which is what the defect report proposed — would have repaired
            // five ABIs and BROKEN the one that was right.
            switch (params.unnamedBitFieldAlignment) {
                case UnnamedBitFieldAlignment::Contributes:
                    // The NATURAL alignment, deliberately NOT `*unitAlign`: on this ABI
                    // the contribution is UNCAPPED by `#pragma pack`, the same uncapped
                    // quantity that decides the break two lines below. ✔MEASURED on
                    // aarch64-linux: `pack(1) {char c; u64 :0; char d;}` is 16/8, and
                    // `pack(2) {char c; unsigned :0; char d;}` is 8/4. Folding the
                    // CAPPED unit align here is what made dss answer 6/2 to that second
                    // one — a value NO reference produces, on either side of the axis.
                    out.align = maxAlign(out.align, fl->align);
                    break;
                case UnnamedBitFieldAlignment::Ignored:
                    break;   // contributes nothing; the break below still happens
                case UnnamedBitFieldAlignment::None:
                    // Undeclared. FAIL LOUD rather than pick a side: both answers are a
                    // real ABI, so a default would be a silent miscompile on every
                    // platform holding the other. Reached only when a zero-width
                    // bit-field is ACTUALLY laid out, so a format that declares no C
                    // ABI is unaffected.
                    return std::nullopt;
            }
            bitCursor = ((bitCursor + unitBits - 1) / unitBits) * unitBits;
            out.fieldOffsets.push_back(bitCursor / 8);   // marker; unitBytes stays 0
            continue;
        }
        // A bit-field WITH STORAGE contributes its unit's (capped) alignment. Folded
        // here rather than above the zero-width arm so that arm can decide for itself.
        //
        // ⚠ KNOWN GAP, MEASURED AND FILED, NOT AN OVERSIGHT: the same per-ABI axis
        // governs an UNNAMED bit-field that HAS storage — `{char c; unsigned :3; char
        // d;}` is 3/1 under `ignored` where `{char c; unsigned x:3; char d;}` is 4/4
        // (✔MEASURED on x86_64-linux AND on the macOS host, Apple clang 21.0.0). This
        // line therefore over-contributes for an unnamed non-zero-width field on an
        // `ignored` ABI. It is NOT fixable here: `TypeInterner` stores a per-field
        // WIDTH and no per-field NAME, so this packer cannot tell `unsigned :3` from
        // `unsigned x:3`. Zero width needs no such channel — a NAMED zero-width
        // bit-field is a hard error, so width 0 is unnamed by construction. Closing the
        // gap means carrying unnamed-ness from the semantic phase through the interner;
        // the key above is already named for the general rule so that lands with no
        // second decision. See D-CSUBSET-ZERO-WIDTH-BITFIELD-ALIGNMENT.
        out.align = maxAlign(out.align, *unitAlign);
        if (w > unitBits) return std::nullopt;   // defensive (semantic validates first)
        // If w bits at the cursor would straddle the type's unit boundary, bump to
        // the next aligned unit (the GNU rule) — UNLESS a member-alignment cap is in
        // effect, which turns the bump off entirely (see `allowStraddlePadding`).
        if (allowStraddlePadding
            && bitCursor / unitBits != (bitCursor + w - 1) / unitBits) {
            bitCursor = ((bitCursor + unitBits - 1) / unitBits) * unitBits;
        }
        // With the bump suppressed a field CAN now cross its allocation unit, and a
        // straddling placement is NOT REPRESENTABLE: `BitFieldPlacement` describes one
        // `unitBytes`-wide integer load/store at `fieldOffsets[i]`, so the consumers
        // (hir_to_mir's `emitBitfieldExtract`/`-Insert`, asm.cpp's initializer packer)
        // would read/write only the bits inside that unit — and the signed-extract
        // shift `B - bitOffset - bitWidth` would UNDERFLOW. Re-anchoring the unit onto
        // the field cannot be done safely either: the only anchors that cover the
        // field either run past the struct's own end (a read-modify-write there
        // CLOBBERS the next array element) or need the final struct size, which this
        // single forward pass does not yet know.
        //
        // So: fail LOUD (nullopt → positioned diag) rather than emit a placement whose
        // accesses silently drop the overhanging bits. Closing this properly means
        // widening `BitFieldPlacement` to a multi-unit/byte-granular access and
        // teaching both consumers — a change well outside the layout engine.
        // MEASURED: inert on the whole macOS SDK pack+bit-field closure (no straddler);
        // the one shape that trips it is `pack(2) struct { unsigned a; u64 b:40; }`.
        std::uint64_t const bitInUnit = bitCursor % unitBits;
        if (bitInUnit + w > unitBits) return std::nullopt;
        std::uint64_t const unitByteOffset = (bitCursor / unitBits) * fl->size;
        out.fieldOffsets.push_back(unitByteOffset);
        out.bitFields[i] = BitFieldPlacement{
            static_cast<std::uint32_t>(fl->size),
            static_cast<std::uint32_t>(bitInUnit),
            w};
        bitCursor += w;
    }
    out.size = out.align.alignUp((bitCursor + 7) / 8);
    return out;
}

// MsvcStraddle: Microsoft x64 (PE). Each bit-field allocation unit is aligned to
// its declared-type natural alignment; a unit is REUSED by a following bit-field
// ONLY when the declared-type SIZE matches AND the bits fit. Any type-size
// change, intervening ordinary field, zero-width field, or straddle CLOSES the
// unit and opens a FRESH type-aligned unit at the high-water mark. Struct size
// covers the LAST unit's FULL declared-type width (so `{char a:1;}`→1,
// `{int a:1;}`→4, `{int a:1; char b:1;}`→8 with b@byte4). The rule + every
// golden below was derived empirically from cl.exe 14.51
// (D-CSUBSET-BITFIELD-ABI-EXACT conformance witness).
[[nodiscard]] std::optional<StructLayout>
layoutStructBitfieldsMsvcStraddle(TypeId id, std::span<TypeId const> fields,
                                  TypeInterner const& interner,
                                  AggregateLayoutParams params, DataModel dm,
                                  StructLayout out) {
    out.bitFields.assign(fields.size(), BitFieldPlacement{});
    // TF-C97 (D-CSUBSET-PACKED-BITFIELD-INTERACTION): the member-alignment cap, read
    // ONCE and fed through `clampedBaselineAlign` at every alignment site below —
    // the same treatment the GnuPacked packer gets. `#pragma pack(N)` is MSVC's OWN
    // native mechanism, so ignoring it here was the more glaring half of the defect.
    // ⚠ INFERRED, not cl.exe-MEASURED (no MSVC toolchain on this host): the clamp
    // follows MSVC's documented `#pragma pack(n)` semantics ("align on the smaller of
    // natural alignment or n") applied to a bit-field's allocation unit, matching
    // clang's MicrosoftRecordLayoutBuilder (`FieldAlign = min(FieldAlign,
    // MaxFieldAlignment)` in its `layoutBitField`). The unit-REUSE rule above is
    // untouched — unlike the GNU straddle bump it is not gated on the cap, so this
    // packer keeps its cl.exe-derived placement exactly.
    bool const          packed  = interner.isPacked(id);
    std::uint32_t const packCap = interner.maxFieldAlign(id);
    std::uint64_t highWaterByte = 0;   // one past all placed content
    std::uint64_t unitTypeSize  = 0;   // bytes of the open bit-field unit (0 = none)
    std::uint64_t unitStartByte = 0;   // byte offset where the open unit begins
    std::uint64_t unitBitsUsed  = 0;   // bits consumed in the open unit
    for (std::size_t i = 0; i < fields.size(); ++i) {
        TypeId const f  = fields[i];
        auto const    bw = interner.fieldBitWidth(id, i);
        if (!bw.has_value()) {
            // Ordinary field (incl. a FAM): closes any open bit-unit; lands at
            // the next byte aligned to its own alignment.
            unitTypeSize = 0;   // close the unit (MSVC never reopens it)
            if (interner.isIncompleteArray(f)) {
                if (i + 1 != fields.size()) return std::nullopt;
                auto const fops = interner.operands(f);
                if (fops.empty()) return std::nullopt;
                auto const elem = computeLayout(fops[0], interner, params, dm);
                if (!elem) return std::nullopt;
                // A FAM can carry `alignas`: raise its effective alignment
                // (D-CSUBSET-MEMBER-ALIGNAS), mirroring the non-bit-field path.
                auto const ea = bitfieldPackerEffectiveAlign(
                    interner, id, i, packed, packCap, elem->align);
                if (!ea) return std::nullopt;
                std::uint64_t const fo = ea->alignUp(highWaterByte);
                out.fieldOffsets.push_back(fo);
                out.align = maxAlign(out.align, *ea);
                out.hasFlexibleArrayMember = true;
                highWaterByte = fo;   // no size contribution
                continue;
            }
            auto const fl = computeLayout(f, interner, params, dm);
            if (!fl) return std::nullopt;
            // Fold the cap + a member `alignas` override into the ordinary field's
            // alignment (D-CSUBSET-MEMBER-ALIGNAS) — legal on a non-bit-field member
            // of a bit-field-bearing struct; the offset AND struct align honor both.
            auto const ea = bitfieldPackerEffectiveAlign(
                interner, id, i, packed, packCap, fl->align);
            if (!ea) return std::nullopt;
            std::uint64_t const fo = ea->alignUp(highWaterByte);
            out.fieldOffsets.push_back(fo);
            out.align = maxAlign(out.align, *ea);
            highWaterByte = fo + fl->size;
            continue;
        }
        // Bit-field: the allocation unit is its declared type's size, aligned to
        // its declared type's natural alignment.
        auto const fl = computeLayout(f, interner, params, dm);
        if (!fl || fl->size == 0) return std::nullopt;   // non-int bitfield → fail loud
        // The unit's alignment — its declared type's, CLAMPED by the cap. Used both
        // for the struct-alignment fold and to place the unit itself below, so a
        // capped composite can never open a unit at an over-aligned byte.
        auto const unitAlign = clampedBaselineAlign(packed, packCap, fl->align);
        if (!unitAlign) return std::nullopt;
        std::uint64_t const t        = fl->size;          // unit type size (bytes)
        std::uint64_t const unitBits = t * 8;
        std::uint32_t const w        = *bw;
        if (w == 0) {
            // D-CSUBSET-ZERO-WIDTH-BITFIELD-ALIGNMENT. A zero-width unnamed bit-field
            // under the MSVC rule is CONDITIONAL on there being an OPEN allocation
            // unit: it TERMINATES a run of bit-fields, and where there is no run to
            // terminate it is a complete NO-OP — it neither raises the struct's
            // alignment nor moves the high-water mark.
            //
            // ✔MEASURED, cl.exe 19.51 (`/std:c17`, `_Static_assert` on sizeof AND
            // __alignof, so a wrong expectation names itself):
            //   `{char c; unsigned :0; char d;}`           -> 2/1  (d at byte 1)
            //   `{char c; unsigned long long :0; char d;}` -> 2/1
            //   `{char c; unsigned :0;}`                   -> 1/1
            //   `{unsigned :0; char c;}`                   -> 1/1
            //   `{unsigned a:1; unsigned :0; unsigned b:1;}`           -> 8/4
            //   `{unsigned a:1; unsigned long long :0; unsigned b:1;}` -> 16/8
            // The last two are the pair that fixes the rule: with a unit OPEN the
            // zero-width field DOES fold its (capped) type alignment and DOES bump the
            // high-water to it — that is the only way `u64 :0` between two `u32`
            // bit-fields reaches 16/8 rather than 8/4 — while the first four show that
            // with NO unit open the very same declaration changes nothing at all.
            //
            // ⚠ THE OLD ARM APPLIED THE EFFECTIVE HALF UNCONDITIONALLY, and that was a
            // SILENT LAYOUT MISCOMPILE on every PE target: `{char c; unsigned :0; char
            // d;}` came out 8/4 where cl.exe says 2/1 — no diagnostic, just a struct
            // laid out to the wrong ABI. It also mis-sized `#pragma pack(2)`/`(4)`
            // variants (4/2 and 8/4 where cl.exe says 2/1). `pack(1)` happened to agree
            // ONLY because the cap clamped the folded alignment to 1, which is how the
            // defect survived the TF-C97 pack battery.
            //
            // ⚠ This is the MSVC rule ONLY. The GNU/AAPCS/Apple family answers this
            // question differently AND does not agree with itself across targets — see
            // the zero-width arm in `layoutStructBitfieldsGnuPacked`.
            if (unitTypeSize != 0) {
                out.align     = maxAlign(out.align, *unitAlign);
                highWaterByte = unitAlign->alignUp(highWaterByte);
                unitTypeSize  = 0;   // close the run (MSVC never reopens it)
            }
            out.fieldOffsets.push_back(highWaterByte);   // marker; unitBytes stays 0
            continue;
        }
        // A bit-field with STORAGE always contributes its unit's (capped) alignment.
        // Folded here rather than above the zero-width arm so the zero-width case can
        // decide for itself (D-CSUBSET-ZERO-WIDTH-BITFIELD-ALIGNMENT).
        out.align = maxAlign(out.align, *unitAlign);
        if (w > unitBits) return std::nullopt;   // defensive (semantic validates first)
        bool const canContinue =
            (unitTypeSize == t) && (unitBitsUsed + w <= unitBits);
        if (!canContinue) {
            // Open a fresh unit of this type at the next (capped) type-aligned byte.
            unitStartByte = unitAlign->alignUp(highWaterByte);
            unitTypeSize  = t;
            unitBitsUsed  = 0;
        }
        out.fieldOffsets.push_back(unitStartByte);
        out.bitFields[i] = BitFieldPlacement{
            static_cast<std::uint32_t>(t),
            static_cast<std::uint32_t>(unitBitsUsed),
            w};
        unitBitsUsed += w;
        // The struct must cover the FULL declared-type width of every opened unit.
        highWaterByte = std::max(highWaterByte, unitStartByte + t);
    }
    out.size = out.align.alignUp(highWaterByte);
    return out;
}

} // namespace

std::optional<std::uint64_t> scalarByteSize(TypeKind kind, DataModel dm) noexcept {
    switch (kind) {
        // 1-byte: C `_Bool`, `char` (signed/unsigned char map to I8/U8), I8/U8, Byte.
        case TypeKind::Bool: case TypeKind::I8: case TypeKind::U8:
        case TypeKind::Char: case TypeKind::Byte:
            return 1;
        case TypeKind::I16: case TypeKind::U16: case TypeKind::F16:
            return 2;
        case TypeKind::I32: case TypeKind::U32: case TypeKind::F32:
            return 4;
        case TypeKind::I64: case TypeKind::U64: case TypeKind::F64:
            return 8;
        // FC17.9(e) (D-CSUBSET-LONG-DOUBLE): F80 (x87 80-bit) STORES as 16/16 —
        // x86_64-SysV and darwin-x86_64 both pad the 10 significant bytes to a
        // 16-byte, 16-aligned slot (the same size/align binary128 uses).
        case TypeKind::I128: case TypeKind::U128: case TypeKind::F80:
        case TypeKind::F128:
            return 16;
        // Pointer-class scalars take the model's pointer width. C23 nullptr_t has
        // the same size/representation as `void*` (§6.2.5), so `sizeof(nullptr)`
        // is the pointer width — even though the `nullptr` value itself lowers to
        // the integer-0 null constant and never materializes as a NullptrT value.
        case TypeKind::Ptr: case TypeKind::Ref: case TypeKind::FnPtr:
        case TypeKind::NullptrT:
            return pointerBytes(dm);
        // Not a sized scalar: aggregates are handled by `computeLayout`; Void and
        // the out-of-scope kinds (FnSig/Slice/Tuple/Vector/Matrix/Nullable/
        // Optional/Param/Bind/Enum/Struct/Union/Array/Extension) return nullopt —
        // the caller's fail-loud signal.
        default:
            return std::nullopt;
    }
}

std::optional<std::uint64_t>
sizeOfScalarOrBitInt(TypeInterner const& interner, TypeId id, DataModel dm) noexcept {
    if (!id.valid()) return std::nullopt;
    TypeKind const k = interner.kind(id);
    if (k != TypeKind::BitInt) return scalarByteSize(k, dm);
    // C23 _BitInt(N) (D-CSUBSET-BITINT): the CONTAINER size (params-independent, so
    // no AggregateLayoutParams needed) — mirrors the `computeLayout` BitInt arm's
    // size ladder exactly (N≤64 → {1,2,4,8}; N>64 → ceil(N/64) eightbytes).
    std::int64_t const n = interner.bitIntWidth(id);
    if (n <= 0) return std::nullopt;
    std::uint64_t const bits = static_cast<std::uint64_t>(n);
    if (bits <= 8)  return 1;
    if (bits <= 16) return 2;
    if (bits <= 32) return 4;
    if (bits <= 64) return 8;
    return ((bits + 63) / 64) * 8;
}

// ── The WIDE-INTEGER facade (D-CSUBSET-BITINT-C2-WIDE + D-CSUBSET-UINT128-TYPE) ──
// The ONE place that knows WHICH kinds are multi-limb. TF-C94 generalized this from
// the BitInt-only `isWideBitInt`: I128/U128 are exactly 2 limbs, so they ride the
// shipped limb emitters unchanged once width + signedness come from the two
// accessors below rather than the BitInt-only interner ones (which abort on them —
// deliberately: that abort is the backstop for a facade site this sweep missed).
bool isWideInt(TypeInterner const& interner, TypeId id) noexcept {
    if (!id.valid()) return false;
    switch (interner.kind(id)) {
        // __int128 / unsigned __int128 (D-CSUBSET-UINT128-TYPE): 128 bits has no
        // native container on either shipped CPU — 2 limbs, memory-resident, exactly
        // like `_BitInt(128)`. They join by SHAPE; the standard-rank vs bit-precise
        // distinction is a SEMANTIC one (type_rules.hpp `kindAtRank`) and does not
        // belong here. ⚠ `_BitInt(128)` (align 8) and U128 (align 16) remain
        // INDEPENDENT layouts — see computeLayout's BitInt arm vs scalarByteSize.
        case TypeKind::I128:
        case TypeKind::U128:
            return true;
        case TypeKind::BitInt:
            return interner.bitIntWidth(id) > 64;   // C1 (N≤64) is single-container
        default:
            return false;
    }
}

std::int64_t wideIntWidthBits(TypeInterner const& interner, TypeId id) {
    if (id.valid()) {
        switch (interner.kind(id)) {
            case TypeKind::I128:
            case TypeKind::U128:
                return 128;
            case TypeKind::BitInt: {
                std::int64_t const n = interner.bitIntWidth(id);
                if (n > 64) return n;
                break;   // a NARROW _BitInt is not multi-limb — fall to fail-loud
            }
            default:
                break;
        }
    }
    wideIntFatal("wideIntWidthBits: TypeId is not a WIDE integer — only a "
                 "_BitInt(N>64), __int128 or unsigned __int128 is multi-limb "
                 "(D-CSUBSET-BITINT-C2-WIDE / D-CSUBSET-UINT128-TYPE); the caller "
                 "skipped its isWideInt gate");
}

bool wideIntIsSigned(TypeInterner const& interner, TypeId id) {
    if (id.valid()) {
        switch (interner.kind(id)) {
            case TypeKind::I128: return true;    // __int128 is signed by definition
            case TypeKind::U128: return false;   // unsigned __int128
            case TypeKind::BitInt:
                if (interner.bitIntWidth(id) > 64) return interner.bitIntIsSigned(id);
                break;   // a NARROW _BitInt is not multi-limb — fall to fail-loud
            default:
                break;
        }
    }
    wideIntFatal("wideIntIsSigned: TypeId is not a WIDE integer — only a "
                 "_BitInt(N>64), __int128 or unsigned __int128 is multi-limb "
                 "(D-CSUBSET-BITINT-C2-WIDE / D-CSUBSET-UINT128-TYPE); the caller "
                 "skipped its isWideInt gate");
}

bool isComplex(TypeInterner const& interner, TypeId id) noexcept {
    return id.valid() && interner.kind(id) == TypeKind::Complex;
}

bool isMemoryResidentType(TypeInterner const& interner, TypeId id) noexcept {
    if (!id.valid()) return false;
    switch (interner.kind(id)) {
        case TypeKind::Struct:
        case TypeKind::Union:
        case TypeKind::Array:
        // C99 _Complex (D-CSUBSET-COMPLEX): a complex is a memory-resident by-value
        // aggregate {re, im} reached by ADDRESS, mirroring a wide `_BitInt` exactly
        // — it has no bare-SSA aggregate value.
        case TypeKind::Complex:
        // D-CSUBSET-UINT128-TYPE (TF-C94): `__int128`/`unsigned __int128` are
        // MEMORY-RESIDENT for the same reason a wide `_BitInt` is — 128 bits has no
        // native register/ALU width on either shipped CPU (MEASURED: mir_to_lir.cpp
        // `requireNativeIntWidth` gates I128/U128 alongside the sub-32 kinds), so a
        // 128-bit value has no SSA form and must be reached by ADDRESS. THIS is the
        // arm that routes 128-bit values into the multi-limb emitters; without it
        // they fall to a bare-SSA scalar path that carries only the low 8 bytes.
        case TypeKind::I128:
        case TypeKind::U128:
            return true;
        case TypeKind::BitInt:
            return interner.bitIntWidth(id) > 64;   // wide _BitInt is multi-limb
        default:
            return false;
    }
}

bool isByValueClass(TypeInterner const& interner, TypeId id) noexcept {
    if (!id.valid()) return false;
    switch (interner.kind(id)) {
        case TypeKind::Struct:
        case TypeKind::Union:
        // C99 _Complex (D-CSUBSET-COMPLEX): passed/returned/copy-assigned BY VALUE
        // like a struct{re, im} (the by-address call/return/init/assign gates funnel
        // here). NOT Array — a complex does not decay.
        case TypeKind::Complex:
        // D-CSUBSET-UINT128-TYPE (TF-C94): a 128-bit integer is passed / returned /
        // copy-assigned BY VALUE like a wide `_BitInt` — the calling-convention gates
        // hand it to classifyAggregate (2 eightbytes) instead of a single GPR, and
        // the copy sites move all 16 bytes instead of a low-8 scalar Load+Store. NOT
        // Array (it does not decay), which is why this list differs from the sibling.
        case TypeKind::I128:
        case TypeKind::U128:
            return true;
        case TypeKind::BitInt:
            return interner.bitIntWidth(id) > 64;   // ARRAY excluded (it decays)
        default:
            return false;
    }
}

std::optional<StructLayout>
computeLayout(TypeId id, TypeInterner const& interner,
              AggregateLayoutParams params, DataModel dm) {
    // c27 (D-CSUBSET-VOLATILE-POINTEE): a `volatile T` has the SAME layout as T
    // (C 6.7.3 — a qualifier never changes size/alignment). Strip the VolatileQual
    // skin ONCE here so the whole engine — incl. the raw-kind incomplete checks
    // below and the recursive field/element layouts — operates on the material
    // type. This single strip makes `sizeof(volatile T) == sizeof(T)` hold by
    // construction and routes a volatile-qualified struct/array/scalar down its
    // normal arm. (The transparent `kind()`/`operands()` would mostly suffice, but
    // `isIncompleteComposite`/`isIncompleteArray` read the RAW record kind.)
    id = interner.stripVolatile(id);
    TypeKind const kind = interner.kind(id);

    // D-CSUBSET-SELF-REFERENTIAL-STRUCT: an INCOMPLETE composite (a forward-declared
    // struct/union whose body has not been seen) has NO size — `sizeof` of it, or a
    // by-value member of it, is ill-formed (C 6.5.3.4 / 6.7.2.1). Fail loud (nullopt
    // → positioned diagnostic), never a guessed/zero size. This is also the backstop
    // that keeps layout from recursing on a self-by-value cycle (the semantic phase
    // leaves such a composite incomplete after emitting S_IncompleteTypeMember).
    if (interner.isIncompleteComposite(id)) return std::nullopt;

    // Scalars + pointers: degenerate layout (no field offsets).
    if (auto const sz = scalarByteSize(kind, dm)) {
        return StructLayout{*sz, scalarAlign(*sz, params), {}, false};
    }

    switch (kind) {
        case TypeKind::Enum: {
            // size/align = the underlying integer primitive (scalars[0] = kind).
            auto const sc = interner.scalars(id);
            if (sc.empty()) return std::nullopt;
            auto const under = static_cast<TypeKind>(sc[0]);
            auto const sz = scalarByteSize(under, dm);
            if (!sz) return std::nullopt;
            return StructLayout{*sz, scalarAlign(*sz, params), {}, false};
        }
        case TypeKind::BitInt: {
            // C23 _BitInt(N) (D-CSUBSET-BITINT): the ABI layout. scalars[0] = N bits.
            // N≤64 → the smallest native container {1,2,4,8}B, align == size (matches
            // gcc/clang: sizeof(_BitInt(4))==1, _BitInt(17)==4, _BitInt(40)==8). N>64
            // (C2 multi-limb; C1 rejects it at semantic, but layout is ready) →
            // ceil(N/64) eightbytes, align 8 per the x86-64 psABI — sizeof(_BitInt(128))
            // ==16 but _Alignof==8 (NOT 16). A non-positive N is malformed (the
            // semantic gate rejects it first) → fail loud (nullopt).
            auto const sc = interner.scalars(id);
            if (sc.empty() || sc[0] <= 0) return std::nullopt;
            std::uint64_t const n = static_cast<std::uint64_t>(sc[0]);
            std::uint64_t size;
            std::uint64_t alignBytes;
            if (n <= 8)       { size = 1; alignBytes = 1; }
            else if (n <= 16) { size = 2; alignBytes = 2; }
            else if (n <= 32) { size = 4; alignBytes = 4; }
            else if (n <= 64) { size = 8; alignBytes = 8; }
            else {
                size       = ((n + 63) / 64) * 8;   // ceil(N/64) eightbytes
                alignBytes = 8;                       // psABI: align 8 even at N>64
            }
            return StructLayout{size, scalarAlign(alignBytes, params), {}, false};
        }
        case TypeKind::Complex: {
            // C99 _Complex (D-CSUBSET-COMPLEX) §6.2.5p13: a complex lays out EXACTLY
            // like an array {re, im} of two element-float components — real@0,
            // imag@elemSize, size 2×elemSize, align = element align. This IS the ABI
            // leaf layout (collectLeaves emits 2 float leaves at 0/elemSize). A
            // long-double-complex (F80/F128 element) sizes fine here (decl/sizeof/
            // ABI-reject work); only its VALUE arithmetic walls loud downstream.
            auto const ops = interner.operands(id);
            if (ops.empty()) return std::nullopt;
            auto const elem = computeLayout(ops[0], interner, params, dm);
            if (!elem) return std::nullopt;
            std::uint64_t const es = elem->size;
            return StructLayout{es * 2, elem->align, {}, false};
        }
        case TypeKind::Array: {
            // A bare flexible/incomplete array `T[]` has NO standalone size — it is
            // only legal as a struct's last field (handled in the Struct arm).
            if (interner.isIncompleteArray(id)) return std::nullopt;
            auto const ops = interner.operands(id);
            auto const sc  = interner.scalars(id);
            if (ops.empty() || sc.empty() || sc[0] < 0) return std::nullopt;
            auto const elem = computeLayout(ops[0], interner, params, dm);
            if (!elem) return std::nullopt;
            std::uint64_t const stride = elem->align.alignUp(elem->size);
            std::uint64_t const len    = static_cast<std::uint64_t>(sc[0]);
            return StructLayout{stride * len, elem->align, {}, false};
        }
        case TypeKind::Struct: {
            auto const fields = interner.operands(id);
            StructLayout out{};
            // D-CSUBSET-COMPOSITE-ALIGNED (TF-C73): SEED the aggregate alignment with
            // the whole-composite `aligned(N)` request (1 when there is none — the
            // unchanged path). Every later fold is a `maxAlign`, so this realizes
            // C 6.7.5's max(natural, requested) rule for EVERY struct arm at once —
            // the explicit-offset arm, the packed/plain byte arm, and both bit-field
            // packers, which each end with `out.align.alignUp(...)`. Field OFFSETS
            // are unaffected: they are placed from the per-FIELD `effectiveAlign`,
            // never from `out.align`.
            auto const seed = compositeSeedAlign(interner, id);
            if (!seed) return std::nullopt;   // unrepresentable stored request
            out.align = *seed;
            out.fieldOffsets.reserve(fields.size());
            // c107 (D-FFI-DESCRIPTOR-UNION-OVERLAY): a struct carrying EXPLICIT
            // per-field byte offsets (an FFI overlapping-union modeled as a struct)
            // uses those offsets verbatim instead of natural-alignment derivation.
            // Offsets may OVERLAP (ULARGE_INTEGER {QuadPart@0, LowPart@0, HighPart@4}):
            // size = the max field extent, align = the max field alignment. This is
            // a SEPARATE channel from bitfields (offsets are not in scalars — F1), so
            // an explicit-offset struct never carries bit-fields; a config that pairs
            // them is rejected here (fail loud) rather than silently mis-laid.
            if (interner.hasExplicitOffsets(id)) {
                if (!interner.scalars(id).empty()) return std::nullopt;  // bitfields + offsets: unsupported
                std::uint64_t extent = 0;
                for (std::size_t i = 0; i < fields.size(); ++i) {
                    auto const off = interner.explicitFieldOffset(id, i);
                    if (!off) return std::nullopt;                 // partial offsets: malformed
                    auto const fl = computeLayout(fields[i], interner, params, dm);
                    if (!fl) return std::nullopt;
                    out.fieldOffsets.push_back(*off);
                    out.align = maxAlign(out.align, fl->align);
                    extent = std::max(extent, *off + fl->size);
                }
                out.size = out.align.alignUp(extent);
                return out;
            }
            // D-CSUBSET-PACKED: the whole-composite packed flag (C/C23
            // `__attribute__((packed))`) removes ALL derived inter-field padding —
            // the per-field baseline alignment becomes 1 — and the aggregate's own
            // alignment stays at the seed (1 unless a whole-composite `aligned(N)`
            // raised it). Read once; fed into `effectiveAlign`'s baseline below. An
            // UNPACKED struct leaves `packed` false and the baseline is the field's
            // natural align (the unchanged path).
            //
            // D-CSUBSET-COMPOSITE-ALIGNED: packed and a whole-composite `aligned(N)`
            // are NOT in conflict and BOTH apply — packed removes the inter-field
            // padding (baseline 1 below), the request raises the aggregate's own
            // alignment (already in `out.align`). That combination is exactly why
            // clang gives `struct S { char a; int b; } __attribute__((packed,
            // aligned(16)));` sizeof 16 / _Alignof 16, not packed's bare 5 / 1.
            bool const packed = interner.isPacked(id);
            // FC8 bitfields (D-CSUBSET-BITFIELD): a bitfield-free struct interns
            // with EMPTY scalars (see TypeInterner::structType), so this O(1) test
            // routes every existing struct down the unchanged byte path below.
            bool const anyBitfield = !interner.scalars(id).empty();
            // ★ D-CSUBSET-PACKED-BITFIELD-INTERACTION — THE BELT IS GONE, and the
            // deliberate cycle its predecessor asked for is THIS one. A `packed`
            // aggregate that also carries a bit-field used to fail loud twice (the
            // semantic `S_PackedBitfieldUnsupported` front door + a `packed &&
            // anyBitfield → nullopt` line here and in the Union arm). Both are
            // removed: `packed` now flows into the SAME two packers every other
            // bit-field struct uses, and the layout it produces is the references'.
            //
            // ✔MEASURED (gcc 13.3.0 + clang 18.1.3, x86_64-linux, `_Static_assert` on
            // sizeof AND `_Alignof`, compile rc=0): `__attribute__((packed))` and
            // `#pragma pack(1)` are BYTE-IDENTICAL on every shape probed —
            // `{char c; unsigned b:9;}` 3/1 · `{char c; u64 b:60; char d;}` 10/1 ·
            // `{u64 a; unsigned b:1;}` 9/1 · `{char c; unsigned :0; char d;}` 5/1 ·
            // `{unsigned a:3; unsigned b:5;}` 1/1 · `{int f0; unsigned f1:3;}` 5/1 ·
            // `{unsigned a:3; char x; unsigned b:5;}` 3/1. That equivalence is what
            // makes this a DELETION rather than a new algorithm: the `#pragma pack(1)`
            // half has shipped and been conformance-pinned since TF-C97, and `packed`
            // reaches the identical code through `clampedBaselineAlign` (baseline 1)
            // and `allowStraddlePadding` (off for BOTH spellings).
            //
            // ⚠ WHY NOT KEEPING IT WAS THE FAIL-LOUD CHOICE, not a relaxation of one:
            // all three references ACCEPT the construct (gcc 13.3.0, clang 18.1.3 via
            // `__attribute__((packed))`; cl.exe 19.51 and mingw-w64 gcc 13.2.0 via
            // `#pragma pack(1)`, which is MSVC's only spelling), so refusing it was a
            // conformance divergence, not a guard. The guard that remains is the one
            // that matters: a bit-field the placement model cannot EXPRESS still
            // returns nullopt below (the `bitInUnit + w > unitBits` refusal), so a
            // packed straddler is refused rather than silently mis-placed.
            if (!anyBitfield) {
                // D-CSUBSET-MEMBER-ALIGNAS: a struct may carry per-field `alignas`
                // overrides. Read them once here; `effectiveAlign` folds field i's
                // override (when non-zero) into its natural alignment with MAX
                // semantics — a member alignas RAISES a field's alignment, never
                // lowers it. An align-free struct (the common case) leaves
                // `hasAligns` false and this collapses to the unchanged path below.
                bool const hasAligns = interner.hasExplicitAligns(id);
                // ★★ TF-C82 (D-PP-PRAGMA-REGISTRY): the `#pragma pack(N)` CAP this
                // composite was defined under (0 = none, i.e. every composite until
                // this cycle). It CLAMPS each field's natural alignment — the exact
                // dual of the whole-composite `aligned(N)` seeded into `out.align`
                // above, which RAISES the aggregate's. MEASURED against clang on
                // arm64: `sys/fcntl.h`'s `struct log2phys` under `#pragma pack(4)`
                // is offsets 0/4/12, sizeof 20, _Alignof 4; uncapped it is 0/8/16,
                // sizeof 24, _Alignof 8 — the wrong-ABI struct DSS used to compute
                // for a live `fcntl(F_LOG2PHYS)` call.
                std::uint32_t const packCap = interner.maxFieldAlign(id);
                auto effectiveAlign =
                    [&](std::size_t i, Alignment natural) -> std::optional<Alignment> {
                    // D-CSUBSET-PACKED / TF-C82: `packed` drops the per-field BASELINE
                    // to 1 and `#pragma pack(N)` caps it at N — both via the ONE
                    // shared clamp (TF-C97), which the bit-field packers now call too.
                    // A member `alignas` still RAISES it via the MAX-fold below
                    // (alignas wins per-field even under packed — `alignas(8) int x;`
                    // in a packed struct keeps 8-byte alignment).
                    auto const clamped = clampedBaselineAlign(packed, packCap, natural);
                    if (!clamped) return std::nullopt;   // upstream bug — never silent
                    Alignment const baseline = *clamped;
                    if (!hasAligns) return baseline;
                    std::uint32_t const ovr = interner.explicitFieldAlign(id, i);
                    if (ovr == 0) return baseline;   // no override on this field
                    // A stored override must be a power of two in [1, 256]; a value
                    // outside that is an upstream (alignas-semantics) bug — fail loud
                    // rather than silently mis-pad.
                    auto const a = Alignment::fromBytes(ovr);
                    if (!a) return std::nullopt;
                    return maxAlign(baseline, *a);
                };
                std::uint64_t off = 0;
                for (std::size_t i = 0; i < fields.size(); ++i) {
                    TypeId const f = fields[i];
                    // A flexible array member takes its element's alignment + an
                    // offset, but contributes ZERO to the struct size (the unsized
                    // tail). It is legal ONLY as the LAST field — a non-last FAM is
                    // malformed (the following fields would silently overlay the
                    // unsized tail), so fail loud rather than mislay them.
                    if (interner.isIncompleteArray(f)) {
                        if (i + 1 != fields.size()) return std::nullopt;
                        auto const fops = interner.operands(f);
                        if (fops.empty()) return std::nullopt;
                        auto const elem = computeLayout(fops[0], interner, params, dm);
                        if (!elem) return std::nullopt;
                        // A FAM can carry alignas (`alignas(16) int fam[];`): raise
                        // its effective alignment the same way as an ordinary field.
                        auto const ea = effectiveAlign(i, elem->align);
                        if (!ea) return std::nullopt;
                        off = ea->alignUp(off);
                        out.fieldOffsets.push_back(off);
                        out.align = maxAlign(out.align, *ea);
                        out.hasFlexibleArrayMember = true;
                        continue;  // no size contribution
                    }
                    auto const fl = computeLayout(f, interner, params, dm);
                    if (!fl) return std::nullopt;   // out-of-scope field type → fail loud
                    auto const ea = effectiveAlign(i, fl->align);
                    if (!ea) return std::nullopt;
                    off = ea->alignUp(off);
                    out.fieldOffsets.push_back(off);
                    off += fl->size;
                    out.align = maxAlign(out.align, *ea);
                }
                out.size = out.align.alignUp(off);
                return out;
            }
            // ── bit-field packing path (D-CSUBSET-BITFIELD-ABI-EXACT) ──
            // The per-ABI rule is config-SELECTED by switching ONLY on the
            // strategy enum — NEVER on a target/format name (the per-ABI value
            // is resolved upstream from the active object FORMAT). A struct that
            // contains a bit-field but whose strategy is unrealized (`None` = not
            // declared, or a future un-built value) FAILS LOUD here (nullopt →
            // positioned diag) — no silent fallback can bake a wrong placement.
            switch (params.bitFieldStrategy) {
                case BitFieldStrategy::GnuPacked:
                    return layoutStructBitfieldsGnuPacked(
                        id, fields, interner, params, dm, std::move(out));
                case BitFieldStrategy::MsvcStraddle:
                    return layoutStructBitfieldsMsvcStraddle(
                        id, fields, interner, params, dm, std::move(out));
                case BitFieldStrategy::None:
                    return std::nullopt;   // not declared → fail loud
            }
            return std::nullopt;   // unrealized strategy → fail loud
        }
        case TypeKind::Union: {
            auto const fields = interner.operands(id);
            StructLayout out{};
            // D-CSUBSET-COMPOSITE-ALIGNED (TF-C73): SEED with the whole-composite
            // `aligned(N)` request (1 when none), exactly as the Struct arm does —
            // the per-member folds below only ever RAISE it, and the arm's closing
            // `out.align.alignUp(maxSize)` then grows the union's size to match.
            // clang: `union U { char a; int b; } __attribute__((aligned(32)));`
            // is sizeof 32 / _Alignof 32 (MEASURED).
            auto const seed = compositeSeedAlign(interner, id);
            if (!seed) return std::nullopt;   // unrepresentable stored request
            out.align = *seed;
            out.fieldOffsets.assign(fields.size(), 0);  // every variant at offset 0
            // FC8 bitfields: a union bit-field member occupies bits [0, W) of its
            // OWN allocation unit at offset 0 (members are independent). This
            // placement is identical under EVERY realized strategy — a lone
            // member never straddles, never has a type-transition neighbour, and
            // gnu_packed/msvc_straddle agree on a single field's unit — so the
            // arm needs only the fail-loud gate (an unrealized/undeclared
            // strategy → nullopt), not a per-strategy dispatch. Empty scalars ⇒
            // no bitfield ⇒ the unchanged byte path.
            // D-CSUBSET-PACKED: a packed union (`union {…} __attribute__((packed))`)
            // has natural alignment 1 — the members already sit at offset 0, so packed
            // only lowers the union's OWN alignment (and thus how it aligns when
            // embedded). Read once; fed into effectiveAlign's baseline below. A
            // whole-composite `aligned(N)` still RAISES it back (the seed above) —
            // packed and the request are independent, as in the Struct arm.
            bool const packed = interner.isPacked(id);
            bool const anyBitfield = !interner.scalars(id).empty();
            // ★ D-CSUBSET-PACKED-BITFIELD-INTERACTION — the union half of the belt is
            // gone with the struct half. Its stated reason ("the packed baseline is
            // applied ONLY on the non-bit-field path below") went STALE at TF-C97: the
            // `effectiveAlign` lambda below runs for EVERY member, bit-field included,
            // and routes through the one shared `clampedBaselineAlign`, so a packed
            // union's bit-field member gets baseline 1 like every other member. A
            // premise that stops being true does not announce itself — this one was
            // false for a whole cycle while the line it justified kept the feature shut
            // ([[feedback-a-rows-premise-has-a-shelf-life]]).
            //
            // ✔MEASURED, gcc 13.3.0 + clang 18.1.3 (x86_64-linux) AND cl.exe 19.51 +
            // mingw-w64 gcc 13.2.0: `union { unsigned a:3; unsigned b:30; char x; }`
            // is 4/1 under BOTH spellings on BOTH ABIs — the one shape where the two
            // bit-field strategies happen to agree, because a union member never has a
            // neighbour to straddle into.
            if (anyBitfield) {
                if (!bitFieldStrategyRealized(params.bitFieldStrategy))
                    return std::nullopt;
                out.bitFields.assign(fields.size(), BitFieldPlacement{});
            }
            // D-CSUBSET-MEMBER-ALIGNAS: a union member may carry an `alignas`
            // override too (`union { alignas(16) char c; int i; }`). Fold it into
            // the member's natural alignment with MAX semantics — identical to the
            // struct arm; an align-free union leaves `hasAligns` false and this
            // collapses to the unchanged `fl->align` path. (A union places every
            // member at offset 0, so a member alignas only ever RAISES the union's
            // overall alignment — and thus its size, rounded up — never a field's
            // offset.)
            bool const hasAligns = interner.hasExplicitAligns(id);
            // TF-C82: a union defined under `#pragma pack(N)` is capped the same
            // way a struct is. A union places every member at offset 0, so the cap
            // cannot change an offset — it lowers the union's own ALIGNMENT, and
            // therefore the size it rounds up to.
            std::uint32_t const packCap = interner.maxFieldAlign(id);
            auto effectiveAlign =
                [&](std::size_t i, Alignment natural) -> std::optional<Alignment> {
                // D-CSUBSET-PACKED / TF-C82: a packed union drops the per-member
                // baseline to 1 and `#pragma pack(N)` caps it — the ONE shared clamp
                // (TF-C97); a member `alignas` still RAISES it via the MAX-fold.
                auto const clamped = clampedBaselineAlign(packed, packCap, natural);
                if (!clamped) return std::nullopt;   // upstream bug — never silent
                Alignment const baseline = *clamped;
                if (!hasAligns) return baseline;
                std::uint32_t const ovr = interner.explicitFieldAlign(id, i);
                if (ovr == 0) return baseline;   // no override on this member
                auto const a = Alignment::fromBytes(ovr);
                if (!a) return std::nullopt;    // stored non-pow2/>256 = upstream bug
                return maxAlign(baseline, *a);
            };
            // D-CSUBSET-ZERO-WIDTH-BITFIELD-ALIGNMENT. A union member that is a
            // ZERO-WIDTH unnamed bit-field declares no storage and names nothing, so
            // under the MSVC rule it contributes NEITHER alignment NOR size — there is
            // no run of bit-fields for it to terminate in a union (every member sits at
            // offset 0 in its own unit), which makes it the same complete NO-OP the
            // MsvcStraddle struct packer's zero-width arm documents.
            //
            // ✔MEASURED, cl.exe 19.51 (`_Static_assert` on sizeof AND __alignof):
            //   `union {char c; unsigned :0;}`           -> 1/1  (dss was 4/4)
            //   `union {char c; unsigned long long :0;}` -> 1/1  (dss was 8/8)
            // Folding the member in raised BOTH the union's alignment and — through
            // the closing `align.alignUp(maxSize)` — its size, a silent layout
            // miscompile on every PE target.
            //
            // ⚠ STRATEGY-CONDITIONAL BECAUSE THE ANSWER GENUINELY DIVERGES, and this
            // arm's standing comment above ("identical under EVERY realized strategy")
            // is true of a member with STORAGE only. The GNU family does not agree with
            // itself here: the same two unions are 1/1 and 1/1 under SysV-x86_64 and
            // Apple arm64, but 4/4 and 8/8 under AAPCS64 ELF (✔MEASURED, gcc 13.3.0 and
            // clang 18.1.3, both targets, agreeing per target). Picking either answer
            // for `gnu_packed` would be right on one shipped format and a NEW silent
            // miscompile on the other, so the GNU half is deliberately UNTOUCHED here
            // and waits on the per-ABI layout key the row calls for. MSVC is decidable
            // today because cl.exe is the single reference for every PE format and it
            // answers the same way on x64 and arm64.
            // Is a ZERO-WIDTH member inert (contributes neither alignment nor size)?
            // A union places every member at offset 0 in its own unit, so there is no
            // run for it to terminate — under msvc_straddle that makes it a complete
            // no-op, and under gnu_packed the answer is the SAME per-ABI axis the
            // struct packer reads. ✔MEASURED, `union { char c; unsigned :0; }`:
            //     cl.exe 19.51 (PE) ......................... 1/1
            //     x86_64-linux · arm64-apple (ignored) ...... 1/1
            //     aarch64-linux (contributes) ............... 4/4
            // dss computed 4/4 for ALL of them, so it raised the union's alignment AND
            // — through the closing `align.alignUp(maxSize)` — its SIZE.
            bool const msvc = params.bitFieldStrategy == BitFieldStrategy::MsvcStraddle;
            bool zeroWidthMemberIsInert = msvc;
            if (!msvc && anyBitfield) {
                switch (params.unnamedBitFieldAlignment) {
                    case UnnamedBitFieldAlignment::Ignored:
                        zeroWidthMemberIsInert = true;   break;
                    case UnnamedBitFieldAlignment::Contributes:
                        zeroWidthMemberIsInert = false;  break;
                    case UnnamedBitFieldAlignment::None:
                        // Only fatal if a zero-width member is actually present —
                        // decided in the loop, so a bitfield-bearing union WITHOUT one
                        // still lays out. Fail loud, never a silent side.
                        break;
                }
            }
            bool const zeroWidthUndeclared =
                !msvc && anyBitfield
                && params.unnamedBitFieldAlignment == UnnamedBitFieldAlignment::None;
            std::uint64_t maxSize = 0;
            for (std::size_t i = 0; i < fields.size(); ++i) {
                auto const fl = computeLayout(fields[i], interner, params, dm);
                if (!fl) return std::nullopt;
                auto const bw = anyBitfield ? interner.fieldBitWidth(id, i)
                                            : std::optional<std::uint32_t>{};
                bool const isZeroWidth = bw.has_value() && *bw == 0;
                if (isZeroWidth && zeroWidthUndeclared) return std::nullopt;
                if (isZeroWidth && zeroWidthMemberIsInert) continue;
                auto const ea = effectiveAlign(i, fl->align);
                if (!ea) return std::nullopt;
                // D-CSUBSET-ZERO-WIDTH-BITFIELD-ALIGNMENT (the msvc_straddle union
                // rule, measured alongside the zero-width one): under MSVC a bit-field
                // member gives the union its unit's SIZE but NOT its alignment.
                // ✔MEASURED, cl.exe 19.51: `union {char c; unsigned b:1;}` 4/1 ·
                // `union {char c; u64 b:1;}` 8/1 · `union {unsigned b:1;}` 4/1 — while
                // an ORDINARY member still contributes normally, which is what makes
                // `union {int a; unsigned b:1;}` 4/4 and `union {double d; unsigned
                // b:1;}` 8/8. dss answered 4/4 and 8/8 to the first three. The GNU
                // family does NOT share this: every gnu_packed target measures 4/4.
                if (!(msvc && bw.has_value())) {
                    out.align = maxAlign(out.align, *ea);
                }
                if (bw.has_value() && *bw > 0) {
                    if (fl->size == 0) return std::nullopt;
                    out.bitFields[i] = BitFieldPlacement{
                        static_cast<std::uint32_t>(fl->size), 0, *bw};
                }
                maxSize = std::max(maxSize, fl->size);
            }
            out.size = out.align.alignUp(maxSize);
            return out;
        }
        default:
            // Void + out-of-scope kinds (FnSig/Slice/Tuple/Vector/Matrix/Nullable/
            // Optional/Param/Bind/Extension): no C-aggregate layout. Fail loud
            // (nullopt) — NEVER a guessed size.
            return std::nullopt;
    }
}

bool compositeFieldsOverlap(TypeId id, TypeInterner const& interner,
                            AggregateLayoutParams params, DataModel dm) {
    // Only a STRUCT/UNION has a field set at all — a scalar/pointer/array/Complex
    // has nothing to intersect, and callers DO hand those in (the MIR and asm
    // aggregate walks ask of whatever member type they are recursing on). This is
    // the filter `hasExplicitOffsets` used to provide as a side effect of being
    // Struct/Union-only; it is stated directly now that the offsets are no longer
    // the only channel.
    TypeKind const kind = interner.kind(id);   // qualifier-transparent
    if (kind != TypeKind::Struct && kind != TypeKind::Union) return false;

    // D-CORE-COMPOSITE-OVERLAP-CLAIM-BLIND-TO-BITFIELDS: THE TWO CHANNELS in which
    // a composite's members can share bytes, both answerable in O(1) BEFORE any
    // layout is attempted:
    //   * EXPLICIT per-field byte offsets (an FFI overlay a descriptor pins);
    //   * BIT-FIELDS — two bit-fields can land in the same byte of one allocation
    //     unit, which is the case this predicate used to answer `false` for.
    // `scalars()` is the layout engine's OWN "any bit-field?" test (a bit-field-free
    // composite interns with an empty bit-width pool), so the two agree by
    // construction rather than by a second derivation.
    bool const explicitOffsets = interner.hasExplicitOffsets(id);
    bool const anyBitField     = !interner.scalars(id).empty();
    // O(1) short-circuit, SCOPED to exactly the case its justification covers — and
    // that scope is the STRUCT, which is the whole of the justification and used to
    // be more than it could carry. With neither channel present a STRUCT's layout is
    // natural and MONOTONIC (each field is placed at `alignUp(off)` and then advances
    // `off` by its own size), so no two members can intersect. That is a property of
    // the ALGORITHM and not of a particular outcome, which is why answering here
    // WITHOUT attempting a layout does not forfeit the header's "un-computable ⇒
    // `true`" promise: there is no field list for which the struct path could
    // produce an intersection.
    //
    // ★ D-CORE-COMPOSITE-OVERLAP-CLAIM-BLIND-TO-UNIONS: A UNION IS THE ONE
    // COMPOSITE THE THEOREM DOES NOT COVER, AND IT USED TO LAND HERE ANYWAY. The
    // union arm of `computeLayout` does not advance an offset at all — it places
    // EVERY member at 0 and folds a max size — so a union with two or more sizeable
    // members shares byte 0 BY DEFINITION and the monotonicity argument above is
    // simply not about it. The old code answered `false` for such a union, which is
    // the plainest possible wrong answer from the function that calls itself the
    // authority for the question.
    //
    // The fix is by KIND rather than by making the promise literally unconditional,
    // and the choice is deliberate: the struct theorem is SOUND and it is what keeps
    // this O(1) for the overwhelmingly common shape (no layout computed, no ranges
    // built, no sort). Routing structs through the sweep to buy a uniformity the
    // answer does not need would cost every naturally-laid-out struct in the corpus
    // a full `computeLayout` to be told what the algorithm already guarantees. A
    // union falls through to the layout + range sweep below, where its all-zero
    // `fieldOffsets` produce the correct answer with no union-specific arithmetic:
    // two sizeable members intersect at byte 0, a single-member union does not, and
    // a union whose members are all zero-size does not.
    if (kind == TypeKind::Struct && !explicitOffsets && !anyBitField) return false;

    // From here the answer is derived from the LAID-OUT type, and the header's
    // "un-computable ⇒ `true`" promise now actually holds: no arm returns before
    // this layout is attempted.
    auto const lay = computeLayout(id, interner, params, dm);
    if (!lay) return true;                           // un-computable: conservative
    auto const fields = interner.operands(id);
    if (fields.size() != lay->fieldOffsets.size()) return true;   // malformed
    // Collect each member's occupied byte range. A field whose layout is
    // un-computable makes the answer CONSERVATIVELY `true` (see the header): the
    // caller must keep refusing rather than admit an unverified layout.
    struct Range {
        std::uint64_t begin;
        std::uint64_t end;
    };
    std::vector<Range> ranges;
    ranges.reserve(fields.size());
    for (std::size_t i = 0; i < fields.size(); ++i) {
        std::uint64_t const off = lay->fieldOffsets[i];
        // ★ A BIT-FIELD OCCUPIES THE BYTES ITS BITS LAND IN — **NOT** ITS WHOLE
        // ALLOCATION UNIT, and the difference is the difference between a correct
        // answer and a false positive. Under `gnu_packed`, `struct { unsigned a:3;
        // char x; }` places a in a 4-byte unit at 0 and `x` at byte 1 — the next
        // ordinary member is packed INTO the unit's spare bytes (✔MEASURED, gcc
        // 13.3.0 + clang 18.1.3: sizeof 4, `offsetof(x) == 1`) — so sweeping UNIT
        // ranges would report `a` and `x` as sharing bytes when they provably do
        // not, and `examples/c/bitfield_init`'s own `struct T` is that shape.
        // (`msvc_straddle` closes the unit at an ordinary member, so the two
        // derivations happen to agree there; `gnu_packed` is where they part.)
        // The bits of field
        // i run `[bitOffset, bitOffset+bitWidth)` inside the unit at `off`, so the
        // BYTES it occupies are `off + bitOffset/8 .. off + ceil((bitOffset+
        // bitWidth)/8)` — a range the packers' straddle refusal keeps inside the
        // unit. The bit→byte mapping is `BitFieldPlacement`'s own documented LSB-
        // first model (the engine has exactly one), NOT a target test: no format or
        // arch name is read here, and a future ABI that renumbers bits changes the
        // model in one place and this derivation with it.
        bool const isBitField =
            i < lay->bitFields.size() && lay->bitFields[i].unitBytes != 0;
        if (isBitField) {
            BitFieldPlacement const& p = lay->bitFields[i];
            std::uint64_t const bitLo = p.bitOffset;
            std::uint64_t const bitHi = bitLo + p.bitWidth;   // exclusive
            ranges.push_back(Range{off + bitLo / 8u, off + (bitHi + 7u) / 8u});
            continue;
        }
        // A ZERO-WIDTH bit-field (`unsigned : 0;`) is a packing BREAK with no
        // storage: `unitBytes == 0` AND a width is recorded, and its `fieldOffsets`
        // entry deliberately ALIASES the next unit. Charging it its declared type's
        // width would manufacture an overlap out of a marker that occupies nothing —
        // the same skip `asm.cpp`'s initializer packer makes for the same reason.
        if (interner.fieldBitWidth(id, i).has_value()) continue;
        // A FLEXIBLE ARRAY MEMBER contributes NO bytes to the struct (its tail is
        // unsized), which is why the layout arms give it an offset and no size. Its
        // own `computeLayout` is nullopt by design, so it must be skipped BEFORE the
        // un-sizeable-field refusal below or every FAM-bearing bit-field struct
        // would answer a false conservative `true`.
        if (interner.isIncompleteArray(fields[i])) continue;
        auto const fl = computeLayout(fields[i], interner, params, dm);
        if (!fl) return true;                        // un-sizeable field
        if (fl->size == 0) continue;                 // occupies no bytes
        ranges.push_back(Range{off, off + fl->size});
    }
    // Sort by start offset, then a single adjacent-pair sweep: with the ranges
    // ordered, ANY intersection shows up between neighbours (a later range that
    // reaches back into an earlier one must start before that one's end, and the
    // running max-end below carries a long field over the short ones it swallows).
    std::sort(ranges.begin(), ranges.end(),
              [](Range const& a, Range const& b) { return a.begin < b.begin; });
    std::uint64_t reach = 0;   // max `end` seen so far
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        if (i != 0 && ranges[i].begin < reach) return true;
        reach = std::max(reach, ranges[i].end);
    }
    return false;
}

std::optional<StructLayout>
operandLayout(TypeId id, TypeInterner const& interner,
              AggregateLayoutParams params, DataModel dm,
              NonObjectTypeSizes const& sizes) {
    // THE OBJECT QUESTION FIRST, ALWAYS. Anything with a real layout answers from
    // the one engine, byte-identically to every pre-P42 caller — the non-object
    // arm below is reached ONLY where `computeLayout` had nothing to say. Asking in
    // this order is what makes the change additive rather than an override: no
    // declared size can ever displace a layout the engine actually computed.
    if (auto const layout = computeLayout(id, interner, params, dm)) return layout;

    if (!id.valid()) return std::nullopt;
    // The qualifier skin is transparent to `kind()`, so `const void` / `volatile
    // void` reach the Void arm without a second strip (and `computeLayout` above
    // already stripped volatile for its own answer).
    std::optional<std::uint64_t> declared;
    switch (interner.kind(id)) {
        case TypeKind::Void:  declared = sizes.voidBytes;     break;
        case TypeKind::FnSig: declared = sizes.functionBytes; break;
        // EVERY OTHER KIND FALLS THROUGH TO THE REFUSAL, and that is the fail-loud
        // contract: an incomplete composite, an incomplete array, a VLA, a
        // Slice/Tuple/Vector a language has not taught the engine to lay out — all
        // of them keep `computeLayout`'s nullopt and their callers' loud
        // diagnostics. A future objectless lattice kind refuses here until someone
        // teaches it, rather than inheriting a guessed size from a neighbour.
        default: return std::nullopt;
    }
    if (!declared.has_value()) return std::nullopt;   // dialect declared none

    // A ZERO operand size is refused rather than synthesized. It cannot be
    // meaningful: `sizeof` would be 0 where both references say 1, and a stride of
    // 0 makes every element of an arithmetic sequence alias byte offset 0 — the
    // exact silent-miscompile `scaleIndexToBytes` already rejects for an empty
    // aggregate. A schema asking for it gets the loud refusal, never the aliasing.
    if (*declared == 0) return std::nullopt;

    // `{size = declared, align = 1}`. There is no OBJECT of these types, so there is
    // no natural alignment to derive from one; 1 is the alignment a byte-addressed
    // offset needs, and it is what both references report for `_Alignof(void)`.
    // Deliberately NOT `scalarAlign(*declared, params)`: that answers the object
    // question this whole function exists to stop asking, and for a declared size of
    // 1 it happens to agree — an agreement that would quietly stop holding if a
    // dialect ever declared 2.
    return StructLayout{*declared, Alignment{}, {}, false};
}

} // namespace dss
