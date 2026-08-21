# Repository structure, toolchain, and header map

## 2. Repository Structure

| Directory | Purpose |
|-----------|---------|
| `src/core/` | The static `core` library — compiled into `libdss-code-prime.dll` |
| `src/core/types/` | The tree/node model and friends |
| `src/tokenizer/` | Lexing (`tokenizer`, `token_stream`, `source_reader`) |
| `src/analysis/` | Parsing + semantic analysis; **preprocessing is `src/analysis/preprocess/`**, not a top-level `src/preprocess/` |
| `src/hir/`, `src/mir/`, `src/lir/` | The three-tier IR |
| `src/opt/` | The optimizer passes (`debug` / `release` pipelines) |
| `src/asm/` | DSS's own assembler — x86_64 + arm64 byte encoding, round-trip oracle |
| `src/link/` | DSS's own linker — ELF / PE / Mach-O, static + dynamic |
| `src/ffi/` | FFI descriptors — how a target library's symbols are declared |
| `src/program/`, `src/source-factory/`, `src/lsp/` | Driver / project manifest, source assembly, language server |
| `src/dss-config/` | **The config vocabulary — this is where "target = data, not code" lives**: `sources/` (`.lang.json`), `targets/` (`.target.json`), `object-formats/` (`.format.json`), `pipelines/`, `shippedLibs/`, `schemas/` |
| `examples/c-subset/` | The runnable corpus — each dir is `main.c` + `expected.json`, compiled → executed → exit/stdout asserted |
| `tests/` | GoogleTest unit + integration tests (one executable per file) |
| **`real-examples/`** | **★ The real-world repository registry — see §2.1. Known upstream projects DSS compiles from unmodified source and whose own test suites it runs** |
| `integrated_tests/` | The CLI-subprocess examples runner (live ctest entry `integrated_tests`) — sibling of the in-process `tests/examples/examples_runner.cpp`; **a capability added to one MUST be added to the other** |
| `docs/` | User-facing onboarding docs |
| `.plans/` | Internal design records, roadmap, and `_deferred-anchor-registry.md` |
| `scripts/`, `packaging/` | Every repo script — one directory per script, siblings inside — plus build/publish tooling. Index: `scripts/README.md` |
| `build*/` | CMake build dirs (gitignored). Windows Debug gate dir is **`build-dbg`** |

### 2.1 `real-examples/` — the real-world repository registry

**This is the project's primary end-to-end evidence, and the answer to "what real software does
DSS actually compile?"** It is not a snapshot of vendored source: each entry is a harness that
**clones the real upstream repository**, builds it with DSS from **unmodified** source, and then
runs **that project's own test suite**, classifying every failure.

| Entry | What it proves |
|---|---|
| [`real-examples/c/sqlite`](../../../real-examples/c/sqlite) | SQLite, full upstream source — **189 translation units** through one `--project` manifest → SQLite's own `testfixture` → SQLite's own unit corpus. `full` tier green on three legs — Linux x86_64 **7 / 1,061,830**, Linux arm64-under-qemu **12 / 1,060,828**, Windows pe64 **0 / 979,736** (the pe64 count is lower because platform gating reaches fewer test files, not because anything was skipped). Every residual failure is a matched-control confound. The `sqlite3` CLI is ALSO built from full source — **103 TUs**, not the amalgamation (`real-examples/c/sqlite/gen-pe64-manifest.py` emits its manifest). The single-file amalgamation is compiled and run as a separate, much faster probe — an ADDITIONAL check, never a stand-in for the real build |

Each entry ships **both drivers** — `build-and-test.sh` (Linux; adds an arm64-under-qemu leg on
an x86_64 host) and `build-and-test.ps1` (Windows pe64). Tiers: `veryquick | quick | full | all`.

Non-negotiable rules for anything under `real-examples/`:

- **Never patch the upstream tree** — not the library, not the test infrastructure — and **never
  exclude a test file to reach green**. An upstream bug is root-caused, anchored and reported
  upstream; the harness must *survive* it, not edit it away.
- **A "known confound" must be EARNED, per name and per platform, with a matched control** — a
  reference `testfixture` built by the system compiler (containing no DSS code) failing the same
  test on the same machine. Family resemblance is not a control, and a confound earned on one
  platform does not transfer to another.
- **Every unit gets a verdict.** Silence about a unit is a harness bug.

---

## 3. Build System and Toolchain

- **CMake floor: 4.0** (`CMAKE_MINIMUM_REQUIRED`). Verified working with CMake 4.3.2 on Windows.
- **C++ standard: 23** project-wide.
- **Dependencies are vendored via `FetchContent`** — `nlohmann/json` v3.12.0 and GoogleTest v1.17.0.
  `nlohmann/json` is `PRIVATE` to the one TU that needs it (`grammar_schema_json.cpp`).
- **Tests use `dss_add_test`** — a project-local helper macro that registers a GoogleTest
  executable AND a ctest entry. One executable per test file (parallelism + per-file failure isolation).
- **Public include path** is set once via `target_include_directories(core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)`
  — header-only additions (`tree_visitor.hpp`, `tree_attrs.hpp`, `tree_views.hpp`, `well_known_names.hpp`)
  don't need explicit registration.
- **Build commands** (Windows / PowerShell). ⚠ **Use `build-dbg`, not `build`.** `build-dbg` is the
  healthy single-config Ninja + g++ gate dir (no `-C <config>`); the `build` dir is an MSVC
  configuration that goes stale and is bigobj-fragile. Building the wrong one is a recurring
  source of false reds.
  ```powershell
  cmake --build C:\Source\DailySoftware\dss-code-prime\build-dbg
  cmake --build C:\Source\DailySoftware\dss-code-prime\build-dbg --target dss_core_test_tree_views
  ctest --test-dir C:\Source\DailySoftware\dss-code-prime\build-dbg --output-on-failure
  ```
  Test targets are `dss_<dir>_test_<x>`; the matching ctest names are `<dir>/test_<x>`.
  **Run a FULL build (no `--target`) whenever a shared header struct changed.**

### 3.1 Real non-x86-Windows hardware is reachable over SSH (added 2026-08-04)

Two physical machines are now scriptable, which changes what "verified" can mean for the
non-native targets. Both are reached through **capability-paired** helpers in `scripts/`:
`ssh-arm64-vps.{sh,ps1}` and `ssh-macos.{sh,ps1}`.

| host | what it is | why it matters |
|---|---|---|
| **aarch64 Ubuntu 24.04 VPS** | native arm64 Linux, 4 cores / 23 GB | Every prior arm64 result came from **qemu**, which says nothing about real silicon. The `.sh` driver was first exercised end-to-end here (2026-08-04: **331,330 tests, 1 known non-DSS confound**, `elf64-arm64` running NATIVELY). Also a genuinely *third* host for the de-host-locking property. |
| **macOS 26.5.2, arm64** | the operator's personal MacBook | macOS is the ONE target with **no off-Mac emulator** — nothing on Windows or Linux runs a Mach-O. It is the only way a Darwin artefact is ever proven to RUN. |

**Contract, and it is not optional:**

- **Connection data is NEVER tracked.** Precedence is CLI parameter → env → `.secrets/<name>.env`
  → **fail loud naming what to set**. `.secrets/` is gitignored; this repo is slated to go
  public (PR #37). It holds host names, logins and key **PATHS** — never key material, never
  a password.
- **Key-based auth only.** Both helpers pass `BatchMode=yes` so ssh **fails** rather than
  hanging at a prompt. Password auth is not supported by design: it cannot be automated
  without putting the secret in the environment, and a credential in a repo script is a
  committed secret.
- **★★ THE MAC IS A PERSONAL MACHINE, NOT CI. It is usually OFF. ASK THE OPERATOR TO TURN IT
  ON BEFORE USING IT.** Never wake it, never poll for it. "Cannot reach" is the EXPECTED
  state, not an error to route around.
- Use a **temp directory** for build experiments on either host; do not scribble in the repo.

**Two measured traps, both already cost time:**

- **The Mac's IP changes every lease** — it is resolved by `.local` mDNS per invocation, which
  is also why `StrictHostKeyChecking=no` is kept deliberately (a moving address would trip a
  host-key mismatch on every reconnect). That forgoes MITM protection: fine on a home LAN,
  not on an untrusted one.
- **`Test-Connection <host>.local` resolves the IPv6 link-local FIRST**, so `.IPv4Address` is
  `$null` and a naive script reports "cannot resolve" for a Mac that is powered on and
  answering pings. Use `Resolve-DnsName` filtered to IPv4. MEASURED 2026-08-04.
- `.local` very often does **not** resolve from WSL (no mDNS responder); the `.sh` helper says
  so and names the literal-IP override rather than leaving it to be discovered.

**What the Mac settled that nothing else could** (MEASURED 2026-08-04): it carries **no
`/usr/lib/libtcl*.dylib` at all**; system Tcl is a *framework* at **8.5**; Homebrew is not
installed. So the testfixture's Tcl **8.6** symbols cannot come from the Mac even natively —
the MacPorts `pinned-archive` acquisition is the only source, and `@loader_path` bundling is
the correct shipping shape rather than a fallback.

---

## 12. Where to Look for Canonical Examples

| Want to see | Look at |
|---|---|
| A clean test file that asserts STRICTLY | `tests/core/test_tree_visitor.cpp` (23 tests, full-sequence comparisons, allocation counter, static_asserts) |
| The strictest broken-path pattern | `tests/core/test_tree_end_to_end.cpp` (9 tests, full pretty-print equality, exact diagnostic counts, error-leaf walks) |
| A header-only template done right | `src/core/types/tree_attrs.hpp` (`NodeAttribute<T>`, custom move ops, dual storage) |
| A typed view | `src/core/types/tree_views.hpp` (all 7 views, ~250 lines) |
| The fatal helper pattern | `src/core/types/tree.cpp:20-25` (`treeFatal`) or `src/core/types/tree_attrs.hpp:35-40` (`attrFatal`) |
| Driving `TreeBuilder` from a test | `tests/core/test_tree_builder.cpp` (40+ tests covering every recovery flavor) |
| The shipped grammar config | `src/dss-config/sources/toy.lang.json` |
| Onboarding docs writing style | `docs/tree-model.md` (the WhileStmtView cookbook is the template) |

---

## 14. Quick-Reference Header Map

| Need | Include |
|---|---|
| Read a tree | `core/types/tree.hpp` |
| Build a tree | `core/types/tree_builder.hpp` |
| Move through a tree | `core/types/tree_cursor.hpp` |
| Walk every node | `core/types/tree_visitor.hpp` |
| Attach data to nodes | `core/types/tree_attrs.hpp` |
| Typed views | `core/types/tree_views.hpp` |
| Standard rule / token names | `core/types/well_known_names.hpp` |
| Source location | `core/types/source_span.hpp`, `core/types/source_buffer.hpp` |
| Tokens (input to builder) | `core/types/token.hpp` |
| Strong IDs | `core/types/strong_ids.hpp` |
| Diagnostics | `core/types/parse_diagnostic.hpp`, `core/types/diagnostic_reporter.hpp` |
| Grammar schema | `core/types/grammar_schema.hpp` |
| Schema JSON loader | `core/types/grammar_schema_json.hpp` (PRIVATE — only `grammar_schema.cpp` should include) |
| Interner template | `core/types/interner.hpp` |
| Schema cursor | `core/types/schema_cursor.hpp` (internal — accessed via `GrammarSchema`) |
| Scope kinds | `core/types/scope_kind.hpp` |
| Rule IDs | `core/types/rule_id.hpp` |
| `DSS_EXPORT` macro | `core/export.hpp` |
