#!/usr/bin/env python3
"""Substitutes placeholders in a session script.

    expand.py IN OUT NAME=VALUE...

Replaces each NAME with its VALUE and writes the result to OUT.

This exists rather than a sed invocation because every value here is a
Windows path.  GNU sed reads a backslash in the replacement text as an
escape: the separators vanish, and a path with a "U" or an "L" after one
turns the rest of the line into upper or lower case.
"""

import io
import sys


def main():
    if len(sys.argv) < 3:
        sys.stderr.write("usage: expand.py IN OUT NAME=VALUE...\n")
        return 2

    src, dst = sys.argv[1], sys.argv[2]
    text = io.open(src, encoding="utf-8", newline="").read()

    for pair in sys.argv[3:]:
        name, _, value = pair.partition("=")
        text = text.replace(name, value)

    io.open(dst, "w", encoding="utf-8", newline="").write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
