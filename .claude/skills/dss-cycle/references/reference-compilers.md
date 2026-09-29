# The reference compilers are the spec — the union, over what WORKS

What a reference compiler's behaviour is evidence FOR, and what it is not: one working reference
makes a behaviour required, the disjunction decides acceptance and not meaning, and the union is taken
over the references that WORK. The P14 case behind the first ruling is `the-bar.md` §A.3b.

## Contents
- The goal is to WORK — one working reference makes the behaviour REQUIRED (operator, 2026-08-19)
- The disjunction decides ACCEPTANCE, not MEANING (operator ruling 2026-08-28)
- The union is over what WORKS, not only over what is accepted (operator ruling 2026-09-02)

## ★★★ THE GOAL IS TO *WORK* — one working reference makes the behaviour REQUIRED

Operator ruling 2026-08-19: *"we must never crash on correct code, even if gcc fails, we must do it right. … If we have a reference that works, we must too (of course, with the implementation always following our project's best practices)."*

**The test is the DISJUNCTION, not the consensus.** If ANY reference (gcc, clang, MSVC, …) compiles and runs a correct construct, DSS must too. A reference's FAILURE is therefore never evidence against DSS — when DSS accepts what one reference rejects and another accepts, DSS is **right**, and the divergence is a **NON-DSS CONFOUND** to attribute and record. **Never make DSS fail in order to match a failing reference.** This bounds the bidirectional rule: "accepting what no reference accepts is a defect" turns on **NO** — not one. The implementation still owes the full bar (agnostic, config-driven, best-long-term, fail-loud, strictly tested): "it works" is the requirement, not the excuse. ⚠ Probe references **separately** — "the reference" is not one voice, and P14 nearly narrowed a working header chain because only gcc's failure was on file and MSVC's success was not. Full case: `references/the-bar.md` §A.3b.

### ★★★★ THE DISJUNCTION DECIDES **ACCEPTANCE**, NOT **MEANING** — operator ruling 2026-08-28

**The rule above settles whether a construct is ACCEPTED. It does NOT automatically settle what a
program MEANS when the references disagree about the meaning itself.** ✔The case that produced this,
measured with each reference probed separately: `#pragma once` — gcc dedups two DIFFERENT files with
byte-identical contents (**content-keyed**); clang 18.1.3 and MSVC 19.51 do not (**identity-keyed**).
The unanimous rows (`./h.h`, `sub/../h.h`, symlink, hard link — all DEDUP everywhere) make
`core::PathIdentity` REQUIRED; the split row decides the KEY.

A mechanical reading of the disjunction picks **gcc, the minority**. ⇒ **THE OPERATOR RULED
IDENTITY-KEYED**: DSS refuses a program gcc compiles (a vendored/copied header), deliberately.

**The two reasons, because they generalise:**
1. **Content-keying does not merely accept MORE — it SILENTLY OMITS TEXT** in a constructible case
   (two byte-identical headers whose meaning differs because a macro was redefined between the
   `#include`s). "Accept more" is not a virtue when the extra acceptance is bought by DROPPING code,
   and a silent omission is the class the bar most abhors.
2. **2 of 3, including the vendor that invented the pragma.** A minority-of-one winning on the
   disjunction deserves a second look — not a veto, a prompt to state the trade-off.

⇒ **Before invoking the disjunction, ask which question the references are splitting on.**
Accept-vs-refuse ⇒ the disjunction governs and the accepting reference wins. Disagreement about what
a valid program MEANS ⇒ **that is an architectural fork: PAUSE and ask.** AMENDED 2026-09-21 by "you do everything. I'm not your babysitter." — a meaning fork is decided by the agent, by measurement and the references' own documentation, the rationale written into the row and the decision reported veto-able (see SKILL.md, the decision gate)
⇒ **Record the refusal cost in the row**, or a later cycle applying the disjunction by reflex will
"fix" it back. This ruling is exactly that shape.

### ★★★★ THE UNION IS OVER WHAT **WORKS**, NOT ONLY OVER WHAT IS ACCEPTED — operator ruling 2026-09-02

> *"our own readme says: `DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO C`. We are meant to be the best of our
> references. Everything that works in one of the vertices, must work here. That's it, so the
> tiebreaker is not clang, is whether one of the references make it work or not."*
> … *"Last resort is check against iso C, since our goal is to work if any references work."*
> … **"We always aim to work, to have quality."**

**THERE IS NO PRIVILEGED REFERENCE. THERE IS NO TIEBREAKER VERTEX.** ⚠ This section previously said
"clang breaks the tie" and that was WRONG — a general rule mistaken for a special case, corrected by
the operator the same day it was written. Clang was named in the ruling as *an example of who to ask
for another opinion*, not as an arbiter. **The union already decides it.**

**The rule, and it is the one the README has always stated:** the union is taken over references
that **WORK**, not merely over references that ACCEPT. Acceptance is the weaker reading — a
reference can accept a program and then emit code that faults, tears, or silently drops the meaning.
⇒ **If ANY vertex compiles a correct construct AND the result WORKS, DSS must work too**, whichever
vertex that turns out to be. A vertex that accepts-but-does-not-work casts no vote for its own
output; it is simply not a working reference for that construct.

| the split is about | what governs |
|---|---|
| **ACCEPT vs REFUSE** | the union — any reference that accepts a correct construct makes it REQUIRED |
| **QUALITY — one reference WORKS and another silently does not** | **the union again, read over WORKING**: match the one that works, whichever it is |
| **What a valid program MEANS**, both readings defensible and both working | an architectural fork — **PAUSE and ask** AMENDED 2026-09-21 by the decision gate — decided by the agent, recorded in the row, reported veto-able (see SKILL.md) |

⚠ **A quality split is NOT a meaning fork, and must not be escalated as one.** A meaning fork is two
defensible readings of the same program (`#pragma once` content-keyed vs identity-keyed) where both
implementations deliver their own reading correctly. A QUALITY split has a right answer: the
references agree on what the program means and one of them fails to deliver it. **Measure which one
works and match it.**

✔**THE CASE THAT PRODUCED THIS, and it corrected a call I had already escalated.** A packed
`_Atomic int` at offset 1 on x86_64: clang emits the generic `__atomic_load`/`__atomic_store`
libcall, **gcc INLINES**, MSVC abstains (refusing `_Atomic` even aligned). I read that as 1–1 with
no tiebreaker and asked the operator. It never was a tie: gcc's `LOCK`-prefixed store IS atomic,
but its paired plain `movl` LOAD is not when the 4-byte access spans a cache line, so **gcc's route
silently loses atomicity** — strictly worse than the arm64 case, where the same shape SIGBUSes
loudly. gcc is therefore not a working reference here, clang is, and the union settles it with no
adjudication needed.

★★ **AND THE MEASUREMENT NEEDS A CONTROL, OR IT IS NOT ONE.** ✔MEASURED `clang 18.1.3 -O1 -S`:
`x86_64-pc-windows-msvc`, `x86_64-w64-windows-gnu` and `x86_64-pc-linux-gnu` **all three** emit the
libcall for the under-aligned member — and **the naturally-aligned control emits NO libcall and an
inline `xchgl` on all three.** Without that control, "clang emits a libcall" is equally consistent
with *"this target lowers all atomics through libcalls"*, which would say nothing about alignment.
⇒ **Probe the DEFECTIVE case and the HEALTHY case, on every target you intend to rule for.**

⚠ **A reference's output is not evidence of the property you care about until you NAME the
property.** *"DSS runs rc 42 on x86_64"* proved **no fault**; it did not prove **atomicity**. Two
different questions, and only one of them was the row's subject. ⇒ State which property you are
measuring before you read the result.

⇒ **Record in the row WHICH reference was the working one and on what measurement**, so a later
cycle applying the union by reflex over ACCEPTANCE cannot quietly revert it to the majority answer.
