#include "core/types/config_document_parse.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// ══ ERROR, NOT WARNING — THE DECISION AND ITS DEFENCE ═══════════════════════
//
// This is a judgement about USER-FACING BEHAVIOUR, so it is written down rather
// than quietly picked. A repeated key REFUSES THE LOAD. Four reasons, in the
// order that decides it:
//
// 1. ★★ A WARNING IS DROPPABLE, AND THE DEFECT IS THE SILENCE. A diagnostic
//    passes three independent volume gates on the way out (dedup window,
//    per-code cap, global cap — see `DiagnosticReporter::report`) and `
//    --suppress` can silence it outright. A "warning" here would therefore
//    restore, for any user who capped or suppressed it, exactly the state ARM 3
//    measured: a different program, compiled silently. A refusal is structural
//    — every caller returns `std::unexpected` on it — so no diagnostic policy
//    can put the silence back.
//
// 2. THE PERMISSIVE READING COSTS THE USER NOTHING. The two values are equal or
//    they are not. If equal, the duplicate carries no information and deleting
//    it is free. If unequal, one of them is a lie, and there is no reading of
//    the document under which the compiler can know which. There is no third
//    case, so there is no legitimate document this refusal rejects.
//
// 3. THE WEAKER SIBLING IS ALREADY AN ERROR. The closed root-key vocabularies
//    (TF-C72 / TF-C74, `config_key_vocabulary.hpp`) refuse an UNKNOWN key —
//    a key that changes nothing, because nothing reads it. A DUPLICATE key
//    changes what the engine does. Refusing the harmless case while shrugging
//    at the harmful one is not a policy, it is an inconsistency.
//
// 4. THE STANDARD PERMITS IT. RFC 8259 §4 says object names SHOULD be unique
//    and leaves the behaviour of a parser receiving a repeat unspecified;
//    ECMA-404 does not constrain it either. A strict refusal is a conforming
//    choice, and it is the only one that is not a guess about intent.
//
// ⓘ Scope, stated so the next reader does not have to infer it: this refuses a
// key repeated inside ONE object. It says nothing about the same key appearing
// in two DIFFERENT documents — that is `languageReferences` merging, which has
// its own rules and its own diagnostics.

namespace dss::detail {

namespace {

using json = nlohmann::json;

// House-style fail-loud sink. Unreachable BY CONSTRUCTION: the scan below runs
// only over bytes `json::parse` has already accepted, so a syntax error
// reaching it means the DOM parser and the SAX parser disagreed about the same
// buffer. That is a compiler bug, and a swallowed one would leave the
// duplicate-key check silently disabled for the document that triggered it —
// the exact silence this whole file exists to end.
[[noreturn]] void scannerDisagreedFatal(char const* what) {
    std::fputs("dss::detail::parseConfigDocument fatal: the duplicate-key scan "
               "rejected bytes that json::parse had already accepted; the DOM "
               "and SAX parsers disagree (this is a compiler bug, not a config "
               "error)\n",
               stderr);
    std::fprintf(stderr, "  detail: %s\n", what);
    std::abort();
}

// ── THE KEY-ONLY SAX SCAN ───────────────────────────────────────────────────
//
// Consumes the nlohmann SAX event stream and answers exactly one question:
// does any object declare the same member name twice? It builds NO document —
// every value callback is a cursor bump — and in the steady state it allocates
// NOTHING: the frame stack and the flat name store both grow only to the
// document's high-water mark and are then reused, and the JSON pointer is built
// only when there is a duplicate to name. See the two notes further down for
// the measurement that forced both, and for why neither was the first design.
//
// ★ IT ABORTS ON THE FIRST DUPLICATE (returns `false` from `key`), which stops
// the parse where it stands. The FIRST duplicate in document order is the one
// reported, deterministically, on every host: a second one would be a second
// diagnostic about a document that is already refused.
class DuplicateKeyScanner {
public:
    // ── the nlohmann SAX interface (exact signatures of `json_sax`) ──
    bool null() { return finishValue(); }
    bool boolean(bool) { return finishValue(); }
    bool number_integer(json::number_integer_t) { return finishValue(); }
    bool number_unsigned(json::number_unsigned_t) { return finishValue(); }
    bool number_float(json::number_float_t, json::string_t const&) {
        return finishValue();
    }
    bool string(json::string_t&) { return finishValue(); }
    bool binary(json::binary_t&) { return finishValue(); }

    bool start_object(std::size_t) { return openContainer(/*isObject=*/true); }
    bool start_array(std::size_t) { return openContainer(/*isObject=*/false); }
    bool end_object() { return closeContainer(); }
    bool end_array() { return closeContainer(); }

    bool key(json::string_t& name) {
        if (depth_ == 0 || !frames_[depth_ - 1].isObject) {
            scannerDisagreedFatal("a key arrived outside any object");
        }
        Frame& frame = frames_[depth_ - 1];
        for (std::size_t i = 0; i < frame.keyCount; ++i) {
            // Length first: two names of different length cannot be equal, and
            // this is the comparison the overwhelming majority of pairs take.
            std::string const& seen = keys_[frame.keyBase + i];
            if (seen.size() == name.size() && seen == name) {
                duplicatePointer_ = pointerToDuplicate(name);
                duplicateKey_     = name;
                found_            = true;
                return false;  // ★ THE ABORT
            }
        }
        // Slot assignment, never `push_back` past the high-water mark: a sibling
        // object at the same depth REUSES the `std::string` buffers its
        // predecessor allocated. See the allocation note above the class.
        std::size_t const slot = frame.keyBase + frame.keyCount;
        if (slot == keys_.size()) {
            keys_.push_back(name);
        } else {
            keys_[slot] = name;
        }
        ++frame.keyCount;
        return true;
    }

    bool parse_error(std::size_t, std::string const&, json::exception const& e) {
        scannerDisagreedFatal(e.what());
    }

    [[nodiscard]] bool found() const noexcept { return found_; }
    [[nodiscard]] std::string const& duplicatePointer() const noexcept {
        return duplicatePointer_;
    }
    [[nodiscard]] std::string const& duplicateKey() const noexcept {
        return duplicateKey_;
    }

private:
    struct Frame {
        bool        isObject  = false;
        std::size_t nextIndex = 0;  // array cursor: which element comes next
        std::size_t keyBase   = 0;  // where this object's names start in `keys_`
        std::size_t keyCount  = 0;  // how many it has taken
    };

    // Called once per COMPLETED value, whatever its shape. Only an array frame
    // has anything to do: its cursor advances so the next element gets the next
    // index. An object frame needs nothing — its cursor is the last name it
    // took, and the next `key` event appends after it.
    bool finishValue() {
        if (depth_ != 0 && !frames_[depth_ - 1].isObject) {
            ++frames_[depth_ - 1].nextIndex;
        }
        return true;
    }

    bool openContainer(bool isObject) {
        // The frame vector GROWS ONLY; `depth_` is the live cursor. Re-entering
        // a depth reuses the frame that was there, so a document 6 levels deep
        // with 40,000 objects allocates 6 frames, not 40,000.
        if (depth_ == frames_.size()) frames_.emplace_back();
        Frame& frame  = frames_[depth_];
        frame.isObject = isObject;
        frame.nextIndex = 0;
        // Its names start where the parent's live names end. Nothing is erased
        // on close: the slots simply become reachable again from the next
        // sibling at this depth.
        frame.keyBase  = (depth_ == 0)
                             ? 0
                             : frames_[depth_ - 1].keyBase + frames_[depth_ - 1].keyCount;
        frame.keyCount = 0;
        ++depth_;
        return true;
    }

    bool closeContainer() {
        if (depth_ == 0) scannerDisagreedFatal("a container closed at depth 0");
        --depth_;
        // The container just closed is a completed value in its parent, so the
        // parent's cursor advances.
        return finishValue();
    }

    // ── THE POINTER IS BUILT ONLY WHEN THERE IS A DUPLICATE TO NAME ─────────
    //
    // ⚠ ✔MEASURED 2026-08-31, and it decided this design. The first cut kept a
    // live `std::vector<std::string> path_` and pushed a freshly-built segment
    // on EVERY container — so a clean document paid one string allocation per
    // object and per nested array element, for a path nobody would ever read.
    // Decomposed on the compiler's own `build-config` clock over a one-line C
    // compile (`--time`, 9 runs, medians): the SECOND LEX PASS costs **+1 ms**
    // and is effectively free; the eager bookkeeping cost **+53 ms**, 36.7% of
    // the phase. ⇒ the two-pass design was never the expense, and a single-pass
    // rewrite (which would have meant depending on `nlohmann::detail::
    // json_sax_dom_parser`) would have bought about a millisecond while keeping
    // every one of the 53.
    //
    // Nothing is needed to NAME the path that the frames do not already hold:
    //   * an OBJECT parent's segment is the last name it took, and no further
    //     `key` event can fire at that level while its child is open
    //   * an ARRAY parent's segment is `nextIndex`, which advances only when a
    //     value COMPLETES — so while the child is open it still names the child
    // ⇒ the path is a pure function of the frame stack, reconstructed once, on
    // the refusal path, and never on the path a healthy document takes.
    [[nodiscard]] std::string pointerToDuplicate(std::string_view key) const {
        std::string out;
        for (std::size_t i = 1; i < depth_; ++i) {
            Frame const& parent = frames_[i - 1];
            out += '/';
            if (parent.isObject) {
                out += escapeJsonPointerSegment(keys_[parent.keyBase + parent.keyCount - 1]);
            } else {
                out += std::to_string(parent.nextIndex);
            }
        }
        out += '/';
        out += escapeJsonPointerSegment(key);
        return out;
    }

    // ONE flat store for the member names of every OPEN object, each frame
    // owning the half-open range `[keyBase, keyBase + keyCount)`. A per-frame
    // `unordered_set<std::string>` was the first cut and is where most of the
    // 53 ms lived: it allocates buckets and a fresh string per key, for objects
    // that overwhelmingly hold a handful of short names.
    // ⓘ The membership test is LINEAR, deliberately, and the bound it rests on
    // is ✔MEASURED rather than assumed: across all 84 shipped documents the
    // WIDEST single object is `c.lang.json`'s `/shapes` at 168 members, so the
    // worst object in the tree costs ~14k comparisons, most of which the
    // `size()`-first test settles on an integer. If a document ever arrives
    // whose one object holds thousands of members, THAT is the moment to index
    // this — measured, not pre-emptively.
    std::vector<std::string> keys_;
    std::vector<Frame>       frames_;
    std::size_t              depth_ = 0;
    std::string              duplicatePointer_;
    std::string              duplicateKey_;
    bool                     found_ = false;
};

} // namespace

std::string escapeJsonPointerSegment(std::string_view raw) {
    // RFC 6901 §3, and the ORDER IS LOAD-BEARING: `~` first, then `/`. Escaping
    // `/` first would turn a literal `/` into `~1` and the second pass would
    // then escape that `~` into `~01`, which addresses a different key.
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (c == '~') {
            out += "~0";
        } else if (c == '/') {
            out += "~1";
        } else {
            out += c;
        }
    }
    return out;
}

std::expected<nlohmann::json, ConfigDocumentParseError>
parseConfigDocument(std::string_view jsonText) {
    // ── 1. THE DOM, EXACTLY AS BEFORE ───────────────────────────────────────
    // Malformed bytes must produce the message they have always produced:
    // every caller wraps `e.what()` into its own diagnostic, and several tests
    // match that text. This function ADDS a refusal; it moves none.
    json doc;
    try {
        doc = json::parse(jsonText);
    } catch (json::parse_error const& e) {
        return std::unexpected(ConfigDocumentParseError{
            ConfigDocumentParseFailure::NotJson, std::string{}, e.what()});
    }

    // ── 2. THE DUPLICATE-KEY SCAN ───────────────────────────────────────────
    // Runs SECOND, on purpose. The scan therefore only ever walks bytes that
    // are known-good JSON, which makes its own `parse_error` callback
    // unreachable and its abort unambiguous — the parse stopped because a key
    // repeated, and for no other reason.
    DuplicateKeyScanner scanner;
    json::sax_parse(jsonText, &scanner, json::input_format_t::json,
                    /*strict=*/true);
    if (scanner.found()) {
        return std::unexpected(ConfigDocumentParseError{
            ConfigDocumentParseFailure::DuplicateKey, scanner.duplicatePointer(),
            "the key '" + scanner.duplicateKey()
                + "' is declared more than once in the same object; JSON keeps "
                  "only the LAST declaration, so an earlier one would be "
                  "silently discarded — delete the declaration that does not "
                  "apply"});
    }

    return doc;
}

} // namespace dss::detail
