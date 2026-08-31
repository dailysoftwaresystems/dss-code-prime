#pragma once

#include "core/export.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <vector>

// ══ CONTENT-ADDRESSED MEMO FOR A BUILT CONFIG SCHEMA ════════════════════════
//
// A `.lang.json` / `.target.json` / `.format.json` document is loaded MANY
// times inside ONE process, and every load rebuilds the identical structure
// from the identical bytes. ✔MEASURED 2026-08-25 (cycle P34, the
// `D-CONFIG-A-SCHEMA-DOCUMENT-IS-REBUILT-ONCE-PER-LOAD-INSIDE-ONE-PROCESS`
// census), Windows Debug, at `8cb9afbd`:
//
//   dss_analysis_semantic_test_semantic_analyzer_c  58.4 s wall, of which
//       53.4 s (91.4%) is 876 loads of ONE 506,143-byte `c.lang.json`
//   dss_hir_test_hir_lowering_c                     29.8 s wall, of which
//       22.1 s (74.2%) is 361 loads of that same document
//   one `dsscp --compile` of a ONE-LINE C file      18 config loads for 8
//       distinct documents, 312.3 ms of a 647 ms process
//
// Every one of those repeats produces a structure that is byte-for-byte the
// same decision surface as the one before it, so the rebuild is pure waste.
//
// ── WHY THE KEY IS THE CONTENT DIGEST AND NOT THE NAME ──────────────────────
// ⚠ The obvious design — memoize by document NAME — is the one already in the
// tree at `src/lsp/schema_cache.cpp`, and it has NO invalidation at all: an
// editor session that outlives a config edit serves the pre-edit grammar and
// nothing detects it. A schema drives every downstream decision, so a stale
// hit is a SILENT MISCOMPILE, not a stale cache entry. ⛔ Do not reproduce
// that pattern here.
//
// The key is therefore the SHA-256 OF THE DOCUMENT BYTES, which the load path
// already computes on every load for `contentDigest()` — so the key costs
// NOTHING NEW, and a document whose bytes changed by even one byte simply
// cannot address the old entry. There is no invalidation policy to get wrong,
// because there is no invalidation: an entry is reachable only from the bytes
// that produced it.
//
// ★ THE LABEL IS PART OF THE KEY, and that is not belt-and-braces. A loader
// writes `sourceLabel` into the diagnostics it carries and into its
// per-shape origin map, so two loads of the SAME bytes under DIFFERENT labels
// legitimately differ in what they report. Keying on the pair keeps the memo
// transparent rather than merely fast.
//
// ── THE HALF A CONTENT DIGEST CANNOT SEE, WHICH IS WHY `dependencies` EXISTS ─
// ⚠ A language document may fold ANOTHER document into its build —
// `languageReferences` reads `<ref>.lang.json` off disk and merges its shapes.
// The host's digest covers the HOST's bytes and says nothing about the
// referenced document, so `c.lang.json` + an EDITED `asm.lang.json` digests
// identically to `c.lang.json` + the original. Keying on the host digest alone
// would serve a grammar built from the old fragment, silently.
//
// ⇒ every entry records the resolved path AND the build-time digest of every
// document that entered its build, and a lookup RE-READS AND RE-DIGESTS each
// one before it will hand the entry back. A dependency that moved, or that can
// no longer be read at all, is a MISS — never a hit with a shrug. The re-read
// costs what the build was going to spend reading that document anyway.
//
// ── SCOPE: THIS PROCESS ONLY ────────────────────────────────────────────────
// ⓘ Deliberately in-process. It is not a disk cache and holds nothing across
// invocations: correctness here rests entirely on the bytes being re-read and
// re-digested by the same process that will use the result, so there is no
// serialized object graph to keep in step with the structures it rebuilds and
// no compiler-identity term to get wrong. A cross-process cache is a separate
// mechanism with a separate failure surface; see the P34 lane report.
//
// ── CAPACITY ────────────────────────────────────────────────────────────────
// Bounded, LEAST-RECENTLY-USED, and the bound is a MEMORY guard rather than a
// correctness one: evicting an entry can only cost a rebuild, never a wrong
// answer. The bound only ever bites on a harness that walks many distinct
// MUTANTS of one document — exactly the case where holding them all would be
// the defect.
//
// ⚠⚠ THE FIRST CUT OF THIS PARAGRAPH STATED A PREMISE THAT TWO SHIPPED CODE
// PATHS REFUTE, AND THE PREMISE IS WHAT SET THE NUMBER
// (D-CONFIG-MEMO-CAPACITY-IS-HALF-THE-SHIPPED-CORPUS-SO-A-TOTAL-SCAN-THRASHES-IT).
// It read *"the real working set is the handful of documents one invocation
// touches (the shipped-language scan is six)"* and fixed the bound at 16.
// ✔MEASURED 2026-08-31 (cycle P46, lane `dg`), Windows Debug, at `f865897c`:
// the shipped corpus is **32** documents — 24 `.format.json`, 6 `.lang.json`,
// 2 `.target.json` — and TWO production paths scan the object-format class in
// FULL, in one uninterruptible loop, because each is proving UNIQUENESS and a
// uniqueness proof cannot early-exit:
//   * `runtime::resolveArchiveSiblingFormat` (`program/runtime_object_cache.cpp`),
//     run once per BUILD — the root's and every dependency sub-build's;
//   * `Resolver::shippedFormats_` (`program/dependency_resolver.cpp`), once per
//     dependency resolve.
// A 24-document loop through a 16-slot store evicts EIGHT OF ITS OWN ENTRIES
// before it finishes, so the sweep's hit rate is ZERO no matter how many times
// it runs — and it additionally evicts everything loaded BEFORE it, which
// includes the 506 KB language document, the single most expensive build in the
// process. ✔MEASURED, a project-mode compile with one `staticlib` dependency at
// one consumer target: `build-config` runs — the phase whose `runs` IS the miss
// count — **105**, against **29** for the same project with no `dependsOn`, and
// dead linear at **+26 per additional dependency and +25 per additional consumer
// target** (105 / 131 / 183 / 287 at K = 1 / 2 / 4 / 8).
//
// ★★★ SO THE DEFECT WAS NEVER "THE NUMBER IS TOO SMALL" — IT IS THAT A COUNT
// BOUND HAS A SILENT CLIFF AND NOTHING WATCHES IT. The store degrades from a
// 100% hit rate to a 0% one the moment one document class outgrows the bound,
// with no diagnostic, no test, and no symptom other than the compiler getting
// slower. ⇒ TWO changes, and the second is the load-bearing one:
//   1. the policy is LRU, not FIFO. FIFO evicts the OLDEST-INSERTED entry,
//      which is precisely the language document — loaded first, used
//      throughout, and the most expensive one to rebuild. LRU keeps a document
//      that is still being used, so a total scan over a DIFFERENT class can no
//      longer displace it. (LRU alone does not fix a working set larger than
//      the bound; it fixes which entry loses when one is over.)
//   2. `kCapacity` is stated as a MULTIPLE OF THE WHOLE SHIPPED CORPUS rather
//      than as a guess about a working set — and
//      `tests/program/test_config_memo_holds_a_total_scan` COUNTS the real
//      corpus on disk and goes RED when it no longer fits with headroom. That
//      is what makes the cliff loud: the document that would silently halve the
//      compiler now reddens a test in the same commit that ships it.
//
// ⓘ Capacity is a compiled constant and deliberately not config: it names no
// vocabulary, changes no output, and a document that has to be READ to learn how
// much of itself to remember is a bootstrapping problem with nothing to gain.
// `capacity()` is exported so the test asserts the RELATION against a measured
// corpus instead of re-spelling the number.

namespace dss::detail {

// One document folded into a schema's build, and what it hashed to then.
struct DSS_EXPORT ConfigDocumentDependency {
    std::string path;    // resolved path, as the loader resolved it
    std::string digest;  // lowercase 64-hex SHA-256 of its bytes at build time
};

// SHA-256 of the file's bytes, or `nullopt` when it cannot be read. Reads
// through the same checked whole-file read the loaders use, so a short read is
// a failure here rather than a digest of a truncated document.
//
// ⓘ Takes the path as a STRING and resolves nothing. A dependency's spelling is
// recorded verbatim from the loader that already resolved it, and re-read under
// that exact spelling — so this file has no path IDENTITY question to answer
// and deliberately owns no `<filesystem>` of its own
// (`scripts/check-path-identity` names the rule).
[[nodiscard]] DSS_EXPORT std::optional<std::string>
digestConfigDocumentFile(std::string_view path);

// ── ONE STORE, TYPE-ERASED, AND THAT IS NOT AN IMPLEMENTATION DETAIL ────────
//
// ⚠ ✔MEASURED 2026-08-25 (cycle P34) — the FIRST cut of this file held its
// entries in `inline static` members of the class template below, which reads
// as the obvious C++17 answer and is WRONG across this project's shared
// library. The engine is built with `-fvisibility=hidden` into `libdsscp`, so a
// test binary that instantiates `ConfigDocumentMemo<GrammarSchema>` gets its
// OWN copy of those statics: two memos, and the one the loader actually uses is
// not the one an external caller can see or clear. The soundness arm caught it
// by asking for a COLD rebuild, clearing the memo it could reach, and getting
// the same pointer back.
// ★ The mechanism generalises past this file: any header-only template that
//   owns MUTABLE process-wide state silently forks at a hidden-visibility
//   library boundary, and nothing about it looks wrong at the call site.
// ⇒ the state lives in ONE exported, non-template class, defined once in
//   `config_document_memo.cpp`; the template below is a typed FACADE with no
//   storage of its own.
class DSS_EXPORT ConfigDocumentMemoStore {
public:
    struct Stats {
        std::uint64_t hits                 = 0;
        std::uint64_t misses               = 0;
        std::uint64_t dependencyRejections = 0;
        std::uint64_t evictions            = 0;
    };

    // `family` separates one schema type's entries from another's; the facade
    // supplies the type's own mangled name, so it cannot collide and cannot
    // drift out of step with a hand-maintained table.
    [[nodiscard]] static std::shared_ptr<void> lookup(std::string_view family,
                                                      std::string_view label,
                                                      std::string_view digest);

    static void store(std::string_view family, std::string label,
                      std::string digest,
                      std::vector<ConfigDocumentDependency> dependencies,
                      std::shared_ptr<void> schema);

    // Drop every entry, every family. For tests that need a COLD memo to
    // observe the build path itself; nothing in the compiler calls it.
    static void clear();

    [[nodiscard]] static Stats stats();
    static void resetStats();

    // The entry bound, so a test can assert it against the corpus it has just
    // COUNTED rather than against a number copied out of the implementation —
    // see the capacity note above for why the relation, not the value, is what
    // has to hold.
    [[nodiscard]] static std::size_t capacity() noexcept;

    // How many entries are resident right now. Exported for the same reason:
    // "the total scan fit" is a statement about residency, and a test that
    // inferred it from a hit count could not tell "fit" from "was re-read".
    [[nodiscard]] static std::size_t size();
};

// The typed facade. `SchemaT`'s own `typeid` name is the family key.
template <class SchemaT>
class ConfigDocumentMemo {
public:
    using Stats = ConfigDocumentMemoStore::Stats;

    // The memoized schema for this (label, digest), or `nullptr`. The store
    // re-reads and re-digests every recorded dependency first; any mismatch is
    // a miss.
    [[nodiscard]] static std::shared_ptr<SchemaT> lookup(std::string_view label,
                                                         std::string_view digest) {
        return std::static_pointer_cast<SchemaT>(
            ConfigDocumentMemoStore::lookup(family(), label, digest));
    }

    static void store(std::string label, std::string digest,
                      std::vector<ConfigDocumentDependency> dependencies,
                      std::shared_ptr<SchemaT> schema) {
        ConfigDocumentMemoStore::store(family(), std::move(label),
                                       std::move(digest), std::move(dependencies),
                                       std::move(schema));
    }

    static void clear() { ConfigDocumentMemoStore::clear(); }
    [[nodiscard]] static Stats stats() { return ConfigDocumentMemoStore::stats(); }

private:
    // ★ The COMPILER's own name for the type, compared as a STRING rather than
    // by `type_info` address: the address is not stable across a shared-library
    // boundary, which is the very hazard this whole indirection exists to
    // survive. The `static_pointer_cast` above is safe exactly because no two
    // distinct types can produce the same mangled name.
    [[nodiscard]] static std::string_view family() {
        return typeid(SchemaT).name();
    }
};

} // namespace dss::detail
