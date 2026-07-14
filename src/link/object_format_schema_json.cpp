#include "link/object_format_schema.hpp"

#include "core/substrate/diagnostic_collector.hpp"
#include "core/substrate/mint_monotonic_id.hpp"
#include "core/substrate/relocation_table.hpp"
#include "core/types/artifact_profile.hpp"  // isRegisteredArtifactProfile / registeredArtifactProfileList (AP3, shared w/ grammar loader)
#include "core/types/parse_diagnostic.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace dss {

namespace {

using json = nlohmann::json;
using Collector = substrate::DiagnosticCollector;

} // namespace

LoadResult<std::shared_ptr<ObjectFormatSchema>>
ObjectFormatSchema::loadFromText(std::string_view jsonText,
                                  std::string_view sourceLabel) {
    Collector coll;
    json doc;
    try {
        doc = json::parse(jsonText);
    } catch (json::parse_error const& e) {
        coll.emit(DiagnosticCode::C_MalformedJson, std::string{sourceLabel},
                  std::format("JSON parse error: {}", e.what()));
        return std::unexpected(std::move(coll).release());
    }
    if (!doc.is_object()) {
        coll.emit(DiagnosticCode::C_MalformedJson, std::string{sourceLabel},
                  "top-level value must be a JSON object");
        return std::unexpected(std::move(coll).release());
    }

    // dssObjectFormatVersion — same per-schema-file version contract
    // as TargetSchema's. v1 is the only accepted version today;
    // future LK* cycles bump as schema shape grows.
    if (!doc.contains("dssObjectFormatVersion")
     || !doc.at("dssObjectFormatVersion").is_number_integer()) {
        coll.emit(DiagnosticCode::C_VersionMismatch, std::string{sourceLabel},
                  "missing or non-integer 'dssObjectFormatVersion'");
        return std::unexpected(std::move(coll).release());
    }
    int const ver = doc.at("dssObjectFormatVersion").get<int>();
    if (ver != 1) {
        coll.emit(DiagnosticCode::C_VersionMismatch, "/dssObjectFormatVersion",
                  std::format("only version 1 supported (got {})", ver));
        return std::unexpected(std::move(coll).release());
    }

    detail::ObjectFormatData data;
    data.id = substrate::mintMonotonicId<ObjectFormatSchemaId>();

    if (!doc.contains("format") || !doc.at("format").is_object()) {
        coll.emit(DiagnosticCode::C_MissingField, std::string{sourceLabel},
                  "missing 'format' object");
        return std::unexpected(std::move(coll).release());
    }
    auto const& format = doc.at("format");
    if (!format.contains("name") || !format.at("name").is_string()) {
        coll.emit(DiagnosticCode::C_MissingField, "/format/name",
                  "missing or non-string 'name'");
        return std::unexpected(std::move(coll).release());
    }
    data.name = format.at("name").get<std::string>();
    // Cross-tier symmetry with `target.name` (D-LK6-8.2 post-fold #2
    // architect Q3): `format.name` is the label every walker
    // diagnostic message uses. An empty or whitespace-only name
    // would produce unintelligible diagnostics silently. The same
    // non-empty-non-whitespace discipline applies on both sides.
    auto const isAsciiSpace = [](char c) noexcept {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r'
            || c == '\v' || c == '\f';
    };
    bool const isBadName = [&]() noexcept {
        if (data.name.empty()) return true;
        if (isAsciiSpace(data.name.front())) return true;
        if (isAsciiSpace(data.name.back()))  return true;
        for (char c : data.name) if (!isAsciiSpace(c)) return false;
        return true;  // all whitespace
    }();
    if (isBadName) {
        coll.emit(DiagnosticCode::C_MissingField, "/format/name",
                  "'name' must be a non-empty string with no leading "
                  "or trailing whitespace — appears verbatim in every "
                  "walker diagnostic.");
        return std::unexpected(std::move(coll).release());
    }
    if (format.contains("version") && format.at("version").is_string()) {
        data.version = format.at("version").get<std::string>();
    }
    if (!format.contains("kind") || !format.at("kind").is_string()) {
        coll.emit(DiagnosticCode::C_MissingField, "/format/kind",
                  "missing or non-string 'kind' (one of 'elf' / 'pe' / "
                  "'macho' / 'wasm' / 'spirv')");
        return std::unexpected(std::move(coll).release());
    }
    auto const kindOpt = objectFormatKindFromName(
        format.at("kind").get<std::string>());
    if (!kindOpt.has_value()) {
        coll.emit(DiagnosticCode::C_MalformedJson, "/format/kind",
                  "expected 'elf' / 'pe' / 'macho' / 'wasm' / 'spirv'");
        return std::unexpected(std::move(coll).release());
    }
    data.kind = *kindOpt;

    // Cross-format identity-block validation (test-analyzer Gap 6
    // fold, LK8 review): a schema's `kind` is the load-bearing
    // dispatcher — every per-format identity block (`elf`, `pe`,
    // `optionalHeader`, `macho`, `image`) is only consumed when
    // its matching kind is set. A schema declaring `kind: wasm`
    // with a stray `elf` / `pe` / `macho` block would silently
    // drop the block. Reject loudly so a copy-paste-then-rename
    // mistake surfaces at schema load.
    struct CrossKindGuard {
        ObjectFormatKind expectedKind;
        char const*      blockName;
    };
    constexpr CrossKindGuard kCrossKindRules[] = {
        { ObjectFormatKind::Elf,    "elf"            },
        { ObjectFormatKind::Pe,     "pe"             },
        { ObjectFormatKind::Pe,     "optionalHeader" },
        { ObjectFormatKind::MachO,  "macho"          },
        { ObjectFormatKind::MachO,  "image"          },
    };
    for (auto const& rule : kCrossKindRules) {
        if (data.kind != rule.expectedKind
         && doc.contains(rule.blockName)) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      std::string{"/"} + rule.blockName,
                      std::string{"identity block '"} + rule.blockName
                          + "' is only meaningful when format.kind == '"
                          + std::string{
                                objectFormatKindName(rule.expectedKind)}
                          + "' (got kind '"
                          + std::string{objectFormatKindName(data.kind)}
                          + "'). A stray block of the wrong kind would "
                            "be silently dropped — fix the block name or "
                            "the format.kind.");
        }
    }
    // Universal-field positive assertion for Wasm + Spirv (type-
    // design Q3 fold, LK8 post-fold review). WASM has no native
    // relocations, no per-section file-layout knobs, and its
    // entry-point lives inside the Start section's function index
    // (not a top-level symbol name). Spirv's `OpEntryPoint` is
    // similarly emitted inline as a typed module instruction, not
    // declared as a substrate-tier symbol name. Skeleton schemas
    // (`wasm32-v1.format.json`) ship with these fields ABSENT; a
    // future plan-18 schema that declares them would be silently
    // ignored by the walker. Reject loudly so a stray
    // `sections` / `relocations` / `entryPoint` surfaces at load
    // and gets re-anchored against plan 18 / plan 17 vocabulary.
    if (data.kind == ObjectFormatKind::Wasm
     || data.kind == ObjectFormatKind::Spirv) {
        char const* const universalFields[] = {
            "sections", "relocations", "entryPoint",
            // dim-2 HIGH #3 (7425905 audit fold): D-LK10-ENTRY
            // Slice B fields are meaningless on Wasm/SPIR-V
            // (no trampoline emitter; their exit semantics are
            // format-native). Defense-in-depth: reject loudly
            // rather than silently accept dead data.
            "processExit", "entryCallingConvention",
            // D-RUNTIME-MAIN-ARGC-ARGV: the program-entry argument
            // mechanism rides the same trampoline emitter — dead
            // data on Wasm/SPIR-V for the same reason.
            "processArgs",
            // D-LK2-RODATA closure: the producer-data-section
            // capability is meaningless on Wasm/SPIR-V (their
            // walkers emit rodata via format-native section
            // vocabulary, not via the cross-format dataItems
            // pipeline).
            "supportedDataSections",
            // D-FFI-EXTERN-CALL-DISPATCH: the PLT-stub vs IAT-slot
            // extern-call shape is an ELF/PE/Mach-O dynamic-import
            // notion; WASM/SPIR-V reach imports through their own
            // format-native mechanisms (WASM import section / SPIR-V
            // linkage decorations), so a top-level declaration here
            // would be dead data.
            "externCallDispatch",
            // D-LK-EXTERN-DATA-IMPORT: the extern-DATA import binding
            // model (copy relocations) is likewise an ELF/PE/Mach-O
            // dynamic-import notion — dead data on WASM/SPIR-V.
            "dataImportBinding",
            // D-CSUBSET-THREAD-LOCAL (TLS C1): the thread-local access
            // model (segment-register / TEB / TLV descriptor) is an
            // ELF/PE/Mach-O native-image notion — dead data on
            // WASM/SPIR-V (their thread-local story is format-native
            // vocabulary when it lands).
            "tlsAccess",
            // D-CSUBSET-C11-THREADS-MACHO: the shipped-library synth
            // vehicle (kernel32 / pthread primitive family for a
            // compiler-synthesized shim) is an ELF/PE/Mach-O native-
            // runtime notion — dead data on WASM/SPIR-V.
            "librarySynthesis",
        };
        for (auto const* field : universalFields) {
            if (doc.contains(field)) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          std::string{"/"} + field,
                          std::string{"format kind '"}
                              + std::string{objectFormatKindName(data.kind)}
                              + "' must not declare a top-level '"
                              + field
                              + "' field — WASM / SPIR-V emit this "
                                "information through their own format-"
                                "native section vocabulary (plan 18 / "
                                "plan 17). A top-level declaration "
                                "would be silently ignored by the "
                                "walker.");
            }
        }
    }

    // ── FC3 c1: `dataModel` — REQUIRED on EVERY format ──────────────
    //
    // The per-OS C-family width triple ("LP64" / "LLP64" / "ILP32").
    // Closed enum + required: a missing field or unknown spelling is a
    // LOAD reject — a silent default would bake wrong `long` widths
    // into every compile for the format (the knob-that-lies failure the
    // unknown-key discipline exists to prevent). Required on wasm /
    // spirv skeletons too (ILP32, declared-only — the semantic consumer
    // fails loud when an ILP32 format is actually selected).
    if (!doc.contains("dataModel")) {
        coll.emit(DiagnosticCode::C_MissingField, "/dataModel",
                  "missing required 'dataModel' — every object format "
                  "must declare its C-family width triple ('LP64', "
                  "'LLP64', or 'ILP32'); a silent default would bake "
                  "wrong primitive widths");
    } else if (!doc.at("dataModel").is_string()) {
        coll.emit(DiagnosticCode::C_MalformedJson, "/dataModel",
                  "'dataModel' must be a string ('LP64', 'LLP64', or "
                  "'ILP32')");
    } else {
        auto const s = doc.at("dataModel").get<std::string>();
        auto const dm = dataModelFromName(s);
        if (!dm) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/dataModel",
                      std::format("unknown dataModel '{}' — expected one "
                                  "of 'LP64', 'LLP64', 'ILP32'", s));
        } else {
            data.dataModel = *dm;
        }
    }

    // ── D-CSUBSET-BITFIELD-ABI-EXACT: OPTIONAL `bitFieldStrategy` ────
    //
    // The per-ABI C bit-field packing rule ("gnu_packed" / "msvc_straddle").
    // Determined by the OBJECT FORMAT / OS, not the CPU (x86_64 serves BOTH
    // ELF-SysV gnu_packed and PE-MS msvc_straddle), so it lives here next to
    // `dataModel`. OPTIONAL: absent ⇒ BitFieldStrategy::None and the driver
    // falls back to the TARGET's declared `aggregateLayout.bitFieldStrategy`
    // (back-compat). A wrong spelling is a HARD error — a typo can never
    // silently fall back to a wrong rule (the dataModel discipline).
    if (doc.contains("bitFieldStrategy")) {
        if (!doc.at("bitFieldStrategy").is_string()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/bitFieldStrategy",
                      "'bitFieldStrategy' must be a string ('gnu_packed' or "
                      "'msvc_straddle')");
        } else {
            auto const s = doc.at("bitFieldStrategy").get<std::string>();
            auto const bs = bitFieldStrategyFromName(s);
            if (!bs) {
                coll.emit(DiagnosticCode::C_MalformedJson, "/bitFieldStrategy",
                          std::format("unknown bitFieldStrategy '{}' — expected "
                                      "'gnu_packed' or 'msvc_straddle'", s));
            } else if (*bs == BitFieldStrategy::None) {
                // "none" is the sentinel, not a selectable strategy on a format.
                coll.emit(DiagnosticCode::C_MalformedJson, "/bitFieldStrategy",
                          "bitFieldStrategy 'none' is not selectable — omit the "
                          "field to leave it unset (the target's value is used)");
            } else {
                data.bitFieldStrategy = *bs;
            }
        }
    }

    // Top-level `entryPoint` — universal entry-symbol name for
    // executable artifacts (e.g. "_start" / "main" / Mach-O's
    // LC_MAIN target). Empty for relocatable artifacts. The walker
    // resolves this against AssembledModule's symbols at emit time
    // to compute the entry virtual address.
    if (doc.contains("entryPoint")) {
        if (!doc.at("entryPoint").is_string()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/entryPoint",
                      "'entryPoint' must be a string");
        } else {
            data.entryPoint = doc.at("entryPoint").get<std::string>();
        }
    }

    // D-LK10-ENTRY Slice B (plan 14 §2.13): `entryCallingConvention`
    // — names the cc the trampoline emitter resolves via
    // `target.callingConventionByName(...)`. Required (cross-field
    // rule in validate() below) whenever `processExit` is declared;
    // shipped values: "ms_x64" for PE-Exec, "sysv_amd64" for ELF/
    // Mach-O-Exec, "aapcs64" for ARM64.
    if (doc.contains("entryCallingConvention")) {
        if (!doc.at("entryCallingConvention").is_string()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      "/entryCallingConvention",
                      "'entryCallingConvention' must be a string");
        } else {
            auto const cc =
                doc.at("entryCallingConvention").get<std::string>();
            // dim-2 HIGH #2 (7425905 audit fold): non-whitespace
            // check. Without this, leading/trailing whitespace in a
            // hand-edited JSON would silently pass schema-load and
            // fail only at Slice C trampoline-build time via
            // `callingConventionByName()` returning nullptr.
            // Symmetric to the format.name discipline.
            bool const hasWs = std::any_of(cc.begin(), cc.end(),
                [](unsigned char c) { return std::isspace(c); });
            if (cc.empty() || hasWs) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          "/entryCallingConvention",
                          "'entryCallingConvention' must be a "
                          "non-empty string with no whitespace "
                          "(must resolve via `target."
                          "callingConventionByName(...)`).");
            } else {
                data.entryCallingConvention = cc;
            }
        }
    }

    // D-FFI-EXTERN-CALL-DISPATCH: `externCallDispatch` — the format's
    // extern-call shape ("indirect-slot" / "direct-plt"). Optional in
    // the JSON (a relocatable / WASM / SPIR-V format, or an exec format
    // built for a non-FFI purpose, omits it). validate() does NOT require
    // it — the real requirement ("a format that LOWERS an extern call
    // needs a dispatch shape") is enforced at MIR→LIR (the `Lowerer` ctor
    // guard fails loud on extern-imports-under-nullopt). Present-but-
    // unknown IS a fail-loud HERE at load (a typo must NOT silently fall
    // through to a default extern-call shape).
    if (doc.contains("externCallDispatch")) {
        if (!doc.at("externCallDispatch").is_string()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      "/externCallDispatch",
                      "'externCallDispatch' must be a string "
                      "(\"indirect-slot\" or \"direct-plt\")");
        } else {
            auto const s =
                doc.at("externCallDispatch").get<std::string>();
            auto const d = externCallDispatchFromName(s);
            if (!d.has_value()) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          "/externCallDispatch",
                          std::format("unknown externCallDispatch '{}' "
                                      "— accepted: \"indirect-slot\" "
                                      "(PE IAT / Mach-O __got), "
                                      "\"direct-plt\" (ELF PLT stub)",
                                      s));
            } else {
                data.externCallDispatch = *d;
            }
        }
    }

    // D-LK-EXTERN-DATA-IMPORT: `dataImportBinding` — the format's
    // extern-DATA import binding model ("copy-relocation"). Optional in
    // the JSON (a format whose data-import model has not landed — PE /
    // Mach-O / every relocatable flavor — omits it; the linker's pre-
    // walker gate then fails loud on any surviving data import instead
    // of binding a data symbol through the function-import machinery).
    // Present-but-unknown IS a fail-loud HERE at load (a typo must NOT
    // silently degrade to "no data imports supported" — the
    // externCallDispatch discipline).
    if (doc.contains("dataImportBinding")) {
        if (!doc.at("dataImportBinding").is_string()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      "/dataImportBinding",
                      "'dataImportBinding' must be a string "
                      "(\"copy-relocation\")");
        } else {
            auto const s =
                doc.at("dataImportBinding").get<std::string>();
            auto const b = dataImportBindingFromName(s);
            if (!b.has_value()) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          "/dataImportBinding",
                          std::format("unknown dataImportBinding '{}' "
                                      "— accepted: \"copy-relocation\" "
                                      "(ELF ET_EXEC R_*_COPY exec-local "
                                      ".bss copy)",
                                      s));
            } else {
                data.dataImportBinding = *b;
            }
        }
    }

    // D-CSUBSET-THREAD-LOCAL (TLS C1): `tlsAccess` block — the format's
    // thread-local access model + the x86 access-sequence values.
    // Optional in the JSON (a format whose TLS machinery has not landed
    // — PE / Mach-O / every relocatable flavor — omits it; MIR→LIR then
    // fails loud K_FormatLacksThreadLocalSupport on the first
    // thread-local access instead of silently lowering a process-shared
    // alias). A PRESENT block is strict — closed verb set + range
    // checks; a typo must NOT silently degrade to "no TLS support" (the
    // externCallDispatch discipline).
    if (doc.contains("tlsAccess")) {
        auto const& ta = doc.at("tlsAccess");
        if (!ta.is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/tlsAccess",
                      "'tlsAccess' must be an object { \"model\": "
                      "\"local-exec\"|\"pe-indexed\"|\"macho-tlv\", "
                      "\"segmentPrefixByte\": N, \"baseDisplacement\": N }");
        } else {
            TlsAccessInfo info{};
            bool ok = true;
            if (!ta.contains("model") || !ta.at("model").is_string()) {
                coll.emit(DiagnosticCode::C_MissingField, "/tlsAccess/model",
                          "'tlsAccess.model' is required and must be a "
                          "string — accepted: \"local-exec\" (ELF static "
                          "TLS), \"pe-indexed\" (PE TEB slot array), "
                          "\"macho-tlv\" (Mach-O TLV descriptor)");
                ok = false;
            } else {
                auto const s = ta.at("model").get<std::string>();
                auto const m = tlsAccessModelFromName(s);
                if (!m.has_value()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/tlsAccess/model",
                              std::format("unknown tlsAccess model '{}' — "
                                          "accepted: \"local-exec\", "
                                          "\"pe-indexed\", \"macho-tlv\"",
                                          s));
                    ok = false;
                } else {
                    info.model = *m;
                }
            }
            if (ta.contains("segmentPrefixByte")) {
                if (!ta.at("segmentPrefixByte").is_number_integer()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/tlsAccess/segmentPrefixByte",
                              "'segmentPrefixByte' must be an integer in "
                              "[0, 255]");
                    ok = false;
                } else {
                    std::int64_t const b =
                        ta.at("segmentPrefixByte").get<std::int64_t>();
                    if (b < 0 || b > 255) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/tlsAccess/segmentPrefixByte",
                                  std::format("'segmentPrefixByte' ({}) out "
                                              "of range [0, 255]", b));
                        ok = false;
                    } else {
                        info.segmentPrefixByte = static_cast<std::uint8_t>(b);
                    }
                }
            }
            if (ta.contains("baseDisplacement")) {
                if (!ta.at("baseDisplacement").is_number_integer()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/tlsAccess/baseDisplacement",
                              "'baseDisplacement' must be a non-negative "
                              "integer (the tp slot's disp32)");
                    ok = false;
                } else {
                    std::int64_t const d =
                        ta.at("baseDisplacement").get<std::int64_t>();
                    // The value rides an x86 SIGNED disp32 at encode time;
                    // cap at INT32_MAX so the u32→i32 handoff can never
                    // flip sign silently.
                    if (d < 0 || d > 0x7FFFFFFFLL) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/tlsAccess/baseDisplacement",
                                  std::format("'baseDisplacement' ({}) out "
                                              "of range [0, 2^31-1] (it is "
                                              "emitted as a signed disp32)",
                                              d));
                        ok = false;
                    } else {
                        info.baseDisplacement = static_cast<std::uint32_t>(d);
                    }
                }
            }
            // TLS C3 (D-CSUBSET-THREAD-LOCAL): the `_tls_index` slot NAME —
            // the writer-minted module-TLS-index singleton the `pe-indexed`
            // access sequence's riprel read targets. A string when present;
            // REQUIRED (non-empty) for the pe-indexed model below so a
            // pe-indexed format can never silently ship WITHOUT the slot its
            // access sequence indexes (the same closed-verb strictness the
            // model/segment/displacement fields hold). Ignored for
            // local-exec / macho-tlv (their tp reads index no module array).
            if (ta.contains("tlsIndexSlotName")) {
                if (!ta.at("tlsIndexSlotName").is_string()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/tlsAccess/tlsIndexSlotName",
                              "'tlsIndexSlotName' must be a string (the name "
                              "of the module TLS-index slot the pe-indexed "
                              "access sequence reads)");
                    ok = false;
                } else {
                    info.tlsIndexSlotName =
                        ta.at("tlsIndexSlotName").get<std::string>();
                }
            }
            if (ok && info.model == TlsAccessModel::PeIndexed
                && info.tlsIndexSlotName.empty()) {
                coll.emit(DiagnosticCode::C_MissingField,
                          "/tlsAccess/tlsIndexSlotName",
                          "'tlsIndexSlotName' is REQUIRED for the "
                          "'pe-indexed' TLS model — its access sequence "
                          "reads a named module TLS-index singleton "
                          "(`mov ecx, [_tls_index]`); a pe-indexed block "
                          "without it cannot lower a thread-local access");
                ok = false;
            }
            if (ok) data.tlsAccess = info;
        }
    }

    // D-CSUBSET-C11-THREADS-MACHO: `librarySynthesis` block — the format's
    // compiler-synthesized-shim vehicle (the kernel32 vs pthread primitive
    // family a synthesized shipped-library shim, today C11 <threads.h>,
    // emits over) + the native library its on-demand helpers import from.
    // Optional in the JSON: ELF omits it (glibc exports the C11 thread API
    // directly → the synth recipe map is empty on elf → clean no-op). A
    // PRESENT block is strict (closed verb set + non-empty path); a format
    // that carries synthesize-tagged threads symbols yet declares NO block
    // fails loud in `synthesizeThreadsShim` (never a silently-assumed
    // vehicle) — the same fail-loud-on-absence discipline as tlsAccess.
    if (doc.contains("librarySynthesis")) {
        auto const& ls = doc.at("librarySynthesis");
        if (!ls.is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/librarySynthesis",
                      "'librarySynthesis' must be an object { \"vehicle\": "
                      "\"win32\"|\"pthread\", \"libraryPath\": \"…\" }");
        } else {
            LibrarySynthesis info{};
            bool ok = true;
            if (!ls.contains("vehicle") || !ls.at("vehicle").is_string()) {
                coll.emit(DiagnosticCode::C_MissingField,
                          "/librarySynthesis/vehicle",
                          "'librarySynthesis.vehicle' is required and must be "
                          "a string — accepted: \"win32\" (kernel32 "
                          "CRITICAL_SECTION/CONDITION_VARIABLE/Fls*), "
                          "\"pthread\" (POSIX pthread_* / Darwin libSystem)");
                ok = false;
            } else {
                auto const s = ls.at("vehicle").get<std::string>();
                auto const v = librarySynthVehicleFromName(s);
                if (!v.has_value()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/librarySynthesis/vehicle",
                              std::format("unknown librarySynthesis vehicle "
                                          "'{}' — accepted: \"win32\", "
                                          "\"pthread\"",
                                          s));
                    ok = false;
                } else {
                    info.vehicle = *v;
                }
            }
            if (!ls.contains("libraryPath")
                || !ls.at("libraryPath").is_string()) {
                coll.emit(DiagnosticCode::C_MissingField,
                          "/librarySynthesis/libraryPath",
                          "'librarySynthesis.libraryPath' is required and must "
                          "be a string (the native library the synthesized "
                          "shim's helpers import from — e.g. \"kernel32.dll\" "
                          "or \"/usr/lib/libSystem.B.dylib\")");
                ok = false;
            } else {
                info.libraryPath = ls.at("libraryPath").get<std::string>();
                if (info.libraryPath.empty()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/librarySynthesis/libraryPath",
                              "'librarySynthesis.libraryPath' must be "
                              "non-empty");
                    ok = false;
                }
            }
            if (ok) data.librarySynthesis = info;
        }
    }

    // D-LK10-ENTRY Slice B: `processExit` block. Two arms keyed on
    // `mechanism`:
    //   "syscall"        requires syscallNumber (u32) +
    //                    syscallNumGpr (string) +
    //                    syscallOpcodeBytes (array of u8, non-empty).
    //   "by-name-import" requires importLibraryPath (string,
    //                    non-empty) + importMangledName (string,
    //                    non-empty).
    if (doc.contains("processExit")) {
        if (!doc.at("processExit").is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/processExit",
                      "'processExit' must be an object");
        } else {
            auto const& pe = doc.at("processExit");
            ProcessExit out;
            bool armOk = true;
            if (!pe.contains("mechanism")
             || !pe.at("mechanism").is_string()) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          "/processExit/mechanism",
                          "'processExit.mechanism' must be a string "
                          "(\"syscall\" or \"by-name-import\")");
                armOk = false;
            } else {
                auto const mechName =
                    pe.at("mechanism").get<std::string>();
                auto const m = exitMechanismFromName(mechName);
                if (!m.has_value() || *m == ExitMechanism::None) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/processExit/mechanism",
                              std::format("unknown processExit.mechanism"
                                          " '{}' — accepted: "
                                          "\"syscall\", \"by-name-"
                                          "import\"", mechName));
                    armOk = false;
                } else {
                    out.mechanism = *m;
                    // simplifier FOLD-NOW #1 (7425905 audit fold):
                    // collapse the 3 repeated `!contains || !is_string
                    // || .empty()` patterns into one lambda. Behavior
                    // unchanged; diagnostic text preserved.
                    auto requireNonEmptyString =
                        [&](char const* field, std::string& out) -> bool {
                            std::string const path =
                                std::string{"/processExit/"} + field;
                            if (!pe.contains(field)
                             || !pe.at(field).is_string()
                             || pe.at(field).get<std::string>().empty()) {
                                coll.emit(DiagnosticCode::C_MalformedJson,
                                          path,
                                          std::format(
                                              "requires non-empty '{}' "
                                              "(string)", field));
                                return false;
                            }
                            out = pe.at(field).get<std::string>();
                            return true;
                        };
                    if (out.mechanism == ExitMechanism::Syscall) {
                        if (!pe.contains("syscallNumber")
                         || !pe.at("syscallNumber").is_number_unsigned()) {
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      "/processExit/syscallNumber",
                                      "syscall arm requires "
                                      "'syscallNumber' (u32)");
                            armOk = false;
                        } else {
                            out.syscallNumber =
                                pe.at("syscallNumber").get<std::uint32_t>();
                        }
                        if (!requireNonEmptyString("syscallNumGpr",
                                                    out.syscallNumGpr)) {
                            armOk = false;
                        }
                        if (!pe.contains("syscallOpcodeBytes")
                         || !pe.at("syscallOpcodeBytes").is_array()
                         || pe.at("syscallOpcodeBytes").empty()) {
                            coll.emit(DiagnosticCode::C_MalformedJson,
                                      "/processExit/syscallOpcodeBytes",
                                      "syscall arm requires non-empty "
                                      "'syscallOpcodeBytes' (array of u8)");
                            armOk = false;
                        } else {
                            auto const& arr = pe.at("syscallOpcodeBytes");
                            for (std::size_t bi = 0; bi < arr.size(); ++bi) {
                                if (!arr[bi].is_number_unsigned()
                                 || arr[bi].get<std::uint64_t>() > 0xFFu) {
                                    coll.emit(DiagnosticCode::C_MalformedJson,
                                              std::format("/processExit/syscallOpcodeBytes/{}", bi),
                                              "each entry must be u8 (0..255)");
                                    armOk = false;
                                    continue;
                                }
                                out.syscallOpcodeBytes.push_back(
                                    static_cast<std::uint8_t>(
                                        arr[bi].get<std::uint32_t>()));
                            }
                        }
                    } else {  // ByNameImport
                        if (!requireNonEmptyString("importLibraryPath",
                                                    out.importLibraryPath)) {
                            armOk = false;
                        }
                        if (!requireNonEmptyString("importMangledName",
                                                    out.importMangledName)) {
                            armOk = false;
                        }
                    }
                }
            }
            if (armOk) {
                data.processExit = std::move(out);
            }
        }
    }

    // D-RUNTIME-MAIN-ARGC-ARGV (c88): `processArgs` block. One arm
    // keyed on `mechanism`:
    //   "stack-vector" requires argcStackOffset (u32) +
    //                  argvStackOffset (u32) — byte offsets from the
    //                  PROCESS-ENTRY stack pointer of the argc word
    //                  and the first argv slot. BOTH are explicit
    //                  (no silent defaults — a wrong offset reads
    //                  garbage argc, the exact failure this block
    //                  exists to close).
    if (doc.contains("processArgs")) {
        if (!doc.at("processArgs").is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/processArgs",
                      "'processArgs' must be an object");
        } else {
            auto const& pa = doc.at("processArgs");
            ProcessArgs out;
            bool armOk = true;
            if (!pa.contains("mechanism")
             || !pa.at("mechanism").is_string()) {
                coll.emit(DiagnosticCode::C_MalformedJson,
                          "/processArgs/mechanism",
                          "'processArgs.mechanism' must be a string "
                          "(\"stack-vector\")");
                armOk = false;
            } else {
                auto const mechName =
                    pa.at("mechanism").get<std::string>();
                auto const m = argsMechanismFromName(mechName);
                if (!m.has_value() || *m == ArgsMechanism::None) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              "/processArgs/mechanism",
                              std::format("unknown processArgs.mechanism"
                                          " '{}' — accepted: "
                                          "\"stack-vector\", "
                                          "\"crt-out-param\"", mechName));
                    armOk = false;
                } else if (*m == ArgsMechanism::StackVector) {
                    out.mechanism = *m;
                    // StackVector arm: both offsets explicit u32,
                    // bounded to int32 (they feed a MemOffset LIR
                    // operand, an int32 displacement).
                    auto requireOffset =
                        [&](char const* field, std::uint32_t& dst) -> bool {
                            std::string const path =
                                std::string{"/processArgs/"} + field;
                            if (!pa.contains(field)
                             || !pa.at(field).is_number_unsigned()) {
                                coll.emit(DiagnosticCode::C_MalformedJson,
                                          path,
                                          std::format(
                                              "stack-vector arm requires "
                                              "'{}' (u32 byte offset from "
                                              "the process-entry stack "
                                              "pointer)", field));
                                return false;
                            }
                            auto const v =
                                pa.at(field).get<std::uint64_t>();
                            if (v > 0x7FFFFFFFull) {
                                coll.emit(DiagnosticCode::C_MalformedJson,
                                          path,
                                          std::format(
                                              "'{}' = {} exceeds the "
                                              "int32 displacement range "
                                              "the trampoline's memory "
                                              "operand carries", field, v));
                                return false;
                            }
                            dst = static_cast<std::uint32_t>(v);
                            return true;
                        };
                    if (!requireOffset("argcStackOffset",
                                       out.argcStackOffset)) {
                        armOk = false;
                    }
                    if (!requireOffset("argvStackOffset",
                                       out.argvStackOffset)) {
                        armOk = false;
                    }
                } else {
                    // CrtOutParam arm (c111): three non-empty string fields —
                    // the wide + narrow msvcrt arg-fetch export names and the
                    // import library they resolve from. The synthesizer picks
                    // wide vs narrow by the resolved entry's signature.
                    out.mechanism = *m;
                    auto requireStr =
                        [&](char const* field, std::string& dst) -> bool {
                            std::string const path =
                                std::string{"/processArgs/"} + field;
                            if (!pa.contains(field)
                             || !pa.at(field).is_string()
                             || pa.at(field).get<std::string>().empty()) {
                                coll.emit(DiagnosticCode::C_MalformedJson,
                                          path,
                                          std::format(
                                              "crt-out-param arm requires a "
                                              "non-empty string '{}'", field));
                                return false;
                            }
                            dst = pa.at(field).get<std::string>();
                            return true;
                        };
                    if (!requireStr("crtWideArgvFn", out.crtWideArgvFn)) {
                        armOk = false;
                    }
                    if (!requireStr("crtNarrowArgvFn", out.crtNarrowArgvFn)) {
                        armOk = false;
                    }
                    if (!requireStr("crtLibraryPath", out.crtLibraryPath)) {
                        armOk = false;
                    }
                }
            }
            if (armOk) {
                data.processArgs = out;
            }
        }
    }

    // D-LK2-RODATA closure — `supportedDataSections`. Optional
    // top-level array of `DataSectionKind` names ("rodata" / "data" /
    // "bss" / "tdata" / "tbss" — the last two per D-CSUBSET-THREAD-
    // LOCAL) the format's walker accepts on `AssembledModule.
    // dataItems`. Absent / empty = walker rejects all producer-data-
    // section items (the format-side validate() rule below also
    // gates this on isImageFlavor — relocatable .obj cannot declare
    // the capability since rodata in .obj rides through the symbol
    // table, not the dataItems pipeline). Cross-format agnosticism:
    // adding a fourth executable format that supports rodata = drop
    // `"supportedDataSections": ["rodata"]` into its JSON; zero C++
    // changes in the linker substrate.
    if (doc.contains("supportedDataSections")) {
        if (!doc.at("supportedDataSections").is_array()) {
            coll.emit(DiagnosticCode::C_MalformedJson,
                      "/supportedDataSections",
                      "'supportedDataSections' must be an array of "
                      "DataSectionKind names (\"rodata\" / \"data\" / "
                      "\"bss\" / \"tdata\" / \"tbss\")");
        } else {
            auto const& arr = doc.at("supportedDataSections");
            std::size_t i = 0;
            for (auto const& elem : arr) {
                auto const path =
                    std::format("/supportedDataSections/{}", i);
                if (!elem.is_string()) {
                    coll.emit(DiagnosticCode::C_MalformedJson, path,
                              "must be a string DataSectionKind name");
                } else {
                    auto const name = elem.get<std::string>();
                    auto const k = dataSectionKindFromName(name);
                    if (!k.has_value()) {
                        coll.emit(DiagnosticCode::C_MalformedJson, path,
                                  std::format("unknown DataSectionKind "
                                              "'{}' (expected 'rodata' "
                                              "/ 'data' / 'bss' / "
                                              "'tdata' / 'tbss')",
                                              name));
                    } else {
                        bool dup = false;
                        for (auto existing : data.supportedDataSections) {
                            if (existing == *k) { dup = true; break; }
                        }
                        if (dup) {
                            coll.emit(DiagnosticCode::C_MalformedJson, path,
                                      std::format("duplicate "
                                                  "DataSectionKind '{}' "
                                                  "in supportedDataSections",
                                                  name));
                        } else {
                            data.supportedDataSections.push_back(*k);
                        }
                    }
                }
                ++i;
            }
        }
    }

    // artifactProfiles ── which profiles this format SERVES (plan 06 AP3;
    // the format-side symmetric twin of the language's `artifactProfiles[]`,
    // AP1). Each entry is validated against the SHARED registered vocabulary
    // (`isRegisteredArtifactProfile`) — a typo like "clii" fails loud here
    // (`C_UnknownArtifactProfile`) at its source rather than silently
    // mis-serving downstream. Absent ⇒ empty ⇒ serves no profile
    // (fail-closed). Zero C++ changes per new format — pure config; the
    // driver gate is a generic set-membership, never a profile-name branch.
    if (doc.contains("artifactProfiles")) {
        if (!doc.at("artifactProfiles").is_array()) {
            coll.emit(DiagnosticCode::C_UnknownArtifactProfile,
                      "/artifactProfiles",
                      "'artifactProfiles' must be an array of profile names");
        } else {
            auto const& arr = doc.at("artifactProfiles");
            std::size_t i = 0;
            for (auto const& elem : arr) {
                auto const path = std::format("/artifactProfiles/{}", i);
                if (!elem.is_string()) {
                    coll.emit(DiagnosticCode::C_UnknownArtifactProfile, path,
                              "each 'artifactProfiles' entry must be a string");
                } else {
                    auto const name = elem.get<std::string>();
                    if (!isRegisteredArtifactProfile(name)) {
                        coll.emit(DiagnosticCode::C_UnknownArtifactProfile, path,
                                  std::format("unknown artifact profile '{}' "
                                              "(registered profiles: {})",
                                              name, registeredArtifactProfileList()));
                    } else {
                        bool dup = false;
                        for (auto const& existing : data.artifactProfiles) {
                            if (existing == name) { dup = true; break; }
                        }
                        if (dup) {
                            coll.emit(DiagnosticCode::C_ConflictingField, path,
                                      std::format("artifact profile '{}' declared "
                                                  "more than once in artifactProfiles",
                                                  name));
                        } else {
                            data.artifactProfiles.push_back(name);
                        }
                    }
                }
                ++i;
            }
        }
    }

    // relocations[] — substrate-tier; shares the cross-side
    // `relocation_table.hpp` substrate with TargetSchema so the
    // `{name, kind}` shape of plan 13 §2.6's reloc-taxonomy unifier
    // is identical-by-construction on both sides. The `nativeId`
    // field is the format's wire tag (e.g. ELF R_X86_64_PC32 = 2).
    substrate::loadRelocationsTable<ObjectFormatRelocationInfo>(
        doc, data.relocations, data.relocationNameIndex,
        data.relocationKindIndex, coll,
        [](nlohmann::json const& r, ObjectFormatRelocationInfo& info,
           Collector& c, std::size_t i) -> bool {
            if (!r.contains("nativeId") || !r.at("nativeId").is_number_integer()) {
                c.emit(DiagnosticCode::C_MissingField,
                       std::format("/relocations/{}/nativeId", i),
                       "missing or non-integer 'nativeId' (format-specific "
                       "wire tag, e.g. ELF R_X86_64_PC32 = 2)");
                return false;
            }
            std::int64_t const v = r.at("nativeId").get<std::int64_t>();
            if (v <= 0 || v > 0xFFFFFFFFLL) {
                c.emit(DiagnosticCode::C_MalformedJson,
                       std::format("/relocations/{}/nativeId", i),
                       std::format("'nativeId' ({}) must be in (0, 2^32)", v));
                return false;
            }
            info.nativeId = static_cast<std::uint32_t>(v);
            return true;
        });

    // sections[] — D-LK4-2 schema row. Each entry maps a universal
    // SectionKind to format-native name + structural fields.
    if (doc.contains("sections")) {
        if (!doc.at("sections").is_array()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/sections",
                      "'sections' must be an array");
        } else {
            auto const& secs = doc.at("sections");
            data.sections.reserve(secs.size());
            for (std::size_t i = 0; i < secs.size(); ++i) {
                auto const& s = secs[i];
                if (!s.is_object()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/sections/{}", i),
                              "section entry must be an object");
                    continue;
                }
                ObjectFormatSectionInfo info;
                if (!s.contains("kind") || !s.at("kind").is_string()) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              std::format("/sections/{}/kind", i),
                              "missing or non-string 'kind' (one of 'text' "
                              "/ 'rodata' / 'data' / 'bss' / 'symtab' / "
                              "'strtab' / 'reloc' / 'dynamic' / 'note' / "
                              "'debug' / 'custom')");
                    continue;
                }
                auto const kOpt =
                    sectionKindFromName(s.at("kind").get<std::string>());
                if (!kOpt.has_value()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/sections/{}/kind", i),
                              "unknown SectionKind name");
                    continue;
                }
                info.kind = *kOpt;
                if (!s.contains("name") || !s.at("name").is_string()) {
                    coll.emit(DiagnosticCode::C_MissingField,
                              std::format("/sections/{}/name", i),
                              "missing or non-string 'name'");
                    continue;
                }
                info.name = s.at("name").get<std::string>();
                // `segment` is optional for ELF/PE (empty default);
                // Mach-O validate() rejects empty here.
                if (s.contains("segment")) {
                    if (!s.at("segment").is_string()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/sections/{}/segment", i),
                                  "'segment' must be a string");
                    } else {
                        info.segment = s.at("segment").get<std::string>();
                    }
                }
                auto readU64 = [&](char const* field, std::uint64_t& out) {
                    if (!s.contains(field)) return;
                    if (!s.at(field).is_number_integer()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/sections/{}/{}", i, field),
                                  std::format("'{}' must be a non-negative "
                                              "integer",
                                              field));
                        return;
                    }
                    std::int64_t const v = s.at(field).get<std::int64_t>();
                    if (v < 0) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  std::format("/sections/{}/{}", i, field),
                                  std::format("'{}' ({}) must be >= 0",
                                              field, v));
                        return;
                    }
                    out = static_cast<std::uint64_t>(v);
                };
                std::uint64_t typeRaw = 0;
                readU64("type", typeRaw);
                if (typeRaw > 0xFFFFFFFFu) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/sections/{}/type", i),
                              "'type' must fit in 32 bits");
                    continue;
                }
                info.type = static_cast<std::uint32_t>(typeRaw);
                readU64("flags", info.flags);
                readU64("addrAlign", info.addrAlign);
                readU64("entrySize", info.entrySize);
                readU64("virtualAddress", info.virtualAddress);
                std::uint16_t const idx =
                    static_cast<std::uint16_t>(data.sections.size());
                auto [it, fresh] =
                    data.sectionKindIndex.emplace(info.kind, idx);
                if (!fresh) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/sections/{}/kind", i),
                              std::format("duplicate section kind '{}'",
                                          std::string{sectionKindName(info.kind)}));
                    continue;
                }
                data.sections.push_back(std::move(info));
            }
        }
    }

    // Per-format identity sub-block readers — each runs only when
    // `format.kind` matches its arm. Mach-O (LK3) will add a third
    // arm on the same pattern.

    // ELF identity block — read only when format kind is Elf.
    if (data.kind == ObjectFormatKind::Elf && doc.contains("elf")) {
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
                if      (c == "elf32") data.elf.fileClass = 1;
                else if (c == "elf64") data.elf.fileClass = 2;
                else coll.emit(DiagnosticCode::C_MalformedJson, "/elf/class",
                               "'class' must be 'elf32' or 'elf64'");
            }
            // data: "lsb" | "msb".
            if (e.contains("data") && e.at("data").is_string()) {
                auto const d = e.at("data").get<std::string>();
                if      (d == "lsb") data.elf.dataEncoding = 1;
                else if (d == "msb") data.elf.dataEncoding = 2;
                else coll.emit(DiagnosticCode::C_MalformedJson, "/elf/data",
                               "'data' must be 'lsb' or 'msb'");
            }
            // osabi: string name → numeric (ELFOSABI_*). Default 0 = SysV.
            if (e.contains("osabi") && e.at("osabi").is_string()) {
                auto const o = e.at("osabi").get<std::string>();
                if      (o == "sysv"     || o == "none") data.elf.osabi = 0;
                else if (o == "hpux"   ) data.elf.osabi = 1;
                else if (o == "netbsd" ) data.elf.osabi = 2;
                else if (o == "gnu"    || o == "linux") data.elf.osabi = 3;
                else if (o == "freebsd") data.elf.osabi = 9;
                else coll.emit(DiagnosticCode::C_MalformedJson, "/elf/osabi",
                               "'osabi' must be one of 'sysv' / 'gnu' / "
                               "'freebsd' / 'netbsd' / 'hpux' / 'none'");
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
                              "'type' must be 'rel' / 'exec' / 'dyn'");
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

    // PE/COFF identity block — read only when format kind is Pe.
    if (data.kind == ObjectFormatKind::Pe && doc.contains("pe")) {
        auto const& p = doc.at("pe");
        if (!p.is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/pe",
                      "'pe' must be an object when format.kind == 'pe'");
        } else {
            auto readU16 = [&](char const* field, std::uint16_t& out,
                               std::int64_t max) {
                if (!p.contains(field) || !p.at(field).is_number_integer())
                    return;
                std::int64_t const v = p.at(field).get<std::int64_t>();
                if (v < 0 || v > max) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/pe/{}", field),
                              std::format("'{}' ({}) out of range [0, {}]",
                                          field, v, max));
                    return;
                }
                out = static_cast<std::uint16_t>(v);
            };
            readU16("machine", data.pe.machine, 0xFFFF);
            readU16("characteristics", data.pe.characteristics, 0xFFFF);
            // `type`: closed-enum PeObjectType (obj/exec/dll).
            // Default Obj keeps LK2 cycle 1 schemas unchanged.
            if (p.contains("type") && p.at("type").is_string()) {
                auto const tName = p.at("type").get<std::string>();
                auto const tEnum = peObjectTypeFromName(tName);
                if (tEnum.has_value()) {
                    data.pe.objectType = *tEnum;
                } else {
                    coll.emit(DiagnosticCode::C_MalformedJson, "/pe/type",
                              "'type' must be 'obj' / 'exec' / 'dll'");
                }
            }
        }
    }

    // PE32+ Optional Header — read only when PE objectType != Obj.
    // The walker emits the optional header for Exec/Dll; Obj schemas
    // never carry it, and validate() rejects an `optionalHeader` key
    // on an Obj schema as a load-time config error (symmetric with
    // ELF ET_REL's virtualAddress=0 rejection).
    if (data.kind == ObjectFormatKind::Pe && doc.contains("optionalHeader")) {
        auto const& oh = doc.at("optionalHeader");
        if (!oh.is_object()) {
            coll.emit(DiagnosticCode::C_MalformedJson, "/optionalHeader",
                      "'optionalHeader' must be an object");
        } else {
            auto readU16 = [&](char const* field, std::uint16_t& out) {
                if (!oh.contains(field)) return;
                if (!oh.at(field).is_number_integer()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/optionalHeader/{}", field),
                              std::format("'{}' must be an integer", field));
                    return;
                }
                std::int64_t const v = oh.at(field).get<std::int64_t>();
                if (v < 0 || v > 0xFFFF) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/optionalHeader/{}", field),
                              std::format("'{}' ({}) out of u16 range",
                                          field, v));
                    return;
                }
                out = static_cast<std::uint16_t>(v);
            };
            auto readU32 = [&](char const* field, std::uint32_t& out) {
                if (!oh.contains(field)) return;
                if (!oh.at(field).is_number_integer()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/optionalHeader/{}", field),
                              std::format("'{}' must be an integer", field));
                    return;
                }
                std::int64_t const v = oh.at(field).get<std::int64_t>();
                if (v < 0 || v > 0xFFFFFFFFLL) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/optionalHeader/{}", field),
                              std::format("'{}' ({}) out of u32 range",
                                          field, v));
                    return;
                }
                out = static_cast<std::uint32_t>(v);
            };
            auto readU64 = [&](char const* field, std::uint64_t& out) {
                if (!oh.contains(field)) return;
                if (!oh.at(field).is_number_integer()) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/optionalHeader/{}", field),
                              std::format("'{}' must be an integer", field));
                    return;
                }
                std::int64_t const v = oh.at(field).get<std::int64_t>();
                if (v < 0) {
                    coll.emit(DiagnosticCode::C_MalformedJson,
                              std::format("/optionalHeader/{}", field),
                              std::format("'{}' ({}) must be non-negative",
                                          field, v));
                    return;
                }
                out = static_cast<std::uint64_t>(v);
            };
            readU16("magic", data.peOptionalHeader.magic);
            readU64("imageBase", data.peOptionalHeader.imageBase);
            readU32("sectionAlignment",
                    data.peOptionalHeader.sectionAlignment);
            readU32("fileAlignment", data.peOptionalHeader.fileAlignment);
            readU16("majorOperatingSystemVersion",
                    data.peOptionalHeader.majorOperatingSystemVersion);
            readU16("minorOperatingSystemVersion",
                    data.peOptionalHeader.minorOperatingSystemVersion);
            readU16("majorSubsystemVersion",
                    data.peOptionalHeader.majorSubsystemVersion);
            readU16("minorSubsystemVersion",
                    data.peOptionalHeader.minorSubsystemVersion);
            readU16("subsystem", data.peOptionalHeader.subsystem);
            readU16("dllCharacteristics",
                    data.peOptionalHeader.dllCharacteristics);
            readU64("sizeOfStackReserve",
                    data.peOptionalHeader.sizeOfStackReserve);
            readU64("sizeOfStackCommit",
                    data.peOptionalHeader.sizeOfStackCommit);
            readU64("sizeOfHeapReserve",
                    data.peOptionalHeader.sizeOfHeapReserve);
            readU64("sizeOfHeapCommit",
                    data.peOptionalHeader.sizeOfHeapCommit);
            // Plan 14 LK7 — Authenticode codesign placeholder
            // reservation. Optional; defaults to 0 (no reservation,
            // no security directory entry). Multiple-of-8 enforced
            // at validate() (PE COFF §5.9.1 alignment).
            readU32("attributeCertReserveSize",
                    data.peOptionalHeader.attributeCertReserveSize);
        }
    }

    // Mach-O identity block — read only when format kind is MachO.
    if (data.kind == ObjectFormatKind::MachO && doc.contains("macho")) {
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
    if (data.kind == ObjectFormatKind::MachO && doc.contains("image")) {
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
                    // platform (closed enum; required).
                    if (!bv.contains("platform")
                     || !bv.at("platform").is_string()) {
                        coll.emit(DiagnosticCode::C_MalformedJson,
                                  "/image/buildVersion/platform",
                                  "'platform' is required and must be a "
                                  "string (\"macos\").");
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
                                                  "accepted: \"macos\" "
                                                  "(PLATFORM_MACOS).", s));
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

    for (auto&& problem : data.validate()) {
        coll.emitRaw(std::move(problem));
    }

    if (coll.hasErrors()) {
        return std::unexpected(std::move(coll).release());
    }

    return std::make_shared<ObjectFormatSchema>(std::move(data));
}

} // namespace dss
