#!/usr/bin/env python3
# PURPOSE: measure the RUNTIME of an emitted sqlite3 binary, the standing runtime-differential instrument.
"""sqlite-runtime-bench.py — RUNTIME of an emitted sqlite3 binary, measured.

The standing runtime-differential instrument (P10, D-OPT7-CROSSCU-LTO-
SINGLE-OPTIMIZE): compile-time and exe size are PROXIES for output quality;
this measures the thing itself. Rule context: the shipped unit-stage variant
must not be slower at RUNTIME than the incumbent, same machine, repeat
protocol, median.

Discipline (each measured, each bitten someone):
- MONOTONIC clock only, both reads in THIS one process (time.monotonic()'s
  reference point is undefined; only differences within a process are valid).
  Never wall-clock dates — WSL2's CLOCK_REALTIME oscillates ±34s/5s
  (project_wsl2_clock_realtime_broken_2026_08_01).
- DETERMINISTIC workload: fixed seed-free data volume, FILE db (not :memory:;
  a memory db changes what the pager does), one connection, fixed statement
  set. Same SQL for every binary.
- MEDIAN over repeats, printed per binary; the caller compares.

Usage: sqlite-runtime-bench.py --binary <path> [--repeats 7]
       (run it on the OS the binary targets — an elf64-linux image runs on
        Linux/WSL, not on the Windows host that compiled it)
"""
import argparse
import os
import statistics
import subprocess
import sys
import tempfile
import time

WORKLOAD = """
PRAGMA journal_mode=OFF;
CREATE TABLE t(id INTEGER PRIMARY KEY, a INTEGER, b TEXT);
BEGIN;
WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x < 120000)
INSERT INTO t(id, a, b) SELECT x, x % 97, printf('row-%d', x) FROM c;
COMMIT;
CREATE INDEX ta ON t(a);
SELECT count(*), sum(a), max(length(b)) FROM t WHERE a BETWEEN 10 AND 80;
SELECT b FROM t WHERE id % 9973 = 0 ORDER BY id;
DELETE FROM t WHERE a = 41;
SELECT count(*) FROM t;
"""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--repeats", type=int, default=7)
    args = ap.parse_args()

    with tempfile.TemporaryDirectory(prefix="dss-rt-bench-") as td:
        # Warm-up run (page cache, first-touch) — never counted.
        db0 = os.path.join(td, "warm.db")
        subprocess.run([args.binary, db0], input=WORKLOAD, capture_output=True,
                       text=True, check=True)
        times = []
        for i in range(args.repeats):
            db = os.path.join(td, f"run{i}.db")
            t0 = time.monotonic()
            r = subprocess.run([args.binary, db], input=WORKLOAD,
                               capture_output=True, text=True)
            elapsed = time.monotonic() - t0
            if r.returncode != 0:
                print(f"BENCH-FAILED rc={r.returncode}: {r.stderr[:300]}",
                      file=sys.stderr)
                return 1
            times.append(elapsed)
        med = statistics.median(times)
        print(f"RUNTIME-BENCH binary={os.path.basename(args.binary)} "
              f"repeats={args.repeats} median={med:.3f}s "
              f"min={min(times):.3f}s max={max(times):.3f}s "
              f"samples={' '.join(f'{t:.3f}' for t in times)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
