---
name: dss-code-prime
description: >
  Repository guide for DSS Code Prime, a config-driven compiler whose source language, CPU target,
  and object format are declared in JSON rather than compiled in. Use this whenever working in this
  repo and you need to know how something is structured or what the local rules are — the Tree/Node
  model, TreeCursor and visitor walks, NodeAttribute side-tables, typed views, the
  .lang.json/.target.json/.format.json schema system, TreeBuilder, the .plans/ system, CMake wireup,
  diagnostic discipline, the mandatory coding conventions, or the strict-assertion testing posture.
  Also use it before adding a type, a typed view, a grammar, or a diagnostic code, even if the user
  never says "skill". NOT for running a development cycle (use dss-cycle), auditing an implementation
  (use dss-audit), or reconciling plan staleness (use dss-plan-sweep) — this skill explains how the
  repo works, it does not drive work. When it disagrees with docs/ or .plans/, the doc and plan win.
user-invocable: true
argument-hint: "[topic or question]"
---

# DSS Code Prime — Repository Guide

A universal, configurable compiler frontend: one C++23 engine compiles *any* defined language to
*any* supported target, with the vocabulary in config rather than in code.

## When to use

- You need to know how a subsystem works before changing it, or where the canonical example lives.
- You are about to add a public type, a typed view, a `.lang.json` grammar, or a diagnostic code.
- You need the local conventions — they are mandatory and differ from general C++ habit.

**Not this skill:** driving a development cycle → `dss-cycle`. Judging an implementation against
the bar → `dss-audit`. Reconciling plan drift → `dss-plan-sweep`.

**Authority order:** [`docs/tree-model.md`](../../../docs/tree-model.md) and
[`docs/language-config-spec.md`](../../../docs/language-config-spec.md) and
[`.plans/`](../../../.plans/) all outrank this skill. When they disagree, the doc and the plan win.
[`real-examples/`](../../../real-examples/) outranks every claim here — it is the registry of real
repositories DSS compiles from unmodified upstream source and whose own suites it then runs.

## What exists today

The **whole pipeline ships**: tokenizer → preprocessor → parser → semantic analysis → HIR → MIR →
LIR → optimizer → register allocation → **DSS's own assembler** (x86_64 + arm64, with a round-trip
encoding oracle) → **DSS's own linker** (ELF / PE / Mach-O, static and dynamic). No `as`, no `ld`,
no LLVM. The tree/node foundation (T0–T12), schema-expressiveness v2 (PR0–PR8) and substrate
hardening (SH1–SH4) are the substrate underneath it, not the whole product.

**Stack:** C++23, CMake 4.0+, FetchContent for `nlohmann/json` 3.12.0 and GoogleTest 1.17.0. Local
Windows dev uses MinGW GCC 13.2 (ucrt); CI exercises Linux/GCC-13, Linux/Clang-19+ASan,
Windows/MSVC and macOS/AppleClang on every PR.

## The rules that actually break things when ignored

These three cause real damage, so they get the emphasis rather than every convention:

1. **Agnosticism.** No `if (lang == …)`, `if (arch == …)` or `if (format == …)` in shared substrate
   (`src/{opt,mir,hir,lir,core,analysis,asm,tokenizer,link,preprocess}`). Vocabulary belongs in
   `.lang.json` / `.target.json` / `.format.json`. A fact with an owner does not get a second owner.
2. **Strict asserts in tests.** A test that cannot go red is not protecting anything — this is the
   posture that makes every regression visible, and it is why the suite is trusted.
3. **Fail loud, never silently miscompile.** Invariant guards use the local `*Fatal` helpers, never
   `<cassert>`. Producing plausible wrong output is the worst outcome available.

Everything else — strongly-typed IDs, immutable post-build `Tree`, `DSS_EXPORT` discipline,
`[[nodiscard]]`, comment policy, move semantics, no abbreviations — is in
`references/testing-and-conventions.md` and is equally mandatory, just less explosive.

## Workflow

1. Identify which subsystem the question touches, and read that reference file (see file map).
2. Check the canonical example before writing anything new — this repo has an established pattern
   for nearly everything, and matching it is cheaper than inventing.
3. Apply the conventions from `references/testing-and-conventions.md`. They are not style
   preferences; the fatal-helper and strict-assert rules are load-bearing.
4. Verify against the authority order above whenever this skill and a doc disagree.

## What a change must satisfy

Before considering any change complete:

- Agnosticism holds — no language/arch/format branch entered shared substrate.
- New behaviour has a test that goes **red when the change is reverted**, not merely one that passes.
- Invariants fail loud through the local `*Fatal` helpers.
- Public types carry `DSS_EXPORT`; accessors and consequential returns carry `[[nodiscard]]`.
- The relevant `.plans/` surface is updated in the same change.

The full checklist, including the per-pattern recipes, is in `references/workflows-and-status.md`.

## File map

- Read `references/tree-model.md` for the Tree/Node storage model, strong IDs, `TreeCursor`,
  visitor walks, `NodeAttribute<T>` side-tables, typed views, and diagnostics.
- Read `references/schema-and-builder.md` for the `.lang.json` schema system — loading, what the
  schema captures, built-in token kinds, loader diagnostics, well-known names — and `TreeBuilder`.
- Read `references/testing-and-conventions.md` before writing any test or any new code. It holds
  the strict-assert rules, known-good test patterns, Windows/MinGW death tests, and all eight
  mandatory coding conventions.
- Read `references/repo-map.md` when you need the directory layout, `real-examples/`, the build
  system and toolchain, the SSH-reachable non-x86 hardware, canonical examples, or the header map.
- Read `references/workflows-and-status.md` for the step-by-step recipes (adding a type, a typed
  view, a grammar, a diagnostic code, driving `TreeBuilder` from tests), the `.plans/` system, the
  honest done/not-done status, and the contribution checklist.

## Failure modes this skill exists to prevent

- **Breaking agnosticism** by putting a language, arch, or format fact in shared substrate instead
  of config — the slow break that no grep catches until a second target arrives.
- **Writing a test that passes both ways.** Verify red-on-disable directly.
- **Reaching for `<cassert>`** instead of the local `*Fatal` helpers.
- **Trusting this skill over the docs, the plans, or `real-examples/`** when they disagree.
- **Inventing a pattern** the repo already has a canonical example for.
