// ─────────────────────────────────────────────────────────────────────────
// ELF object-format backend.
// D-LINK-OBJECT-FORMAT-SCHEMA-RETAINS-KIND-IDENTITY-BRANCHES (TF-C125).
// ─────────────────────────────────────────────────────────────────────────
//
// This TU is the SANCTIONED REALIZATION TIER. It is allowed to know that it
// is ELF — that is the whole reason the seam exists. What it must never
// do is let the shared substrate know: `object_format_schema.cpp` and
// `object_format_schema_json.cpp` reach this code only through the abstract
// `ObjectFormatBackend`, and they cannot even SPELL `ObjectFormatKind::Elf`
// (both TUs carry a compile-error pin that makes the name ambiguous).
//
// ★ EVERY RULE BODY BELOW WAS MOVED VERBATIM. The `validateIdentity` and
// `readIdentity` bodies are byte-for-byte the blocks that used to sit behind
// `if (kind == ObjectFormatKind::Elf)` in the schema triple — same rules,
// same wording, same JSON pointers. The pointers are load-bearing: 163
// `countAtPath` assertions across `tests/link/` pin diagnostics at exact
// pointers, and this refactor is only correct if every one of them still
// resolves. The rules moved; their pointers did not.
//
// The binding preamble at the top of `validateIdentity` re-establishes the
// unqualified member names the moved code used when it was a member of
// `ObjectFormatData::validate()`. Renaming those references instead would
// have made a 1,000-line move impossible to verify by inspection — and a rule
// that silently starts reading a different field is exactly the class of
// defect this refactor must not introduce.

#include "link/format/object_format_backends.hpp"

#include "core/substrate/diagnostic_collector.hpp"
#include "core/types/config_key_vocabulary.hpp"  // detail::{keysOf,renderAllowedList}
#include "core/types/parse_diagnostic.hpp"
#include "link/format/elf.hpp"
#include "link/object_format_schema.hpp"

#include "link/object_format_identity_doc.hpp"

#include <array>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dss::link::format {
namespace {

char const* const kElfBlocks[] = { "elf" };

// D-LK-WEAK-DEFINITION-DIALECT-UNCONSULTED-BY-ELF-AND-MACHO-WRITERS. The weak-
// definition spellings THIS backend's walker writes. One row: `elf.cpp` spells
// a weak definition as STB_WEAK and nothing else, so an ELF document declaring
// `comdat` or `symbol-flag` is refused at LOAD rather than at emit.
constexpr WeakDefinitionDialect kElfWeakDialects[] = {
    WeakDefinitionDialect::SymbolBinding,
};

// ── The three ELF header vocabularies that have no `EnumNameTable` ────────
// D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET.
//
// `class`, `data` and `osabi` map a config spelling straight onto the psABI's
// numeric field — there is no DSS enum in between, so the accepting if-chain WAS
// the vocabulary and the refusal beside it RETYPED that vocabulary. Same class,
// same failure, and it had already fired:
//
// ✔MEASURED 2026-08-20 by reading the `osabi` if-chain against its own message:
// the chain accepted SEVEN spellings (`sysv`, `none`, `hpux`, `netbsd`, `gnu`,
// `linux`, `freebsd`) and the message advertised SIX — `'linux'` was accepted by
// the check and DENIED by the sentence, telling an author by name that a
// spelling this loader takes is not allowed.
//
// A ROW TABLE, not an if-chain: the spelling and the wire value sit in one row,
// the lookup walks it, and the message is `keysOf` + `renderAllowedList` over
// the same rows — so the two cannot disagree. `ELFOSABI_*` deliberately keeps
// its ALIASES (`sysv`/`none` → 0, `gnu`/`linux` → 3, the psABI's own alternate
// spellings), and they now appear in the message because they are in the table.
struct ElfHeaderVerb {
    std::string_view name;
    std::uint8_t     value;
};

constexpr std::array<ElfHeaderVerb, 2> kElfFileClassRows{{
    {"elf32", 1},   // ELFCLASS32
    {"elf64", 2},   // ELFCLASS64
}};
constexpr std::array<ElfHeaderVerb, 2> kElfDataEncodingRows{{
    {"lsb", 1},     // ELFDATA2LSB
    {"msb", 2},     // ELFDATA2MSB
}};
constexpr std::array<ElfHeaderVerb, 7> kElfOsAbiRows{{
    {"sysv",    0}, {"none",    0},   // ELFOSABI_NONE / SYSV — psABI aliases
    {"hpux",    1},                   // ELFOSABI_HPUX
    {"netbsd",  2},                   // ELFOSABI_NETBSD
    {"gnu",     3}, {"linux",   3},   // ELFOSABI_GNU / LINUX — psABI aliases
    {"freebsd", 9},                   // ELFOSABI_FREEBSD
}};

[[nodiscard]] constexpr std::optional<std::uint8_t>
elfHeaderVerbValue(std::span<ElfHeaderVerb const> rows, std::string_view s) {
    for (auto const& r : rows) {
        if (r.name == s) return r.value;
    }
    return std::nullopt;
}

// The accepted set, rendered. `keysOf` projects the ROW table onto its key
// half — never a second literal list beside it.
template <std::size_t N>
[[nodiscard]] std::string
elfHeaderVerbList(std::array<ElfHeaderVerb, N> const& rows,
                  std::string_view sep = " / ") {
    return ::dss::detail::renderAllowedList(
        ::dss::detail::keysOf(rows, [](ElfHeaderVerb const& r) { return r.name; }),
        sep);
}

// ── `Elf64_Ehdr` geometry the BYTE PROBE reads (gABI Ch. 4, Fig. 4-3) ─────
//
// Offsets only, for the four bytes of magic, the two `e_ident` bytes that
// declare how the rest of the file is spelled, and `e_type`. The probe reads
// nothing else, so nothing else is declared here.
//
// ★ THE OFFSETS ARE THE SAME IN ELF32 AND ELF64 — that is a property of the
// format, not an assumption: `e_ident[16]` is fixed-size and `e_type` is the
// first half-word after it in both classes. The probe can therefore decide
// `e_type` on a file whose class it has not yet accepted, which is what lets
// it reject an ELF32 object by CLASS rather than by mis-reading it.
constexpr std::size_t kElfEiClassOff = 4;   // e_ident[EI_CLASS]
constexpr std::size_t kElfEiDataOff  = 5;   // e_ident[EI_DATA]
constexpr std::size_t kElfETypeOff   = 16;  // e_type (half-word)

// The EI_DATA and e_type wire values, PROJECTED rather than retyped. `lsb`/
// `msb` come from the same `kElfDataEncodingRows` the loader resolves
// `elf.data` through, and ET_REL from `ElfObjectType`, whose enumerators ARE
// the gABI's `ET_*` values (see its declaration in object_format_schema.hpp).
// Spelling `1` and `2` here would make this file a SECOND owner of both
// facts; projecting them means a renamed row breaks the build (the `.value()`
// on a disengaged optional is not a constant expression) instead of silently
// leaving the probe reading the old number.
constexpr std::uint8_t kElfData2Lsb =
    elfHeaderVerbValue(kElfDataEncodingRows, "lsb").value();
constexpr std::uint8_t kElfData2Msb =
    elfHeaderVerbValue(kElfDataEncodingRows, "msb").value();
constexpr std::uint16_t kElfEtRel = static_cast<std::uint16_t>(ElfObjectType::Rel);

// Bounds-checked half-word read in the FILE's own declared byte order.
//
// ★ LOCAL, NOT `ffi/binary_readers/reader_common.hpp`'s `readU16`, for two
// independent reasons and the first one is structural: `ffi` depends UP on
// `link` (`ffi/ingest.hpp`, `ffi/abi/abi_catalog.hpp` and
// `ffi/mangling/c_mangle.hpp` all include `link/object_format_schema.hpp`),
// so a `link` -> `ffi` include closes a dependency CYCLE. That is the same
// call `elf_object_reader.cpp` records at its own `kEhdrSz` block, and
// `coff_object_reader.cpp` at its `kFileHeaderSz` block; this is the third
// site, not a new opinion. The second reason: that helper indexes `b[off]`
// UNCHECKED — correct for a reader that has already validated its buffer,
// wrong for a `noexcept` predicate handed an arbitrary file.
//
// `msb` is a property of the FILE (its own `e_ident[EI_DATA]`), never of the
// host: DSS has a live big-endian s390x leg, so "read it the way the machine
// reads it" is a bug that only shows up on the leg nobody is watching.
[[nodiscard]] constexpr std::optional<std::uint16_t>
elfProbeReadU16(std::span<std::uint8_t const> b, std::size_t off,
                bool msb) noexcept {
    // Written as a SUBTRACTION on the size rather than `off + 2 > b.size()`
    // so it stays correct for an `off` near `SIZE_MAX`, which a future caller
    // could compute even though today's two call sites pass constants.
    if (b.size() < 2u || off > b.size() - 2u) return std::nullopt;
    auto const first  = static_cast<std::uint16_t>(b[off + 0]);
    auto const second = static_cast<std::uint16_t>(b[off + 1]);
    return static_cast<std::uint16_t>(
        msb ? (first << 8) | second : (second << 8) | first);
}

class ElfBackend final : public ObjectFormatBackend {
public:
    [[nodiscard]] std::string_view configName() const noexcept override {
        return "elf";
    }
    [[nodiscard]] ObjectFormatKind kind() const noexcept override {
        return ObjectFormatKind::Elf;
    }
    [[nodiscard]] std::span<char const* const>
    identityBlockNames() const noexcept override { return kElfBlocks; }
    [[nodiscard]] std::span<char const* const>
    rejectedRootFields() const noexcept override { return {}; }
    [[nodiscard]] std::string_view
    rejectedRootFieldsReason() const noexcept override { return {}; }
    [[nodiscard]] std::span<StackReserveVehicle const>
    stackReserveVehicles() const noexcept override {
        // ELF has NO image field carrying a stack SIZE. `PT_GNU_STACK` encodes
        // EXECUTABILITY; the size is a kernel/`ulimit -s` property. Claiming no
        // vehicle is what makes a stack-reserve request against an ELF format
        // fail loud instead of being written into a field nothing reads.
        return {};
    }
    [[nodiscard]] std::span<WeakDefinitionDialect const>
    weakDefinitionDialects() const noexcept override {
        // ELF puts the weakness in the SYMBOL's binding field — STB_WEAK in
        // the high nibble of `st_info`, written by `elf.cpp`'s
        // `stbForBinding`. No section flag and no per-symbol side flag is
        // involved, which is exactly what distinguishes this dialect from the
        // other two. The row is
        // D-LK-WEAK-DEFINITION-DIALECT-UNCONSULTED-BY-ELF-AND-MACHO-WRITERS.
        return kElfWeakDialects;
    }

    [[nodiscard]] bool
    isImageFlavor(detail::ObjectFormatData const& d) const noexcept override {
        return d.elf.objectType != ElfObjectType::Rel;
    }

    // ★★ THE FOUR-MEMBER RE-DERIVATION, KEPT DELIBERATELY.
    //
    // `validate()` pins the c151 D-LK1-4 PIE entry cluster ALL-OR-NONE, so on a
    // LOADED schema any one member is a faithful witness of the other three —
    // and a one-member test would be enough. All four are re-derived anyway,
    // because `ObjectFormatSchema{ObjectFormatData}` is a PUBLIC constructor
    // that runs no validation at all, and on that path there is no cluster rule
    // to lean on. A hand-built struct that sets only `processExit` must not be
    // able to present itself as a PIE executable. This is also why the axis was
    // NOT turned into a declared `"execFlavor": true` key during the TF-C125
    // move: a declared boolean hands the single-field fake straight back.
    [[nodiscard]] bool
    isExecFlavor(detail::ObjectFormatData const& d) const noexcept override {
        bool const elfDynPieShape =
            d.elf.objectType == ElfObjectType::Dyn
         && !d.elf.interpreter.empty()
         && d.processExit.has_value()
         && !d.entryCallingConvention.empty()
         && d.processArgs.has_value();
        return d.elf.objectType == ElfObjectType::Exec || elfDynPieShape;
    }

    // A `.so` may reference symbols the executable (or another loaded object)
    // defines; ld.so resolves them from the global scope at load. A PIE is an
    // EXECUTABLE and must not — `processExit` presence is the PIE
    // discriminator (the cluster rule makes it a faithful single-member
    // witness), never a format-name check. Only reached for image flavors:
    // the relocatable answer is decided by the caller.
    [[nodiscard]] bool allowsUndefinedImports(
            detail::ObjectFormatData const& d) const noexcept override {
        return d.elf.objectType == ElfObjectType::Dyn
            && !d.processExit.has_value();
    }

    [[nodiscard]] bool isRelocatableMember(
            detail::ObjectFormatData const& d) const noexcept override {
        return d.elf.objectType == ElfObjectType::Rel;
    }

    // ★★ THE BYTE PROBE. Three conditions, each DEFINITE:
    //   (1) the file opens with `0x7F 'E' 'L' 'F'` — the gABI magic;
    //   (2) its `e_ident[EI_CLASS]` and `e_ident[EI_DATA]` are the ones THIS
    //       schema declares (`elf.class` / `elf.data`); and
    //   (3) `e_type` is ET_REL.
    //
    // ★ WHY (2) IS PART OF THE ANSWER AND NOT PEDANTRY. The predicate's
    // question is "could I have WRITTEN this", and `elf.cpp`'s header writer
    // emits `fileClass` and `dataEncoding` straight out of the schema — so an
    // ELF32 or big-endian object is a file this format demonstrably did not
    // produce and cannot read back (`elf::readRelocatableObject` refuses both
    // by name). Accepting it here would route it to a reader whose next act
    // is to refuse it, and the operator would get a linker refusal about a
    // file the driver claimed to recognize.
    //
    // ★ AND WHY (3) IS READ THROUGH THE FILE'S OWN EI_DATA. `e_type` is a
    // half-word, so its bytes mean opposite things under the two encodings —
    // ET_REL is `01 00` little-endian and `00 01` big-endian. Reading it in
    // HOST order works on every host DSS is usually built on and silently
    // inverts on the s390x leg. The encoding byte is validated first (only
    // ELFDATA2LSB and ELFDATA2MSB are defined; anything else is not an ELF
    // header this probe will decode), then used as the lens.
    //
    // ⚠ `e_machine` IS DELIBERATELY NOT CHECKED, and the omission is the
    // difference between "is this MY family's object" and "will this link".
    // A wrong-arch ET_REL is still a relocatable object, and handing it to
    // the linker gets the operator the linker's arch-mismatch diagnostic —
    // which names the two machines — instead of the tokenizer's `illegal
    // character 0x7f`. Class and encoding are checked because they decide how
    // the file is SPELLED (a reader cannot even decode the wrong-class
    // header's tables); the machine only decides whether the link succeeds,
    // and that is not this predicate's question.
    //
    // ⓘ FAIL-CLOSED ON A HAND-BUILT SCHEMA falls out with no extra arm: the
    // validation-bypassing `ObjectFormatSchema{ObjectFormatData}` constructor
    // leaves `fileClass`/`dataEncoding` at 0, and no real ELF file carries 0
    // in either byte, so such a schema recognizes nothing.
    [[nodiscard]] bool looksLikeRelocatableObject(
            detail::ObjectFormatData const& d,
            std::span<std::uint8_t const>   bytes) const noexcept override {
        // The magic plus the two identity bytes occupy `[0, 6)`. Everything
        // shorter is answered here, so the reads below are inside the buffer
        // by construction and `elfProbeReadU16` re-checks the one after it.
        if (bytes.size() <= kElfEiDataOff) return false;
        if (bytes[0] != 0x7Fu || bytes[1] != 'E'
         || bytes[2] != 'L'   || bytes[3] != 'F') {
            return false;
        }

        std::uint8_t const eiData = bytes[kElfEiDataOff];
        if (eiData != kElfData2Lsb && eiData != kElfData2Msb) return false;
        if (bytes[kElfEiClassOff] != d.elf.fileClass) return false;
        if (eiData != d.elf.dataEncoding) return false;

        auto const eType =
            elfProbeReadU16(bytes, kElfETypeOff, eiData == kElfData2Msb);
        return eType.has_value() && *eType == kElfEtRel;
    }

    // ELF section headers carry a single name; the two-level (segment,
    // section) naming is Mach-O's alone.
    [[nodiscard]] bool sectionsCarrySegmentNames() const noexcept override {
        return false;
    }

    void readIdentity(ObjectFormatIdentityDoc const&  docWrap,
                      detail::ObjectFormatData&       data,
                      substrate::DiagnosticCollector& coll) const override {
        // Unwrap once; the relocated reader bodies below are verbatim and
        // spell `doc` exactly as they did in the loader.
        nlohmann::json const& doc = docWrap.json();

        // ELF identity block — read only when format kind is Elf.
        if (doc.contains("elf")) {
            auto const& e = doc.at("elf");
            if (!e.is_object()) {
                coll.emit(DiagnosticCode::C_MalformedJson, "/elf",
                          "'elf' must be an object when format.kind == 'elf'");
            } else {
                auto readU16 = [&](char const* field, std::uint16_t& out,
                                   std::int64_t max) {
                    if (!e.contains(field) || !e.at(field).is_number_integer())
                        return;
                    std::int64_t const v = e.at(field).get<std::int64_t>();
                    if (v < 0 || v > max) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/elf/{}", field),
                                  std::format("'{}' ({}) out of range [0, {}]",
                                              field, v, max));
                        return;
                    }
                    out = static_cast<std::uint16_t>(v);
                };
                // class: "elf32" | "elf64" (ELFCLASS values from psABI).
                if (e.contains("class") && e.at("class").is_string()) {
                    auto const c = e.at("class").get<std::string>();
                    if (auto const v = elfHeaderVerbValue(kElfFileClassRows, c)) {
                        data.elf.fileClass = *v;
                    } else {
                        coll.emit(DiagnosticCode::C_MalformedJson, "/elf/class",
                                  std::format("'class' must be {}",
                                              elfHeaderVerbList(
                                                  kElfFileClassRows, " or ")));
                    }
                }
                // data: "lsb" | "msb".
                if (e.contains("data") && e.at("data").is_string()) {
                    auto const d = e.at("data").get<std::string>();
                    if (auto const v =
                            elfHeaderVerbValue(kElfDataEncodingRows, d)) {
                        data.elf.dataEncoding = *v;
                    } else {
                        coll.emit(DiagnosticCode::C_MalformedJson, "/elf/data",
                                  std::format("'data' must be {}",
                                              elfHeaderVerbList(
                                                  kElfDataEncodingRows, " or ")));
                    }
                }
                // osabi: string name → numeric (ELFOSABI_*). Default 0 = SysV.
                if (e.contains("osabi") && e.at("osabi").is_string()) {
                    auto const o = e.at("osabi").get<std::string>();
                    if (auto const v = elfHeaderVerbValue(kElfOsAbiRows, o)) {
                        data.elf.osabi = *v;
                    } else {
                        coll.emit(DiagnosticCode::C_MalformedJson, "/elf/osabi",
                                  std::format("'osabi' must be one of {}",
                                              elfHeaderVerbList(kElfOsAbiRows)));
                    }
                }
                std::uint16_t abiVerRaw = 0;
                readU16("abiVersion", abiVerRaw, 255);
                data.elf.abiVersion = static_cast<std::uint8_t>(abiVerRaw);
                readU16("machine", data.elf.machine, 0xFFFF);
                // `type`: closed-enum `ElfObjectType` (rel/exec/dyn)
                // round-tripped through `EnumNameTable`. Default Rel
                // keeps LK1 cycle 1 schemas working unchanged.
                if (e.contains("type") && e.at("type").is_string()) {
                    auto const tName = e.at("type").get<std::string>();
                    auto const tEnum = elfObjectTypeFromName(tName);
                    if (tEnum.has_value()) {
                        data.elf.objectType = *tEnum;
                    } else {
                        coll.emit(DiagnosticCode::C_MalformedJson, "/elf/type",
                                  std::format("'type' must be {}",
                                              ::dss::detail::renderAllowedList(
                                                  allNames(kElfObjectTypeTable),
                                                  " / ")));
                    }
                }
                // `interpreter`: PT_INTERP path (dynamic linker name).
                // Optional in JSON. An empty-string literal (`""`) is
                // rejected at load: the Linux kernel rejects ELFs with a
                // zero-length PT_INTERP path, so `""` is unambiguously a
                // config error (3-agent convergence: code-reviewer +
                // silent-failure + comment-analyzer on LK6 cycle 2b.1
                // review). Absent field = field stays at its default
                // `""` and the walker treats it as "self-contained
                // executable" (no PT_INTERP emission).
                if (e.contains("interpreter")) {
                    if (!e.at("interpreter").is_string()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/elf/interpreter",
                                  "'interpreter' must be a string (e.g. "
                                  "'/lib64/ld-linux-x86-64.so.2')");
                    } else {
                        auto const value =
                            e.at("interpreter").get<std::string>();
                        if (value.empty()) {
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      "/elf/interpreter",
                                      "'interpreter' must not be empty — "
                                      "the Linux kernel rejects ELFs with "
                                      "a zero-length PT_INTERP path. Omit "
                                      "the field entirely for self-"
                                      "contained executables.");
                        } else {
                            data.elf.interpreter = value;
                        }
                    }
                }
                // `soname`: DT_SONAME for an ET_DYN shared library
                // (c150, D-LK1-4). Optional; absent = no DT_SONAME
                // emitted (the `gcc -shared` no-`-soname` shape). An
                // empty-string literal is rejected like `interpreter`:
                // a zero-length DT_SONAME is unambiguously a config
                // error (omit the field instead). validate() rejects
                // the field on non-dyn schemas.
                if (e.contains("soname")) {
                    if (!e.at("soname").is_string()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/elf/soname",
                                  "'soname' must be a string (e.g. "
                                  "'libfoo.so.1')");
                    } else {
                        auto const value = e.at("soname").get<std::string>();
                        if (value.empty()) {
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      "/elf/soname",
                                      "'soname' must not be empty -- omit "
                                      "the field entirely to emit no "
                                      "DT_SONAME.");
                        } else {
                            data.elf.soname = value;
                        }
                    }
                }
                // `pageAlign`: PT_LOAD p_align for Exec images. Required
                // for ET_EXEC at validate() — the kernel rejects ELF
                // exec'd images whose p_align is smaller than the
                // runtime page size. Each (arch × OS) schema declares
                // its own value (D-LK6-3).
                if (e.contains("pageAlign")) {
                    if (!e.at("pageAlign").is_number_integer()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/elf/pageAlign",
                                  "'pageAlign' must be an integer (PT_LOAD "
                                  "p_align, e.g. 4096 for x86_64 Linux or "
                                  "65536 for ARM64-64K)");
                    } else {
                        std::int64_t const pa =
                            e.at("pageAlign").get<std::int64_t>();
                        if (pa <= 0
                         || (static_cast<std::uint64_t>(pa) &
                             (static_cast<std::uint64_t>(pa) - 1u)) != 0u) {
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      "/elf/pageAlign",
                                      "'pageAlign' must be a positive "
                                      "power of two (kernel constraint: "
                                      "p_vaddr % p_align == p_offset % "
                                      "p_align)");
                        } else {
                            data.elf.pageAlign =
                                static_cast<std::uint64_t>(pa);
                        }
                    }
                }
                // `bindNow`: eager vs lazy dynamic-binding choice.
                // Optional; defaults to `true` (v1 stance, plan 14 §5
                // risk row). `false` is the lazy-binding upgrade path
                // anchored at D-LK6-11 — v1 walker fails loud on
                // `bindNow == false` until D-LK6-11 lands.
                if (e.contains("bindNow")) {
                    if (!e.at("bindNow").is_boolean()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/elf/bindNow",
                                  "'bindNow' must be a boolean (true = "
                                  "eager / DF_1_NOW, false = lazy / "
                                  ".rela.plt + JUMP_SLOT — anchored at "
                                  "D-LK6-11, not yet implemented)");
                    } else {
                        data.elf.bindNow =
                            e.at("bindNow").get<bool>();
                    }
                }
            }
        }
    }

    void validateIdentity(detail::ObjectFormatData const& d,
                          SchemaProblemSink const&        fail) const override {
        // Binding preamble — see the file header. These re-establish the
        // unqualified names the moved body used inside
        // `ObjectFormatData::validate()`, so the body below is verbatim.
        auto const& elf                    = d.elf;
        auto const& sections               = d.sections;
        auto const& entryPoint             = d.entryPoint;
        auto const& processExit            = d.processExit;
        auto const& entryCallingConvention = d.entryCallingConvention;
        auto const& processArgs            = d.processArgs;

    // ELF identity: when format kind is Elf, the identity block must
    // be populated. `fileClass=0` means "no class declared" which
    // would emit an invalid ELF header byte.
        {
        if (elf.fileClass == 0) {
            fail("/elf/class", "ELF format requires 'elf.class' "
                               "(one of 'elf32' / 'elf64')");
        }
        if (elf.dataEncoding == 0) {
            fail("/elf/data", "ELF format requires 'elf.data' "
                              "(one of 'lsb' / 'msb')");
        }
        if (elf.machine == 0) {
            fail("/elf/machine", "ELF format requires 'elf.machine' "
                                 "(EM_* value, e.g. 62 for x86_64, "
                                 "183 for aarch64)");
        }
        // e_type — all three closed-enum members now have walker
        // arms: ET_REL (LK1 cycle 1), ET_EXEC (LK1 cycle 2 + the
        // LK6 dynamic arm), ET_DYN (c150 — the D-LK1-4 shared-
        // library half; the entry-less `.so` shape enforced below).
        // ET_EXEC schemas must declare which sections are loaded and
        // at what virtual address. Today the walker uses sh_addr =
        // section.virtualAddress directly (no relocation of
        // virtualAddress). When `virtualAddress == 0` for SectionKind::
        // Text on an ET_EXEC schema, the walker would emit an
        // executable loaded at virtual address 0 — null-deref on
        // first instruction. Reject explicitly.
        if (elf.objectType == ElfObjectType::Exec) {
            auto const* secText = [&]() -> ObjectFormatSectionInfo const* {
                for (auto const& s : sections) {
                    if (s.kind == SectionKind::Text) return &s;
                }
                return nullptr;
            }();
            if (secText != nullptr && secText->virtualAddress == 0) {
                fail("/sections/<text>/virtualAddress",
                     "ELF ET_EXEC format requires `virtualAddress != 0` "
                     "on the .text section row — the walker uses this "
                     "as sh_addr and as the base for e_entry. Loading "
                     ".text at virtual address 0 would null-deref on "
                     "the first instruction. Typical Linux x86_64 base "
                     "is 0x400000 (per linker convention).");
            }
            // PT_LOAD `p_align` — the Linux kernel rejects ELF
            // executables whose `p_align` is smaller than the
            // runtime page size (`ENOEXEC` at exec time, silent
            // from the toolchain's POV). x86_64 Linux uses 4 KB;
            // ARM64 Linux on Apple Silicon / certain Graviton
            // kernels uses 16 KB or 64 KB. Each (arch × OS) ELF
            // exec schema declares its own value — D-LK6-3.
            if (elf.pageAlign == 0) {
                fail("/elf/pageAlign",
                     "ELF ET_EXEC format must declare 'elf.pageAlign' "
                     "(PT_LOAD p_align). The kernel rejects exec'd "
                     "images whose p_align is smaller than the "
                     "runtime page size. Common values: 4096 "
                     "(x86_64 Linux, ARM64 4K pages), 16384 (Apple "
                     "Silicon Asahi 16K), 65536 (ARM64 64K pages on "
                     "some Graviton / embedded kernels). Declaration "
                     "is mandatory per (arch × OS) — anchored at plan "
                     "14 §3.1 D-LK6-3.");
            }
            // `interpreter` (PT_INTERP path) is OPTIONAL at schema
            // load time. The JSON loader rejects an empty-string
            // literal (`""`) at load (zero-length PT_INTERP paths
            // are kernel-rejected at execve()), so we don't repeat
            // the rule here — absent and populated are the only two
            // observable states by the time validate() runs.
            // The walker (LK6 cycle 2b — D-LK6-4) enforces non-empty
            // when externImports is non-empty.
        }
        // ── ET_DYN shape rules — c150 (shared-library half) + c151
        // (PIE half), D-LK1-4. An ELF ET_DYN schema describes ONE of
        // exactly TWO artifact shapes, discriminated by ENTRY-CLUSTER
        // presence (never a new e_type — a PIE IS ET_DYN):
        //   * a SHARED LIBRARY (`.so`): loaded by an already-running
        //     ld.so; no process entry — NONE of the entry cluster.
        //   * a POSITION-INDEPENDENT EXECUTABLE (PIE): execve'd
        //     directly; the kernel maps ld.so via PT_INTERP, ld.so
        //     relocates the image at a randomized base and jumps to
        //     base + e_entry — ALL of the entry cluster (the modern
        //     gcc-default executable shape: ET_DYN + PT_PHDR +
        //     PT_INTERP + non-zero e_entry + DF_1_PIE).
        // Both are base-0, loader-slid images: the base-0 layout
        // rules below (pageAlign + text VA == pageAlign) apply to
        // both shapes uniformly.
        if (elf.objectType == ElfObjectType::Dyn) {
            if (elf.pageAlign == 0) {
                fail("/elf/pageAlign",
                     "ELF ET_DYN format must declare 'elf.pageAlign' "
                     "(PT_LOAD p_align) -- same kernel/loader page-"
                     "congruence contract as ET_EXEC (D-LK6-3).");
            }
            auto const* secText = [&]() -> ObjectFormatSectionInfo const* {
                for (auto const& s : sections) {
                    if (s.kind == SectionKind::Text) return &s;
                }
                return nullptr;
            }();
            if (secText == nullptr) {
                fail("/sections",
                     "ELF ET_DYN format requires a Text section row "
                     "(SectionKind::Text) -- a shared library without "
                     "code has nothing to export.");
            } else if (elf.pageAlign != 0
                       && secText->virtualAddress != elf.pageAlign) {
                // ET_DYN VAs are BASE-RELATIVE (the loader slides the
                // whole image). The walker computes baseImageVa =
                // text.virtualAddress - pageAlign; requiring equality
                // pins baseImageVa to 0 BY CONSTRUCTION — the gcc
                // `.so` convention (first page holds Ehdr + PHT +
                // dynamic metadata, `.text` opens the second page).
                // Any other value would bake a nonzero base offset
                // into every "base-relative" VA for no benefit.
                fail("/sections/<text>/virtualAddress",
                     std::format("ELF ET_DYN format requires the .text "
                                 "row's 'virtualAddress' to equal "
                                 "'elf.pageAlign' (got {:#x}, pageAlign "
                                 "{:#x}) -- ET_DYN images are base-0 "
                                 "(loader-slid); .text sits one page in "
                                 "so headers + dynamic metadata fill "
                                 "page zero. D-LK1-4.",
                                 secText->virtualAddress, elf.pageAlign));
            }
            // The ENTRY CLUSTER (c151, D-LK1-4 PIE half). Exactly
            // FOUR fields form it:
            //   1. `elf.interpreter`        (the PT_INTERP path)
            //   2. `processExit`            (termination mechanism)
            //   3. `entryCallingConvention` (paired with processExit
            //      by the generic §2.13 rule below)
            //   4. `processArgs`            (argc/argv materialization)
            // ALL FOUR present = a PIE; NONE = a `.so`. A half-
            // configured state is a schema bug that would emit a
            // broken image either way (an interpreter with no
            // trampoline leaves e_entry = 0 — the kernel jumps to the
            // load base and executes header bytes; a trampoline with
            // no interpreter emits no PT_INTERP — execve gets no
            // loader to resolve the trampoline's libc `exit` import),
            // so it rejects loud naming exactly which members are
            // missing. `entryPoint` is NOT a cluster member: empty
            // means "functions[0]" on every exec-flavored schema
            // (the shipped exec JSONs all leave it empty), so its
            // presence cannot discriminate — it is instead rejected
            // on the `.so` shape below.
            bool const hasInterp = !elf.interpreter.empty();
            bool const hasExit   = processExit.has_value();
            bool const hasCc     = !entryCallingConvention.empty();
            bool const hasArgs   = processArgs.has_value();
            int const clusterCount = static_cast<int>(hasInterp)
                                   + static_cast<int>(hasExit)
                                   + static_cast<int>(hasCc)
                                   + static_cast<int>(hasArgs);
            bool const isPieShape = clusterCount == 4;
            if (clusterCount != 0 && !isPieShape) {
                fail("/elf",
                     std::format(
                         "ELF ET_DYN format declares a PARTIAL entry "
                         "cluster -- elf.interpreter: {}, processExit: "
                         "{}, entryCallingConvention: {}, processArgs: "
                         "{}. An ET_DYN schema is EITHER a shared "
                         "library (NONE of the four) or a PIE "
                         "executable (ALL FOUR: ET_DYN + PT_INTERP + "
                         "entry trampoline, the gcc-default shape). A "
                         "half-configured state would emit a broken "
                         "image (no-trampoline: e_entry = 0 executes "
                         "header bytes; no-interpreter: no loader to "
                         "resolve the trampoline's libc exit import). "
                         "D-LK1-4.",
                         hasInterp ? "present" : "MISSING",
                         hasExit   ? "present" : "MISSING",
                         hasCc     ? "present" : "MISSING",
                         hasArgs   ? "present" : "MISSING"));
            }
            if (!entryPoint.empty() && !isPieShape) {
                fail("/entryPoint",
                     std::format("ELF ET_DYN format must not declare "
                                 "'entryPoint' (got '{}') without the "
                                 "full PIE entry cluster -- a shared "
                                 "library has no process entry "
                                 "(e_entry = 0). On a PIE schema "
                                 "(interpreter + processExit + "
                                 "entryCallingConvention + processArgs "
                                 "all present) a non-empty entryPoint "
                                 "is legal and names the user entry "
                                 "function. D-LK1-4.",
                                 entryPoint));
            }
            // A PIE is NOT a library: nothing ever links against it,
            // so DT_SONAME (read only by DT_NEEDED lookups) is
            // legal-but-meaningless ELF there. Rejecting keeps
            // configs honest — a soname on a PIE schema is a
            // copy-paste from the `.so` sibling.
            if (isPieShape && !elf.soname.empty()) {
                fail("/elf/soname",
                     std::format("ELF ET_DYN PIE format (full entry "
                                 "cluster) must not declare "
                                 "'elf.soname' (got '{}') -- DT_SONAME "
                                 "names a shared library for DT_NEEDED "
                                 "lookups; nothing links against a "
                                 "PIE, so the field is dead config "
                                 "copy-pasted from the .so sibling. "
                                 "D-LK1-4.",
                                 elf.soname));
            }
            // (There USED to be a rule here rejecting
            // `dataImportBinding: "copy-relocation"` on an ET_DYN
            // schema — copy relocations were exec-only, so a `.so`
            // declaring one was invalid ELF. That rule is GONE with its
            // SUBJECT: the enum value was deleted outright
            // (D-LK-ELF-COPY-RELOC-CLAIMS-ONE-NAME-OF-AN-ALIAS-SET), so
            // no schema of ANY flavour can spell it and the closed-enum
            // reject in the loader refuses the file first. A property
            // whose subject no longer exists asserts nothing; keeping it
            // would just be an unreachable branch pretending to guard.)
        }
        // `soname` is an ET_DYN-only field (DT_SONAME names a shared
        // library; a `.o` / executable carrying one is dead config —
        // a copy-paste error the bindNow/interpreter rules' shape
        // already polices for their fields).
        if (elf.objectType != ElfObjectType::Dyn && !elf.soname.empty()) {
            fail("/elf/soname",
                 std::format("ELF '{}' format must not declare "
                             "'elf.soname' (got '{}') -- DT_SONAME is "
                             "meaningful only on ET_DYN shared "
                             "libraries.",
                             std::string{elfObjectTypeName(elf.objectType)},
                             elf.soname));
        }
        // ET_REL must NOT carry an interpreter path — `.interp` /
        // PT_INTERP are exec-image concepts and have no role in
        // relocatable objects. A non-empty `interpreter` on a
        // .o-shaped schema is a copy-paste error from an exec
        // schema (type-design symmetry with the virtualAddress=0
        // rule below; LK6 cycle 2b.1 review type-design Concern #2
        // convergence).
        if (elf.objectType == ElfObjectType::Rel
         && !elf.interpreter.empty()) {
            fail("/elf/interpreter",
                 std::format("ELF ET_REL format must not declare "
                             "'elf.interpreter' (got '{}'). The "
                             "PT_INTERP path is an exec-image "
                             "concept; .o files leave it empty.",
                             elf.interpreter));
        }
        // Symmetric reject: ET_REL must NOT set `bindNow` (defaults
        // to true; a JSON typo setting it to false on a .o is a
        // copy-paste error from an exec schema). Eager-vs-lazy
        // binding is an exec-image concept; .o files don't bind at
        // all (the linker resolves at exec build time). Without
        // this rule, a `.o` schema with `bindNow=false` would
        // silently load, the field would be ignored at MH_OBJECT
        // walker time, and the typo would mask itself until the
        // schema was reused as an exec template. (Type-design
        // HIGH, LK6 cycle 2c post-fold review.)
        if (elf.objectType == ElfObjectType::Rel && !elf.bindNow) {
            fail("/elf/bindNow",
                 "ELF ET_REL format must not set 'elf.bindNow' to "
                 "false. Eager-vs-lazy binding (DF_1_NOW / "
                 "R_X86_64_GLOB_DAT) is an exec-image concept; "
                 ".o files do not bind at all — the linker resolves "
                 "at exec build time.");
        }
        // Conversely, ET_REL must NOT carry virtual addresses (they're
        // set by the LINKER at exec build time, not declared on the
        // .o's section rows). A non-zero `virtualAddress` here would
        // be silently dropped when emitting `sh_addr = 0` for the
        // .o. Reject so a JSON edit can't no-op.
        if (elf.objectType == ElfObjectType::Rel) {
            for (std::size_t i = 0; i < sections.size(); ++i) {
                if (sections[i].virtualAddress != 0) {
                    fail(std::format("/sections/{}/virtualAddress", i),
                         std::format("section '{}': 'virtualAddress' "
                                     "must be 0 for ELF ET_REL format "
                                     "rows (sh_addr in relocatable .o "
                                     "is unbound; the linker assigns "
                                     "addresses at exec build time)",
                                     sections[i].name));
                }
            }
        }
        }
    }

    [[nodiscard]] std::vector<std::uint8_t>
    encode(AssembledModule const&    module,
           TargetSchema const&       targetSchema,
           ObjectFormatSchema const& objectFormatSchema,
           DiagnosticReporter&       reporter,
           ImageRequest const&       request) const override {
        // `request` is unused here BY CONSTRUCTION, not by omission:
        // `stackReserveVehicles()` above is empty, so the load-time coherence
        // rule rejects any ELF schema declaring a vehicle and the linker's
        // pre-walker gate refuses any request against a format that declares
        // no capability. An ELF walker can only ever be handed an empty one.
        (void)request;
        return elf::encode(module, targetSchema, objectFormatSchema, reporter);
    }
};

} // namespace

// ★ FUNCTION-LOCAL STATIC, NOT A NAMESPACE-SCOPE ONE, and the difference is
// load-bearing. `objectFormatBackendTable()` takes this object's ADDRESS from
// another TU. A namespace-scope static would make that a static-INITIALIZATION-
// ORDER dependency across translation units: the address is valid before
// construction, but the VTABLE POINTER is not, so a table built during static
// init would hold an object whose virtual calls are undefined. The magic-static
// below is constructed on first call, in order, always. (Unreachable today —
// nothing calls the table during static init — which is exactly why it would
// have been found the hard way.)
ObjectFormatBackend const& elfBackend() noexcept {
    static ElfBackend const instance{};
    return instance;
}

} // namespace dss::link::format
