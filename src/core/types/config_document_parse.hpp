#pragma once

#include "core/export.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

// ══ THE ONE ENTRY EVERY DSS CONFIG DOCUMENT IS PARSED THROUGH ═══════════════
//
// INTERNAL header, in the same family as `config_key_vocabulary.hpp` (the
// closed-key substrate) and `config_document_memo.hpp` (the content-addressed
// build memo). It is never included by a public type header; it declares one
// function and the shape of its refusal.
//
// ── WHY IT EXISTS: A REPEATED KEY SILENTLY COMPILES A DIFFERENT PROGRAM ─────
//   [[D-CONFIG-A-DUPLICATE-JSON-KEY-IS-DROPPED-WITHOUT-A-DIAGNOSTIC]]
//
// nlohmann's parser — like Python's, and like most JSON parsers — accepts an
// object whose member name is declared twice and keeps the LAST one, silently.
// In this architecture a `.lang.json` / `.target.json` / `.format.json`
// document IS the compiler's behaviour, so the discarded declaration is not a
// cosmetic loss: it is a knob whose stated value never reached the engine.
//
// ✔MEASURED 2026-08-31 through the shipped CLI, on a COPY of the config tree
// (`DSS_CONFIG_ROOT` pointed at it), arm64 target, `.worktrees/jk`:
//   ARM 1  duplicate BEFORE the original, shape-valid ......... rc=0, zero
//          diagnostics, emitted artifact byte-identical to control
//   ARM 2  duplicate AFTER, value `false` where an object is required . rc=1,
//          but the refusal is `C_MalformedJson: at /charIsUnsigned:
//          'charIsUnsigned' must be an OBJECT` — that is the SHAPE validator
//          objecting to the WINNING VALUE, not duplicate detection. It proves
//          the late key won; it proves nothing about noticing a duplicate.
//   ARM 3  duplicate AFTER, SHAPE-VALID, `default` flipped .... rc=0, ZERO
//          diagnostics, emitted bytes MOVED and the control returned.
// ⇒ ARM 3 is the decisive one: a well-formed, schema-valid document with a
// repeated key compiled a DIFFERENT PROGRAM and said nothing.
//
// ⚠ AND THE DISCARDED TEXT IS THE ONE A HUMAN READS FIRST. The last
// declaration wins, so the loser is the EARLIER one — the one nearer the top
// of the file, the one a reader scrolling the document sees and believes.
//
// ── ERROR, NOT WARNING, AND THE REASONING IS IN `config_document_parse.cpp` ──
//
// ── ONE OWNER, NOT ONE CHECK PER READER ─────────────────────────────────────
// ✔MEASURED: `src/` held NINE JSON ingestion sites for config-class documents
// across six files, in THREE different spellings (`json::parse`, `operator>>`
// into a `json`, and a streaming `sax_parse`). A duplicate check pasted at each
// is the *a partial fix reads as a complete one* failure with nine copies of
// the rule, and the tenth reader added next year gets none of them. So the rule
// has exactly one home, every reader calls this function instead of
// `json::parse`, and `tests/core/test_config_document_parse_is_the_one_owner.cpp`
// refuses a new raw call anywhere in `src/`.
//
// ── COST — ✔MEASURED, AND THE FIRST TWO ANSWERS WERE WRONG ──────────────────
//
// The document is walked TWICE: `json::parse` builds the DOM (its refusal text
// for malformed bytes passed through UNCHANGED, so no existing diagnostic
// moved), then a key-only SAX scan looks for a repeat. Both walks are on the
// memo MISS path only — every schema family consults `ConfigDocumentMemo`
// BEFORE it reaches a loader — so this is paid once per distinct document per
// process and does NOT scale with the size of the program being compiled.
//
// ✔MEASURED 2026-08-31, Windows Debug `build/jk`, a one-line C compile, on the
// compiler's OWN `build-config` phase clock (`--time`, 9 runs, medians):
//   the SECOND LEX PASS ........ +32 ms
//   the key/path bookkeeping ... +1 ms
//   TOTAL ...................... +33 ms, 33% of a 100 ms `build-config`
//
// ⚠ TWO EARLIER READINGS OF THIS SAME NUMBER WERE WRONG, AND BOTH WOULD HAVE
// SENT THE DESIGN THE WRONG WAY:
//   (1) PROCESS WALL-CLOCK IS NOT AN INSTRUMENT ON THIS HOST. Two runs of
//       IDENTICAL code disagreed 4× (90 ms vs 21 ms) because sibling lanes were
//       compiling. A ~30 ms effect is below that noise floor; the phase clock
//       is not a refinement, it is the difference between measuring and
//       guessing.
//   (2) THE FIRST DECOMPOSITION BLAMED THE LEX AND IT WAS THE BOOKKEEPING. The
//       original scanner built a pointer segment on every container and kept a
//       per-object `unordered_set<std::string>`: **53 ms** of allocation for a
//       path no healthy document ever reads. Rewritten to reconstruct the
//       pointer only on the refusal path (see the note in the `.cpp`), that
//       53 ms became 1 ms. ⇒ had the "second pass is inherently expensive"
//       reading stood, the fix would have been a single-pass rewrite that
//       bought ~1 ms and kept all 53.
//
// ⓘ WHY IT IS STILL NOT SINGLE-PASS, stated as a trade rather than assumed.
// The remaining +32 ms IS the second lex, and only a single pass removes it.
// That pass means driving `nlohmann::detail::json_sax_dom_parser` — an INTERNAL
// template whose second parameter exists solely to name a lexer we would pass
// as null — plus `detail::input_adapter`. Two dependencies on a library's
// private namespace, in the one function every config document in the product
// is parsed by, to save ~32 ms of a DEBUG build on the cold path. The public
// API was chosen; the number is here so the trade can be re-opened with
// evidence rather than re-argued from taste.

namespace dss::detail {

enum class ConfigDocumentParseFailure : std::uint8_t {
    // The bytes are not JSON at all. `message` is nlohmann's own `what()`,
    // verbatim, and `pointer` is empty.
    NotJson,
    // One object declares the same member name twice. `pointer` addresses the
    // SECOND declaration.
    DuplicateKey,
};

struct DSS_EXPORT ConfigDocumentParseError {
    ConfigDocumentParseFailure kind = ConfigDocumentParseFailure::NotJson;
    // RFC 6901 JSON pointer of the offending key — `/charIsUnsigned`,
    // `/opcodes/17/mnemonic` — spelled the way the shape validators already
    // spell a path, so a reader does not have to learn a second locator.
    // EMPTY for `NotJson`.
    std::string pointer;
    // For `DuplicateKey`, one complete sentence ready to be a diagnostic's
    // detail text. For `NotJson`, nlohmann's `what()` and nothing else — the
    // caller supplies the prefix it has always supplied.
    std::string message;

    // ── THE TWO SHAPES, RENDERED IN ONE PLACE ───────────────────────────────
    // Seven readers emit this refusal. Written out at each of them, the
    // "pointer if there is one, else the document label" choice is the same
    // conditional seven times — and the eighth reader gets whichever half its
    // author copied. These two render it once.

    // Where the diagnostic points. A duplicate key has a JSON POINTER, which is
    // how every shape diagnostic in every loader is already located. Bytes that
    // are not JSON have no position INSIDE a document that does not exist, so
    // they keep the caller's document label.
    [[nodiscard]] std::string locus(std::string_view documentLabel) const {
        return pointer.empty() ? std::string{documentLabel} : pointer;
    }

    // What the diagnostic says. The duplicate-key sentence is complete on its
    // own; a parse failure is nlohmann's `what()` under the caller's own
    // prefix, UNCHANGED from what that caller emitted before this owner
    // existed — so routing a reader through here moves no existing message.
    [[nodiscard]] std::string detailText(std::string_view notJsonPrefix) const {
        return kind == ConfigDocumentParseFailure::DuplicateKey
                   ? message
                   : std::string{notJsonPrefix} + message;
    }

    // The same two shapes for a channel that has NO separate path field:
    // `parseProjectConfig`'s reporter and the shipped-lib descriptor's
    // `emitMalformed` each take ONE string. It folds the locus in with the
    // SAME spelling the `ConfigDiagnostic` renderer uses (`grammar_schema.hpp`
    // — `"at " + cd.path`), so a user meets one sentence shape across every
    // family rather than one per channel.
    [[nodiscard]] std::string detailTextWithLocus(
        std::string_view notJsonPrefix) const {
        return pointer.empty() ? detailText(notJsonPrefix)
                               : "at " + pointer + ": " + message;
    }
};

// Parse a DSS config document. THE ONLY sanctioned way to turn config bytes
// into a `nlohmann::json` inside `src/`.
//
// ⚠ Takes TEXT, resolves nothing, reads no file and owns no `<filesystem>`:
// every caller has already performed its own checked read
// (D-CORE-SHIPPED-CONFIG-LOADERS-DRAIN-A-STREAM-WITHOUT-CHECKING-IT) and knows
// what to call the document in a diagnostic.
[[nodiscard]] DSS_EXPORT std::expected<nlohmann::json, ConfigDocumentParseError>
parseConfigDocument(std::string_view jsonText);

// The RFC 6901 §3 escape for ONE pointer segment: `~` becomes `~0` and `/`
// becomes `~1`, in that order. Exposed because the test that pins the pointer
// spelling must not re-implement it — a second implementation is a second
// answer, and the one under test would be free to drift toward it.
[[nodiscard]] DSS_EXPORT std::string escapeJsonPointerSegment(std::string_view raw);

} // namespace dss::detail
