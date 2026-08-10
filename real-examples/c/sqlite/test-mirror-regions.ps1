# ── THE `dss:` REGION / MIRROR VERIFIER, as a Step-0 self-test entry ─────────
#
# D-HARNESS-CORPUS-ENGINE-MIRROR-CLAIMS-A-VERIFIER-THAT-DOES-NOT-EXIST.
#
# The .ps1 twin of test-mirror-regions.sh, and DELIBERATELY as thin: both call
# harness_legs.py --check-regions, so the two drivers run the SAME verifier
# rather than two copies of it. A verifier that existed twice would be subject to
# the exact divergence it exists to detect.
#
# ✔MEASURED (TF-C123): the `dss:corpus-engine` header claimed "the verifier
# extracts it from this file by these sentinels" and no such verifier existed —
# `grep -rl 'dss:corpus-engine'` returned only the two drivers. The mirrored
# region was unenforced while carrying a note saying it could not be.
#
# WHAT IT PROVES: every `dss:` region is declared with who verifies it (a claimed
# verifier that does not read the region, or an unverified region with no stated
# reason, is a LOUD failure); and for a region declared MIRRORED, the symbol
# pairing plus DIFFERENTIAL EXECUTION of both copies — extracted from the shipped
# drivers — on byte-identical input.
#
# ★ A host missing either interpreter SKIPS the differential arms by name; the
# driver turns a nonzero skip count into a WARN naming what went unproven.
$ErrorActionPreference = 'Stop'

$here = $PSScriptRoot
$py = Get-Command python3 -ErrorAction SilentlyContinue
if (-not $py) { $py = Get-Command python -ErrorAction SilentlyContinue }
if (-not $py) {
  # LOUD, not a skip — see the .sh sibling: python3 is a hard requirement of this
  # driver either way, so its absence is a broken host, not an unmet option.
  Write-Host "FATAL: python3 is not on PATH, so the dss: region verifier cannot run."
  Write-Host "       It is a hard requirement of this harness either way — the leg plan"
  Write-Host "       and every project manifest are python — so this is not a skip."
  Write-Host "passed=0 failed=1 skipped=0"
  exit 1
}

# The rc is taken DIRECTLY off the process, never after a pipe: $LASTEXITCODE is
# read on the line following the call and nothing is interposed.
& $py.Source (Join-Path $here 'harness_legs.py') '--check-regions'
$rc = $LASTEXITCODE
exit $rc
