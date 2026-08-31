#include "opt/optimizer.hpp"

#include "core/substrate/checked_file_read.hpp"   // the ONE checked whole-file read
#include "core/substrate/diagnostic_collector.hpp"
#include "core/types/config_document_parse.hpp"  // THE ONE config-document parse
#include "core/types/config_key_vocabulary.hpp"  // the ONE closed-key check + the `$` carve-out
#include "core/types/config_path_walk.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// `*.pipeline.json` loader. Mirrors the 7-step shape shared by every
// shipped config (`TargetSchema::loadFromText` / `GrammarSchema` / ...):
// parse → version → required → optional → enum-resolve → validate → return.
//
// Schema (D-OPT1-PIPELINE-FROM-CONFIG; schedule grammar = P10 operator
// ruling, 2026-08-18):
//   { "dssPipelineVersion": 1,
//     "pipeline": {
//       "name": "<str>",
//       "passes": [ <element>, ... ]        — element =
//           "<PassId-name>"                                 (leaf)
//         | {"repeat":   {"count": N, "passes": [element,...]}}
//         | {"fixpoint": {"max":  N, "passes": [element,...]}}
//       "maxIterations": <1..32>,           — FLAT spelling only; refused
//                                              beside structural nodes
//       "inlineThreshold": <1..100000>,
//       "inlineCallerGrowthPercent": <0..100000>,
//       "verifyEveryPass": <bool> } }
//
// FLAT documents stay valid (all-string `passes` + optional top-level
// `maxIterations` desugar to one top-level fixpoint). Every document
// NORMALIZES to a root Fixpoint (see parsePipelineDoc). Load-time
// budgets, fail-loud: count/max ∈ [1,32]; nesting depth (combinator
// levels, root counts as 1) ≤ 8; total worst-case unrolled invocations
// ≤ 4096; empty bodies rejected before cost evaluation.
//
// Unknown sub-keys under `pipeline` (and under any node object) are
// rejected (D-CONFIG-LOADER-UNKNOWN-KEYS-FAIL-LOUD). Unknown top-level
// keys are also rejected so a typo in `dssPipelineVersion` doesn't
// silently load with the default.

namespace dss::opt {

namespace {
using json = nlohmann::json;

void emitMalformed(substrate::DiagnosticCollector& coll,
                   std::string path, std::string what) {
    coll.emit(DiagnosticCode::X_PipelineMalformed,
              std::move(path), std::move(what));
}

// D-CONFIG-LOADER-UNKNOWN-KEYS-FAIL-LOUD enforcement for any JSON
// object whose key set is fully closed. Emits X_PipelineMalformed
// (with object-path context) for every key not in the allow-list.
//
// ★★ AN ADAPTER OVER THE SHARED CHECK, AND THE MOVE FIXED A LIVE BUG HERE.
// This was one of four independently hand-written `rejectUnknownKeys`
// helpers, and it applied NO `$`-documentation carve-out at all — so a
// `$comment` (or any `$…Comment`) in a pipeline document was REFUSED as a
// typo, in a codebase whose stated convention is that any config object may
// carry one. The inverse of a silent drop, and just as wrong. The carve-out
// is now unskippable because the caller no longer writes the loop.
void rejectUnknownKeys(substrate::DiagnosticCollector& coll,
                       nlohmann::json const& obj,
                       std::string const& objPath,
                       std::string_view objectLabel,
                       std::initializer_list<std::string_view> allowed) {
    detail::rejectUnknownKeys(obj, allowed, objectLabel,
        [&](std::string_view, std::string message) {
            // The anchor id stays in the text: it is how this rule is found
            // from a failing document, and the shared sentence cannot carry
            // a per-loader anchor.
            message += " (D-CONFIG-LOADER-UNKNOWN-KEYS-FAIL-LOUD)";
            emitMalformed(coll, objPath, std::move(message));
        });
}

[[nodiscard]] LoadResult<OptPipeline>
parsePipelineDoc(json const& doc, std::string_view sourceLabel) {
    substrate::DiagnosticCollector coll;

    if (!doc.is_object()) {
        emitMalformed(coll, std::string{sourceLabel},
                      "top-level value must be a JSON object");
        return std::unexpected(std::move(coll).release());
    }

    // Step 2: version gate.
    if (!doc.contains("dssPipelineVersion")
     || !doc.at("dssPipelineVersion").is_number_integer()) {
        coll.emit(DiagnosticCode::X_PipelineVersionMismatch,
                  std::string{sourceLabel},
                  "missing or non-integer 'dssPipelineVersion'");
        return std::unexpected(std::move(coll).release());
    }
    int const ver = doc.at("dssPipelineVersion").get<int>();
    if (ver != 1) {
        coll.emit(DiagnosticCode::X_PipelineVersionMismatch,
                  "/dssPipelineVersion",
                  std::format("only version 1 supported (got {})", ver));
        return std::unexpected(std::move(coll).release());
    }

    // Step 3: required `pipeline` object.
    if (!doc.contains("pipeline") || !doc.at("pipeline").is_object()) {
        emitMalformed(coll, std::string{sourceLabel},
                      "missing 'pipeline' object");
        return std::unexpected(std::move(coll).release());
    }
    json const& pipe = doc.at("pipeline");

    rejectUnknownKeys(coll, doc, std::string{sourceLabel},
                      "the pipeline document",
                      {"dssPipelineVersion", "pipeline", "unitPipeline"});

    // P10 stage topology: optional top-level `unitPipeline` — the NAME of
    // the pipeline document that runs at the per-CU (Unit) stage instead
    // of this one (D-OPT7-CROSSCU-LTO-SINGLE-OPTIMIZE). A name, never an
    // embedded schedule; the stage routing + the unknown-name failure live
    // in `optimizeModule` (it can actually resolve the name against the
    // shipped set). Non-string or EMPTY are refused here — an empty name
    // is a typo away from "run nothing at the unit stage" and would read
    // as the absent-key default in every log.
    if (doc.contains("unitPipeline")) {
        if (!doc.at("unitPipeline").is_string()
            || doc.at("unitPipeline").get<std::string>().empty()) {
            emitMalformed(coll, std::string{sourceLabel},
                          "'unitPipeline' must be a non-empty pipeline "
                          "document NAME (the per-CU stage's schedule; the "
                          "Program stage always runs this document)");
            return std::unexpected(std::move(coll).release());
        }
    }

    // `pipeline.name`.
    if (!pipe.contains("name") || !pipe.at("name").is_string()) {
        emitMalformed(coll, std::string{sourceLabel} + "/pipeline",
                      "missing or non-string 'name'");
        return std::unexpected(std::move(coll).release());
    }
    std::string const name = pipe.at("name").get<std::string>();

    // `pipeline.passes` — the SCHEDULE TREE's top sequence (P10
    // operator ruling, 2026-08-18). An element is either a pass-name
    // string (leaf, resolved against kPassNameTable) or a node object
    // — exactly one of:
    //   {"repeat":   {"count": N, "passes": [...]}}   bounded unroll
    //   {"fixpoint": {"max":  N, "passes": [...]}}    rerun to
    //                                                  convergence
    // NO seq node (the array is the sequence), NO conditionals, NO
    // per-step pass params, NO parallel node — unknown kinds/keys fail
    // loud here.
    if (!pipe.contains("passes") || !pipe.at("passes").is_array()) {
        emitMalformed(coll, std::string{sourceLabel} + "/pipeline",
                      "missing or non-array 'passes'");
        return std::unexpected(std::move(coll).release());
    }

    // Parse one `passes` element. `combDepth` = the number of
    // combinator (repeat/fixpoint) nodes ENCLOSING this element's
    // position, counting the normalized top-level Fixpoint root as 1 —
    // a structural element therefore SITS at depth combDepth + 1, and
    // the load-time depth budget (≤ kMaxPipelineDepth) is enforced
    // here, at the node, before any recursion into its body.
    std::function<std::optional<OptPipelineNode>(json const&, std::string,
                                                 unsigned)>
    parseElement = [&](json const& el, std::string path,
                       unsigned combDepth) -> std::optional<OptPipelineNode> {
        if (el.is_string()) {
            auto const resolved = optPassIdFromName(el.get<std::string>());
            if (!resolved.has_value()) {
                coll.emit(DiagnosticCode::X_UnknownPassName, std::move(path),
                          std::format("unknown PassId name '{}' "
                                      "(not in optPassIdFromName)",
                                      el.get<std::string>()));
                return std::nullopt;
            }
            return OptPipelineNode::leaf(*resolved);
        }
        if (!el.is_object()) {
            emitMalformed(coll, std::move(path),
                          "pass entry must be a string pass name or a "
                          "pipeline node object");
            return std::nullopt;
        }
        bool const hasRepeat   = el.contains("repeat");
        bool const hasFixpoint = el.contains("fixpoint");
        if (hasRepeat == hasFixpoint) {
            emitMalformed(coll, std::move(path),
                          hasRepeat
                              ? "a node object must declare exactly ONE of "
                                "'repeat' / 'fixpoint', not both"
                              : "unknown pipeline node kind — a node object "
                                "must declare exactly one of 'repeat' / "
                                "'fixpoint' (no seq/conditional/parallel "
                                "node exists)");
            return std::nullopt;
        }
        char const* const nodeKind = hasRepeat ? "repeat" : "fixpoint";
        rejectUnknownKeys(coll, el, path, "a pipeline node", {nodeKind});
        json const& inner = el.at(nodeKind);
        if (!inner.is_object()) {
            emitMalformed(coll, path + "/" + nodeKind,
                          std::format("'{}' value must be an object", nodeKind));
            return std::nullopt;
        }
        rejectUnknownKeys(coll, inner, path + "/" + nodeKind,
                          hasRepeat ? "a 'repeat' node body"
                                    : "a 'fixpoint' node body",
                          {std::string_view{hasRepeat ? "count" : "max"},
                           std::string_view{"passes"}});

        // Depth budget (fail-loud at load). The node itself sits at
        // combDepth + 1; anything past kMaxPipelineIterations-deep
        // nesting is a pathological schedule, not a pipeline.
        unsigned const nodeDepth = combDepth + 1;
        if (nodeDepth > kMaxPipelineDepth) {
            emitMalformed(coll, std::move(path),
                          std::format("nesting depth {} exceeds the budget of "
                                      "{} combinator levels (the normalized "
                                      "top-level fixpoint counts as 1)",
                                      nodeDepth,
                                      static_cast<int>(kMaxPipelineDepth)));
            return std::nullopt;
        }

        // count / max — same bounds discipline as the flat document's
        // top-level maxIterations: [1, kMaxPipelineIterations]. 0 is a
        // silent no-op trap; > 32 signals non-convergence or
        // pathological input.
        std::uint32_t bound = 0;
        char const* const boundKey = hasRepeat ? "count" : "max";
        if (!inner.contains(boundKey)
         || !inner.at(boundKey).is_number_integer()) {
            emitMalformed(coll, path + "/" + nodeKind,
                          std::format("missing or non-integer '{}'", boundKey));
        } else {
            auto const v = inner.at(boundKey).get<std::int64_t>();
            if (v < 1 || v > kMaxPipelineIterations) {
                emitMalformed(coll, path + "/" + nodeKind + "/" + boundKey,
                              std::format("must be in [1, {}] (got {})",
                                          static_cast<int>(
                                              kMaxPipelineIterations),
                                          v));
            } else {
                bound = static_cast<std::uint32_t>(v);
            }
        }

        // Body — rejected EMPTY before any cost evaluation (existing
        // empty-pipeline discipline: an empty body would run nothing
        // and read as "an optimizer ran").
        if (!inner.contains("passes") || !inner.at("passes").is_array()) {
            emitMalformed(coll, path + "/" + nodeKind,
                          "missing or non-array 'passes' body");
            return std::nullopt;
        }
        std::vector<OptPipelineNode> body;
        bool bodyOk = true;
        std::size_t cIdx = 0;
        for (auto const& child : inner.at("passes")) {
            auto parsed = parseElement(
                child, std::format("{}/{}/passes[{}]", path, nodeKind, cIdx),
                nodeDepth);
            if (parsed.has_value()) {
                body.push_back(std::move(*parsed));
            } else {
                bodyOk = false;
            }
            ++cIdx;
        }
        if (!bodyOk) return std::nullopt;
        if (body.empty()) {
            emitMalformed(coll, path + "/" + nodeKind + "/passes",
                          "'passes' body must contain at least one entry; "
                          "use [\"Identity\"] for an explicit no-op");
            return std::nullopt;
        }
        return hasRepeat ? OptPipelineNode::repeat(bound, std::move(body))
                         : OptPipelineNode::fixpoint(bound, std::move(body));
    };

    std::vector<OptPipelineNode> elements;
    bool anyStructural = false;
    {
        std::size_t idx = 0;
        bool ok = true;
        for (auto const& el : pipe.at("passes")) {
            auto parsed = parseElement(
                el, std::format("{}/pipeline/passes[{}]", sourceLabel, idx), 1);
            if (parsed.has_value()) {
                anyStructural = anyStructural
                             || parsed->kind != OptPipelineNode::Kind::Leaf;
                elements.push_back(std::move(*parsed));
            } else {
                ok = false;
            }
            ++idx;
        }
        if (!ok) return std::unexpected(std::move(coll).release());
    }

    // Whole-pipeline fixed-point loop (D-OPT-FIXED-POINT-LOOP +
    // D-OPT1-PASS-RUN-MAX-ITER), the FLAT spelling. Optional. Default
    // = 1 (single iteration — historical behavior). Rejected outside
    // [1, kMaxPipelineIterations]: 0 is a silent-no-op trap, and
    // values > 32 indicate non-convergence or pathological input
    // (every realistic mutually-enabling cluster converges in
    // < log(blockCount) iterations).
    std::uint8_t maxIterations = 1;
    bool sawMaxIterations = false;
    if (pipe.contains("maxIterations")) {
        sawMaxIterations = true;
        if (!pipe.at("maxIterations").is_number_integer()) {
            emitMalformed(coll, std::string{sourceLabel} + "/pipeline/maxIterations",
                          "must be an integer");
        } else {
            auto const v = pipe.at("maxIterations").get<std::int64_t>();
            if (v < 1 || v > kMaxPipelineIterations) {
                emitMalformed(coll,
                    std::string{sourceLabel} + "/pipeline/maxIterations",
                    std::format("must be in [1, {}] (got {})",
                                static_cast<int>(kMaxPipelineIterations), v));
            } else {
                maxIterations = static_cast<std::uint8_t>(v);
            }
        }
    }

    // Inline COST bound (OPT7 cycle 28). Optional. Default =
    // `kDefaultInlineThreshold` (absent → the size cap that ships in
    // `release.pipeline.json`). Mirrors the `maxIterations` parse:
    // rejected outside [1, kMaxInlineThreshold] (0 is a silent
    // refuse-all trap; the upper cap is a pathological-size sanity
    // bound) → X_PipelineMalformed via `emitMalformed`. A non-integer
    // is likewise X_PipelineMalformed.
    std::uint32_t inlineThreshold = kDefaultInlineThreshold;
    if (pipe.contains("inlineThreshold")) {
        if (!pipe.at("inlineThreshold").is_number_integer()) {
            emitMalformed(coll,
                          std::string{sourceLabel} + "/pipeline/inlineThreshold",
                          "must be an integer");
        } else {
            auto const v = pipe.at("inlineThreshold").get<std::int64_t>();
            if (v < 1 || v > static_cast<std::int64_t>(kMaxInlineThreshold)) {
                emitMalformed(coll,
                    std::string{sourceLabel} + "/pipeline/inlineThreshold",
                    std::format("must be in [1, {}] (got {})",
                                kMaxInlineThreshold, v));
            } else {
                inlineThreshold = static_cast<std::uint32_t>(v);
            }
        }
    }

    // PER-CALLER CUMULATIVE GROWTH BUDGET (P36 Lane R). Optional. Default =
    // `kDefaultInlineCallerGrowthPercent`. Same parse shape as
    // `inlineThreshold` with ONE deliberate difference: the lower bound is
    // 0, NOT 1. `inlineThreshold: 0` is rejected because it silently refuses
    // every inline; `inlineCallerGrowthPercent: 0` refuses nothing on its
    // own — the allowance floors at `inlineThreshold` instructions — so 0 is
    // a real posture ("grow each caller by at most one maximal callee"), not
    // a trap. Out of [0, kMaxInlineCallerGrowthPercent], or a non-integer →
    // X_PipelineMalformed via `emitMalformed`.
    std::uint32_t inlineCallerGrowthPercent = kDefaultInlineCallerGrowthPercent;
    if (pipe.contains("inlineCallerGrowthPercent")) {
        if (!pipe.at("inlineCallerGrowthPercent").is_number_integer()) {
            emitMalformed(coll,
                          std::string{sourceLabel}
                              + "/pipeline/inlineCallerGrowthPercent",
                          "must be an integer");
        } else {
            auto const v =
                pipe.at("inlineCallerGrowthPercent").get<std::int64_t>();
            if (v < 0
                || v > static_cast<std::int64_t>(kMaxInlineCallerGrowthPercent)) {
                emitMalformed(coll,
                    std::string{sourceLabel}
                        + "/pipeline/inlineCallerGrowthPercent",
                    std::format("must be in [0, {}] (got {})",
                                kMaxInlineCallerGrowthPercent, v));
            } else {
                inlineCallerGrowthPercent = static_cast<std::uint32_t>(v);
            }
        }
    }

    // Verify frequency (D-OPT1-VERIFY-FREQUENCY-CONFIG). Optional bool; default
    // true = verify after EVERY pass (the developer posture: LLVM `-verify-each`
    // / GCC `--enable-checking=yes` — pinpoints the offending pass). false =
    // verify ONCE at pipeline end (the release/production posture — trust tested
    // passes, verify before codegen). Non-bool → X_PipelineMalformed.
    bool verifyEveryPass = true;
    if (pipe.contains("verifyEveryPass")) {
        if (!pipe.at("verifyEveryPass").is_boolean()) {
            emitMalformed(coll,
                          std::string{sourceLabel} + "/pipeline/verifyEveryPass",
                          "must be a boolean");
        } else {
            verifyEveryPass = pipe.at("verifyEveryPass").get<bool>();
        }
    }

    rejectUnknownKeys(coll, pipe, std::string{sourceLabel} + "/pipeline",
                      "the 'pipeline' block",
                      {"name", "passes", "maxIterations", "inlineThreshold",
                       "inlineCallerGrowthPercent", "verifyEveryPass"});

    // ★ ONE SPELLING PER DOCUMENT (P10 ruling): a document using ANY
    // structural node (repeat/fixpoint) REFUSES top-level
    // `maxIterations` — the two spellings of "rerun this" would
    // otherwise compose silently (fixpoint caps multiplying through
    // the wrapper), and a reader could not tell which bound governs.
    if (sawMaxIterations && anyStructural) {
        emitMalformed(coll,
                      std::string{sourceLabel} + "/pipeline/maxIterations",
                      "'maxIterations' cannot be combined with structural "
                      "nodes ('repeat'/'fixpoint') — one spelling per "
                      "document: EITHER flat 'passes' + top-level "
                      "'maxIterations' OR structural 'repeat'/'fixpoint' "
                      "nodes carrying their own 'count'/'max', never both");
    }

    // Empty pipeline = silent no-op at the optimizer engine. Reject
    // at load-time so a stray `"passes": []` doesn't ship a build
    // that thinks optimization happened but ran zero passes.
    if (elements.empty()) {
        emitMalformed(coll, std::string{sourceLabel} + "/pipeline/passes",
                      "'passes' array must contain at least one PassId; "
                      "use [\"Identity\"] for an explicit no-op pipeline");
    }
    if (coll.hasErrors()) {
        return std::unexpected(std::move(coll).release());
    }

    // ★ NORMALIZATION: every document becomes a top-level
    // Fixpoint{N ≥ 1, [...]} — uniform interpreter entry, uniform
    // `iter=` numbering. Flat documents take the whole-pipeline bound;
    // structural documents wrap in N=1 — EXCEPT the collapse: a
    // document whose ENTIRE `passes` is one `fixpoint` node uses THAT
    // node as the root, so the structural `fixpoint{max:4,[…]}` shape
    // desugars to exactly its flat `maxIterations:4` twin's schedule
    // (identical tree, identical counters, depth-1 trace).
    OptPipelineNode root;
    if (!anyStructural) {
        root = OptPipelineNode::fixpoint(maxIterations, std::move(elements));
    } else if (elements.size() == 1
            && elements[0].kind == OptPipelineNode::Kind::Fixpoint) {
        root = std::move(elements[0]);
    } else {
        root = OptPipelineNode::fixpoint(1, std::move(elements));
    }

    // ★ LOAD-TIME BUDGET (fail-loud): total worst-case unrolled
    // invocations, from the ONE shared static cost function (leaf=1,
    // sequence=Σ, Repeat{n,b}=n·cost(b), Fixpoint{m,b}=m·cost(b)).
    // Nested repeat bounds multiply — a schedule that could unroll to
    // more than kMaxPipelineInvocations pass runs is pathological, not
    // a pipeline. (Depth was enforced per-node during the parse, and
    // empty bodies were rejected BEFORE this evaluation.)
    if (auto const total = optPipelineCost(root);
        total > kMaxPipelineInvocations) {
        emitMalformed(coll, std::string{sourceLabel} + "/pipeline/passes",
                      std::format("total worst-case pass invocations exceed "
                                  "the budget of {} (nested 'repeat'/"
                                  "'fixpoint' bounds multiply)",
                                  static_cast<int>(kMaxPipelineInvocations)));
        return std::unexpected(std::move(coll).release());
    }

    OptPipeline out;
    out.name             = std::move(name);
    out.schedule         = std::move(root);
    out.inlineThreshold  = inlineThreshold;
    out.inlineCallerGrowthPercent = inlineCallerGrowthPercent;
    out.verifyEveryPass  = verifyEveryPass;
    if (doc.contains("unitPipeline")) {
        out.unitPipelineName = doc.at("unitPipeline").get<std::string>();
    }
    return out;
}

} // namespace

LoadResult<OptPipeline>
loadPipelineFromText(std::string_view jsonText, std::string_view sourceLabel) {
    substrate::DiagnosticCollector coll;
    // THE ONE CONFIG PARSE (`core/types/config_document_parse.hpp`) —
    // D-CONFIG-A-DUPLICATE-JSON-KEY-IS-DROPPED-WITHOUT-A-DIAGNOSTIC. A
    // `.pipeline.json` declaring `schedule` or `inlineThreshold` twice used to
    // run the LAST one silently, which is a different optimizer over the same
    // source.
    auto parsed = detail::parseConfigDocument(jsonText);
    if (!parsed) {
        coll.emit(DiagnosticCode::C_MalformedJson,
                  parsed.error().locus(sourceLabel),
                  parsed.error().detailText("JSON parse error: "));
        return std::unexpected(std::move(coll).release());
    }
    json doc = std::move(*parsed);
    return parsePipelineDoc(doc, sourceLabel);
}

LoadResult<OptPipeline>
loadShippedPipeline(std::string_view name) {
    auto pathR = findShippedConfig({
        name, "pipelines", ".pipeline.json", "pipeline",
        DiagnosticCode::X_PipelineNameResolutionFailed});
    if (!pathR.has_value()) {
        return std::unexpected(std::move(pathR).error());
    }
    auto const path = pathR.value();
    // THE ONE CHECKED READ (D-CORE-SHIPPED-CONFIG-LOADERS-DRAIN-A-STREAM-WITHOUT-CHECKING-IT).
    // ★ This site also opened in TEXT mode, alone among the shipped-config
    // loaders. On Windows that silently translated CRLF, so the bytes the parser
    // saw were not the bytes on disk — harmless for JSON, but it made this the
    // one loader whose read could not be size-checked at all. The shared helper
    // is unconditionally binary.
    auto text = core::readFileChecked(path);
    if (!text) {
        substrate::DiagnosticCollector coll;
        coll.emit(DiagnosticCode::X_PipelineNameResolutionFailed,
                  path.string(),
                  "pipeline file: " + std::move(text).error().message);
        return std::unexpected(std::move(coll).release());
    }
    return loadPipelineFromText(*std::move(text), path.string());
}

} // namespace dss::opt
