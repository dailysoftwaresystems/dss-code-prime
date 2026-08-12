#include "program/dump_predefined_macros.hpp"

#include "analysis/preprocess/preprocessor.hpp"   // mergePredefinedMacros — THE owner
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"
#include "program/target_spec.hpp"

#include <array>
#include <ostream>
#include <string>
#include <vector>

namespace dss {

namespace {

// Render ONE fail-loud line in the shape the driver's buffer-less diagnostic
// drain produces: `error[<CodeName>] <message>`.
//
// ★ WHY NOT A `DiagnosticReporter`. A diagnostic is DROPPABLE through three
// independent gates in `DiagnosticReporter::report` — `--suppress` naming the
// code, the per-code cap, and the global cap, after which further reports are
// discarded in silence. Any one of them would let this flag print an EMPTY
// answer and still look like it worked, which is the precise failure the dump
// exists to rule out (`C_ConflictingPredefinedMacro` is NOT in
// `kUnsuppressableCodes`, so `--suppress=C_ConflictingPredefinedMacro` really
// would silence it). Same argument the driver's `reportArtifactWritten` makes
// for being a report line rather than an `info[...]` diagnostic.
//
// The CODE SPELLING and the SEVERITY SPELLING still have exactly one owner
// (`diagnosticCodeName` / `severityName`), so nothing about the shape is
// duplicated here beyond the `<<` chain itself.
void emitLoud(std::ostream& err, DiagnosticCode code, std::string_view message) {
    err << severityName(DiagnosticSeverity::Error) << '['
        << diagnosticCodeName(code) << "] " << message << '\n';
}

// Forward a failed `loadShipped`'s own diagnostics, then the driver-tier
// "which file to look at" line. Mirrors the pair of emits every `loadShipped`
// call site in the driver performs; the loader's messages carry the JSON path
// or the searched file name, which is the actionable half.
void emitLoadFailure(std::ostream&                     err,
                     std::span<ConfigDiagnostic const> diags,
                     DiagnosticCode                    driverCode,
                     std::string_view                  summary) {
    for (ConfigDiagnostic const& d : diags) {
        err << severityName(d.severity) << '[' << diagnosticCodeName(d.code)
            << "] " << d.path << ": " << d.message << '\n';
    }
    emitLoud(err, driverCode, summary);
}

// One rendered line. `value` is written LAST (see the header's LINE SHAPE note)
// so a spelling containing spaces stays the unambiguous remainder.
void appendLine(std::string&          out,
                PredefinedMacroOrigin origin,
                std::string_view      kind,
                std::string_view      form,
                std::string_view      name,
                std::string_view      value) {
    out += kPredefinedMacroLineMarker;
    out += " origin=";
    out += predefinedMacroOriginName(origin);
    out += " kind=";
    out += kind;
    out += " form=";
    out += form;
    out += " name=";
    out += name;
    out += " value=";
    out += value;
    out += '\n';
}

// The `form=` field for a config-declared entry: `object`, or
// `function(a,b)` naming the declared parameters. A 0-ary function-like entry
// renders `function()` — `isFunctionLike` (not `params.empty()`) is what
// discriminates, exactly as the preprocessor's own `<built-in>` prologue does,
// so a `#define F()` predefine stays distinguishable from an object-like one.
//
// Params are joined WITHOUT a space so the field remains a single token.
[[nodiscard]] std::string formField(PredefinedMacroDef const& pm) {
    if (!pm.isFunctionLike) return "object";
    std::string f = "function(";
    for (std::size_t i = 0; i < pm.params.size(); ++i) {
        if (i != 0) f += ',';
        f += pm.params[i];
    }
    f += ')';
    return f;
}

// The `value=` field for a config-declared entry. Dispatches ONLY on `kind` and
// `isFunctionLike` — never on the macro's NAME. That is the same rule the
// expansion engine follows (`materialize`'s switch), and it is what lets a
// language that spells `__LINE__` differently dump correctly for free.
//
// `timestamp` is the ONE clock read for this whole dump, so `__DATE__` and
// `__TIME__` in the same output cannot describe different instants.
[[nodiscard]] std::string valueField(PredefinedMacroDef const&   pm,
                                     TranslationTimestamp const& timestamp) {
    // A function-like entry has no single expansion regardless of its kind, so
    // this test comes FIRST. (The config grammar admits `params` only on
    // `constant`, but keying the report off the flag rather than off that
    // coincidence means a future kind gaining params cannot start printing a
    // fabricated value by omission.)
    if (pm.isFunctionLike) return std::string{kNoSingleValueFunctionLike};
    switch (pm.kind) {
        case PredefinedMacroKind::Constant:
            // The static spelling, VERBATIM — including the empty string, which
            // is a real and deliberate declaration (the pe profile's
            // `__stdcall` → empty erase). An empty `value=` field is therefore
            // meaningful output, not a missing one.
            return pm.value;
        case PredefinedMacroKind::Date:
            // Quoted exactly as the materializer quotes it, so the dumped
            // spelling is the token the parser would see.
            return '"' + timestamp.date + '"';
        case PredefinedMacroKind::Time:
            return '"' + timestamp.time + '"';
        case PredefinedMacroKind::Line:
        case PredefinedMacroKind::File:
            // OFFSET-DERIVED: the value is a function of the invocation site, so
            // there is NO single value. Print the reason. A fabricated stand-in
            // (`1`, `"<source>"`) would be trusted precisely because it looks
            // like an answer.
            return std::string{kNoSingleValueOffsetDerived};
    }
    // Unreachable: the kind set is closed and every enumerator is handled above.
    // Reached only if a new kind lands without visiting this switch — in which
    // case saying so is the only honest output.
    return std::string{"<no-single-value: unhandled kind>"};
}

} // namespace

std::string_view predefinedMacroOriginName(PredefinedMacroOrigin o) noexcept {
    switch (o) {
        case PredefinedMacroOrigin::Language:    return "language";
        case PredefinedMacroOrigin::Target:      return "target";
        case PredefinedMacroOrigin::Format:      return "format";
        case PredefinedMacroOrigin::CommandLine: return "command-line";
    }
    return "unknown";
}

std::expected<std::string, std::vector<std::string>>
renderPredefinedMacroDump(PredefinedMacroDumpRequest const& req) {
    // ★ THE SINGLE OWNER. The effective list is not computed here; it is
    // REQUESTED from the function the preprocessor itself calls. Everything this
    // dump knows about which entries survive the per-format filter, in what
    // order, and whether the three families collide, comes from this one call.
    MergedPredefinedMacros const merged = mergePredefinedMacros(
        req.languageMacros, req.targetMacros, req.formatMacros,
        req.activeFormat);
    // `conflicts` non-empty ⇒ `effective` is documented UNUSABLE. Return the
    // merge's own messages and NOTHING else — no header, no partial list.
    if (!merged.conflicts.empty()) return std::unexpected(merged.conflicts);

    // ONE clock read for the whole section (see `valueField`).
    TranslationTimestamp const timestamp = translationTimestamp();

    std::string out;
    out += kPredefinedMacroHeaderMarker;
    out += " language=";
    out += req.languageName;
    out += " target=";
    out += req.targetName;
    out += " format=";
    out += req.formatName;
    out += " object-format-kind=";
    out += req.activeFormat.has_value()
               ? objectFormatKindName(*req.activeFormat)
               // No active format: the merge kept only universal entries, and
               // saying so beats printing a format name that was never resolved.
               : std::string_view{"<none>"};
    out += " effective=";
    out += std::to_string(merged.effective.size());
    out += " command-line-defines=";
    out += std::to_string(req.userDefines.size());
    out += '\n';

    // ── Attributing an ORIGIN without opening a second walk ─────────────────
    //
    // `effective` carries no origin field, and re-testing membership against the
    // three input lists would be exactly the second walk the single-owner rule
    // forbids — worse, for a name two families both declare it would have to
    // pick one, which is the silent last-writer-wins the collision check exists
    // to prevent (unreachable here, since a collision already returned above,
    // and therefore also untestable: a branch that cannot be exercised is not a
    // branch, it is a guess).
    //
    // So the per-family survivor lists are obtained from THE SAME merge, one
    // family at a time. The family SLOT does not matter to the filter — the
    // labels exist only to build a collision message, and with the other two
    // spans empty there is no collision to describe — so a single-family call
    // yields precisely "the entries of this family that survive this format's
    // filter", computed by the one availability predicate the merge's docblock
    // says must never be copied.
    //
    // The merge documents (property (c)) that `effective` is those three lists
    // concatenated in language → target → format order. The loop below CHECKS
    // that rather than trusting it: every printed entry comes from
    // `merged.effective`, and its origin is accepted only while the two agree
    // name-for-name. A future reordering inside the merge therefore produces a
    // loud refusal instead of thirty confidently mislabelled lines.
    struct FamilySurvivors {
        PredefinedMacroOrigin origin;
        MergedPredefinedMacros merged;
    };
    std::array<FamilySurvivors, 3> const families{
        FamilySurvivors{PredefinedMacroOrigin::Language,
                        mergePredefinedMacros(req.languageMacros, {}, {},
                                              req.activeFormat)},
        FamilySurvivors{PredefinedMacroOrigin::Target,
                        mergePredefinedMacros(req.targetMacros, {}, {},
                                              req.activeFormat)},
        FamilySurvivors{PredefinedMacroOrigin::Format,
                        mergePredefinedMacros(req.formatMacros, {}, {},
                                              req.activeFormat)}};

    std::size_t i = 0;
    for (FamilySurvivors const& fam : families) {
        for (PredefinedMacroDef const& own : fam.merged.effective) {
            if (i >= merged.effective.size()
                || merged.effective[i].name != own.name) {
                return std::unexpected(std::vector<std::string>{
                    "internal: the effective predefined-macro list is not the "
                    "documented concatenation of its per-family filtered lists "
                    "(diverged at index " + std::to_string(i) + ", expected '"
                    + own.name + "') — `mergePredefinedMacros` no longer "
                      "produces language-then-target-then-format order, so an "
                      "origin label here would be a guess"});
            }
            // Print the AUTHORITATIVE entry (`merged.effective[i]`), never the
            // single-family copy: the list the compile path seeds from is the
            // one this instrument must describe.
            PredefinedMacroDef const& pm = merged.effective[i];
            appendLine(out, fam.origin, predefinedMacroKindName(pm.kind),
                       formField(pm), pm.name, valueField(pm, timestamp));
            ++i;
        }
    }
    if (i != merged.effective.size()) {
        return std::unexpected(std::vector<std::string>{
            "internal: the effective predefined-macro list holds "
            + std::to_string(merged.effective.size())
            + " entries but its per-family filtered lists account for only "
            + std::to_string(i)
            + " — a fourth predefined-macro family reached the merge without "
              "reaching --dump-predefined-macros, so the dump would silently "
              "omit it"});
    }

    // ── The command line ───────────────────────────────────────────────────
    //
    // `--define` entries are NOT config predefines and do not pass through the
    // merge: the preprocessor lowers each to an ORDINARY `#define` in a
    // synthetic "<command-line>" prologue. They belong in this output anyway,
    // because the question the flag answers is "what does this TU SEE", and an
    // answer that omitted them would describe a build nobody ran.
    //
    // `splitUserDefine` is the preprocessor's OWN split — including the rule
    // that a bare `--define NAME` takes the value `1`. `valueWasStated` is
    // surfaced so the operator can tell an explicit `=1` from the default.
    for (std::string const& d : req.userDefines) {
        UserDefineSplit const s = splitUserDefine(d);
        appendLine(out, PredefinedMacroOrigin::CommandLine,
                   // An ordinary object-like `#define`: the CLI rejects a
                   // function-like `--define` outright, so there is no
                   // params axis to report here.
                   predefinedMacroKindName(PredefinedMacroKind::Constant),
                   s.valueWasStated ? "object" : "object-default-value",
                   s.name, s.value);
    }

    // ── The shadowing NOTE (see `kPredefinedMacroNoteMarker`) ───────────────
    //
    // A `--define` whose NAME the effective set already holds does NOT stack with
    // it — the COMPILE path REFUSES the build. Printing two lines for one name
    // without saying so would read as "both are in effect", which is the one
    // thing that is certainly not true.
    //
    // ★ MEASURED (four arms, pe64 x86_64, a one-line TU): the refusal is real and
    // arrives by TWO different routes, and which one is not this dump's call —
    //   · an OBJECT-LIKE config predefine is seeded into the predefined table, so
    //     the `--define` hits C 6.10.8.1 → `P001B` ("is a predefined macro and
    //     may not be #defined"). Confirmed for a language-origin (`_MSC_VER`), a
    //     target-origin (`__x86_64__`) and an offset-derived (`__LINE__`) entry,
    //     and it fires even for a byte-IDENTICAL value (`_MSC_VER=1943`) — 6.10.8.1
    //     forbids the `#define` outright, so 6.10.3p2's identical-redefinition
    //     tolerance never applies.
    //   · a FUNCTION-LIKE config predefine is lowered to a "<built-in>" prologue
    //     `#define`, i.e. an ORDINARY macro, so the `--define` hits C 6.10.3p2
    //     instead → `P0014` ("incompatible redefinition"). Confirmed with
    //     `--define __declspec=z`.
    // Reproducing that two-route decision here would be a SECOND owner of it, and
    // the routes differ by a property (`isFunctionLike`) whose consequences live in
    // the directive handler. So the note reports the CONDITION and names the rules;
    // the handler keeps the verdict.
    //
    // The overlap is found against `merged.effective` — the authoritative list, not
    // a config family — so the question asked is exactly "does the thing the
    // compiler seeds already own this name".
    for (std::string const& d : req.userDefines) {
        UserDefineSplit const s = splitUserDefine(d);
        for (PredefinedMacroDef const& pm : merged.effective) {
            if (pm.name != s.name) continue;
            out += kPredefinedMacroNoteMarker;
            out += " shadowed-predefine name=";
            out += s.name;
            out += " --define names a macro the effective set ALREADY declares,"
                   " and the two do NOT stack: the compile path REFUSES such a"
                   " build — C 6.10.8.1 (P001B) when the declared entry is"
                   " object-like, even for an identical value; C 6.10.3p2"
                   " (P0014) when it is function-like. Both lines above are"
                   " printed so the collision is visible, NOT because both are"
                   " in effect. Remove the --define, or rename it.\n";
            break;   // one note per --define, however many entries matched
        }
    }
    return out;
}

int dumpPredefinedMacros(CliArgs const& args, std::ostream& out,
                         std::ostream& err) {
    // Defence in depth: `parseCliArgs` already demands both for this mode, but
    // this function is also called directly (tests, embedders), and an empty
    // language would otherwise reach `loadShipped("")` and fail with a message
    // about a missing file rather than about a missing flag.
    if (args.languageName.empty()) {
        emitLoud(err, DiagnosticCode::D_SchemaLoadFailed,
                 "--dump-predefined-macros requires --language <name>: the "
                 "effective predefined-macro set is a property of a "
                 "(language x target x object-format) triple.");
        return 1;
    }
    if (args.targets.empty()) {
        emitLoud(err, DiagnosticCode::D_InvalidTargetSpec,
                 "--dump-predefined-macros requires at least one --target "
                 "<targetName>:<formatName>: the effective set differs per "
                 "target and per object format, so there is no target-less "
                 "answer to give.");
        return 1;
    }

    auto grammarR = GrammarSchema::loadShipped(args.languageName);
    if (!grammarR.has_value()) {
        emitLoadFailure(err, grammarR.error(), DiagnosticCode::D_SchemaLoadFailed,
                        "language schema '" + args.languageName
                        + "' could not be loaded — the reason is in the configuration diagnostic(s) above (config: "
                          "src/dss-config/sources/" + args.languageName
                        + ".lang.json).");
        return 1;
    }
    auto const grammar = *grammarR;

    // A language with NO preprocess block predefines nothing and has no
    // `predefinedMacros` list to report. Saying so is the answer; printing an
    // empty section would read as "this triple defines no macros", which is a
    // different (and unearned) claim about a language that HAS a preprocessor.
    if (!grammar->preprocess().enabled) {
        emitLoud(err, DiagnosticCode::D_SchemaLoadFailed,
                 "language '" + args.languageName
                 + "' declares no `preprocess` block, so it has no predefined "
                   "macros to dump (the preprocessor pass is a strict no-op "
                   "for it). --dump-predefined-macros applies to a language "
                   "that opts in to preprocessing.");
        return 1;
    }

    // ── Render EVERY section before printing ANY ────────────────────────────
    // All-or-nothing across targets (see the header): a failure on target N must
    // not leave targets 1..N-1 on stdout looking complete.
    std::vector<std::string> sections;
    sections.reserve(args.targets.size());
    for (std::string const& spec : args.targets) {
        auto parsed = TargetSpec::parse(spec);
        if (!parsed) {
            emitLoud(err, DiagnosticCode::D_InvalidTargetSpec,
                     "--target '" + spec + "' is not a "
                     "'<targetName>:<formatName>' spec ("
                     + std::string{targetSpecErrorName(parsed.error())}
                     + ") — e.g. x86_64:elf64-x86_64-linux-exec.");
            return 1;
        }
        auto targetR = TargetSchema::loadShipped(parsed->targetName);
        if (!targetR.has_value()) {
            emitLoadFailure(err, targetR.error(),
                            DiagnosticCode::D_SchemaLoadFailed,
                            "target schema '" + parsed->targetName
                            + "' could not be loaded — the reason is in the configuration diagnostic(s) above (config: "
                              "src/dss-config/targets/" + parsed->targetName
                            + ".target.json).");
            return 1;
        }
        auto formatR = ObjectFormatSchema::loadShipped(parsed->formatName);
        if (!formatR.has_value()) {
            emitLoadFailure(err, formatR.error(),
                            DiagnosticCode::D_SchemaLoadFailed,
                            "object-format schema '" + parsed->formatName
                            + "' could not be loaded — the reason is in the configuration diagnostic(s) above (config: "
                              "src/dss-config/object-formats/"
                            + parsed->formatName
                            + ".format.json).");
            return 1;
        }
        PredefinedMacroDumpRequest req;
        req.languageName   = args.languageName;
        req.targetName     = parsed->targetName;
        req.formatName     = parsed->formatName;
        req.languageMacros = grammar->preprocess().predefinedMacros;
        req.targetMacros   = (*targetR)->predefinedMacros();
        req.formatMacros   = (*formatR)->predefinedMacros();
        // The FORMAT SCHEMA's kind — the same value the driver's
        // `formatKindOfSpec` reads for the front-end build key, and the same one
        // the preprocessor receives as `activeFormat`.
        req.activeFormat   = (*formatR)->kind();
        req.userDefines    = args.defines;

        auto rendered = renderPredefinedMacroDump(req);
        if (!rendered.has_value()) {
            // The merge's own messages, each naming BOTH declaring config paths.
            // One line per colliding NAME, exactly as the compile path emits
            // them — and, like the compile path, nothing usable follows.
            for (std::string const& msg : rendered.error()) {
                emitLoud(err, DiagnosticCode::C_ConflictingPredefinedMacro,
                         "[target=" + spec + "] " + msg);
            }
            return 1;
        }
        sections.push_back(std::move(*rendered));
    }

    for (std::string const& s : sections) out << s;
    return 0;
}

} // namespace dss
