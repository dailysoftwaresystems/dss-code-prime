# Contributing to DSS Code Prime

Thanks for looking. This file covers what to send, what the bar is, and how
contributions are licensed.

## What's welcome right now

**Issues, bug reports, reproductions, and discussions — always, no agreement needed.**
The [issue forms](.github/ISSUE_TEMPLATE) will guide you, and
[discussions](.github/DISCUSSION_TEMPLATE) are open for questions and ideas.

The single most valuable thing you can send is **a small C program that this
compiler gets wrong**, together with what you expected and what you got. A
reduced reproduction is worth more here than a patch, because every fix in this
project has to come with a test that pins the behaviour permanently.

**Code contributions** — see [Licensing of contributions](#licensing-of-contributions)
below first. Please open an issue before writing a patch so we can confirm the
terms and the approach before you invest the time.

## The bar

These are not style preferences. A change that breaks one of these is rejected
regardless of whether the test suite is green.

### 1. The shared engine stays source-, target-, and format-agnostic

No `if (language == …)`, `if (arch == …)`, or `if (format == …)` in the shared
substrate — `src/{opt,mir,hir,lir,core,analysis,asm,tokenizer,link,preprocess}`.

Language, CPU, and object-format knowledge lives in configuration, not in code:

| Vocabulary | Declared in |
|---|---|
| Source language | `.lang.json` |
| CPU target | `.target.json` |
| Object format | `.format.json` |

If you need the engine to know something new about a platform, the fix is
almost always a new field read from config, not a branch in the substrate. This
is the property the whole design exists to protect — it is why a new target is a
JSON file rather than a fork.

### 2. Fail loud — never silently miscompile

Given something it cannot handle correctly, the compiler must emit a real
diagnostic naming the actual problem. Producing plausible-looking wrong code is
the worst outcome in this project, worse than crashing and far worse than
refusing. Don't add a fallback path that guesses.

### 3. New behaviour comes with a test that goes red without it

Not "a test that passes." A test that **fails when you revert your change**.
Verify that directly — disable the fix, watch the test go red, re-enable it.
A test that passes both ways is not protecting anything.

Where the change is user-visible, add a runnable example under `examples/` with
an `expected.json`, including a `release` arm so the optimizer is exercised and
not just the debug path.

### 4. No workarounds

Fix the cause. If something blocks you and the real fix is out of scope, say so
in the issue rather than routing around it — a documented gap is worth more than
a quiet patch over it.

## Building and testing

Prerequisites: a C++23 compiler (MSVC 17.5+, GCC 13+, Clang 16+), CMake 4.0+, and
network access on first configure (FetchContent pulls nlohmann/json and GoogleTest).

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

```bash
cd build && ctest --output-on-failure
```

Always use `--output-on-failure`. A failing test's output is the evidence, and a
summary line without it is not a bug report.

This compiler cross-emits, so it is normal for the suite to *compile* artifacts
for platforms you are not running on and skip only the execution step. A change
that breaks cross-emission shows up as a compile failure on your own machine —
please don't skip those tests to get green locally.

## What a good pull request looks like

- One concern per PR.
- The failing test in the first commit, the fix in the second, if you can split them.
- A description that says what was wrong, not only what you changed.
- No unrelated reformatting — it hides the actual diff.

## Licensing of contributions

DSS Code Prime is licensed under **Apache License 2.0** (see [LICENSE](LICENSE)
and [NOTICE](NOTICE)). Copyright is currently held in full by **Daily Software
Systems LTDA** — which is what made relicensing the project from proprietary to
open source possible in the first place.

**We are finalizing our formal contribution terms.** Until they are published:

- **Issues, bug reports, reproductions, and discussions require no agreement.**
  Send them freely.
- **Code contributions are accepted by prior arrangement.** Open an issue or
  email us first and we will confirm the terms before you write anything. We
  would rather have that conversation up front than ask you to re-license work
  you have already done.

This is not a closed door — it is us not wanting to take your work under terms
neither side has read yet.

## Security

Please do not open public issues for security problems. Email
[rafaelgasperetti@dailysoftwaresystems.com](mailto:rafaelgasperetti@dailysoftwaresystems.com)
directly.

## Contact

Maintained by **Rafael Gasperetti** —
[rafaelgasperetti@dailysoftwaresystems.com](mailto:rafaelgasperetti@dailysoftwaresystems.com).
For commercial work, partnerships, or sponsorship, see
[dailysoftwaresystems.com](https://dailysoftwaresystems.com/).
