# Hosts, drivers, and the operational rules that each cost a run

## 2. Hosts and drivers

| host | driver | notes |
|---|---|---|
| Windows | `build-and-test.ps1` | pe64 native; ELF legs run through the declared `wsl.exe -e` launcher |
| Linux / WSL x86_64 | `build-and-test.sh` | elf64-x86_64 native; elf64-arm64 under `qemu-aarch64` |
| macOS (Apple Silicon) | `build-and-test.sh` | macho-arm64 native; macho-x86_64 under `arch -x86_64` (Rosetta) |
| arm64 Linux VPS | `build-and-test.sh` | native aarch64 — the best control for "is this provider really declaration-driven?" |

★ **One driver per host, and `.sh` on Windows is NOT a supported configuration.** The `.sh`
driver's `case "$(uname -s)"` accepts only `Linux` and `Darwin`, and that is the design, not
a gap — `.ps1` *is* the Windows driver. Its `die` states the real contract. Do not "fix" it:
a second driver on a host that already has one is duplication, not independence, and would
make the two harnesses the same experiment wearing two names.

★ **The VPS is the strongest control in the set.** It is native aarch64 with no Mac, no
Windows and no `/mnt/c`. If `elf64-x86_64` and `pe64-x86_64` build *there*, the providers are
genuinely declaration-driven rather than "worked because the host happened to match".

---

### Operational rules that have each cost a run

- **`SRC_DIR` is mandatory when the checkout is not at `$HOME/src/dss-code-prime`.** The
  harness REFUSES to run rather than silently clone `main` — because an unattended multi-hour
  corpus run would validate a compiler that is not the branch under test, and the results
  section would print that commit as if it were. Set `SRC_DIR=<checkout>` explicitly.
- **Remote runs must survive the session close**: `setsid nohup … < /dev/null &`, then
  **verify**. A launch line that printed is not a process that is running.
  ⚠⚠ **`setsid` DOES NOT EXIST ON macOS** — it is util-linux, not BSD. ✔MEASURED 2026-09-09: this
  exact prescription died with `bash: line 1: setsid: command not found` on the darwin carriage
  while the same line worked on the arm64 VPS. ★ It is the `pgrep -c` trap below wearing the other
  face, in the same paragraph that warns about it: **an idiom this document RECOMMENDS is itself
  Linux-only.** On macOS use `nohup … < /dev/null > <log> 2>&1 &` (launching from a
  non-interactive ssh command there is already outside the caller's job table), or `caffeinate -i`
  if the box may idle-sleep mid-run.
  ⚠ **AND VERIFY BEHAVIOURALLY, NOT BY ARGV.** ✔MEASURED the same day: a `ps ax -o command |
  grep -c '[b]uild-and-test.sh'` probe reported **`alive: 2` when the `cd` had failed and NOTHING
  had launched** — because it matches the ssh command SHELL, whose own argv carries the marker. The
  bracket keeps grep from matching itself and does nothing about its parent. ⇒ **Ask whether the LOG
  GREW between two samples.** A process that is running writes; one that never started does not,
  and no argv can fake it.
  ⚠ **`pgrep -c` DOES NOT EXIST ON macOS, and the failure is silent-by-idiom.** BSD `pgrep`
  has no `-c`; it exits 2 with a usage error, which the customary `|| echo 0` then converts
  into a confident **`alive: 0`**. ✔MEASURED 2026-08-07: this reported a healthy multi-hour
  macOS corpus run as DEAD, and the "recovery" relaunch truncated the live run's log before
  the run lock correctly refused it. Use `ps ax -o command | grep -c '[b]uild-and-test.sh'`,
  which is portable and self-excluding. ★ And keep the probe's own command line clear of the
  marker: `pgrep -f <marker>` matches the shell that carries `<marker>` in its argv and
  reports itself as a survivor — already anchored from the `.ps1` side
  (`build-and-test.ps1`), and it bites the status probes exactly the same way.
- **Pin Tcl when the host's default disagrees with the legs' libraries** (`DSS_TCL_VERSION=8.6`).
  Every leg's libtcl is 8.6; a host whose default Tcl is 9.0 will otherwise compile against a
  9.0 header and link an 8.6 library. The per-leg coherence check now catches this and says so.
- **Capture rc DIRECTLY, never after a pipe.** `cmd | head; echo $?` reports *head's* status.
  This has produced false "clean" readings in this project more than once.
- **A machine run must pull COMMITTED state**, so it cannot race local edits. That is also
  what makes it safe to launch remote runs while agents are still editing locally.
- **Never quote a corpus number without its upstream commit.** The harness pulls upstream on
  every run, so two runs execute different corpora. `1 error / 331,745` is meaningless without
  `sqlite @ <sha>`.

---
