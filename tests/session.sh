#!/bin/sh
# Drives cadaver through the scripted sessions in tests/sessions and
# compares each transcript against the checked-in expected output.
#
#   ./tests/session.sh --url URL --name NAME [--regenerate] [SESSION...]
#
# It is not usually run directly: tests/godav.sh and tests/wsgidav.sh
# start a server and call it.  URL must be a collection that exists and
# ends in a slash; a fresh one is made under it for every session, so
# one session cannot leave anything behind that another trips over.
#
# A session is tests/sessions/NAME.cad, and may bring four more files:
#
#   NAME.flags    extra arguments for the cadaver command line, one per
#                 line, so that a session can be run with --json or
#                 --trace
#   NAME.home     a .netrc, placed in a home directory of the session's
#                 own; HOME points at it for the session and for the
#                 setup run before it
#   NAME.stdin    what to feed cadaver's standard input, for a session
#                 that has to answer a prompt.  Without it standard
#                 input is at end of file, as a script's would be
#   NAME.servers  the servers the session applies to, one per line.
#                 Without it the session runs against both
#   NAME.check    a shell snippet run afterwards with $WORK set to the
#                 session's working directory and $RAW to the transcript
#                 before tests/normalise.py touched it, for what a
#                 transcript cannot show or has had replaced.  $PYTHON
#                 is an interpreter
#
# All four go through tests/expand.py, so @WORK@ and @EDITOR@ mean the
# same in them as in the session script.
#
# NAME selects the directory of expected output, tests/expected/NAME,
# because the two test servers legitimately disagree: x/net/webdav has
# no dead property store and wsgidav's LOCK response is not usable, so
# the same session produces different -- and equally correct --
# transcripts against them.
#
# --regenerate rewrites the expected files instead of comparing.  Read
# the diff before committing it: the point of these files is that a
# change in behaviour has to be looked at, and --regenerate will happily
# enshrine a regression.
#
# Environment:
#   CADAVER  the executable to test (default: ./cadaver or ./cadaver.exe)
#   PYTHON   interpreter for tests/normalise.py

set -e

srcdir=`dirname "$0"`/..
cd "$srcdir"
srcdir=`pwd`

URL=
NAME=
REGENERATE=0

while [ $# -gt 0 ]; do
    case $1 in
        --url) URL=$2; shift 2 ;;
        --url=*) URL=${1#--url=}; shift ;;
        --name) NAME=$2; shift 2 ;;
        --name=*) NAME=${1#--name=}; shift ;;
        --regenerate) REGENERATE=1; shift ;;
        --) shift; break ;;
        -*) echo "session.sh: unknown option $1" >&2; exit 2 ;;
        *) break ;;
    esac
done

if [ -z "$URL" ] || [ -z "$NAME" ]; then
    echo "session.sh: --url and --name are both required" >&2
    exit 2
fi

CADAVER=${CADAVER-./cadaver}
[ -x "$CADAVER" ] || CADAVER=./cadaver.exe
if [ ! -x "$CADAVER" ]; then
    echo "session.sh: build cadaver first" >&2
    exit 1
fi
CADAVER=`cd "$(dirname "$CADAVER")" && pwd`/`basename "$CADAVER"`

# normalise.py is stdlib only, so any interpreter will do.
# shellcheck source=tests/python.sh
. tests/python.sh
if [ -z "${PYTHON-}" ]; then
    echo "session.sh: needs a Python interpreter; set \$PYTHON" >&2
    exit 1
fi

SESSIONS=$*
if [ -z "$SESSIONS" ]; then
    SESSIONS=
    for f in tests/sessions/*.cad; do
        SESSIONS="$SESSIONS `basename "$f" .cad`"
    done
fi

EXPECTED="tests/expected/$NAME"
mkdir -p "$EXPECTED"

# cadaver is a native Windows program under MSYS2, so it needs a
# Windows-style path for anything it opens itself; a /c/... path means
# nothing to it.
native_path() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -w "$1"
    else
        echo "$1"
    fi
}

OUT=session-results/$NAME
rm -rf "$OUT"
mkdir -p "$OUT"
OUTABS=`cd "$OUT" && pwd`
OUTNATIVE=`native_path "$OUTABS"`


# The port, so that a transcript does not depend on which one the
# server happened to get.
PORT=`echo "$URL" | sed -n 's,^[a-z]*://[^:/]*:\([0-9]*\)/.*,\1,p'`

# The editor tests/sessions/edit.cad runs.  It appends one line, so the
# change the session makes is the same every time.
EDITOR_SCRIPT="$OUT/append-line"
if [ "${OS-}" = "Windows_NT" ]; then
    EDITOR_SCRIPT="$EDITOR_SCRIPT.bat"
    printf '@echo off\r\necho edited-by-the-test-suite>> %%1\r\n' \
        > "$EDITOR_SCRIPT"
else
    printf '#!/bin/sh\necho edited-by-the-test-suite >> "$1"\n' \
        > "$EDITOR_SCRIPT"
    chmod +x "$EDITOR_SCRIPT"
fi
EDITOR_NATIVE=`native_path "$(cd "$(dirname "$EDITOR_SCRIPT")" && pwd)/$(basename "$EDITOR_SCRIPT")"`

RV=0
FAILED=

for name in $SESSIONS; do
    script="tests/sessions/$name.cad"
    if [ ! -f "$script" ]; then
        echo "session.sh: no such session: $name" >&2
        exit 2
    fi

    # A session may name the servers it applies to: authentication is
    # only set up on one of them, and dead properties only work on the
    # other.
    if [ -f "tests/sessions/$name.servers" ] \
       && ! grep -qx "$NAME" "tests/sessions/$name.servers"; then
        printf '%-10s skipped (not for %s)\n' "$name" "$NAME"
        continue
    fi

    WORK="$OUT/$name"
    mkdir -p "$WORK"
    WORKABS=`cd "$WORK" && pwd`
    WORKNATIVE=`native_path "$WORKABS"`

    # The fixtures every session starts from.  They are written here
    # rather than checked in so that the byte-for-byte one is generated
    # rather than at the mercy of git's line-ending handling.
    printf 'hello from cadaver\nline two\n' > "$WORK/hello.txt"
    printf 'the second file\n' > "$WORK/second.txt"
    "$PYTHON" -c "import sys; sys.stdout.buffer.write(bytes(range(256)))" \
        > "$WORK/every-byte.dat"
    # The first hundred bytes of it, so that `resumeget' has something
    # to resume from and the result can be compared with the whole file.
    "$PYTHON" -c "import sys; sys.stdout.buffer.write(bytes(range(100)))" \
        > "$WORK/partial.dat"
    # A directory and a file whose names contain a space, for the
    # quoting the paths session exercises.
    mkdir -p "$WORK/with space"
    printf 'a name with a space in it\n' > "$WORK/with space/has space.txt"

    # Not sed: both replacements are Windows paths, and GNU sed reads a
    # backslash in the replacement as an escape, so the separators
    # disappear and a path with a "U" or an "L" after one turns into
    # upper or lower case from there on.
    "$PYTHON" tests/expand.py "$script" "$OUT/$name.cad" \
        "@WORK@=$WORKNATIVE" "@EDITOR@=$EDITOR_NATIVE"

    # Extra command-line arguments, if the session asked for any.
    FLAGS=
    if [ -f "tests/sessions/$name.flags" ]; then
        "$PYTHON" tests/expand.py "tests/sessions/$name.flags" \
            "$OUT/$name.flags" "@WORK@=$WORKNATIVE" \
            "@EDITOR@=$EDITOR_NATIVE" "@OUT@=$OUTNATIVE"
        FLAGS=`tr '\n' ' ' < "$OUT/$name.flags"`
    fi

    # A home directory of the session's own, holding the .netrc it
    # brought.  Sessions without one still get an empty directory, so
    # that a .netrc in the real home cannot reach any of them.  It goes
    # beside the working directory rather than inside it: `lls' lists
    # the working directory, and a home directory in there would show up
    # in the transcript of every session that runs one.
    rm -rf "$OUT/$name-home"
    mkdir -p "$OUT/$name-home"
    SESSION_HOME=`native_path "$OUTABS/$name-home"`
    if [ -f "tests/sessions/$name.home" ]; then
        "$PYTHON" tests/expand.py "tests/sessions/$name.home" \
            "$OUT/$name-home/.netrc" "@WORK@=$WORKNATIVE" "@PORT@=$PORT"
    fi

    # What to feed standard input.  End of file unless the session says
    # otherwise, as a script would give it.
    STDIN=/dev/null
    if [ -f "tests/sessions/$name.stdin" ]; then
        "$PYTHON" tests/expand.py "tests/sessions/$name.stdin" \
            "$OUT/$name.stdin" "@WORK@=$WORKNATIVE"
        STDIN="$OUT/$name.stdin"
    fi

    # Each session gets an empty collection of its own, made through
    # the server rather than in the file system so that this works
    # against a server that is not backed by one.  Removing it first
    # covers a leftover from an interrupted run, which would otherwise
    # make MKCOL fail and every session look broken.
    printf 'rmcol %s\nmkcol %s\nquit\n' "$name" "$name" > "$OUT/$name.setup"
    # The same standard input as the session, because a session that
    # answers an authentication prompt has to answer it here too or the
    # collection it works in never gets made.  Each is a separate
    # process, so each reads the file from the beginning.
    ( HOME=$SESSION_HOME; export HOME
      "$CADAVER" -r "`native_path "$OUTABS/$name.setup"`" "$URL" \
        < "$STDIN" ) > "$OUT/$name.setup.log" 2>&1 || :

    printf '%-10s ' "$name"

    # The exit status is the whole point of several of these sessions
    # and a transcript cannot show it, so it goes on the end of one.
    STATUS=0
    ( cd "$WORK" && HOME=$SESSION_HOME; export HOME
      # shellcheck disable=SC2086
      "$CADAVER" $FLAGS -r "`native_path "$OUTABS/$name.cad"`" \
        "$URL$name/" ) < "$STDIN" > "$OUT/$name.raw" 2>&1 || STATUS=$?

    printf -- '-- cadaver exited %d --\n' "$STATUS" >> "$OUT/$name.raw"

    "$PYTHON" tests/normalise.py --port "$PORT" --work "$WORKNATIVE" \
        --editor "$EDITOR_NATIVE" \
        < "$OUT/$name.raw" > "$OUT/$name.txt"

    if [ $REGENERATE -eq 1 ]; then
        cp "$OUT/$name.txt" "$EXPECTED/$name.txt"
        echo "regenerated"
        continue
    fi

    if [ ! -f "$EXPECTED/$name.txt" ]; then
        echo "FAIL (no expected output; run with --regenerate)"
        RV=1
        FAILED="$FAILED $name"
        continue
    fi

    if ! diff -u "$EXPECTED/$name.txt" "$OUT/$name.txt" > "$OUT/$name.diff"; then
        echo "FAIL (transcript differs)"
        sed -n '1,40p' "$OUT/$name.diff"
        RV=1
        FAILED="$FAILED $name"
        continue
    fi

    # A session may come with a companion check: a shell snippet run
    # with $WORK set, for the things a transcript cannot show -- that a
    # downloaded file is byte for byte what was uploaded, for instance.
    if [ -f "tests/sessions/$name.check" ]; then
        if ! ( WORK="$WORKABS"; RAW="$OUTABS/$name.raw"
               export WORK RAW
               # shellcheck source=/dev/null
               . "$srcdir/tests/sessions/$name.check" ) \
               > "$OUT/$name.checklog" 2>&1; then
            echo "FAIL (post-session check)"
            cat "$OUT/$name.checklog"
            RV=1
            FAILED="$FAILED $name"
            continue
        fi
    fi

    echo "ok"
done

if [ $REGENERATE -eq 1 ]; then
    echo "-- Expected output rewritten in $EXPECTED; read the diff --"
    exit 0
fi

if [ $RV -eq 0 ]; then
    echo "All sessions match the expected results"
else
    echo "Sessions that failed:$FAILED"
    echo "Transcripts and diffs are in $OUT"
fi

exit $RV
