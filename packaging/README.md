# Package-manager publishing

`../.github/workflows/publish-packages.yml` publishes each GitHub release to OS
package managers. It fires on `release: published` (after `pipeline-pkg.yml`
builds + attaches the platform tarballs) and fans out to eight managers.

**It is inert by default.** Every job is gated on repository *variables*, so with
none set nothing runs. You enable managers one at a time as you provision each
one's repo + token below.

---

## 0. Prerequisites (must land before a *live* publish)

The rendered manifests install a **`dsscp` command, its shared library, and its
config tree**, from a release tarball that is a **prefix tree** (`bin/`, `lib/`,
`share/`) produced by `cmake --install`.

1. ✅ **CMake `install()` rules + rpath** — **LANDED.** `cmake/DssInstall.cmake`
   is the single owner of what ships: the exe to `bin/`, the shared lib to
   `lib/` with `$ORIGIN/../lib` (Linux) / `@loader_path/../lib` (macOS), and
   `src/dss-config/` to `share/dss-code-prime/<version>/dss-config/`.
   (`D-PKG-NO-PACKAGING-PATH-SHIPS-THE-CONFIG-TREE`.)
2. **The `dss-code-prime` → `dsscp` binary rename** — still queued, so the
   installed command is still `dss-code-prime` while every manifest here names
   `dsscp`.

Until (2) lands, **leave `ENABLE_PKG_PUBLISH` unset.**

### ⛔ The one rule every manifest here obeys

**Take the tarball WHOLE; never cherry-pick files out of it.** `dsscp` locates
its config tree by a path *relative to its own executable*
(`../share/dss-code-prime/<version>/dss-config`) and its shared library the same
way. Rearranging `bin/` and `share/` produces a package that installs cleanly and
then cannot resolve a single `#include <stdio.h>` — which is exactly what these
manifests did before the install set existed, and nothing noticed, because every
check asked "is the binary there?" rather than "does it compile?". The version
segment in that path is deliberate: it makes a binary/config version mismatch
*unrepresentable* rather than merely detectable.

---

## 1. The master switch + enable order

| Variable | Value | Effect |
|---|---|---|
| `ENABLE_PKG_PUBLISH` | `true` | Master switch. Off ⇒ the whole workflow is a no-op. |

Then flip **one manager at a time** (`ENABLE_HOMEBREW`, `ENABLE_SCOOP`, …) as you
finish its setup. Recommended order (value ÷ effort): **Homebrew → Scoop → AUR →
Nix → winget → deb → rpm → snap**.

Set variables/secrets under **Settings → Secrets and variables → Actions**
(*Variables* tab for `ENABLE_*`/repo names, *Secrets* tab for tokens). These are
terminal-panel settings — I can't set them for you.

---

## 2. Secrets (tokens) — the complete list

| Secret | Used by | What it is |
|---|---|---|
| `TAP_PUSH_TOKEN` | Homebrew, Scoop, Nix | A PAT (fine-grained or classic) with **push/contents:write** to the tap/bucket/flake repos. One token covers all three if they're in the same org. |
| `WINGET_TOKEN` | winget | A **classic** PAT with `public_repo` for your fork of `microsoft/winget-pkgs`. |
| `AUR_SSH_PRIVATE_KEY` | AUR | The SSH **private** key whose public half is registered on your AUR account. |
| `CLOUDSMITH_API_KEY` | deb, rpm | Cloudsmith API key (Account → API settings). |
| `SNAPCRAFT_STORE_CREDENTIALS` | snap | Output of `snapcraft export-login -` (Snap Store login). |

## 3. Variables — the complete list

| Variable | Used by | Example |
|---|---|---|
| `ENABLE_PKG_PUBLISH` | master | `true` |
| `ENABLE_HOMEBREW` / `_SCOOP` / `_WINGET` / `_AUR` / `_NIX` / `_DEB` / `_RPM` / `_SNAP` | per manager | `true` |
| `PKG_MAINTAINER` | all (metadata) | `DSS <noreply@dailysoftwaresystems.com>` |
| `HOMEBREW_TAP_REPO` | Homebrew | `dailysoftwaresystems/homebrew-dsscp` |
| `SCOOP_BUCKET_REPO` | Scoop | `dailysoftwaresystems/scoop-dsscp` |
| `NIX_FLAKE_REPO` | Nix | `dailysoftwaresystems/dsscp-nix` |
| `WINGET_IDENTIFIER` | winget | `DailySoftwareSystems.dsscp` |
| `CLOUDSMITH_REPO` | deb, rpm | `dailysoftwaresystems/dsscp` |

---

## 4. Per-manager setup

> Grouped by shared infra below (deb + rpm share Cloudsmith). Enable in the §1 recommended order, not top-to-bottom here.

### Homebrew (macOS + Linux)
1. Create a repo **named `homebrew-dsscp`** (the `homebrew-` prefix is required by `brew tap`).
2. Set `HOMEBREW_TAP_REPO`, `TAP_PUSH_TOKEN`, `ENABLE_HOMEBREW=true`.
3. Users: `brew tap dailysoftwaresystems/dsscp && brew install dsscp`.

### Scoop (Windows)
1. Create a bucket repo, e.g. `scoop-dsscp`.
2. Set `SCOOP_BUCKET_REPO`, `TAP_PUSH_TOKEN`, `ENABLE_SCOOP=true`.
3. Users: `scoop bucket add dsscp https://github.com/dailysoftwaresystems/scoop-dsscp && scoop install dsscp`.

### AUR (Arch)
1. Create an AUR account; add an SSH public key to it. Put the private key in `AUR_SSH_PRIVATE_KEY`.
2. Set `ENABLE_AUR=true`. The first push creates the `dsscp` package.
3. Users: `yay -S dsscp` (or any AUR helper).

### Nix
1. Create a flake repo, e.g. `dsscp-nix`. Set `NIX_FLAKE_REPO`, `TAP_PUSH_TOKEN`, `ENABLE_NIX=true`.
2. Users: `nix run github:dailysoftwaresystems/dsscp-nix`, or add it to their flake inputs.

### deb + rpm (via Cloudsmith)
1. Create a Cloudsmith account + a repository (free OSS tier). Set `CLOUDSMITH_REPO`, `CLOUDSMITH_API_KEY`, `ENABLE_DEB=true` / `ENABLE_RPM=true`.
2. Users follow Cloudsmith's per-repo setup snippet, then `apt install dsscp` / `dnf install dsscp`.
3. *Alternative to Cloudsmith:* self-host an apt/yum repo (GitHub Pages + `reprepro`/`createrepo` + a GPG signing key). Not scaffolded — swap the `cloudsmith push` step if you go this route.

### winget (Windows, stable only) — ⚠️ needs one more asset
winget wants an **installer/portable `.zip`**, not a `.tar.gz`. The release
currently ships `.tar.gz`, so the winget job **fails loud on purpose** until you
either (a) emit a `dsscp-<ver>-windows-x64.zip` in `pipeline-pkg.yml`, or (b)
repackage+upload a `.zip` in the winget job. Then:
1. Fork `microsoft/winget-pkgs`. Put a classic `public_repo` PAT in `WINGET_TOKEN`.
2. Set `WINGET_IDENTIFIER` (e.g. `DailySoftwareSystems.dsscp`), `ENABLE_WINGET=true`, and point `wingetcreate --urls` at the `.zip` (commented reference in the job).
3. The **first** submission is manually reviewed by Microsoft.

### snap (Linux, stable only) — ⚠️ classic-confinement review
1. `snapcraft register dsscp`, then `snapcraft export-login -` → `SNAPCRAFT_STORE_CREDENTIALS`. Set `ENABLE_SNAP=true`.
2. The snap uses **classic** confinement (a compiler reads/writes arbitrary paths), which requires a **manual store review** before the first publish. Multi-arch + shared-lib bundling in `snapcraft.yaml.tmpl` also need finalizing.
3. Users: `snap install dsscp --classic`.

---

## 5. Channels

`vX.Y.Z` → **stable** (package name `dsscp`). `vX.Y.Z-beta` → **beta** (package
name `dsscp-beta`, so both coexist in one tap/bucket/AUR). **winget + snap publish
on stable only.** Want fully separate beta repos instead of a `-beta` suffix? Add
per-channel repo variables and branch on `CHANNEL` in the job — a small tweak.

## 6. Testing without a release

Run **Actions → Publish packages → Run workflow** with a `tag` input (e.g.
`v0.0.2`) to re-publish an existing release. With `ENABLE_PKG_PUBLISH` unset it
does nothing; enable it (and one manager) to dry-run that manager against a real
release's assets.
