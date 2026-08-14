// ─────────────────────────────────────────────────────────────────────────
// Mach-O object-format backend.
// D-LINK-OBJECT-FORMAT-SCHEMA-RETAINS-KIND-IDENTITY-BRANCHES (TF-C125).
// ─────────────────────────────────────────────────────────────────────────
//
// This TU is the SANCTIONED REALIZATION TIER. It is allowed to know that it
// is Mach-O — that is the whole reason the seam exists. What it must never
// do is let the shared substrate know: `object_format_schema.cpp` and
// `object_format_schema_json.cpp` reach this code only through the abstract
// `ObjectFormatBackend`, and they cannot even SPELL `ObjectFormatKind::MachO`
// (both TUs carry a compile-error pin that makes the name ambiguous).
//
// ★ EVERY RULE BODY BELOW WAS MOVED VERBATIM. The `validateIdentity` and
// `readIdentity` bodies are byte-for-byte the blocks that used to sit behind
// `if (kind == ObjectFormatKind::MachO)` in the schema triple — same rules,
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
#include "core/types/parse_diagnostic.hpp"
#include "link/format/macho.hpp"
#include "link/object_format_schema.hpp"

#include "link/object_format_identity_doc.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dss::link::format {
namespace {

char const* const kMachOBlocks[] = { "macho", "image" };

class MachOBackend final : public ObjectFormatBackend {
public:
    [[nodiscard]] std::string_view configName() const noexcept override {
        return "macho";
    }
    [[nodiscard]] ObjectFormatKind kind() const noexcept override {
        return ObjectFormatKind::MachO;
    }
    [[nodiscard]] std::span<char const* const>
    identityBlockNames() const noexcept override { return kMachOBlocks; }
    [[nodiscard]] std::span<char const* const>
    rejectedRootFields() const noexcept override { return {}; }
    [[nodiscard]] std::string_view
    rejectedRootFieldsReason() const noexcept override { return {}; }
    [[nodiscard]] std::span<StackReserveVehicle const>
    stackReserveVehicles() const noexcept override {
        // `LC_MAIN.stacksize` is a genuine equivalent of PE's
        // SizeOfStackReserve, and it is the natural second vehicle row — but
        // it lands WITH its walker arm, never before it, or a Mach-O schema
        // could declare a reserve that is silently dropped at emit.
        return {};
    }

    [[nodiscard]] bool
    isImageFlavor(detail::ObjectFormatData const& d) const noexcept override {
        return d.macho.filetype != MachOObjectType::Object;
    }
    [[nodiscard]] bool
    isExecFlavor(detail::ObjectFormatData const& d) const noexcept override {
        return d.macho.filetype == MachOObjectType::Execute;
    }

    // c153 (D-LK3-3): MH_DYLIB is FALSE. The shipped bind model is the modern
    // TWO-LEVEL namespace (MH_TWOLEVEL — every LC_DYLD_INFO bind opcode names
    // a SPECIFIC dylib ordinal), so unlike ELF's flat global scope there is
    // nothing that resolves a library-less undefined at load. macOS does have
    // a flat-namespace opt-in, but that is a DIFFERENT bind model the eager
    // two-level machinery deliberately does not ship.
    [[nodiscard]] bool allowsUndefinedImports(
            detail::ObjectFormatData const&) const noexcept override {
        return false;
    }

    [[nodiscard]] bool isRelocatableMember(
            detail::ObjectFormatData const& d) const noexcept override {
        return d.macho.filetype == MachOObjectType::Object;
    }

    // ★ THE ONE BACKEND THAT ANSWERS TRUE. Mach-O is the only shipped format
    // using two-level (segment, section) naming — `__TEXT`/`__text`. The rule
    // this feeds used to read `if (kind == Elf || kind == Pe)` and reject a
    // non-empty segment; it now reads "if this backend does not carry segment
    // names", which is the same rule stated as a capability, and it answers
    // for a format nobody had thought of yet instead of omitting it.
    [[nodiscard]] bool sectionsCarrySegmentNames() const noexcept override {
        return true;
    }

    void readIdentity(ObjectFormatIdentityDoc const&  docWrap,
                      detail::ObjectFormatData&       data,
                      substrate::DiagnosticCollector& coll) const override {
        // Unwrap once; the relocated reader bodies below are verbatim and
        // spell `doc` exactly as they did in the loader.
        nlohmann::json const& doc = docWrap.json();

        // Mach-O identity block — read only when format kind is MachO.
        if (doc.contains("macho")) {
            auto const& m = doc.at("macho");
            if (!m.is_object()) {
                coll.emit(DiagnosticCode::C_MalformedJson, "/macho",
                          "'macho' must be an object when format.kind == 'macho'");
            } else {
                auto readU32 = [&](char const* field, std::uint32_t& out) {
                    if (!m.contains(field) || !m.at(field).is_number_integer())
                        return;
                    std::int64_t const v = m.at(field).get<std::int64_t>();
                    if (v < 0
                     || v > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/macho/{}", field),
                                  std::format("'{}' ({}) out of range [0, 2^32)",
                                              field, v));
                        return;
                    }
                    out = static_cast<std::uint32_t>(v);
                };
                readU32("cputype",    data.macho.cputype);
                readU32("cpusubtype", data.macho.cpusubtype);
                readU32("flags",      data.macho.flags);
                // `filetype`: closed enum MachOObjectType. Accepts the
                // string form ("object"/"execute"/"dylib") OR the
                // integer wire value (1/2/6) for back-compat with
                // pre-enum shipped JSONs. Unknown values fail loud.
                if (m.contains("filetype")) {
                    auto const& ft = m.at("filetype");
                    if (ft.is_string()) {
                        auto const tEnum = machoObjectTypeFromName(
                            ft.get<std::string>());
                        if (tEnum.has_value()) {
                            data.macho.filetype = *tEnum;
                        } else {
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      "/macho/filetype",
                                      "'filetype' must be 'object' / "
                                      "'execute' / 'dylib'");
                        }
                    } else if (ft.is_number_integer()) {
                        std::int64_t const v = ft.get<std::int64_t>();
                        if (v == 1) data.macho.filetype = MachOObjectType::Object;
                        else if (v == 2) data.macho.filetype = MachOObjectType::Execute;
                        else if (v == 6) data.macho.filetype = MachOObjectType::Dylib;
                        else {
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      "/macho/filetype",
                                      std::format("'filetype' integer {} "
                                                  "not in {{1,2,6}}", v));
                        }
                    } else {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/macho/filetype",
                                  "'filetype' must be a string or integer");
                    }
                }
            }
        }

        // Mach-O image block — read only when format kind is MachO and
        // an `image` key is present. Validate() will reject the key on
        // a MH_OBJECT schema, and require its full population for
        // MH_EXECUTE (symmetric with PE's optionalHeader gate).
        if (doc.contains("image")) {
            auto const& im = doc.at("image");
            if (!im.is_object()) {
                coll.emit(DiagnosticCode::C_MalformedJson, "/image",
                          "'image' must be an object");
            } else {
                if (im.contains("pageZeroSize")) {
                    if (!im.at("pageZeroSize").is_number_integer()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/image/pageZeroSize",
                                  "'pageZeroSize' must be an integer");
                    } else {
                        std::int64_t const v =
                            im.at("pageZeroSize").get<std::int64_t>();
                        if (v < 0) {
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      "/image/pageZeroSize",
                                      "'pageZeroSize' must be non-negative");
                        } else {
                            data.machoImage.pageZeroSize =
                                static_cast<std::uint64_t>(v);
                        }
                    }
                }
                // VM segment page size (LC_SEGMENT_64 vmaddr/vmsize/fileoff
                // alignment). Optional; default 4 KiB. Power-of-two enforced
                // at validate(). Apple Silicon arm64-darwin sets 16384.
                if (im.contains("segmentPageSize")) {
                    if (!im.at("segmentPageSize").is_number_integer()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/image/segmentPageSize",
                                  "'segmentPageSize' must be an integer");
                    } else {
                        std::int64_t const v =
                            im.at("segmentPageSize").get<std::int64_t>();
                        if (v <= 0) {
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      "/image/segmentPageSize",
                                      "'segmentPageSize' must be positive "
                                      "(a power-of-two VM page size; 4096 for "
                                      "x86_64-darwin, 16384 for arm64-darwin)");
                        } else {
                            data.machoImage.segmentPageSize =
                                static_cast<std::uint64_t>(v);
                        }
                    }
                }
                if (im.contains("dylinkerPath")) {
                    if (!im.at("dylinkerPath").is_string()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/image/dylinkerPath",
                                  "'dylinkerPath' must be a string");
                    } else {
                        data.machoImage.dylinkerPath =
                            im.at("dylinkerPath").get<std::string>();
                    }
                }
                // D-LK3-3 (c153) — the MH_DYLIB LC_ID_DYLIB install name.
                // Optional at parse; validate() requires it non-empty on a
                // Dylib schema and rejects it on every other filetype.
                if (im.contains("installName")) {
                    if (!im.at("installName").is_string()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/image/installName",
                                  "'installName' must be a string (the "
                                  "LC_ID_DYLIB name a client links "
                                  "against, e.g. '@rpath/libdss.dylib' -- "
                                  "MH_DYLIB only)");
                    } else {
                        data.machoImage.installName =
                            im.at("installName").get<std::string>();
                    }
                }
                if (im.contains("loadDylibs")) {
                    if (!im.at("loadDylibs").is_array()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/image/loadDylibs",
                                  "'loadDylibs' must be an array — each "
                                  "entry is either a bare string (path "
                                  "sugar) or an object {path: ...}");
                    } else {
                        auto const& arr = im.at("loadDylibs");
                        for (std::size_t i = 0; i < arr.size(); ++i) {
                            if (arr[i].is_string()) {
                                data.machoImage.loadDylibs.push_back(
                                    MachODylibRef{arr[i].get<std::string>()});
                            } else if (arr[i].is_object()
                                    && arr[i].contains("path")
                                    && arr[i].at("path").is_string()) {
                                data.machoImage.loadDylibs.push_back(
                                    MachODylibRef{arr[i].at("path")
                                                      .get<std::string>()});
                            } else {
                                coll.emit(DiagnosticCode::C_MalformedJson,
                                          std::format("/image/loadDylibs/{}", i),
                                          "each loadDylibs entry must be a "
                                          "string or an object with 'path'");
                            }
                        }
                    }
                }
                // `bindNow`: eager vs lazy dynamic-binding choice on
                // Mach-O — parallel to `elf.bindNow`. Optional; defaults
                // to `true` (v1 stance, plan 14 §5 risk row). `false`
                // is the lazy-binding upgrade path anchored at D-LK6-13.
                if (im.contains("bindNow")) {
                    if (!im.at("bindNow").is_boolean()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/image/bindNow",
                                  "'bindNow' must be a boolean (true = "
                                  "eager / bind_off opcode stream, false "
                                  "= lazy / lazy_bind_off — anchored at "
                                  "D-LK6-13, not yet implemented)");
                    } else {
                        data.machoImage.bindNow =
                            im.at("bindNow").get<bool>();
                    }
                }
                // Plan 14 LK7 — Apple codesign placeholder reservation.
                // Optional; defaults to 0 (no LC_CODE_SIGNATURE emitted).
                // Multiple-of-8 enforced at validate() (Apple SuperBlob
                // alignment).
                if (im.contains("codeSignatureSize")) {
                    if (!im.at("codeSignatureSize").is_number_integer()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/image/codeSignatureSize",
                                  "'codeSignatureSize' must be a "
                                  "non-negative integer (Apple SuperBlob "
                                  "reservation size in bytes; plan 16 "
                                  "fills the bytes post-link).");
                    } else {
                        std::int64_t const v =
                            im.at("codeSignatureSize").get<std::int64_t>();
                        if (v < 0
                         || v > static_cast<std::int64_t>(
                                    std::numeric_limits<std::uint32_t>::max())) {
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      "/image/codeSignatureSize",
                                      std::format("'codeSignatureSize' "
                                                  "({}) out of u32 range",
                                                  v));
                        } else {
                            data.machoImage.codeSignatureSize =
                                static_cast<std::uint32_t>(v);
                        }
                    }
                }
                // D-LK6-14 chained-fixups: optional bool. Default false
                // (legacy LC_DYLD_INFO_ONLY path). When true, the walker
                // emits LC_DYLD_CHAINED_FIXUPS + DYLD_CHAINED_PTR_64
                // chained pointers in __got. Requires bindNow==true.
                if (im.contains("useChainedFixups")) {
                    if (!im.at("useChainedFixups").is_boolean()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/image/useChainedFixups",
                                  "'useChainedFixups' must be a boolean "
                                  "(true = LC_DYLD_CHAINED_FIXUPS chained-"
                                  "pointer table, false = legacy "
                                  "LC_DYLD_INFO_ONLY opcode stream — "
                                  "D-LK6-14).");
                    } else {
                        data.machoImage.useChainedFixups =
                            im.at("useChainedFixups").get<bool>();
                    }
                }
                // D-LK7-ADHOC-CODESIGN-MACHO (increment 2/2) — ad-hoc
                // code-signature FILL request. Optional nested object;
                // absent = the legacy `codeSignatureSize`-only placeholder
                // path. When present the walker DERIVES the reservation
                // size and writes a real CodeDirectory + SuperBlob. The two
                // enums are CLOSED — a typo fails loud here (mirrors the
                // `externCallDispatchFromName` + unknown-value-rejects-loud
                // discipline) rather than defaulting silently to a signing
                // scheme. `pageSize` power-of-two and non-empty `identifier`
                // are likewise fail-loud here (a malformed value must not
                // reach the size derivation / blob builder).
                if (im.contains("codeSignature")) {
                    auto const& cs = im.at("codeSignature");
                    if (!cs.is_object()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/image/codeSignature",
                                  "'codeSignature' must be an object "
                                  "{kind, hashAlgorithm, pageSize, "
                                  "identifier}");
                    } else {
                        MachOCodeSignature sig;
                        bool ok = true;
                        // kind (closed enum; default "adhoc").
                        if (cs.contains("kind")) {
                            if (!cs.at("kind").is_string()) {
                                coll.emit(DiagnosticCode::C_MalformedJson,
                                          "/image/codeSignature/kind",
                                          "'kind' must be a string "
                                          "(\"adhoc\")");
                                ok = false;
                            } else {
                                auto const s =
                                    cs.at("kind").get<std::string>();
                                auto const k =
                                    machoCodeSignatureKindFromName(s);
                                if (!k.has_value()) {
                                    coll.emit(
                                        DiagnosticCode::C_MalformedJson,
                                        "/image/codeSignature/kind",
                                        std::format("unknown codeSignature "
                                                    "kind '{}' — accepted: "
                                                    "\"adhoc\" (CS_ADHOC "
                                                    "CodeDirectory, no CMS).",
                                                    s));
                                    ok = false;
                                } else {
                                    sig.kind = *k;
                                }
                            }
                        }
                        // hashAlgorithm (closed enum; default "sha256").
                        if (cs.contains("hashAlgorithm")) {
                            if (!cs.at("hashAlgorithm").is_string()) {
                                coll.emit(DiagnosticCode::C_MalformedJson,
                                          "/image/codeSignature/hashAlgorithm",
                                          "'hashAlgorithm' must be a string "
                                          "(\"sha256\")");
                                ok = false;
                            } else {
                                auto const s = cs.at("hashAlgorithm")
                                                   .get<std::string>();
                                auto const h =
                                    machoCodeSignatureHashAlgoFromName(s);
                                if (!h.has_value()) {
                                    coll.emit(
                                        DiagnosticCode::C_MalformedJson,
                                        "/image/codeSignature/hashAlgorithm",
                                        std::format("unknown codeSignature "
                                                    "hashAlgorithm '{}' — "
                                                    "accepted: \"sha256\" "
                                                    "(CS_HASHTYPE_SHA256).",
                                                    s));
                                    ok = false;
                                } else {
                                    sig.hashAlgorithm = *h;
                                }
                            }
                        }
                        // pageSize (power of two; default 4096).
                        if (cs.contains("pageSize")) {
                            if (!cs.at("pageSize").is_number_integer()) {
                                coll.emit(DiagnosticCode::C_MalformedJson,
                                          "/image/codeSignature/pageSize",
                                          "'pageSize' must be an integer "
                                          "power of two (code-slot page "
                                          "size; typical 4096).");
                                ok = false;
                            } else {
                                std::int64_t const v =
                                    cs.at("pageSize").get<std::int64_t>();
                                // Power of two in [4096, 65536]: the
                                // conventional code-signing hash-page size is
                                // 4096; the bounded range keeps log2(pageSize)
                                // in [12,16] (one byte) AND keeps the
                                // nCodeSlots*hashSize layout math safely within
                                // u32 for any in-range codeLimit (a sub-page
                                // pageSize like 1 would overflow the slot table
                                // on a multi-GiB binary — D-LK7-CODESIGN-
                                // PAGESIZE-OVERFLOW-HARDENING for the residual
                                // ~4 GiB-codeLimit case).
                                if (v < 4096
                                 || v > 65536
                                 || (static_cast<std::uint64_t>(v)
                                     & (static_cast<std::uint64_t>(v) - 1u))
                                        != 0u) {
                                    coll.emit(
                                        DiagnosticCode::C_MalformedJson,
                                        "/image/codeSignature/pageSize",
                                        std::format("'pageSize' ({}) must be "
                                                    "a power of two in "
                                                    "[4096, 65536] (the "
                                                    "CodeDirectory stores "
                                                    "log2(pageSize) in one "
                                                    "byte; 4096 is the "
                                                    "conventional value).",
                                                    v));
                                    ok = false;
                                } else {
                                    sig.pageSize =
                                        static_cast<std::uint32_t>(v);
                                }
                            }
                        }
                        // identifier (non-empty; required).
                        if (!cs.contains("identifier")) {
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      "/image/codeSignature/identifier",
                                      "'identifier' is required and must be "
                                      "a non-empty string (the CodeDirectory "
                                      "identOffset payload; the kernel keys "
                                      "the signature on it).");
                            ok = false;
                        } else if (!cs.at("identifier").is_string()
                                || cs.at("identifier").get<std::string>()
                                       .empty()) {
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      "/image/codeSignature/identifier",
                                      "'identifier' must be a non-empty "
                                      "string.");
                            ok = false;
                        } else {
                            sig.identifier =
                                cs.at("identifier").get<std::string>();
                        }
                        if (ok) {
                            data.machoImage.codeSignature = std::move(sig);
                        }
                    }
                }
                // LC_BUILD_VERSION platform / min-OS / SDK (D-LK10-ENTRY-
                // MACHO-EXIT). Optional nested object; absent → no
                // LC_BUILD_VERSION emitted. `platform` is a CLOSED enum (a
                // typo fails loud, mirroring codeSignature). `minOs`/`sdk`
                // are dotted "X.Y[.Z]" version strings encoded to the on-wire
                // (major<<16)|(minor<<8)|patch nibble form; a malformed
                // version fails loud rather than silently shipping 0.0.0.
                if (im.contains("buildVersion")) {
                    auto const& bv = im.at("buildVersion");
                    if (!bv.is_object()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/image/buildVersion",
                                  "'buildVersion' must be an object "
                                  "{platform, minOs, sdk}");
                    } else {
                        // "X.Y" / "X.Y.Z" → (major<<16)|(minor<<8)|patch.
                        // major 16-bit, minor/patch 8-bit each (the
                        // build_version_command field layout).
                        auto parseVer = [](std::string_view s)
                                          -> std::optional<std::uint32_t> {
                            std::uint32_t parts[3] = {0u, 0u, 0u};
                            std::size_t idx = 0;
                            std::uint32_t cur = 0;
                            bool any = false;
                            for (char c : s) {
                                if (c == '.') {
                                    if (idx >= 2) return std::nullopt;
                                    if (!any) return std::nullopt;
                                    parts[idx++] = cur;
                                    cur = 0;
                                    any = false;
                                } else if (c >= '0' && c <= '9') {
                                    cur = cur * 10u
                                        + static_cast<std::uint32_t>(c - '0');
                                    if (cur > 0xFFFFu) return std::nullopt;
                                    any = true;
                                } else {
                                    return std::nullopt;
                                }
                            }
                            if (!any) return std::nullopt;  // empty / trailing '.'
                            parts[idx] = cur;
                            if (parts[1] > 0xFFu || parts[2] > 0xFFu)
                                return std::nullopt;
                            return (parts[0] << 16) | (parts[1] << 8) | parts[2];
                        };
                        MachOBuildVersion ver;
                        bool ok = true;
                        // The accepted spellings are RENDERED FROM THE TABLE,
                        // never typed into the message. Both strings here used
                        // to say `"macos"` was the only one, which was true
                        // when the vocabulary had one row and became a lie the
                        // moment it grew — the classic rotted-error-message
                        // shape, and the worst place for it, since this is the
                        // text a config author reads to learn what to write.
                        auto acceptedPlatforms = [] {
                            std::string out;
                            for (auto const& row :
                                     kMachOBuildVersionPlatformTable.rows) {
                                if (!out.empty()) out += ", ";
                                out += '"';
                                out += row.second;
                                out += '"';
                            }
                            return out;
                        };
                        // platform (closed enum; required).
                        if (!bv.contains("platform")
                         || !bv.at("platform").is_string()) {
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      "/image/buildVersion/platform",
                                      std::format("'platform' is required and "
                                                  "must be one of: {}.",
                                                  acceptedPlatforms()));
                            ok = false;
                        } else {
                            auto const s =
                                bv.at("platform").get<std::string>();
                            auto const p =
                                machoBuildVersionPlatformFromName(s);
                            if (!p.has_value()) {
                                coll.emit(DiagnosticCode::C_MalformedJson,
                                          "/image/buildVersion/platform",
                                          std::format("unknown buildVersion "
                                                      "platform '{}' — "
                                                      "accepted: {} (Apple's "
                                                      "PLATFORM_* set from "
                                                      "<mach-o/loader.h>).",
                                                      s, acceptedPlatforms()));
                                ok = false;
                            } else {
                                ver.platform = *p;
                            }
                        }
                        // minOs (version string; required).
                        if (!bv.contains("minOs")
                         || !bv.at("minOs").is_string()) {
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      "/image/buildVersion/minOs",
                                      "'minOs' is required and must be a "
                                      "version string (\"11.0\" / \"11.0.0\").");
                            ok = false;
                        } else {
                            auto const v =
                                parseVer(bv.at("minOs").get<std::string>());
                            if (!v.has_value()) {
                                coll.emit(DiagnosticCode::C_MalformedJson,
                                          "/image/buildVersion/minOs",
                                          "'minOs' must be a dotted version "
                                          "\"X.Y[.Z]\" (major<65536, "
                                          "minor/patch<256).");
                                ok = false;
                            } else {
                                ver.minOs = *v;
                            }
                        }
                        // sdk (version string; optional — defaults to minOs).
                        if (bv.contains("sdk")) {
                            if (!bv.at("sdk").is_string()) {
                                coll.emit(DiagnosticCode::C_MalformedJson,
                                          "/image/buildVersion/sdk",
                                          "'sdk' must be a version string.");
                                ok = false;
                            } else {
                                auto const v =
                                    parseVer(bv.at("sdk").get<std::string>());
                                if (!v.has_value()) {
                                    coll.emit(DiagnosticCode::C_MalformedJson,
                                              "/image/buildVersion/sdk",
                                              "'sdk' must be a dotted version "
                                              "\"X.Y[.Z]\".");
                                    ok = false;
                                } else {
                                    ver.sdk = *v;
                                }
                            }
                        } else {
                            ver.sdk = ver.minOs;
                        }
                        if (ok) {
                            data.machoImage.buildVersion = ver;
                        }
                    }
                }
            }
        }
    }

    void validateIdentity(detail::ObjectFormatData const& d,
                          SchemaProblemSink const&        fail) const override {
        // Binding preamble — see the file header.
        auto const& relocations            = d.relocations;
        auto const& sections               = d.sections;
        auto const& macho                  = d.macho;
        auto const& machoImage             = d.machoImage;
        auto const& entryPoint             = d.entryPoint;
        auto const& processExit            = d.processExit;
        auto const& entryCallingConvention = d.entryCallingConvention;
        auto const& processArgs            = d.processArgs;

    // Mach-O identity: cputype/cpusubtype/filetype must be declared.
    // Mach-O is also the only format whose section rows REQUIRE a
    // non-empty `segment` (two-level naming) — ELF/PE leave it
    // empty. Section + segment names also must fit in 16 chars
    // (Mach-O has no long-name escape; the walker writes a fixed
    // 16-byte field).
        {
        if (macho.cputype == 0) {
            fail("/macho/cputype",
                 "Mach-O format requires 'macho.cputype' (CPU_TYPE_* "
                 "value, e.g. 0x01000007 for x86_64, 0x0100000C for "
                 "arm64)");
        }
        // All three MachOObjectType members have walker arms: MH_OBJECT
        // (LK3 cycle 1), MH_EXECUTE (LK3 cycle 2), MH_DYLIB (c153,
        // D-LK3-3 — the Mach-O mirror of ELF ET_DYN c150 + PE Dll
        // c152). The closed enum + the loader's fromName check already
        // reject any other value; nothing to gate here since c153.
        for (std::size_t i = 0; i < sections.size(); ++i) {
            auto const& s = sections[i];
            if (s.segment.empty()) {
                fail(std::format("/sections/{}/segment", i),
                     std::format("section '{}': Mach-O requires a "
                                 "non-empty 'segment' name (e.g. "
                                 "'__TEXT' for the section '__text'); "
                                 "the single-string substrate field is "
                                 "for ELF/PE only",
                                 s.name));
            }
            if (s.name.size() > 16) {
                fail(std::format("/sections/{}/name", i),
                     std::format("section '{}' length {} exceeds the "
                                 "16-char Mach-O section_64.sectname "
                                 "field (no long-name escape exists)",
                                 s.name, s.name.size()));
            }
            if (s.segment.size() > 16) {
                fail(std::format("/sections/{}/segment", i),
                     std::format("segment '{}' length {} exceeds the "
                                 "16-char Mach-O section_64.segname "
                                 "field",
                                 s.segment, s.segment.size()));
            }
            // Mach-O MH_OBJECT files use section_64.addr = 0 (vmaddr
            // assignment happens at exec build time via LC_SEGMENT_64).
            // MH_EXECUTE will use virtualAddress; that arm lands at
            // D-LK3-2. For cycle 1 (filetype == MH_OBJECT), reject
            // non-zero virtualAddress to prevent silent drift.
            if (macho.filetype == MachOObjectType::Object
             && s.virtualAddress != 0) {
                fail(std::format("/sections/{}/virtualAddress", i),
                     std::format("section '{}': 'virtualAddress' must "
                                 "be 0 for Mach-O MH_OBJECT rows "
                                 "(section_64.addr in relocatable .o "
                                 "is 0; MH_EXECUTE rows declare VAs "
                                 "explicitly — LK3 cycle 2)",
                                 s.name));
            }
            // 9945457 audit fold (silent-failure A1 + test-analyzer-
            // dim-2 #5): reject MH_OBJECT + useChainedFixups loud at
            // validate() rather than silently ignoring the flag in
            // the MH_OBJECT encoder path. Apple's chained fixups are
            // linker-output-only; .o files have no dyld binding
            // semantics so the combination is semantically nonsensical.
            if (macho.filetype == MachOObjectType::Object
             && machoImage.useChainedFixups) {
                fail("/image/useChainedFixups",
                     "'useChainedFixups' = true is invalid for "
                     "Mach-O MH_OBJECT (filetype = 'object') — "
                     "chained fixups are a linker-output binding "
                     "format; relocatable .o files have no dyld "
                     "binding semantics. Either set 'filetype' = "
                     "'execute' (and route through encodeExecDynamic) "
                     "or clear 'useChainedFixups'.");
            }
        }
        // Mach-O nativeId packing reserves bit 27 (r_extern) and
        // bits 0..23 (r_symbolnum) for walker-filled fields. JSON
        // values setting those bits silently corrupt the
        // relocation_info word at emit time (silent-failure C1 +
        // type-design Q4 convergence). Validate the packing mask
        // here so JSON typos fail loud at load time.
        constexpr std::uint32_t kMachOReservedBits =
            (1u << 27) | 0x00FFFFFFu;
        for (std::size_t i = 0; i < relocations.size(); ++i) {
            auto const& r = relocations[i];
            if ((r.nativeId & kMachOReservedBits) != 0) {
                fail(std::format("/relocations/{}/nativeId", i),
                     std::format("relocation '{}': 'nativeId' "
                                 "0x{:08X} sets reserved bits "
                                 "(bit 27 = r_extern is walker-filled; "
                                 "bits 0..23 = r_symbolnum are filled "
                                 "per-reloc with the symtab index). "
                                 "Mach-O packing must only set bits "
                                 "28..31 (r_type), 25..26 (r_length), "
                                 "and 24 (r_pcrel)",
                                 r.name, r.nativeId));
            }
        }
        // MH_EXECUTE / MH_DYLIB image rules. The walker emits
        // LC_LOAD_DYLINKER (the dynamic linker path) and
        // LC_LOAD_DYLIB (each library the image needs at load
        // time). __PAGEZERO segment vmsize comes from
        // machoImage.pageZeroSize. All three are mandatory for
        // any loadable Mach-O image — the loader rejects the
        // binary at exec time if they're missing or zero. Reject
        // at load time instead.
        auto const& mi = machoImage;
        bool const isObj = macho.filetype == MachOObjectType::Object;
        if (isObj) {
            // `bindNow` is included in the anySet check below: its
            // default is `true`, so the user must have explicitly
            // set it (to either true or false) on a .o schema for
            // the field to read non-default. Either way the field
            // is meaningless on MH_OBJECT — reject. (Symmetric with
            // ELF ET_REL bindNow rule above; type-design HIGH, LK6
            // cycle 2c post-fold review.) Note: MachOImage's
            // default-constructed `bindNow = true` cannot be
            // distinguished from "user set true" at this stage, so
            // the reject fires only on `bindNow == false`. The
            // `bindNow == true` case is structurally a no-op (the
            // default) on .o paths.
            // ★ `buildVersion` IS THE ONE MEMBER OF THIS BLOCK AN MH_OBJECT
            // MAY DECLARE, and it is deliberately absent from the disjunction
            // below (D-LK10-ENTRY-MACHO-EXIT, MH_OBJECT arm). Every OTHER key
            // here describes how a LOADABLE IMAGE is mapped — a zero page, a
            // dynamic linker, libraries to bind, a signature the kernel
            // validates — none of which a relocatable input to a later link
            // has. `LC_BUILD_VERSION` is not that kind of field: it states
            // which PLATFORM the file was built for, and Mach-O makes it legal
            // in an MH_OBJECT exactly as in an MH_EXECUTE. ✔MEASURED
            // 2026-08-13 on macOS 26.5.2 arm64: Apple `clang -c` emits it into
            // every plain `.o` (LC_SEGMENT_64 / LC_BUILD_VERSION / LC_SYMTAB /
            // LC_DYSYMTAB), and ld64 (PROJECT:ld-1267) warns `no platform load
            // command found in '<tu>.o', assuming: macOS` once per object that
            // omits it. The `image.*` naming means "this Mach-O FILE", not "a
            // loadable image" — filetype is the axis that decides loadability,
            // and it is read directly above.
            //
            // The message keeps naming ONLY the rejected keys, so it stays a
            // true statement of what fired rather than a list that has to be
            // read against an exception.
            bool const anySet = mi.pageZeroSize != 0
                || mi.segmentPageSize != kDefaultMachoSegmentPageSize
                || !mi.dylinkerPath.empty()
                || !mi.loadDylibs.empty()
                || !mi.installName.empty()
                || !mi.bindNow
                || mi.codeSignatureSize != 0
                || mi.codeSignature.has_value();
            if (anySet) {
                fail("/image",
                     "Mach-O MH_OBJECT format must NOT declare an "
                     "'image' block — pageZeroSize / segmentPageSize / "
                     "dylinkerPath / loadDylibs / installName / bindNow / "
                     "codeSignatureSize / codeSignature live only in "
                     "MH_EXECUTE / MH_DYLIB images. Set macho.filetype = 2 "
                     "(MH_EXECUTE) if this schema describes an executable. "
                     "('image.buildVersion' is the ONE exception — "
                     "LC_BUILD_VERSION is legal in a relocatable object and "
                     "the MH_OBJECT walker emits it.)");
            }
        } else {
            // Image flavors: MH_EXECUTE and MH_DYLIB (c153, D-LK3-3).
            // Shared rules first, then the per-flavor divergences —
            // keyed on the declared filetype (closed-enum schema data,
            // never a format-name string).
            bool const isDylibImage =
                macho.filetype == MachOObjectType::Dylib;
            if (!isDylibImage && mi.pageZeroSize == 0) {
                fail("/image/pageZeroSize",
                     "Mach-O MH_EXECUTE image requires non-zero "
                     "'image.pageZeroSize' (__PAGEZERO segment vmsize "
                     "— typical 0x100000000 = 4 GiB on x86_64/ARM64 "
                     "darwin; catches null-pointer derefs at the "
                     "kernel level).");
            }
            // D-LK3-3: a dylib carries NO __PAGEZERO — it is mapped at
            // a dyld-chosen slid base (link-time base 0), and a 4 GiB
            // zero-page segment inside a library would reserve address
            // space at every dlopen; ld64 likewise rejects
            // -pagezero_size on non-main links. The walker keys the
            // __PAGEZERO emission (and __TEXT.vmaddr = pageZeroSize)
            // on this field, so a non-zero value here would emit a
            // loader-rejected dylib.
            if (isDylibImage && mi.pageZeroSize != 0) {
                fail("/image/pageZeroSize",
                     std::format("Mach-O MH_DYLIB must declare "
                                 "'image.pageZeroSize' = 0 (got 0x{:x}) "
                                 "-- a dylib carries no __PAGEZERO; its "
                                 "__TEXT.vmaddr is the base-0 link-time "
                                 "address dyld slides at load "
                                 "(D-LK3-3).",
                                 mi.pageZeroSize));
            }
            if (!isDylibImage && mi.dylinkerPath.empty()) {
                fail("/image/dylinkerPath",
                     "Mach-O MH_EXECUTE image requires non-empty "
                     "'image.dylinkerPath' (LC_LOAD_DYLINKER — "
                     "typical '/usr/lib/dyld' on macOS).");
            }
            // D-LK3-3: LC_LOAD_DYLINKER names the loader the KERNEL
            // execs for a MAIN executable; a dylib is mapped by the
            // already-running dyld and must not carry one (ld64 emits
            // none for -dylib output).
            if (isDylibImage && !mi.dylinkerPath.empty()) {
                fail("/image/dylinkerPath",
                     std::format("Mach-O MH_DYLIB must not declare "
                                 "'image.dylinkerPath' (got '{}') -- a "
                                 "dylib is mapped by the already-running "
                                 "dyld; LC_LOAD_DYLINKER is a "
                                 "main-executable load command "
                                 "(D-LK3-3).",
                                 mi.dylinkerPath));
            }
            if (mi.loadDylibs.empty()) {
                fail("/image/loadDylibs",
                     std::format("Mach-O {} image requires at least one "
                                 "'image.loadDylibs' entry (typical "
                                 "'/usr/lib/libSystem.B.dylib' -- libc / "
                                 "process start function provider; every "
                                 "real dylib ld64 produces depends on "
                                 "libSystem too).",
                                 std::string{machoObjectTypeName(
                                     macho.filetype)}));
            }
            // D-LK3-3: the LC_ID_DYLIB install name — REQUIRED on a
            // dylib (every ld64-produced MH_DYLIB carries one; clients
            // record it at link time), dead config anywhere else. The
            // walker never derives it from the output file name
            // (config-driven + honest — the c150 DT_SONAME
            // discipline), so an unset field must fail HERE, not
            // silently emit an identity-less dylib.
            if (isDylibImage && mi.installName.empty()) {
                fail("/image/installName",
                     "Mach-O MH_DYLIB requires non-empty "
                     "'image.installName' (the LC_ID_DYLIB name a "
                     "client links against, e.g. "
                     "'@rpath/libdss.dylib'). The walker never derives "
                     "it from the output file name (D-LK3-3).");
            }
            if (!isDylibImage && !mi.installName.empty()) {
                fail("/image/installName",
                     std::format("'image.installName' (got '{}') is "
                                 "only legal on a Mach-O MH_DYLIB "
                                 "schema -- LC_ID_DYLIB is the dylib's "
                                 "identity; an executable carries none "
                                 "(D-LK3-3).",
                                 mi.installName));
            }
            // Mach-O mmap-congruence: vmaddr % page == fileoff %
            // page. __TEXT.fileoff = 0 by Apple convention, so
            // __TEXT.vmaddr (= pageZeroSize) must be page-aligned.
            // Common convention uses 0x1000 (x86_64) or 0x4000 /
            // 0x10000 (ARM64 16K/64K page configs). Anything that
            // isn't a positive power of two breaks the kernel's
            // congruence check and the loader fails ENOEXEC.
            // (silent-failure-hunter MEDIUM, LK6 cycle 2c review)
            if (mi.pageZeroSize != 0
             && ((mi.pageZeroSize & (mi.pageZeroSize - 1u)) != 0u)) {
                fail("/image/pageZeroSize",
                     std::format("'image.pageZeroSize' (0x{:x}) "
                                 "must be a power of two so that "
                                 "__TEXT.vmaddr (= pageZeroSize) "
                                 "preserves the kernel's mmap "
                                 "congruence (vmaddr % page == "
                                 "fileoff % page).",
                                 mi.pageZeroSize));
            }
            // segmentPageSize is the LC_SEGMENT_64 vmaddr/vmsize/fileoff
            // alignment the walker feeds to alignUp() — it MUST be a
            // positive power of two (alignUp masks with page-1) or the
            // emitted segments overlap / misalign and the kernel rejects
            // with EBADMACHO. 4 KiB (x86_64-darwin default) and 16 KiB
            // (arm64-darwin, the Apple-Silicon requirement) are the live
            // values. (D-LK10-ENTRY-MACHO-EXIT.)
            if (mi.segmentPageSize == 0
             || (mi.segmentPageSize & (mi.segmentPageSize - 1u)) != 0u) {
                fail("/image/segmentPageSize",
                     std::format("'image.segmentPageSize' (0x{:x}) must "
                                 "be a positive power of two — it is the "
                                 "VM segment alignment fed to alignUp(); "
                                 "a non-power-of-two misaligns every "
                                 "LC_SEGMENT_64 and the kernel rejects "
                                 "the image with EBADMACHO. Use 4096 "
                                 "(x86_64-darwin) or 16384 (arm64-darwin "
                                 "/ Apple Silicon).",
                                 mi.segmentPageSize));
            }
            for (std::size_t i = 0; i < mi.loadDylibs.size(); ++i) {
                if (mi.loadDylibs[i].path.empty()) {
                    fail(std::format("/image/loadDylibs/{}/path", i),
                         "loadDylibs entries must declare a non-empty "
                         "path");
                }
            }
            // __TEXT segment must sit at or above __PAGEZERO's end —
            // otherwise the walker's `sectionVa - pageZeroSize`
            // subtraction underflows silently to ~2^64 and the
            // emitted segment vmsize wraps (silent-failure H4 + code-
            // reviewer C2 convergence).
            for (auto const& s : sections) {
                if (s.kind != SectionKind::Text) continue;
                if (s.virtualAddress < mi.pageZeroSize) {
                    fail("/sections/<text>/virtualAddress",
                         std::format("Mach-O image: __text "
                                     "virtualAddress 0x{:x} is below "
                                     "__PAGEZERO end 0x{:x} (pageZeroSize) "
                                     "— __TEXT would overlap __PAGEZERO; "
                                     "loader rejects.",
                                     s.virtualAddress, mi.pageZeroSize));
                } else if (mi.segmentPageSize != 0
                        && ((s.virtualAddress - mi.pageZeroSize)
                                % mi.segmentPageSize) != 0) {
                    // mmap congruence: __TEXT.fileoff = 0 by Apple
                    // convention, so __text's VM offset within __TEXT
                    // (virtualAddress - pageZeroSize) must be a multiple
                    // of segmentPageSize to match its page-aligned file
                    // offset. A 4 KiB-congruent VA (e.g. 0x100001000)
                    // under a 16 KiB page is the canonical arm64 EBADMACHO
                    // (the kernel maps __text at the wrong VA). Catch the
                    // misconfiguration here rather than ship a binary that
                    // fails to load. (D-LK10-ENTRY-MACHO-EXIT.)
                    fail("/sections/<text>/virtualAddress",
                         std::format("Mach-O image: __text "
                                     "virtualAddress 0x{:x} is not aligned "
                                     "to segmentPageSize 0x{:x} relative to "
                                     "pageZeroSize 0x{:x} (offset 0x{:x} "
                                     "violates vmaddr % page == fileoff % "
                                     "page — the kernel rejects with "
                                     "EBADMACHO). On a 0x{:x} page, set "
                                     "virtualAddress to a multiple of it "
                                     "above pageZeroSize.",
                                     s.virtualAddress, mi.segmentPageSize,
                                     mi.pageZeroSize,
                                     (s.virtualAddress - mi.pageZeroSize)
                                         % mi.segmentPageSize,
                                     mi.segmentPageSize));
                }
            }
            // Plan 14 LK7 — codesign placeholder reservation must
            // be a multiple of 8 (Apple SuperBlob alignment per
            // `cs_blobs.h`). 0 = no reservation (default); any
            // other value must align so plan 16's blob fills
            // without padding mid-blob.
            if (mi.codeSignatureSize != 0
             && (mi.codeSignatureSize % 8u) != 0u) {
                fail("/image/codeSignatureSize",
                     std::format("'image.codeSignatureSize' ({}) must "
                                 "be a multiple of 8 (Apple SuperBlob "
                                 "alignment; plan 16 fills the "
                                 "reserved bytes with a CodeDirectory "
                                 "blob whose layout requires 8-byte "
                                 "alignment).",
                                 mi.codeSignatureSize));
            }
            // ── Mach-O MH_DYLIB shape rules — c153, D-LK3-3 (the
            // Mach-O mirror of the ELF ET_DYN `.so` block + the PE Dll
            // block). A DSS `.dylib` ships entry-less — NO LC_MAIN, no
            // trampoline — so the entry cluster is ILLEGAL here,
            // exactly as on the `.so`/`.dll`. The generic exec-flavor
            // gates at the bottom of validate() back these up; these
            // carry the precise dylib-shaped message.
            if (isDylibImage) {
                bool sawText = false;
                for (auto const& s : sections) {
                    if (s.kind != SectionKind::Text) {
                        continue;
                    }
                    sawText = true;
                    // Base-0 image BY CONSTRUCTION (the ELF dyn
                    // `virtualAddress == pageAlign` mirror): the walker
                    // computes __TEXT.vmaddr = pageZeroSize = 0, and
                    // textFileOff = alignUp(header+cmds, page) = one
                    // page — so the schema VA must equal ONE
                    // segmentPageSize (page 0 holds the mach header +
                    // load commands; __text opens page 1; dyld slides
                    // the whole base-0 image). The walker's
                    // textSegmentVaMatchesFileOff chokepoint re-checks
                    // the computed equality at emit time.
                    if (s.virtualAddress != mi.segmentPageSize) {
                        fail("/sections/<text>/virtualAddress",
                             std::format(
                                 "Mach-O MH_DYLIB: __text "
                                 "virtualAddress 0x{:x} must equal "
                                 "image.segmentPageSize 0x{:x} -- a "
                                 "dylib is a base-0 image (no "
                                 "__PAGEZERO; header page 0, __text "
                                 "page 1) that dyld slides at load "
                                 "(D-LK3-3).",
                                 s.virtualAddress, mi.segmentPageSize));
                    }
                }
                if (!sawText) {
                    fail("/sections",
                         "Mach-O MH_DYLIB format requires a Text "
                         "section row (SectionKind::Text) -- a dynamic "
                         "library without code has nothing to export. "
                         "D-LK3-3.");
                }
                if (processExit.has_value()
                 || !entryCallingConvention.empty()
                 || processArgs.has_value()) {
                    fail("/macho",
                         std::format(
                             "Mach-O MH_DYLIB format declares "
                             "entry-cluster machinery -- processExit: "
                             "{}, entryCallingConvention: {}, "
                             "processArgs: {}. A DSS .dylib has NO "
                             "LC_MAIN (a dylib has no process entry; "
                             "dyld runs no entry code for it), so no "
                             "entry trampoline is synthesized and the "
                             "cluster is dead config that would "
                             "silently diverge from the emitted image "
                             "(D-LK3-3; mirrors the ELF .so + PE Dll "
                             "rejects).",
                             processExit.has_value() ? "present"
                                                     : "absent",
                             entryCallingConvention.empty() ? "absent"
                                                            : "present",
                             processArgs.has_value() ? "present"
                                                     : "absent"));
                }
                if (!entryPoint.empty()) {
                    fail("/entryPoint",
                         std::format("Mach-O MH_DYLIB format must not "
                                     "declare 'entryPoint' (got '{}') "
                                     "-- a DSS .dylib has no process "
                                     "entry (no LC_MAIN; D-LK3-3).",
                                     entryPoint));
                }
                // The dylib arm emits its EXPORT TRIE through the
                // legacy LC_DYLD_INFO_ONLY export_off field; the
                // chained-fixups path would need the separate
                // LC_DYLD_EXPORTS_TRIE load command, which is not
                // implemented — reject the combination loud rather
                // than emit a dylib whose exports dyld never finds
                // (D-LK3-DYLIB-CHAINED-FIXUPS-EXPORT-TRIE).
                if (mi.useChainedFixups) {
                    fail("/image/useChainedFixups",
                         "'useChainedFixups' = true is not supported "
                         "on a Mach-O MH_DYLIB schema -- the dylib "
                         "export trie is emitted through the legacy "
                         "LC_DYLD_INFO_ONLY export_off field; the "
                         "chained-fixups path would need the separate "
                         "LC_DYLD_EXPORTS_TRIE load command "
                         "(D-LK3-DYLIB-CHAINED-FIXUPS-EXPORT-TRIE). "
                         "Set 'useChainedFixups' = false.");
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
        (void)request;  // no declared vehicle — see stackReserveVehicles()
        return macho::encode(module, targetSchema, objectFormatSchema,
                             reporter);
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
ObjectFormatBackend const& machoBackend() noexcept {
    static MachOBackend const instance{};
    return instance;
}

} // namespace dss::link::format
