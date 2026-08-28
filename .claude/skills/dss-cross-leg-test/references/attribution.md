# Attribution — before blaming the compiler

## 5. Attribution — before blaming the compiler

**Never report a red cell as a DSS defect without running the oracle.** The client ships a
same-platform reference build for exactly this:

```
wsl.exe -e <stage>/reference-testfixture <the same staged .test>
```

**If the reference fails the same way, it is upstream or environment — not DSS.**

### The two attribution traps this project has actually fallen into

1. **Read the assertion VALUES, not the test NAMES.** A 57-failure population named
   `wal2-*`, `walsetlk-*`, `journal3-*`, `e_walauto-*` was diagnosed as the WAL/journal
   *timing* family and routed to a known clock defect. The values said
   `expected [00644 00400 00644]` / `got [00777 00555 00777]` — it was the file **permission**
   family, and only 1 of 57 was clock-related. A test's name is a label someone chose; its
   assertion is the measurement.
2. **A different population is not a control.** "pe64 passed in the same run" proved nothing,
   because every failing family there was gated `ne "Windows NT"` — pe64 never executed those
   assertions at all.

### Grep the registry BEFORE commissioning an experiment

Search `_deferred-anchor-registry*.md` for the leg, the artifact, the test family and the
symptom; cite what you find or state that nothing matched. A 2×2 attribution was once
commissioned from scratch whose identical experiment and verdict were already in the registry
from seven cycles earlier — and the un-cited row would have pre-empted three false statements
that reached a commit. **Recall finds what is similar; grep finds what is the same.**

### Known confound families (sqlite) — all provenanced per leg in `legs.json`

- **WSL2 `CLOCK_REALTIME` oscillates ±34.47 s** — the `walsetlk`/`busy2` timing family.
- **DrvFs has no `metadata` mount option** — `chmod 644 → stat 777`, `400 → 555`. Any
  permission assertion fails when a launched leg's rundir sits on `/mnt/c`.
- **`zipfile-25.0`** — an upstream symlink/`fread` leak, compiler- and filesystem-independent.

⚠ A confound must carry `earnedOn` / `earnedAt` / `mechanism` / `anchor`. A suppression rule
without provenance is either inert or unearned — one shipped for weeks matching nothing at
all, and was invisible precisely because it never fired.

---
