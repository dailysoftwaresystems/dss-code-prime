#!/usr/bin/env bash
# ── THE `dss:` REGION / MIRROR VERIFIER, as a Step-0 self-test entry ─────────
#
# D-HARNESS-CORPUS-ENGINE-MIRROR-CLAIMS-A-VERIFIER-THAT-DOES-NOT-EXIST.
#
# ✔MEASURED (TF-C123): the `dss:corpus-engine` region header said "the verifier
# extracts it from this file by these sentinels". `grep -rl 'dss:corpus-engine'`
# over the whole repository returned exactly the two driver files themselves.
# NOTHING read the sentinel. The mirrored region was entirely unenforced, the two
# copies could diverge silently, and the region carried a note saying they could
# not — an instrument credited with an observation it never made.
#
# THIS FILE IS THE ENTRY POINT, NOT THE IMPLEMENTATION, and that is the point.
# The checks live in harness_legs.py (`--check-regions`) so that the .sh and the
# .ps1 self-test run the SAME verifier rather than two copies of it — the very
# defect class the verifier exists to find would otherwise reappear inside it.
# What lives here is the one thing that cannot be shared: the shape a
# `_SELFTESTS` entry must have (`passed=N failed=N skipped=N` on stdout, rc 0
# only when nothing failed).
#
# WHAT IT PROVES, in two layers:
#   1. every `dss:` region in either driver is DECLARED, with who verifies it —
#      and a claimed verifier that does not actually read the region, or an
#      unverified region with no stated reason, is a LOUD failure. That is the
#      generalisation: a region whose verifier is missing can no longer be
#      silent about it.
#   2. for a region declared MIRRORED, the symbol pairing AND DIFFERENTIAL
#      EXECUTION — both copies extracted from the shipped drivers, run on
#      byte-identical input, answers compared. That second half is what catches
#      a changed regex, and it is why this is a verifier and not a headcount.
#
# ★ ON A HOST WITH ONLY ONE INTERPRETER the differential arms SKIP, by name, and
# the driver turns a nonzero skip count into a WARN saying what went unproven.
# A skip is not a pass here any more than it is in the other self-tests.
#
# COST: ✔MEASURED 3.9 s for 152 assertions on this Windows workstation, of which
# essentially all is the twelve interpreter start-ups the differential battery
# needs (six cases x two languages). That is an order of magnitude more than
# test-confound-scope.sh's 0.20 s and still nothing against a run measured in
# hours — and it is paid ONCE, before the build, rather than after the corpus.
set -Eeuo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if ! command -v python3 >/dev/null 2>&1; then
  # LOUD, and NOT a skip: python3 is already a hard requirement of both drivers
  # (the leg plan and every manifest are python), so "no python3" is a broken
  # host rather than an unmet optional prerequisite. Reporting it as a skip
  # would let a host that can run nothing report a clean partial result.
  echo "FATAL: python3 is not on PATH, so the dss: region verifier cannot run."
  echo "       It is a hard requirement of this harness either way — the leg plan"
  echo "       and every project manifest are python — so this is not a skip."
  echo "passed=0 failed=1 skipped=0"
  exit 1
fi

# exec: the rc is the verifier's own, taken DIRECTLY and never through a pipe,
# and its output — including the summary line the driver parses — passes through
# untouched.
exec python3 "$HERE/harness_legs.py" --check-regions
