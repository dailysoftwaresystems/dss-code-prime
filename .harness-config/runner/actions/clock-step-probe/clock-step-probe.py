"""Counts CLOCK_REALTIME discontinuities, measured against CLOCK_MONOTONIC.

Monotonic time is the ruler. Real time and monotonic time are read together, repeatedly, and
an interval in which the two advanced by amounts differing by more than TOLERANCE_SECONDS is
an interval in which real time was STEPPED - not one in which more time passed. Nothing here
decides a verdict: the count is printed, and a gate decides what it is worth.

WHAT THIS IS NOT. It is not the failure window. A once-per-run sample was measured and retired
on 2026-09-15, because a loaded run sampled ABSENT and charged to DSS the very failures a quiet
run excused; the window a gate counts in is the failing unit's own output up to its verdict
line, and that belongs to the caller. This program answers one question - did the host it was
asked on step its clock while it was asked - and a gate is free to find that insufficient.

EVERY NUMBER IS PRINTED, not just the count. A bare `steps=0` cannot be told apart from a probe
that sampled nothing, slept through the window, or was handed a tolerance no step could exceed.
The sample count, the window actually covered, the tolerance applied and the largest divergence
seen are what make the count readable as a measurement.

THE SUBJECT IS REAL, and both arms were measured on 2026-09-16 by running THIS program:

    wsl Ubuntu     steps=2  maxdivergence=28.407399s  |  steps=2  maxdivergence=28.413666s
    local Windows  steps=0  maxdivergence=0.000003s   |  steps=0  maxdivergence=0.000110s

Each host twice, five-second windows throughout; the Windows pair came back through
`DssHarness run clock-step-probe`, the WSL pair from the interpreter directly. The WSL2
outliers are one CLOCK_REALTIME excursion out and straight back - measured earlier the same
day as +28.391743 then -28.389587 in adjacent intervals - which is the WSL2 CLOCK_REALTIME
defect this repository already records, reproduced on demand rather than remembered. ★ THE WINDOWS ARM IS THE NEGATIVE CONTROL AND IS
THE HALF THAT MAKES THE OTHER ONE MEAN ANYTHING: six orders of magnitude below the WSL reading
and four below the tolerance. An instrument that only ever answers one way has not been shown to
be answering the question.
"""

import time

# Mirrors defaults.clockStepToleranceMilliseconds (2000) in .harness-config/config.json.
# RESTATED rather than read: an action file's values come from .harness-config/runner/.env and
# .harness-config/runner/.secrets, and nothing substitutes a `defaults` key into a run line or
# into this program's arguments, so the two numbers are kept in step by this comment alone.
TOLERANCE_SECONDS = 2.0

# 250 samples at 20 ms covers about five seconds. Long enough to catch the excursion measured on
# WSL2, short enough that a gate can afford to run it while a failure is being adjudicated.
SAMPLE_COUNT = 250
SAMPLE_INTERVAL_SECONDS = 0.02


def sample():
    """Real time and monotonic time read as close together as the interpreter allows."""
    samples = []

    for _ in range(SAMPLE_COUNT):
        time.sleep(SAMPLE_INTERVAL_SECONDS)
        samples.append((time.monotonic(), time.time()))

    return samples


def divergences(samples):
    """How much further real time moved than monotonic time did, per interval, signed."""
    return [
        (later[1] - earlier[1]) - (later[0] - earlier[0])
        for earlier, later in zip(samples, samples[1:])
    ]


def main():
    samples = sample()
    spread = divergences(samples)
    steps = sum(1 for divergence in spread if abs(divergence) > TOLERANCE_SECONDS)
    largest = max([abs(divergence) for divergence in spread] + [0.0])

    print(
        "clock-step-probe steps=%d samples=%d window=%.3fs tolerance=%.3fs maxdivergence=%.6fs"
        % (
            steps,
            len(samples),
            samples[-1][0] - samples[0][0],
            TOLERANCE_SECONDS,
            largest,
        )
    )


if __name__ == "__main__":
    main()
