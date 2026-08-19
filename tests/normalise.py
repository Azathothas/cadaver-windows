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
import json
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
    # Only the default, which is built from them: an owner a session set
    # for itself is a value the session is checking and stays as it is.
    (re.compile(r"^(\s*lockowner: )mailto:.*$", re.M), r"\1<LOCKOWNER>"),
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
    # The server names itself, and wsgidav names the Python running it,
    # neither of which is cadaver's behaviour.
    (re.compile(r"^(\s*• server: ).*$", re.M), r"\1<SERVER>"),
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
    # What `bench' reports is a measurement: it differs between runs by
    # design, and tests/bench_check.py is what looks at it.  The byte
    # counts and the iteration count are not measurements and stay.
    (re.compile(r"min [0-9.]+ ms, median [0-9.]+ ms, max [0-9.]+ ms"),
     "min <MS> ms, median <MS> ms, max <MS> ms"),
    (re.compile(r"in [0-9.]+ s wall clock, [0-9.]+ MiB/s"),
     "in <SECONDS> s wall clock, <RATE> MiB/s"),
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


# Fields of the --json document that are a measurement, a clock reading
# or an identifier the server chose: they differ between runs by design
# and say nothing about whether cadaver is working.
JSON_VOLATILE = {
    "duration": "<DURATION>",
    "started": "<DATE>",
    "token": "<TOKEN>",
    "version": "<VERSION>",
    # `bench' only.  The byte counts and the iteration count next to
    # these are exact and stay, so a change in what bench transfers
    # still shows up here.
    "min_ms": "<MS>",
    "median_ms": "<MS>",
    "max_ms": "<MS>",
    "seconds": "<SECONDS>",
    "mib_per_second": "<RATE>",
}

# Response headers whose value is the server's, not cadaver's.
JSON_HEADERS = {"etag": "<ETAG>", "last-modified": "<DATE>",
                "date": "<DATE>", "server": "<SERVER>"}


def normalise_json(obj, text):
    """Normalises a parsed --json document in place.

    Working on the structure rather than on the text matters: a
    substitution written for a transcript, applied to a JSON document,
    would happily eat a closing quote and leave something that no longer
    parses.  Every string value goes through `text' -- the same
    normaliser the transcripts use -- and the parts whose order is the
    server's choice are sorted.

    RFC 4918 does not constrain the order of properties in a
    multistatus, and x/net/webdav walks a Go map, so the same request
    genuinely answers differently from one run to the next.
    """
    if isinstance(obj, dict):
        # A property whose value is an etag the server chose.
        if obj.get("name") == "getetag" and "value" in obj:
            obj["value"] = "<ETAG>"

        for key, value in list(obj.items()):
            if key in JSON_VOLATILE and not isinstance(value, (dict, list)):
                obj[key] = JSON_VOLATILE[key]
            elif key == "headers" and isinstance(value, dict):
                for name in value:
                    if name.lower() in JSON_HEADERS:
                        value[name] = JSON_HEADERS[name.lower()]
                obj[key] = dict(sorted(value.items()))
            elif key == "properties" and isinstance(value, list):
                normalise_json(value, text)
                obj[key] = sorted(
                    value, key=lambda prop: (prop.get("namespace", ""),
                                             prop.get("name", "")))
            elif key == "output" and isinstance(value, list):
                # The command's own output, which is the transcript it
                # would have printed: normalised and sorted the same way.
                obj[key] = sort_runs(text("\n".join(value))).split("\n")
            elif isinstance(value, str):
                obj[key] = text(value)
            else:
                normalise_json(value, text)
    elif isinstance(obj, list):
        for index, item in enumerate(obj):
            # A string reached through a list rather than through a
            # member name is still a string: "args" holds the working
            # directory `lcd' was given, which is a path like any other.
            if isinstance(item, str):
                obj[index] = text(item)
            else:
                normalise_json(item, text)
    return obj


# How cadaver's --json document begins.  The harness merges standard
# error into the transcript, so the document is not necessarily the only
# thing there: a prompt goes to standard error under --json, and so does
# a trace with no file named.
JSON_MARKER = '{"tool":"cadaver"'


def split_json(text):
    """Splits `text' around cadaver's --json document.

    Returns (everything else, the document), or (text, None) when there
    is no document in it.
    """
    cut = text.rfind(JSON_MARKER)
    if cut < 0:
        return text, None

    end = text.find("\n", cut)
    if end < 0:
        end = len(text)

    return text[:cut] + text[end:], text[cut:end]


def expand_json(document, text):
    """Pretty-prints a --json document.

    cadaver writes it as a single line, which diffs unreadably.  Parsing
    and re-printing it makes a difference legible, and makes the test
    fail outright on output that is not well-formed JSON -- the property
    that matters most about it.
    """
    try:
        obj = json.loads(document)
    except ValueError as err:
        return "%s\n<<< NOT VALID JSON: %s >>>\n" % (document, err)

    # ensure_ascii=False so that a bullet stays a bullet: the expected
    # files are UTF-8 and easier to read for it.
    return json.dumps(normalise_json(obj, text), indent=2,
                      ensure_ascii=False) + "\n"


def spellings(path):
    """The ways one Windows path can appear in cadaver's output.

    As it was given, with forward slashes, and with every backslash
    doubled -- the last because a path inside a JSON string is escaped.
    Longest first, so that a path which is a prefix of another does not
    shadow it.
    """
    return sorted({path, path.replace("\\", "/"),
                   path.replace("\\", "\\\\")}, key=len, reverse=True)


def normalise(text, port=None, work=None, sort=True):
    if work:
        for path in spellings(work):
            text = text.replace(path, "<WORK>")

    if port:
        text = text.replace(":%s" % port, ":<PORT>")

    for pattern, replacement in SUBSTITUTIONS:
        text = pattern.sub(replacement, text)

    return sort_runs(text) if sort else text


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
        for path in spellings(args.editor):
            text = text.replace(path, "<EDITOR>")

    # The exit status line tests/session.sh appends is not part of what
    # cadaver wrote, so it is held back while the JSON is expanded and
    # put back afterwards.
    status = ""
    marker = "-- cadaver exited "
    if marker in text:
        cut = text.rindex(marker)
        status = text[cut:]
        text = text[:cut]

    def as_text(value):
        return normalise(value, port=args.port, work=args.work, sort=False)

    # A --json document is normalised through its structure, because a
    # substitution written for a transcript would happily eat a closing
    # quote; whatever else was on the streams is a transcript and is
    # normalised as the text it is.
    rest, document = split_json(text)
    text = normalise(rest, port=args.port, work=args.work)
    if document is not None:
        text = text + expand_json(document, as_text)

    sys.stdout.buffer.write((text + status).encode("utf-8",
                                                   "surrogateescape"))


if __name__ == "__main__":
    main()
