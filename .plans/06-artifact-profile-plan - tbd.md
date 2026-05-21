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
| Status        | ⏳ **planned.** v1 production-critical; gates §8 phase #11 (`gen-link`) acceptance. |
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

### 2.4 Codegen consumption

`gen/link` reads the profile to:
- Pick the entry-point symbol name (`main` for `cli`, `WinMain`/`wWinMain` for `gui` on Windows, `DllMain` for `lib` on Windows + `_init`/`_fini` on ELF, `__attribute__((constructor))` registration on Mach-O).
- Set the PE subsystem field (`IMAGE_SUBSYSTEM_WINDOWS_CUI` vs `IMAGE_SUBSYSTEM_WINDOWS_GUI` vs the DLL flag).
- Choose the output file extension (`.exe` / `.dll` / `.so` / `.dylib` / no-extension).
- Pick the linker default-library set (CRT for CLI/GUI, none for freestanding libraries).

For T-SQL-style languages (`script`, `sproc`), "codegen" is a text-emission phase, not a native-binary phase — the driver picks an entirely different backend variant.

---

## 3. Built-in profile vocabulary

The schema interner predeclares these profile names so the loader can validate `artifactProfiles` entries:

| Profile  | Meaning                                                        | Codegen path                                       |
|----------|----------------------------------------------------------------|----------------------------------------------------|
| `cli`    | Console executable. Entry: `main`.                             | Native binary, console subsystem.                  |
| `gui`    | Windowed application. Entry: `WinMain`/`wWinMain` (Win), `main` + Cocoa bootstrap (macOS), `main` + WM-toolkit bootstrap (Linux). | Native binary, GUI subsystem on PE; same binary shape on ELF/Mach-O but linked against the platform UI runtime. |
| `lib`    | Shared library (`.dll` / `.so` / `.dylib`). No entry point; named exports. | Native shared-object format per OS.                |
| `staticlib` | Static archive (`.lib` / `.a`). Object-file bundle.         | Object emission + archiver.                        |
| `script` | Text-emission profile for SQL-shaped languages. Output is a `.sql` file or equivalent. No native codegen. | Lowering pass that walks the CST and emits target dialect text. |
| `sproc`  | Stored-procedure deployment package. Output is a JSON/ZIP bundle the host DB engine consumes. | SQL emission + deployment-manifest assembly.       |

**Extension points.** A language config that needs a profile not in this list emits `C_UnknownArtifactProfile`. The right response is a follow-up plan (or v3 schema bump) adding the profile name to the built-in set, not config-level name invention — profiles need codegen-backend support to mean anything, so a name without a backend is dead text.

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
| AP1 | Schema field + loader                              | `dssSchemaVersion` bump to 3 (back-compat for absent field per §2.1); new field `artifactProfiles` on `GrammarSchemaData`; new diagnostic codes `C_UnknownArtifactProfile`, `C_MissingArtifactProfile`. Loader pass validates entries against the built-in vocabulary. Unit tests in `tests/core/test_grammar_schema.cpp`. |
| AP2 | Project-config file + driver enforcement           | New `program/project_config.{hpp,cpp}` + JSON loader (reuses the nlohmann/json infra). New `D_*` diagnostic namespace + `D_ArtifactProfileNotSupported`. Driver-layer integration test fixture. |
| AP3 | `CompilationContext` plumbing                      | Resolved profile flows from driver → IR → codegen via the compilation context. Each backend reads it instead of looking it up per-phase. Pinned by an interface test that the IR builder sees the right profile.                                                                  |
| AP4 | Onboarding for shipped languages                   | Add `artifactProfiles` to toy / c-subset / tsql-subset. Snapshot tests for the per-language matrix in §4. Driver integration test that compiles each language × each profile, asserting the produced artifact has the expected shape (extension, entry-point symbol, subsystem flag).                                                                                                                                                                                |

---

## 6. Open questions

| # | Question | Default if unanswered |
|---|----------|-----------------------|
| 1 | Should `artifactProfile` participate in the schema's compiled position graph (e.g. `shapes` keyed per profile so a `main` function is required for `cli` but not `lib`)? | **No** for v1 — profile-specific shape requirements are codegen-phase work, not schema-phase. Revisit when the first real grammar needs it. |
| 2 | Multi-profile compilation in one driver invocation (build both `lib` and `cli` from the same sources)? | **No** for v1 — project config picks exactly one profile. Multi-profile builds run the driver twice. |
| 3 | Versioning of the profile vocabulary itself — what if a future plan adds `kernelmod` or `wasm`? | Built-in vocab is owned by the loader; adding a profile = schema-version bump. Configs that pin `dssSchemaVersion` get deterministic behavior. |
| 4 | Should the field be plural-required on the language side (`artifactProfiles`) and singular on the project side (`artifactProfile`)? | **Yes** — semantic difference: language declares a set, project picks one. Loader rejects scalar on the language side and array on the project side. |
| 5 | Tooling — should the LSP / IDE plugin (when it lands) consume the profile to constrain code-completion (e.g. don't suggest `WinMain` in a `lib` project)? | **Out of v1 scope.** Logged for the future tooling plan. |

---

## 7. Acceptance criteria

- [ ] Every shipped `.lang.json` declares `artifactProfiles` (AP4).
- [ ] Every shipped language has at least one integration test that compiles real source through each declared profile and asserts the produced artifact's shape.
- [ ] Mismatched profile in a project config produces `D_ArtifactProfileNotSupported` with a clear actionable message ("language `c-subset` does not support `gui`; supported profiles are: `cli`, `lib`, `staticlib`").
- [ ] Codegen backends key off the resolved profile rather than the language name. Switching profile for a project requires zero language-config edits.
- [ ] Docs: `docs/language-config-spec.md` §10 (new) documents the field; a separate `docs/project-config-spec.md` covers the project-config file format.
