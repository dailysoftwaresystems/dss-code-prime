# 26 — Publish Plan: `dsscp` → OS package managers

> **Status:** SCAFFOLDED (inert) — not yet live.
> **Branch / PR:** `feature/enhance-workflows` / PR #45 (commit `b832e843`).
> **Goal:** every `dsscp` release auto-publishes to Windows / Linux / macOS package managers.
> **Deep reference (per-manager commands + gotchas):** [`packaging/README.md`](../packaging/README.md).
> **Machinery already built:**
> - [`.github/workflows/publish-packages.yml`](../.github/workflows/publish-packages.yml) — fires on `release: published`, **inert until repo variables are set**.
> - [`packaging/`](../packaging/) — `render.py` + 5 manifest templates (homebrew / scoop / aur / snap / nix).
> - [`.github/workflows/pipeline-pkg.yml`](../.github/workflows/pipeline-pkg.yml) — builds the 4 platform tarballs **and now emits `SHA256SUMS`**.

## How it works (one paragraph)

`pipeline-pkg.yml` builds `dsscp-<version>-<platform>.tar.gz` for linux-x64 / windows-x64 / macos-arm64 / linux-arm64 and publishes the GitHub release. `publish-packages.yml` then fires on `release: published`: a `prepare` job resolves the version/channel from the tag (`vX.Y.Z` = stable → package `dsscp`; `vX.Y.Z-beta` = beta → package `dsscp-beta`) and downloads each asset to record its sha256, then fans out to one job per package manager. Every job is gated on a repository **variable**, so with none set **nothing runs**. You turn managers on one at a time as you provision each one's repo + token.

---

## ⛔ PHASE 0 — BLOCKERS (nothing publishes *cleanly* until these land)

- [ ] **P0.1 — CMake `install()` rules + rpath.** Install the exe to `bin/` and the shared lib to `lib/`, with `$ORIGIN/../lib` (Linux) / `@loader_path/../lib` (macOS) so the exe finds the lib. *(The "keep exe+lib" decision. Compiler-repo change — behind the linker/FFI hold.)*
- [ ] **P0.2 — `dss-code-prime` → `dsscp` binary rename.** So the installed command is `dsscp`. *(Compiler-repo change — behind the hold. The release tarball is already named `dsscp`; only the binary inside is still `dss-code-prime`.)*
- [ ] **P0.3 — (winget only) a windows `.zip` asset.** winget wants an installer/portable `.zip`, not the `.tar.gz` we ship. Emit `dsscp-<ver>-windows-x64.zip` in `pipeline-pkg.yml` (or repackage in the winget job). Not needed for the other 7 managers.

> Keep `ENABLE_PKG_PUBLISH` **unset** until P0.1 + P0.2 are done — a live publish before then ships a manifest that can't install cleanly.

---

## 📦 PHASE 1 — one-time accounts + repos to create

- [ ] **Homebrew** — create repo **`dailysoftwaresystems/homebrew-dsscp`** *(the `homebrew-` prefix is mandatory for `brew tap`)*.
- [ ] **Scoop** — create repo `dailysoftwaresystems/scoop-dsscp`.
- [ ] **Nix** — create repo `dailysoftwaresystems/dsscp-nix`.
- [ ] **AUR** — create an AUR account; register an SSH **public** key on it.
- [ ] **deb + rpm** — create a **Cloudsmith** account + a repository (free OSS tier).
- [ ] **Snap** — `snapcraft register dsscp` on the Snap Store.
- [ ] **winget** — fork `microsoft/winget-pkgs` under a bot/user account.

---

## 🔑 PHASE 2 — secrets + variables (Settings → Secrets and variables → Actions)

### Secrets (*Secrets* tab)
- [ ] `TAP_PUSH_TOKEN` — a PAT with **push/contents:write** to the tap + bucket + flake repos (one token covers Homebrew + Scoop + Nix if same org).
- [ ] `AUR_SSH_PRIVATE_KEY` — the SSH **private** key matching the AUR-registered public key.
- [ ] `CLOUDSMITH_API_KEY` — Cloudsmith API key (Account → API settings). Covers deb + rpm.
- [ ] `SNAPCRAFT_STORE_CREDENTIALS` — output of `snapcraft export-login -`.
- [ ] `WINGET_TOKEN` — a **classic** PAT with `public_repo` (for your winget-pkgs fork).

### Variables (*Variables* tab)
- [ ] `ENABLE_PKG_PUBLISH = true` — **master switch** (off ⇒ whole workflow is a no-op).
- [ ] `HOMEBREW_TAP_REPO = dailysoftwaresystems/homebrew-dsscp`
- [ ] `SCOOP_BUCKET_REPO = dailysoftwaresystems/scoop-dsscp`
- [ ] `NIX_FLAKE_REPO = dailysoftwaresystems/dsscp-nix`
- [ ] `CLOUDSMITH_REPO = dailysoftwaresystems/dsscp`
- [ ] `WINGET_IDENTIFIER = DailySoftwareSystems.dsscp`
- [ ] `PKG_MAINTAINER = DSS <noreply@dailysoftwaresystems.com>` *(optional; metadata)*
- [ ] per-manager enable flags (set as you roll each out in Phase 3): `ENABLE_HOMEBREW` · `ENABLE_SCOOP` · `ENABLE_AUR` · `ENABLE_NIX` · `ENABLE_DEB` · `ENABLE_RPM` · `ENABLE_SNAP` · `ENABLE_WINGET` = `true`.

---

## 🚀 PHASE 3 — enable managers ONE AT A TIME (recommended order: value ÷ effort)

For each: set its `ENABLE_*=true`, then dry-run (Phase 4) before moving on.

- [ ] **1. Homebrew** (macOS + Linux) — needs `HOMEBREW_TAP_REPO` + `TAP_PUSH_TOKEN`. Users: `brew tap dailysoftwaresystems/dsscp && brew install dsscp`.
- [ ] **2. Scoop** (Windows) — needs `SCOOP_BUCKET_REPO` + `TAP_PUSH_TOKEN`. Users: `scoop bucket add dsscp https://github.com/dailysoftwaresystems/scoop-dsscp && scoop install dsscp`.
- [ ] **3. AUR** (Arch) — needs `AUR_SSH_PRIVATE_KEY`. First push creates the package. Users: `yay -S dsscp`.
- [ ] **4. Nix** — needs `NIX_FLAKE_REPO` + `TAP_PUSH_TOKEN`. Users: `nix run github:dailysoftwaresystems/dsscp-nix`.
- [ ] **5. winget** (Windows, STABLE only) — needs P0.3 (the `.zip`) + `WINGET_TOKEN` + `WINGET_IDENTIFIER`. **First submission is manually reviewed by Microsoft.** Wire `wingetcreate --urls` at the `.zip` (commented reference is in the job).
- [ ] **6. deb** (Debian/Ubuntu) — needs `CLOUDSMITH_REPO` + `CLOUDSMITH_API_KEY`. Users follow Cloudsmith's apt setup snippet, then `apt install dsscp`.
- [ ] **7. rpm** (Fedora/RHEL) — same Cloudsmith creds. Users: `dnf install dsscp`.
- [ ] **8. Snap** (Linux, STABLE only) — needs `SNAPCRAFT_STORE_CREDENTIALS`. **Classic confinement ⇒ manual Snap Store review before the first publish.** Also finalize multi-arch + lib bundling in `snapcraft.yaml.tmpl`. Users: `snap install dsscp --classic`.

---

## ✅ PHASE 4 — verify each manager

- [ ] After enabling a manager, run **Actions → Publish packages → Run workflow** with a `tag` input (e.g. `v0.0.2`) to re-publish against a real release's assets.
- [ ] Confirm the manifest/package landed (formula committed to the tap, `.deb` in Cloudsmith, etc.).
- [ ] Install it on a clean machine/VM and run `dsscp` end-to-end.
- [ ] Only then enable the next manager.

---

## 📋 Reference: every secret + variable

| Secret | Managers |
|---|---|
| `TAP_PUSH_TOKEN` | Homebrew, Scoop, Nix |
| `AUR_SSH_PRIVATE_KEY` | AUR |
| `CLOUDSMITH_API_KEY` | deb, rpm |
| `SNAPCRAFT_STORE_CREDENTIALS` | Snap |
| `WINGET_TOKEN` | winget |

| Variable | Purpose |
|---|---|
| `ENABLE_PKG_PUBLISH` | master switch |
| `ENABLE_{HOMEBREW,SCOOP,WINGET,AUR,NIX,DEB,RPM,SNAP}` | per-manager on/off |
| `HOMEBREW_TAP_REPO` / `SCOOP_BUCKET_REPO` / `NIX_FLAKE_REPO` | git push targets |
| `CLOUDSMITH_REPO` | Cloudsmith `org/repo` for deb+rpm |
| `WINGET_IDENTIFIER` | winget package id |
| `PKG_MAINTAINER` | optional metadata |

---

## ⚠️ Known gaps / decisions to revisit

- **winget `.zip`** (P0.3) — the only asset-format gap.
- **Snap** — the template packages linux-x64 only via `plugin: dump`; multi-arch + shared-lib bundling + the classic-confinement review need finishing before it works.
- **Cloudsmith distro target** — the jobs push to `…/any-distro/any-version` (a Cloudsmith wildcard). Narrow to specific distros (e.g. `ubuntu/jammy`) if you prefer. Self-hosting an apt/yum repo (GitHub Pages + `reprepro`/`createrepo` + a GPG key) is the no-Cloudsmith alternative — swap the `cloudsmith push` step.
- **Beta channel** — beta uses a `dsscp-beta` package name in the **same** tap/bucket/AUR (winget + snap are stable-only). Want fully separate beta repos? Add per-channel repo variables and branch on `CHANNEL` in the jobs (small tweak).
- **Signing/notarization** *(optional, later)* — without macOS notarization (Apple Developer cert) + Windows Authenticode, users hit Gatekeeper/SmartScreen warnings. Homebrew/Scoop don't require it; add when polishing.
