#!/usr/bin/env python3
"""Checks the numbers `bench' reported against what it was asked to do.

    bench_check.py TRANSCRIPT PAYLOAD_BYTES ITERATIONS

The transcript is the raw one, before tests/normalise.py replaces the
measurements: they differ from one run to the next, which is what makes
them measurements and what keeps them out of the expected files.  The
byte counts and the iteration count do not differ, and neither does the
shape, so this is where they are checked.

It reports every problem it finds rather than the first, and exits
non-zero if there were any.
"""

import json
import re
import sys

ISO8601 = re.compile(r"^[0-9]{4}-[0-9]{2}-[0-9]{2}"
                     r"T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}Z$")

MARKER = '{"tool":"cadaver"'

# A wall-clock second count no local transfer of this size can exceed.
# High enough that a loaded machine does not fail the run, low enough to
# catch a number that came from somewhere other than a clock.
MAX_SECONDS = 300.0


def load(path):
    with open(path, encoding="utf-8", errors="surrogateescape") as fh:
        text = fh.read()

    cut = text.rfind(MARKER)
    if cut < 0:
        sys.exit("bench_check.py: no --json document in %s" % path)

    end = text.find("\n", cut)
    return json.loads(text[cut:] if end < 0 else text[cut:end])


def main():
    path, payload, iterations = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    problems = []

    def want(condition, what):
        if not condition:
            problems.append(what)

    result = load(path)
    benches = [c for c in result["commands"] if "benchmark" in c]

    want(len(benches) == 1,
         "expected exactly one command with a benchmark, found %d"
         % len(benches))
    if not benches:
        sys.exit("bench_check.py: " + "; ".join(problems))

    b = benches[0]["benchmark"]

    want(b["payload_bytes"] == payload,
         "payload_bytes is %r, wanted %d" % (b["payload_bytes"], payload))
    want(b["iterations"] == iterations,
         "iterations is %r, wanted %d" % (b["iterations"], iterations))
    want(isinstance(b["started"], str) and ISO8601.match(b["started"]),
         "started is %r, which is not an ISO 8601 UTC stamp with "
         "milliseconds and a Z" % (b["started"],))
    want(b["target"].endswith("/"),
         "target is %r, which is not a collection" % (b["target"],))

    lat = b["latency"]
    want(lat["op"] == "PROPFIND", "latency op is %r" % (lat["op"],))
    want(lat["samples"] == iterations,
         "latency samples is %r, wanted %d" % (lat["samples"], iterations))
    want(0.0 <= lat["min_ms"] <= lat["median_ms"] <= lat["max_ms"],
         "latency is not ordered: min %r median %r max %r"
         % (lat["min_ms"], lat["median_ms"], lat["max_ms"]))
    want(lat["max_ms"] < MAX_SECONDS * 1000.0,
         "latency max_ms is %r, which no local round trip reaches"
         % (lat["max_ms"],))

    for way in ("upload", "download"):
        part = b[way]
        want(part["bytes"] == payload * iterations,
             "%s bytes is %r, wanted %d"
             % (way, part["bytes"], payload * iterations))
        want(0.0 <= part["seconds"] < MAX_SECONDS,
             "%s seconds is %r" % (way, part["seconds"]))

        if part["seconds"] < 0.0005:
            # Faster than the duration beside it can express, so there
            # is no rate the two numbers would support.  A test runner
            # with a fast disk reaches this; the payload is the thing to
            # change, not the unit.
            want(part["mib_per_second"] is None,
                 "%s took under a millisecond but reports a rate of %r"
                 % (way, part["mib_per_second"]))
            continue

        want(part["mib_per_second"] is not None
             and part["mib_per_second"] > 0.0,
             "%s mib_per_second is %r" % (way, part["mib_per_second"]))

        # The rate has to follow from the byte count and the duration.
        # Not exactly: cadaver reports the duration to millisecond
        # resolution and works the rate out from what it measured, so
        # the real duration is anywhere within half a millisecond of
        # what the document says, and the rate is rounded to two
        # decimals on top of that.  A transfer over the loopback
        # interface takes a few milliseconds, where half of one is
        # several per cent, so the window has to be derived rather than
        # guessed at.
        mib = part["bytes"] / (1024.0 * 1024.0)
        low = mib / (part["seconds"] + 0.0005) - 0.005
        high = mib / max(part["seconds"] - 0.0005, 1e-9) + 0.005
        want(low <= part["mib_per_second"] <= high,
             "%s mib_per_second is %r; %d bytes in %r seconds is between "
             "%.2f and %.2f"
             % (way, part["mib_per_second"], part["bytes"], part["seconds"],
                low, high))

    if problems:
        for problem in problems:
            print("bench_check.py: " + problem, file=sys.stderr)
        sys.exit(1)


main()
