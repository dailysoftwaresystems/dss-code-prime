# The rule-lens — violations a green gate misses

## E. The rule-lens — the subtle violations the green gate misses

This is where the auditor earns its keep. Each entry: the **class**, its **tell**, and the
**disproof** (what the auditor checks to confirm or kill it).

1. **Conservative-default hardcode** (agnosticism, "a tiny bit"). A *language/target semantic* baked
   into shared substrate as a default, rather than declared in config — even in the safe/conservative
   direction. *Tell:* a literal like `TypeKind::Char` or a fixed register handled directly in shared
   code, where every sibling semantic is config-declared. *Disproof:* is this choice declared in
   `.lang.json`/`.target.json` and read by a universal algorithm, or is it in the engine? If a real
   second consumer (another language/target) would be silently wronged or forced to fight it, it must
   be config-shaped *now*. (The char-aliases-all flag was exactly this — sound, but hardcoded; the fix
   was a config bit, not a smaller hardcode.) *Distinguish* from a legitimate **anchored single-target
   residual** (§F) — that has no second consumer to wrong yet and is trigger-gated for the 2nd target.

2. **Effectiveness-masking / the optimization that never ran.** A differential or corpus test where
   the optimized arm silently exercised *nothing* because an earlier pass eliminated the work. *Tell:*
   constant operands feeding the thing under test — e.g. a literal `100/7` folds at compile time, so the
   idiv never executes under the optimized pipeline and the optimized arm tests nothing (cycle 10r's
   division corpus deliberately uses runtime function-arg operands to avoid exactly this).
   *Disproof:* inspect the *optimized* MIR and confirm the opcode under
   test is actually present; require operands the optimizer cannot fold (function args across a
   non-inlined call). A "optimized == baseline" arm passes even with an inert optimizer — demand an
   **effectiveness assertion** (the specific op count drops / `passMutationCount` moves).

3. **The knob that lies / dead config wire.** A config field that looks live but never reaches its
   consumer — behaviour is correct only by coincidence of the default matching. *Tell:* a new
   `*.json` key whose value equals the field default, with no end-to-end test driving the *non*-default
   through the full chain. *Disproof:* a test that sets the non-default value through the real
   pipeline (loader → config → lowering → consumer) and asserts the behaviour changed. Worse than a
   visible hardcode: it *looks* configurable while being ignored.

4. **Asserted-not-proven guard (the missing red-on-disable).** A correctness mechanism exists in
   `src/` but no test forces it to matter. *Tell:* the mechanism is present and the schema/constraint
   is tested, but no *behavior* test exercises the failure it prevents. *Disproof:* a pin that goes
   **red when the guard is disabled** — watched, not asserted. (The regalloc implicit-clobber exclusion
   had the mechanism + a schema test but no behavior pin until a vreg-live-across-the-op test with a
   demonstrated red-on-disable.)

5. **Speculative closure of a trigger-gated deferral.** Closing a `D-*` because it is next in a
   backlog, when its trigger has not fired. *Tell:* an anchor struck `✅ CLOSED` whose registry
   trigger ("first real-input failure" / "3rd consumer" / "WASM-SPIR-V backend") never occurred.
   *Disproof:* the trigger condition is met in this delta. If not → it should read "trigger not
   fired", and building it was over-engineering (a no-workaround violation in the other direction).

6. **Plan ↔ implementation divergence.** The plan claims a state the code doesn't match (or vice
   versa). *Tell:* a `✅ CLOSED` row with no corresponding test/code, an anchor cited in `src/` with
   no registry row, a "commit-pending" row that is already pushed, a stale stepper. *Disproof:* the
   anchor guard for src↔registry; manual cross-read for plan↔code claims.

7. **Cross-platform / CI blind spot.** Local-green that is CI-red. *Tell:* MSVC builds clean but a
   header (`<format>`, `<span>`, `<algorithm>`, `<cstdint>`) is used without explicit include (MSVC's
   transitive includes mask it; GCC/Clang don't); gtest `ASSERT_*` in a non-void helper. *Disproof:*
   only CI confirms it — the auditor **flags it unverified** (§K), never claims green it cannot run.

8. **Over-claimed close.** An anchor marked fully closed when only part of its stated scope landed.
   *Tell:* the registry/anchor text describes more than the commit delivered. *Disproof:* read the
   anchor's *stated scope* and confirm the work satisfies all of it; otherwise it is a **partial**
   close with the remainder re-anchored — not a strike.

---
