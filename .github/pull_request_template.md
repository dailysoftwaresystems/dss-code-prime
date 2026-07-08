## What changed
<!-- Brief description of the modifications made in this PR -->

## Related issue
<!-- Link the GitHub issue this addresses. Use a closing keyword (Closes / Fixes) so it auto-closes on merge. -->
Closes #

## Type of change

- [ ] Bug fix
- [ ] New feature
- [ ] Refactor / code improvement
- [ ] CMake / build-system change
- [ ] CI/CD / infrastructure
- [ ] Documentation

## Checklist

- [ ] **Agnostic** — no `if (arch/format/lang == …)` branch added to the shared substrate (`src/{opt,mir,hir,lir,core,analysis,asm,tokenizer,link}`); target vocabulary stays in config.
- [ ] **Fails loud** — no path can silently miscompile; disabling a feature turns a test red, not quiet.
- [ ] I have built and run the test suite locally (`ctest --output-on-failure`), and it covers this change.
- [ ] No secrets or credentials are committed
- [ ] `Run Pipes` label is applied so the gated pipeline-pr workflow runs
