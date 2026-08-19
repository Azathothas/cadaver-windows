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

OUT=session-results/$NAME
rm -rf "$OUT"
mkdir -p "$OUT"
OUTABS=`cd "$OUT" && pwd`

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

    # Each session gets an empty collection of its own, made through
    # the server rather than in the file system so that this works
    # against a server that is not backed by one.  Removing it first
    # covers a leftover from an interrupted run, which would otherwise
    # make MKCOL fail and every session look broken.
    printf 'rmcol %s\nmkcol %s\nquit\n' "$name" "$name" > "$OUT/$name.setup"
    "$CADAVER" -r "`native_path "$OUTABS/$name.setup"`" "$URL" \
        < /dev/null > "$OUT/$name.setup.log" 2>&1 || :

    printf '%-10s ' "$name"

    ( cd "$WORK" && "$CADAVER" -r "`native_path "$OUTABS/$name.cad"`" \
        "$URL$name/" ) < /dev/null > "$OUT/$name.raw" 2>&1 || :

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
        if ! ( WORK="$WORKABS"; export WORK
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
