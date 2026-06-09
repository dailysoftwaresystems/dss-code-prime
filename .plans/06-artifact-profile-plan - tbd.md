# `artifactProfile` Concept — Sub-Plan

> A first-class concept naming **the shape an output takes** when a language is compiled. The language config declares which profiles it supports; the project config picks one; the driver enforces compatibility. Prevents nonsense like "compile this C source as a UI binary" or "compile this T-SQL as a shared library."
>
> **Naming.** The chosen field name is `artifactProfile`:
> - *artifact* — what the compiler produces (binary, library, script, etc.). Avoids overloading "target" which already means CPU/OS.
> - *profile* — a named configuration that constrains downstream toolchain behavior, in the same sense as `BuildProfile` in modern build systems.
> - Reads naturally: `"artifactProfile": "cli"`, `"artifactProfiles": ["cli", "lib"]`.

## 0. Status (snapshot)

| | |
|---|---|
| Status        | 🟡 **AP1 ✅ + AP2 ✅ (2026-06-09); AP3 + AP4 remain.** v1 production-critical; gates §8 phase #11 (`gen-link`) acceptance. AP1 = schema field + loader. AP2 = `.dss-project.json` loader + driver profile-enforcement gate (`D_ArtifactProfileNotSupported`, fail-CLOSED empty-set, agnostic set-membership). AP3 = profile→codegen via CompilationContext (`D-AP2-COMPILATION-CONTEXT`). AP4 = per-language onboarding integration tests. |
| Predecessors  | None — schema field + project-config field + driver enforcement; no other plan blocks it. |
| Successors    | `00-compiler-implementation-plan - tbd.md` §11 (`gen-link`) consumes `artifactProfile` to pick the codegen backend variant and to drive linker invocation (executable vs shared-library vs script-output). |
| Scope         | **Bounded.** AP1: schema + loader. AP2: project config + driver enforcement. AP3: codegen backend reads it. AP4: real-world coverage (3 shipped languages onboarded). |

---

## 1. Motivation

Compiling a language means **something different per language × per consumer**. Today the project has no way to say:

- "C-subset can produce a CLI binary OR a shared library, but never a GUI bundle."
- "T-SQL doesn't produce a native binary at all — its sensible outputs are a stored-procedure deployment script or an inline embedded statement."
- "Toy is a teaching language — only the CLI profile makes sense."

Without this constraint, a user can author a project that asks for a profile the language can't legitimately produce, and the failure surfaces deep in codegen (or worse, produces a broken artifact). The check belongs at config-load time.

`artifactProfile` is also the right home for **profile-specific codegen knobs** — entry-point name (`main` vs `WinMain` vs `DllMain`), subsystem flag (PE), startup runtime (CRT vs minimal), default linker flags. The language config declares what's allowed; the project config selects; codegen reads both.

---

## 2. Design

### 2.1 Language config field (`*.lang.json`)

```jsonc
{
  "dssSchemaVersion": 2,
  "language": { "name": "c-subset", "version": "0.1.0", "fileExtensions": [".c", ".h"] },

  "artifactProfiles": ["cli", "lib"],   // ← NEW: list of valid profile names this language supports

  "tokens":  { /* ... */ },
  "shapes":  { /* ... */ }
}
```

- **Required** for `dssSchemaVersion >= 3` (new bump). For v2 configs the field is optional; absence is treated as `["cli"]` for backward compatibility with toy/c-subset, and as `["script"]` for SQL-shaped languages (heuristic: language declares a `reservedWordPolicy: "contextual"` AND no `main`-anchored shape).
- Loader emits `C_MissingField` if `dssSchemaVersion >= 3` and the field is absent or empty.
- Each entry must be a recognized profile name (see §3). Unknown entries → `C_UnknownArtifactProfile`.
- Duplicate entries → `C_RedundantField` (warning, dedup silently).

### 2.2 Project config (TBD — new file format)

The "project config" is a per-source-tree `.dss-project.json` (or similar) that points the driver at:
- The language to use (resolves to a `.lang.json`)
- The source files to compile
- The desired artifact profile
- Codegen knobs (target triple, optimization level, output path)

```jsonc
{
  "language":        "c-subset",
  "artifactProfile": "cli",                // ← NEW: must be ∈ language's artifactProfiles
  "targets":         ["linux-x86_64", "win-x86_64", "macos-arm64"],
  "sources":         ["src/**/*.c"],
  "output":          "dist/myprog"
}
```

The project-config file format is itself a v1 deliverable — see `00-compiler-implementation-plan - tbd.md` §4.1 (the `program/` driver layer) and the production-readiness plan.

### 2.3 Driver enforcement

At project-load time, the driver:

1. Loads the language config; reads `artifactProfiles`.
2. Loads the project config; reads `artifactProfile`.
3. Asserts `project.artifactProfile ∈ language.artifactProfiles`. Mismatch → fatal `D_ArtifactProfileNotSupported` (driver-phase diagnostic, new `D_*` code namespace alongside `P_*` and `C_*`).
4. Stamps the resolved profile onto the `CompilationContext` (or equivalent) so every downstream phase can read it without re-walking configs.
5. Stamps the **entry-point selection** (which source file is the artifact's entry, read from the project config) onto the same `CompilationContext`, beside the profile. This is the home for the entry-point decision — the `CompilationUnit` stays entry-agnostic ([`08-compilation-unit-plan`](./08-compilation-unit-plan%20-%20tbd.md) §2.6 C2-D2 re-routes it here from the CU layer). Codegen (§2.4) reads the entry source + the profile-derived entry-point *symbol* name together.

### 2.4 Codegen consumption

> **Config-driven, not hardcoded per profile (thesis decision #4).** The mappings below are **defaults/derivations the codegen computes from config**, not a hardwired `if (profile == "gui")` ladder. The entry-point *symbol convention* is **declared by the language config** (the language config declares the convention — e.g. a `main`-named symbol, or whichever symbol the language's own `semantics` block / per-target overrides indicate) with **per-target overrides**; the profile + target supply the subsystem/extension/runtime knobs. Codegen branches on the **target** axis (PE vs ELF vs Mach-O — expected) and reads language/profile **config**, never on `schema.name()`. A new language picks its entry convention in config, not by editing codegen.

`gen/link` resolves, from the language config + selected profile + target:
- The entry-point symbol — sourced from the language config's declared entry convention (default `main`), with target overrides (e.g. `gui`+PE → `WinMain`/`wWinMain`; `lib`+PE → `DllMain`; ELF `_init`/`_fini`; Mach-O `__attribute__((constructor))` registration).
- The PE subsystem field (`IMAGE_SUBSYSTEM_WINDOWS_CUI` vs `IMAGE_SUBSYSTEM_WINDOWS_GUI` vs the DLL flag) — from profile × target.
- The output file extension (`.exe` / `.dll` / `.so` / `.dylib` / no-extension) — from profile × target.
- Pick the linker default-library set (CRT for CLI/GUI, none for freestanding libraries).

For T-SQL-style languages (`script`, `sproc`), "codegen" is a text-emission phase, not a native-binary phase — the driver picks an entirely different backend variant.

---

## 3. Profile vocabulary — registered set

> **Updated 2026-05-23** per the universal-compiler decisions in [`00-master`](./00-compiler-implementation-plan%20-%20tbd.md) §1 (rev 2). The vocabulary is no longer a compile-time enum; it's a **registered set** owned by the loader and contributable by each shipped backend plan. New profiles arrive via the schema-version bump that introduces their backend, not via config-level name invention.

| Profile     | Meaning                                                        | Codegen path / backend plan |
|-------------|----------------------------------------------------------------|-----------------------------|
| `cli`       | Console executable. Entry: `main`.                             | Native binary, console subsystem — [`14-linker-plan`](./14-linker-plan%20-%20tbd.md). |
| `gui`       | Windowed application. Entry: `WinMain`/`wWinMain` (Win), `main` + Cocoa bootstrap (macOS), `main` + WM-toolkit bootstrap (Linux). | Native binary, GUI subsystem on PE; same binary shape on ELF/Mach-O but linked against the platform UI runtime. |
| `lib`       | Shared library (`.dll` / `.so` / `.dylib`). No entry point; named exports. | Native shared-object format per OS — `14-linker-plan`. |
| `staticlib` | Static archive (`.lib` / `.a`). Object-file bundle.            | Object emission + in-tree archiver. |
| `script`    | Text-emission profile for SQL-shaped languages. Output is a `.sql` file or equivalent. | Lowering pass that walks the CST and emits target dialect text. |
| `sproc`     | Stored-procedure deployment package. Output is a JSON/ZIP bundle the host DB engine consumes. | SQL emission + deployment-manifest assembly. |
| `transpile` | **New.** Output is target-language source text. Driven by a language-pair `.map.json`. | [`10-source-translation-plan`](./10-source-translation-plan%20-%20tbd.md). Requires `transpileTarget` + `languagePair` fields in the project config. |
| `shader`    | **New (reserved for v1.x).** Output is `.spv` (SPIR-V) + sidecar `.spv.json` reflection. | [`17-shader-gpu-plan`](./17-shader-gpu-plan%20-%20tbd.md). Source-language schema must declare shader-shape attributes (`[[shader.*]]`) and the shader-shape lattice members (Vector / Matrix / Sampler / Texture / UAV). |
| `hdl`       | **Reserved.** Output is VHDL / Verilog / SystemVerilog text describing hardware. | [`19-hir-hw-reserved-plan`](./19-hir-hw-reserved-plan%20-%20tbd.md). Reserved namespace; design lands when triggered. |

**Loader behavior.** The loader holds a **registered profile set**, populated at startup by each backend plan's registration call (`ArtifactProfileRegistry::registerProfile`). v1 ships the eight names above. Adding a new profile post-v1 = backend plan + registration call + `dssSchemaVersion` bump. No more compile-time enum; loader iterates the registered set at validation time.

**Diagnostics.**
- `C_UnknownArtifactProfile` — profile not in the registered set at load time.
- `C_ProfileBackendMissing` (new) — profile registered but its backend (per the plan table above) isn't compiled into this build. Allows partial builds (e.g. shader-less dev builds) to fail-loud rather than silently strip output.

---

## 4. Per-language defaults

| Language       | Supported profiles      | Default if project omits the field |
|----------------|-------------------------|------------------------------------|
| `toy`          | `["cli"]`               | `cli`                              |
| `c-subset`     | `["cli", "lib", "staticlib"]` | `cli`                        |
| `tsql-subset`  | `["script", "sproc"]`   | `script`                           |

These ship as part of AP4. The list is per-language data, not a hardcoded driver table — each `.lang.json` declares its own.

---

## 5. PR breakdown

| PR  | Title                                              | Scope                                                                                                                                                                                              |
|-----|----------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| ~~AP1~~ ✅ | Schema field + loader                              | **Closed 2026-05-26.** New field `artifactProfiles` on `GrammarSchemaData` + `GrammarSchema::artifactProfiles()` accessor; new diagnostic `C_UnknownArtifactProfile` (covers unknown-name + malformed-shape + duplicate via `C_ConflictingField`). Loader validates entries against the built-in vocabulary (`cli`/`gui`/`lib`/`staticlib`/`script`/`sproc`/`transpile`/`shader`/`hdl`). Unit tests in `tests/core/test_grammar_schema.cpp`; documented in `docs/language-config-spec.md`. **Deviation from original spec (deliberate):** the field landed as **optional with no `dssSchemaVersion` bump and no `C_MissingArtifactProfile`** — making it *required* is a breaking policy change best decided alongside the driver that consumes it (AP2), so absent = valid (empty list) for now. |
| ~~AP2~~ ✅ | Project-config file + driver enforcement           | **Closed 2026-06-09.** New `program/project_config.{hpp,cpp}` + JSON loader (nlohmann/json; fail-loud `C_MalformedJson`/`C_MissingField`, required `language`/`artifactProfile`/non-empty `targets`+`sources`, type-validated optional `output`, **unknown top-level keys rejected**). New `D_ArtifactProfileNotSupported = 0xD010`. `Program::compileProject` rewritten from the `D_PlanNotLanded` stub → load→`GrammarSchema::loadShipped`→enforce `artifactProfile ∈ grammar.artifactProfiles()`→route by source count→delegate; rep-injection overload added for testability. **Driver gate** = `artifactProfileSupported` (pure set-membership, agnostic — NO profile-name branch) + `enforceArtifactProfile` (emits the diagnostic; empty-set ⇒ **REJECT**, fail-CLOSED, aligning with §2.1's required-field trajectory). **Routing** = shared `routesToMultiUnit` (program.hpp), >1 source ⇒ `compileUnits` (N CUs, LK11) else `compileFiles` — same threshold as the CLI dispatcher. **§2.2 reconciliation:** `targets[]` use `<targetName>:<formatName>` specs (bare-name + default-format = `D-AP2-TARGET-NAME-DEFAULT-FORMAT`); `sources[]` literal (glob = `D-AP2-SOURCES-GLOB`); `output` routing = `D-AP2-OUTPUT-ROUTING`. Strict pins in `tests/program/test_project_config.cpp` (loader + predicate + gate + real-c-subset integration + `compileProject` end-to-end + routing) with RED-on-disable demonstrated. Independent plan-lock (Step 3.5) overrode the architect on 2 forks ON THE BAR: empty-set REJECT (not accept), and NO `CompileOptions::artifactProfile` dead knob (deferred to AP3 = `D-AP2-COMPILATION-CONTEXT`). Pinned: `D-AP2-{SOURCES-GLOB,OUTPUT-ROUTING,COMPILATION-CONTEXT,TARGET-NAME-DEFAULT-FORMAT,MULTIPLE-SOURCES-SEMANTICS,ENTRY-POINT-CONVENTION,PROFILE-REGISTRY-SPLIT}`. |
| AP3 | `CompilationContext` plumbing                      | Resolved profile flows from driver → IR → codegen via the compilation context. Each backend reads it instead of looking it up per-phase. Pinned by an interface test that the IR builder sees the right profile.                                                                  |
| AP4 | Onboarding for shipped languages                   | Config-declaration part ✅ 2026-05-26: `artifactProfiles` added to toy (`["cli"]`) / c-subset (`["cli","lib","staticlib"]`) / tsql-subset (`["script","sproc"]`). **Remaining:** snapshot tests for the per-language matrix in §4 + the driver integration test that compiles each language × each profile and asserts the artifact shape (extension, entry-point symbol, subsystem flag) — both need the AP2/AP3 driver. |

---

## 6. Open questions

| # | Question | Default if unanswered |
|---|----------|-----------------------|
| 1 | Should `artifactProfile` participate in the schema's compiled position graph (e.g. `shapes` keyed per profile so a `main` function is required for `cli` but not `lib`)? | **No** for v1 — profile-specific shape requirements are codegen-phase work, not schema-phase. Revisit when the first real grammar needs it. |
| 2 | Multi-profile compilation in one driver invocation (build both `lib` and `cli` from the same sources)? | **No** for v1 — project config picks exactly one profile. Multi-profile builds run the driver twice. |
| 3 | Versioning of the profile vocabulary itself — what if a future plan adds `kernelmod` or `wasm`? | **Resolved 2026-05-23** — registered set, not compile-time enum (see §3 rewrite). Adding a profile = the corresponding backend plan ships + registers + bumps `dssSchemaVersion`. Configs that pin `dssSchemaVersion` get deterministic behavior. |
| 4 | Should the field be plural-required on the language side (`artifactProfiles`) and singular on the project side (`artifactProfile`)? | **Yes** — semantic difference: language declares a set, project picks one. Loader rejects scalar on the language side and array on the project side. |
| 5 | Tooling — should the LSP / IDE plugin (when it lands) consume the profile to constrain code-completion (e.g. don't suggest `WinMain` in a `lib` project)? | **Out of v1 scope.** Logged for the future tooling plan. |

---

## 7. Acceptance criteria

- [ ] Every shipped `.lang.json` declares `artifactProfiles` (AP4).
- [ ] Every shipped language has at least one integration test that compiles real source through each declared profile and asserts the produced artifact's shape.
- [ ] Mismatched profile in a project config produces `D_ArtifactProfileNotSupported` with a clear actionable message ("language `c-subset` does not support `gui`; supported profiles are: `cli`, `lib`, `staticlib`").
- [ ] Codegen backends key off the resolved profile rather than the language name. Switching profile for a project requires zero language-config edits.
- [ ] Docs: `docs/language-config-spec.md` §10 (new) documents the field; a separate `docs/project-config-spec.md` covers the project-config file format.
