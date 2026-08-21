#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// INTERNAL header — the CLOSED-KEY-VOCABULARY substrate shared by every DSS
// config loader (`grammar_schema_json.cpp`, `target_schema_json.cpp`, …).
// Never included by a public type header; it declares no types, only the two
// compile-time predicates + the `$`-documentation carve-out that make a
// "typo discriminator" honest.
//
// EXTRACTED (TF-C74) from `grammar_schema_json.cpp`, unchanged. The extraction
// is the point: the language loader closed its root-key vocabulary in TF-C72
// and the TARGET loader did not, so a misspelled target root key loaded clean
// and the feature it named silently no-op'd. A guard that lives inside ONE
// loader's .cpp is a guard the sibling loader cannot use — so it lives here.

namespace dss::detail {

// ── the `$`-prefixed documentation-key carve-out ──────────────────────────
//
// A key whose name starts with `$` (`$comment`, `$…Comment`) is PROSE, never
// config: the codebase-wide way to explain a block inside the JSON that
// declares it. Every closed-key vocabulary must skip such keys, and so must
// every place that reads an object's keys AS IDENTIFIERS — most sharply the
// `shapes` map, where a `$`-prefixed sibling of the rule names was read as a
// SHAPE DEFINITION and its prose value as a rule REFERENCE, failing the whole
// load with the paragraph echoed back as if it were a rule name.
//
// ★ ONE predicate, not another copy of the same expression. The convention had
// been open-coded identically at seven sites; a convention spelled out N times
// is a convention that holds only where someone remembered it, and the shapes
// map is the site where it was forgotten. Every site calls this, so
// "documentation key" has exactly one definition to read and to change.
[[nodiscard]] constexpr bool isDocumentationKey(std::string_view key) {
    return !key.empty() && key.front() == '$';
}

// ── the closed-key-vocabulary well-formedness check ───────────────────────
//
// Every `kSomethingKeys` table is a `std::array<string_view, N>` whose N is
// written BY HAND next to the initializer list — and the compiler does NOT
// check that the two agree in the dangerous direction.
// `std::array<std::string_view, 57>` with 56 initializers is perfectly legal:
// it value-initializes the tail, so element 56 becomes the EMPTY string_view
// and the typo discriminator silently starts whitelisting a key of `""`.
// (MEASURED on `kSemanticsKeys`: bumping 56→57 alone compiles clean, and a
// `semantics` key of `""` then loads without a diagnostic.) A DUPLICATE entry
// is silent in a mirror-image way — it inflates the count, so a key that is
// actually missing looks accounted for by the number.
//
// ★ ONE helper, not a copy of the loop per table. The check had been written
// once, for `kDeclarationRowKeys`, and the three sibling tables — including the
// 56-entry `kSemanticsKeys`, the largest and most edit-prone of them — went
// unguarded; a guard that must be remembered per table is a guard that holds
// only where someone remembered it. Every closed-key table calls this, at
// compile time, so the next person to add a key cannot get the number wrong
// without the build saying so.
template <std::size_t N>
[[nodiscard]] constexpr bool isWellFormedKeyVocabulary(
    std::array<std::string_view, N> const& keys) {
    for (std::size_t a = 0; a < N; ++a) {
        if (keys[a].empty()) return false;             // under-filled ⇒ tail is ""
        for (std::size_t b = a + 1; b < N; ++b)
            if (keys[a] == keys[b]) return false;      // duplicate ⇒ count lies
    }
    return true;
}
// The same check for a name→value vocabulary (a verb table). The under-fill
// hazard is WORSE here: the value half zero-initializes too, so the phantom
// `""` verb silently maps to whatever enumerator happens to be 0.
template <typename T, std::size_t N>
[[nodiscard]] constexpr bool isWellFormedKeyVocabulary(
    std::array<std::pair<std::string_view, T>, N> const& rows) {
    for (std::size_t a = 0; a < N; ++a) {
        if (rows[a].first.empty()) return false;
        for (std::size_t b = a + 1; b < N; ++b)
            if (rows[a].first == rows[b].first) return false;
    }
    return true;
}

// ── projecting a NAME→VALUE vocabulary onto its key half ──────────────────
//
// Several closed vocabularies are not `string_view` tables but ROW tables — a
// name plus what the name selects (a member pointer, an enumerator, a prose
// description). Their key check still has to answer the same question, and it
// was answered by a hand-written inner loop per site, next to a hand-written
// renderer for the "the closed set is …" half of the message.
//
// ★ THE POINT IS THAT THE PROJECTION IS COMPUTED, NOT RETYPED. A second literal
// list of the same names is a second owner of one fact, and the two drift
// silently — the rows keep working while the message (or the check) names a set
// that no longer matches. Feed the ROW table in, get the key table out, and the
// two cannot disagree because there is only one of them.
//
// `proj` takes a row and returns something convertible to `std::string_view`.
template <typename Row, std::size_t N, typename Proj>
[[nodiscard]] constexpr std::array<std::string_view, N>
keysOf(std::array<Row, N> const& rows, Proj proj) {
    std::array<std::string_view, N> out{};
    for (std::size_t i = 0; i < N; ++i) out[i] = std::string_view{proj(rows[i])};
    return out;
}

// ── RENDERING A CLOSED SET INTO A DIAGNOSTIC ──────────────────────────────
//
// `'a', 'b', 'c'` — the quoted, separated list that every "expected one of …"
// half of every config diagnostic ends in.
//
// ★ THE POINT IS THAT NOBODY WRITES THIS LOOP AGAIN, and it is a smaller,
// meaner version of the duplication `rejectUnknownKeys` below exists to end.
// The loop itself had two copies (this file and a `closedKindSet` lambda in
// the grammar loader) — but the DAMAGE was never the loop: it was that most
// sites skipped the projection entirely and typed the SET into the sentence,
// which is a second owner of a fact the enum table already owns
// (D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET). Every such
// message stayed correct only for as long as nobody touched the vocabulary,
// and a test asserting the sentence CONTAINS a name stays green while the
// sentence becomes a lie.
//
// `sep` — the separator, because the two house shapes differ (`, ` in the
// language/semantics diagnostics, ` / ` in the target loader) and a cycle
// that unified them would rewrite dozens of pinned sentences for no
// correctness gain. The QUOTING is not a parameter: an unquoted list is
// ambiguous the moment a spelling contains a space or a slash.
//
// `known` — any range of string-convertible spellings. Feed it an
// `EnumNameTable` projection (`allNames` / `namesWhere`), never a literal.
template <typename KnownKeys>
[[nodiscard]] inline std::string renderAllowedList(KnownKeys const& known,
                                                   std::string_view sep = ", ") {
    std::string out;
    for (auto const& k : known) {
        if (!out.empty()) out += sep;
        out += '\'';
        out += std::string_view{k};
        out += '\'';
    }
    return out;
}

// ── THE TYPO DISCRIMINATOR ITSELF ─────────────────────────────────────────
//
// Reject every key of `obj` that is neither a `$`-documentation key nor a
// member of `known`. THE ONE loop; every closed-key object in every DSS config
// loader runs through this one.
//
// ★ WHY IT LIVES HERE AND NOT IN A LOADER. It DID live in a loader — in four
// of them, independently written, plus ~50 open-coded copies at individual
// call sites. `target_schema_json.cpp`'s copy carries a header arguing at
// length that a loop per site IS the defect, and that argument was right and
// could not reach past its own translation unit: two of the four sibling
// helpers (`opt/optimizer_json.cpp` and `ffi/shipped_lib_descriptor.cpp`) omit
// the `$` carve-out entirely, which is the SAME class of miss the target
// loader's header describes, in the other direction — they REFUSE a
// `$…Comment` prose key that the convention says every document may carry.
// A rule that must be remembered per site is a rule that holds only where
// someone remembered it, and the site that forgets is never the one being
// read at the time.
//
// ★ WHAT IS SHARED AND WHAT IS NOT. The CHECK and the DIAGNOSTIC SHAPE are
// shared; the VOCABULARY and the SINK are not. Each loader keeps its own
// allowed-key table next to the object it describes (that is the part a
// maintainer edits, and it must stay where the fields are read), and its own
// diagnostic code, JSON-path convention and reporter — which is why `emit` is
// a callback rather than a `Collector&`. That also keeps this header free of
// any JSON or diagnostic dependency: `obj` and `known` are duck-typed, so
// nothing above `core/types` is dragged into everything that includes this.
//
// ★ WHY IT MATTERS, WHICH IS NOT TIDINESS. On a config-driven compiler a
// dropped key is a CAPABILITY THAT QUIETLY DOES NOT HAPPEN: the document
// still loads, the feature it names stays at its default, and nothing says so.
// That is why the message states the refusal policy rather than only the fact.
//
// `obj`         — anything with `is_object()` and a key/value iteration range
//                 whose iterator exposes `.key()` (nlohmann's object shape).
//                 A non-object is left alone: its SHAPE is the caller's own
//                 diagnostic, and reporting it twice reads as two defects.
// `known`       — any range of string-comparable keys. It need not be a
//                 compile-time table: the object-format loader unions its
//                 static roots with the block names the registered backends
//                 declare, so the ALLOWED list in the message stays true for
//                 a format backend added later.
// `objectLabel` — names the object in prose ("a register row", "the tls
//                 block"), so the message reads as a sentence and not as a
//                 path echo.
// `emit`        — `void(std::string_view key, std::string message)`, called
//                 once per unknown key with the shared sentence. A caller that
//                 needs to say more appends to it; a caller that needs to
//                 stop tracks that itself.
template <typename JsonObject, typename KnownKeys, typename EmitFn>
void rejectUnknownKeys(JsonObject const& obj,
                       KnownKeys const&  known,
                       std::string_view  objectLabel,
                       EmitFn&&          emit) {
    if (!obj.is_object()) return;
    // Rendered at most once, and only if some key actually needs it — this
    // runs over every key of every config object, so the clean path must not
    // build a string. The explicit flag rather than `allowed.empty()`: an
    // EMPTY `known` renders to the empty string, and testing the string would
    // silently re-render per key while reading as "render once".
    std::string allowed;
    bool        allowedRendered = false;
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        // `$`-prefixed keys are PROSE (config-wide convention). The PREFIX
        // predicate, never a literal `"$comment"` — `$templateComment` and
        // `$framePointerComment` are spellings shipped documents really use,
        // and a literal would reject them as typos.
        if (isDocumentationKey(it.key())) continue;
        bool found = false;
        for (auto const& k : known) {
            if (it.key() == k) { found = true; break; }
        }
        if (found) continue;
        if (!allowedRendered) {
            allowed          = renderAllowedList(known);
            allowedRendered  = true;
        }
        std::string message = "unknown key '";
        message += std::string_view{it.key()};
        message += "' in ";
        message += objectLabel;
        message += " — allowed keys are ";
        message += allowed;
        message += " (plus any '$'-prefixed documentation key). An "
                   "unrecognized key is REFUSED rather than ignored: a "
                   "silently-dropped key leaves the feature it names switched "
                   "off with no diagnostic";
        emit(std::string_view{it.key()}, std::move(message));
    }
}

// The QUERY form of the same check: the FIRST key of `obj` that is neither a
// `$`-documentation key nor a member of `known`; nullopt when every key is
// recognized.
//
// A second primitive rather than a mode of the first, because two loaders
// genuinely need this shape: the project-manifest and lockfile loaders report
// ONE diagnostic per failed load, each ending in its own remediation ("delete
// the lockfile and let the next build re-resolve"), so they must own the
// message and only need the finding. They had written this identically, in two
// files, down to the comment on the `$` skip — which is the same duplication
// `rejectUnknownKeys` above exists to end, at a smaller scale.
template <typename JsonObject, typename KnownKeys>
[[nodiscard]] std::optional<std::string>
firstUnknownKey(JsonObject const& obj, KnownKeys const& known) {
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        std::string const& key = it.key();
        if (isDocumentationKey(key)) continue;   // PROSE, never config
        bool recognized = false;
        for (auto const& k : known) {
            if (key == k) { recognized = true; break; }
        }
        if (!recognized) return key;
    }
    return std::nullopt;
}

} // namespace dss::detail

// The one message every closed-key table shares, so the diagnosis reads the
// same wherever the build breaks. FULLY QUALIFIED on purpose: the two loaders
// that use it sit at different scopes (`dss::detail` vs `dss`), and a macro
// that silently requires a `using`-declaration at each call site is exactly
// the "guard you must remember" this file exists to abolish.
#define DSS_CHECK_KEY_VOCABULARY(table)                                        \
    static_assert(::dss::detail::isWellFormedKeyVocabulary(table),             \
                  #table ": the declared std::array size must equal the "      \
                  "initializer count (an under-filled array zero-fills and "   \
                  "would whitelist the empty key) and every entry must be "    \
                  "unique")
