# Project Config (`.dss-project.json`) — Specification

> The **project config** points the driver (`Program::compileProject`) at *what to build*:
> which language, which artifact profile, which targets, which sources. It is the
> file-driven counterpart to the `dss-code-prime` CLI flags. Owned by plan 06
> (`artifactProfile`, AP2/AP3) + the `program/` driver layer.
>
> Companion spec: the per-**language** declaration of which profiles a language *supports*
> lives in [`language-config-spec.md` §11.6](./language-config-spec.md) (`artifactProfiles[]`).

---

## 1. Shape

```jsonc
{
  "language":        "c-subset",                       // required — resolves to a shipped .lang.json
  "artifactProfile": "cli",                            // required — one profile (see §3)
  "targets":         ["x86_64:elf64-x86_64-linux-exec"], // required — ≥1 "<targetName>:<formatName>" spec
  "sources":         ["src/main.c"],                   // required — ≥1 source path (literal; no glob yet)
  "output":          "dist/myprog"                     // optional — see §6 (parsed, not yet routed)
}
```

The driver loads this file, validates it, enforces the two `artifactProfile` gates (§4), then
delegates to the existing compile path — routing by source **count** (§5).

---

## 2. Fields

| Field | Required | Type | Meaning |
|---|---|---|---|
| `language` | **yes** | non-empty string | The shipped language to compile (`src/dss-config/sources/<language>.lang.json`). |
| `artifactProfile` | **yes** | non-empty string | The **single** output shape to produce (§3). Singular — the language declares a *set*, the project picks *one*. |
| `targets` | **yes** | non-empty array of non-empty strings | Each entry is a `"<targetName>:<formatName>"` spec (e.g. `"x86_64:elf64-x86_64-linux-exec"`). One artifact is produced per target. |
| `sources` | **yes** | non-empty array of non-empty strings | The source files. Literal paths today (no glob — `D-AP2-SOURCES-GLOB`). Resolved relative to the process working directory. |
| `output` | no | non-empty string when present | A user output hint. **Parsed + type-validated, but its path routing is not yet wired** (`D-AP2-OUTPUT-ROUTING`) — artifacts currently land at the per-target convention (§5). |

**Unknown top-level keys are rejected** (`C_MalformedJson`) — a typo like `"ouput"` fails loud rather
than being silently ignored, matching the grammar/target/format loaders.

---

## 3. Artifact profiles

`artifactProfile` must be a name in the **registered profile set** (loader-owned vocabulary, shared
with the language side — `core/types/artifact_profile.hpp`):

`cli` · `gui` · `lib` · `staticlib` · `script` · `sproc` · `transpile` · `shader` · `hdl`

See [`06-artifact-profile-plan`](../.plans/06-artifact-profile-plan%20-%20tbd.md) §3 for each
profile's meaning. A profile is only *usable* in a project when **both** gates in §4 accept it.

> **What ships today.** The only profile any shipped object format **serves** is `cli` (the four
> runnable exec formats). `lib`/`staticlib`/`script`/`sproc`/`gui`/… are registered names a language
> may *declare*, but no shipped format *emits* them yet — so a project requesting them is rejected at
> the format gate (§4) until the corresponding backend ships. This is by design: a format may only
> claim a profile it can actually produce.

---

## 4. The two driver gates

Both gates are a single generic set-membership test (no per-profile-name or per-format branch —
the agnosticism veto). They run **before any compilation**, so a bad profile fails fast and cheap.

| # | Gate | Rule | On failure |
|---|---|---|---|
| AP2 | **Language gate** | `artifactProfile ∈` the language's declared `artifactProfiles[]` | `D_ArtifactProfileNotSupported` (`D0010`) — *fix the request or the `.lang.json`*. Message names the language + lists its supported profiles. |
| AP3 | **Format gate** (per target) | `artifactProfile ∈` each target's object-format `artifactProfiles[]` (the profiles that format *serves*) | `D_ArtifactProfileFormatMismatch` (`D0011`) — *pick a target whose format produces this profile, or ship that backend*. Checked for **every** target. |

The two codes are **remediation-distinct**: `D0010` means the *language* can't produce the profile;
`D0011` means the *chosen format* can't. A profile the language declares but no format serves (e.g.
`c-subset` + `staticlib`, or `tsql-subset` + `script`) passes AP2 and is caught by AP3.

An empty declared/served set ⇒ **reject** (fail-closed): a language or format that claims no profiles
is not project-buildable.

---

## 5. Routing & output

- **Source count routing** (shared `routesToMultiUnit`, identical to the CLI dispatcher): `>1`
  source ⇒ N independent compilation units the linker merges (`compileUnits`, `cc a.c b.c`
  semantics); `≤1` ⇒ the single-CU path (`compileFiles`).
- **Output path** (today): `<cwd>/target/<formatName>/<sourceStem><ext>`, where `<ext>` comes from
  `ObjectFormatKind × objectType` (e.g. ELF/Mach-O executable ⇒ no extension; PE executable ⇒
  `.exe`; relocatable ⇒ `.o`). The `output` field does **not** yet redirect this
  (`D-AP2-OUTPUT-ROUTING`).

---

## 6. Loader diagnostics

| Code | When |
|---|---|
| `D_FileNotFound` | the project file can't be opened, or a hard I/O error occurs mid-read. |
| `C_MalformedJson` | invalid JSON; non-object root; an **unknown** top-level key; a field of the wrong type; a non-string / empty array entry; an empty `output` string. |
| `C_MissingField` | a required field (`language` / `artifactProfile` / `targets` / `sources`) is absent, an empty string, or an empty array. |
| `D_ArtifactProfileNotSupported` (`D0010`) | the language gate (§4). |
| `D_ArtifactProfileFormatMismatch` (`D0011`) | the format gate (§4). |
| `D_InvalidTargetSpec` / `D_SchemaLoadFailed` | a `targets[]` entry that doesn't parse as `<name>:<format>`, or names a format that won't load — emitted by the delegated compile (the gates skip such a target rather than double-report). |

---

## 7. Deferred (pinned)

These are intentional gaps, each pinned in the deferred-anchor registry — the doc states them so it
does not over-promise:

| Anchor | Gap |
|---|---|
| `D-AP2-SOURCES-GLOB` | `sources[]` are literal paths — no glob expansion (`"src/**/*.c"` is one literal "file" today). |
| `D-AP2-OUTPUT-ROUTING` | `output` is parsed + validated but not yet routed; the per-target convention (§5) applies. |
| `D-AP2-TARGET-NAME-DEFAULT-FORMAT` | `targets[]` require the explicit `:<formatName>` half; bare names (`"linux-x86_64"`) with an inferred default format aren't resolved yet. |
| `D-AP2-COMPILATION-CONTEXT` | the resolved profile is **not** threaded to codegen (entry-symbol / subsystem / extension); deferred until a profile drives a codegen difference its `(target:format)` doesn't already encode (e.g. `gui`). |

---

## 8. Examples

A console executable from one C source, for Linux x86-64:

```jsonc
{ "language": "c-subset", "artifactProfile": "cli",
  "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["main.c"] }
```

Multi-target (one profile, several formats — one artifact each):

```jsonc
{ "language": "c-subset", "artifactProfile": "cli",
  "targets": ["x86_64:elf64-x86_64-linux-exec", "x86_64:pe64-x86_64-windows-exec"],
  "sources": ["a.c", "b.c"] }
```

A rejected request (lands on `D0011`): `c-subset` *declares* `lib`, but no shipped format *serves* it:

```jsonc
{ "language": "c-subset", "artifactProfile": "lib",
  "targets": ["x86_64:elf64-x86_64-linux-exec"], "sources": ["main.c"] }
// → D_ArtifactProfileFormatMismatch: artifact profile 'lib' is not served by object format 'elf64-x86_64-linux-exec' (serves: cli).
```
