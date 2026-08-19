#!/bin/sh
# Runs the scripted sessions against a local wsgidav server.
#
#   ./tests/wsgidav.sh [--regenerate] [SESSION...]
#
# wsgidav is pure Python and runs natively on Windows.  It has a dead
# property store, which x/net/webdav does not, so this is where the
# property sessions mean anything -- but it answers LOCK with the
# Content-Type "application; charset=utf-8", which is not a media type,
# so neon discards the body unparsed and cadaver never sees the lock
# token.  Verify lock work against tests/godav.sh instead.
#
# On the first run the script creates a virtualenv in ./dav-venv and
# installs wsgidav into it, which needs a native Windows Python from
# https://www.python.org/downloads/ and network access.  Later runs
# reuse it.  Server data goes in ./davroot.  Both are gitignored.
#
# Environment:
#   PYTHON   interpreter to build the venv with
#   PORT     port for the server (default 8908)
#   CADAVER  the executable to test

set -e

srcdir=`dirname "$0"`/..
cd "$srcdir"

PORT=${PORT-8908}
VENV=dav-venv
DAVROOT=davroot

# wsgidav needs a native Windows Python: bcrypt has no mingw wheel.
NEED_NATIVE_PYTHON=1
# shellcheck source=tests/python.sh
. tests/python.sh

if [ -z "${PYTHON-}" ]; then
    cat >&2 <<'MSG'
wsgidav.sh: no working Python interpreter found.

This script needs a native Windows Python (or any Python on Unix). If
one is installed but not on PATH, point at it directly:

    PYTHON=/c/Python313/python.exe ./tests/wsgidav.sh

Do not use MSYS2's own python here: wsgidav pulls in bcrypt, which has
no mingw wheel and needs Rust to build. Install Python from
https://www.python.org/downloads/ if you have none.
MSG
    exit 1
fi

# The venv layout differs between Windows and Unix.  Existing is not
# enough: one from an interrupted run has an interpreter and no
# packages, and one restored from a CI cache can point at an interpreter
# that is no longer installed.  Import what the server needs; if that
# fails, throw the directory away and build it again.
VPYTHON=
for candidate in "$VENV/Scripts/python.exe" "$VENV/bin/python"; do
    if [ -x "$candidate" ] \
       && "$candidate" -c "import wsgidav, cheroot" >/dev/null 2>&1; then
        VPYTHON=$candidate
        break
    fi
done

if [ -z "$VPYTHON" ]; then
    echo "-- Creating virtualenv in $VENV --"
    rm -rf "$VENV"
    "$PYTHON" -m venv "$VENV"
    if [ -x "$VENV/Scripts/python.exe" ]; then
        VPYTHON="$VENV/Scripts/python.exe"
    else
        VPYTHON="$VENV/bin/python"
    fi
    "$VPYTHON" -m pip install --quiet --upgrade pip
    "$VPYTHON" -m pip install --quiet wsgidav cheroot
fi

rm -rf "$DAVROOT"
mkdir -p "$DAVROOT/dav"

# wsgidav is a native Windows program, so it needs a Windows-style path;
# an MSYS /c/... path will not work.
ABSROOT=`cd "$DAVROOT/dav" && pwd`
if command -v cygpath >/dev/null 2>&1; then
    ABSROOT=`cygpath -m "$ABSROOT"`
fi

# property_manager must be on, or every PROPPATCH returns 403 and the
# property session fails in a way that looks like a cadaver bug.
# lock_manager is deliberately not set: it is deprecated in wsgidav 4.x
# and setting it makes startup fail outright.
cat > "$DAVROOT/wsgidav.json" <<EOF
{
  "host": "127.0.0.1",
  "port": $PORT,
  "provider_mapping": {"/": "$ABSROOT"},
  "simple_dc": {"user_mapping": {"*": true}},
  "property_manager": true,
  "verbose": 1
}
EOF

echo "-- Launching wsgidav on port $PORT --"
"$VPYTHON" -m wsgidav.server.server_cli --config "$DAVROOT/wsgidav.json" \
    > "$DAVROOT/wsgidav.log" 2>&1 &
SERVER_PID=$!

cleanup() {
    if kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || :
        # The server is a native Windows program, and an MSYS2 or Git
        # Bash shell can only really signal one of those with SIGKILL:
        # a SIGTERM is accepted and then does nothing, which would
        # leave the port held and make the next run fail to bind.
        kill -9 "$SERVER_PID" 2>/dev/null || :
        wait "$SERVER_PID" 2>/dev/null || :
    fi
}
trap cleanup EXIT INT TERM

# Wait for the port to accept connections rather than sleeping blindly.
"$PYTHON" - "$PORT" <<'PY' || { echo "-- Server did not start; see $DAVROOT/wsgidav.log --"; exit 1; }
import socket, sys, time
port = int(sys.argv[1])
for _ in range(100):
    try:
        socket.create_connection(("127.0.0.1", port), 0.5).close()
        sys.exit(0)
    except OSError:
        time.sleep(0.1)
sys.exit(1)
PY

echo "-- Running sessions --"
# Not exec: that would replace this shell and the trap above would never
# run, leaving the server holding the port for the next invocation.
RV=0
./tests/session.sh --url "http://127.0.0.1:$PORT/" --name wsgidav "$@" || RV=$?
exit $RV
