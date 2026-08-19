#!/usr/bin/env python3
"""Normalises a cadaver transcript so that two runs can be compared.

    normalise.py [--port PORT] [--work DIR] < transcript > normalised

A transcript carries things that differ between runs and say nothing
about whether cadaver is working: the time a file was written, the token
a server chose for a lock, the port the test server happened to get, the
random part of a temporary file name.  Each is replaced with a fixed
placeholder.  Everything else is passed through byte for byte, so a
difference in the output is a difference in behaviour.

Carriage returns go too.  cadaver writes its standard output in text
mode on Windows, so every line ends CR LF there and LF on Unix; the
expected files are checked in with LF endings and .gitattributes keeps
git from rewriting them.
"""

import argparse
import re
import sys

# "Aug 19 10:01" and "Aug 19  2026", the two forms src/utils.c prints
# depending on how old the resource is.
DATE = re.compile(
    r"\b(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) "
    r"[ 0-9][0-9] +(?:[0-9]{2}:[0-9]{2}|[0-9]{4})\b")

# An RFC 1123 date in a response header, from `head'.
HTTP_DATE = re.compile(
    r"\b(?:Mon|Tue|Wed|Thu|Fri|Sat|Sun), "
    r"[0-9]{2} \w{3} [0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2} GMT")

SUBSTITUTIONS = [
    # Both come from the account cadaver runs as: the lock owner from
    # the user and machine names, the lock store from the home
    # directory.
    (re.compile(r"^(\s*lockowner: ).*$", re.M), r"\1<LOCKOWNER>"),
    (re.compile(r"^(\s*lockstore: ).*$", re.M), r"\1<LOCKSTORE>"),
    # Lock tokens are chosen by the server, and so is how long a lock
    # is granted for.  `steal' prints a token on its own rather than
    # after "Lock token", hence the second pattern.
    (re.compile(r"Lock token <[^>]*>"), "Lock token <TOKEN>"),
    (re.compile(r"<opaquelocktoken:[^>]*>"), "<TOKEN>"),
    (re.compile(r"Timeout: [0-9]+ seconds"), "Timeout: <N> seconds"),
    # An ISO 8601 timestamp in a property value, DAV:creationdate.
    (re.compile(r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}"
                r"(?:\.[0-9]+)?Z"), "<DATE>"),
    (re.compile(r"^(\s*• etag: ).*$", re.M), r"\1<ETAG>"),
    (re.compile(r"^(\s*• last-modified: ).*$", re.M), r"\1<DATE>"),
    (re.compile(r"^(\s*• date: ).*$", re.M), r"\1<DATE>"),
    # The temporary file `edit' creates: the middle six characters are
    # random by design, and the directory it lands in is wherever this
    # machine keeps its temporary files.
    (re.compile(r"cadaver-edit-[A-Za-z0-9]{6}"), "cadaver-edit-XXXXXX"),
    (re.compile(r"(?:[A-Za-z]:)?[^\s'\"]*cadaver-edit-XXXXXX"),
     "<TMP>/cadaver-edit-XXXXXX"),
    # An etag returned as a property value rather than as a header.
    (re.compile(r"^(\s*• DAV:getetag = ).*$", re.M), r"\1<ETAG>"),
    # The version, so that bumping VERSION does not rewrite every
    # expected file.  The build reports it and `tests/offline.sh'
    # checks it against VERSION; there is nothing left for a session
    # transcript to add.
    (re.compile(r"^cadaver \S+$", re.M), "cadaver <VERSION>"),
    (re.compile(r"^neon [0-9.]+: .*$", re.M), "neon <VERSION>"),
    (re.compile(r"^readline .*$", re.M), "readline <VERSION>"),
    # The progress indicator emits one dot per transfer callback, and
    # how many of those a transfer takes is the socket's business.
    (re.compile(r"\[\.+"), "[.."),
    (HTTP_DATE, "<DATE>"),
    (DATE, "<DATE>"),
]


# The two listings whose order is the server's choice rather than
# cadaver's: the bulleted lines that `propget' without a property name
# and `head' produce, and the one-name-per-line output of `propnames'.
# RFC 4918 does not constrain the order of properties in a multistatus,
# and x/net/webdav walks a Go map, so it genuinely differs from one
# request to the next.  Sorting each contiguous run keeps the set and
# the position of the listing while dropping the order.
BULLET_LINE = re.compile(r"^\s*• ")
NAME_LINE = re.compile(r"^ \S+$")


def sort_runs(text):
    out = []
    run = []
    run_pattern = None

    def flush():
        if run:
            out.extend(sorted(run))
            del run[:]

    for line in text.split("\n"):
        for pattern in (BULLET_LINE, NAME_LINE):
            if pattern.match(line):
                if run_pattern is not pattern:
                    flush()
                    run_pattern = pattern
                run.append(line)
                break
        else:
            flush()
            run_pattern = None
            out.append(line)

    flush()
    return "\n".join(out)


def normalise(text, port=None, work=None):
    if work:
        # Longest first, so that a path that is a prefix of another does
        # not shadow it.
        for path in sorted({work, work.replace("\\", "/")}, key=len,
                           reverse=True):
            text = text.replace(path, "<WORK>")

    if port:
        text = text.replace(":%s" % port, ":<PORT>")

    for pattern, replacement in SUBSTITUTIONS:
        text = pattern.sub(replacement, text)

    return sort_runs(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="server port to replace with <PORT>")
    parser.add_argument("--work", help="local directory to replace with <WORK>")
    parser.add_argument("--editor",
                        help="editor command to replace with <EDITOR>")
    args = parser.parse_args()

    # Read and write bytes, decoding as UTF-8: cadaver writes UTF-8 and
    # a transcript may hold a name that is not valid in the console's
    # own encoding.  surrogateescape keeps anything undecodable intact
    # rather than failing or replacing it.
    text = sys.stdin.buffer.read().decode("utf-8", "surrogateescape")
    text = text.replace("\r\n", "\n").replace("\r", "\n")

    if args.editor:
        for path in sorted({args.editor, args.editor.replace("\\", "/")},
                           key=len, reverse=True):
            text = text.replace(path, "<EDITOR>")

    text = normalise(text, port=args.port, work=args.work)

    sys.stdout.buffer.write(text.encode("utf-8", "surrogateescape"))


if __name__ == "__main__":
    main()
