#!/bin/sh
# Runs the scripted sessions against golang.org/x/net/webdav.
#
#   ./tests/godav.sh [--regenerate] [SESSION...]
#
# Needs a Go toolchain and, on the first run, network access to fetch
# golang.org/x/net.  The server source is in tests/godav.
#
# Neither test server is complete, which is the point of having both:
# x/net/webdav locks correctly but has no dead property store, so the
# props session fails against it; wsgidav supports dead properties but
# answers LOCK with a Content-Type that is not a media type, so neon
# discards the body and cadaver never sees the lock token.  Verify lock
# work here and property work against tests/wsgidav.sh, and do not read
# either result as the whole picture.
#
# Environment:
#   PORT     port for the server (default 8907)
#   CADAVER  the executable to test

set -e

srcdir=`dirname "$0"`/..
cd "$srcdir"

PORT=${PORT-8907}
OUT=godav-results

if ! command -v go >/dev/null 2>&1; then
    echo "godav.sh: needs a Go toolchain (https://go.dev/dl/)" >&2
    exit 1
fi

# A native Windows Go run from a shell that does not pass USERPROFILE
# through has nowhere to keep its module cache, and says so in a way
# that reads like a problem with this script.  Say what it actually is.
if [ -z "`go env GOMODCACHE 2>/dev/null`" ]; then
    cat >&2 <<'MSG'
godav.sh: the Go toolchain has no module cache directory.

Go works this out from GOMODCACHE, then GOPATH, then the home
directory -- USERPROFILE on Windows. None of them is set here, which
happens when an MSYS2 shell is started without the Windows environment.
Set GOPATH to somewhere writable and try again:

    GOPATH=/c/Users/you/go ./tests/godav.sh
MSG
    exit 1
fi

# Always rebuild: go caches, so this costs a fraction of a second on a
# repeat run, and it removes the trap of an executable left over from an
# older main.go.
echo "-- Building the x/net/webdav server --"
(cd tests/godav && go build -o godav .)
SERVER=tests/godav/godav
[ -x "$SERVER" ] || SERVER=tests/godav/godav.exe

rm -rf "$OUT"
mkdir -p "$OUT/root"

ABSROOT=`cd "$OUT/root" && pwd`
if command -v cygpath >/dev/null 2>&1; then
    ABSROOT=`cygpath -m "$ABSROOT"`
fi

echo "-- Launching x/net/webdav on port $PORT --"
"$SERVER" -addr "127.0.0.1:$PORT" -dir "$ABSROOT" > "$OUT/server.log" 2>&1 &
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

# Wait for the server to announce that it is listening rather than
# sleeping blindly.  It binds the socket before it logs that line, so
# seeing it means the port is accepting connections.
n=0
while [ "$n" -lt 100 ]; do
    if grep -q "listening on" "$OUT/server.log" 2>/dev/null; then
        break
    fi
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "godav.sh: server exited; see $OUT/server.log" >&2
        exit 1
    fi
    n=`expr $n + 1`
    sleep 0.1 2>/dev/null || sleep 1
done

echo "-- Running sessions --"
# Not exec: that would replace this shell and the trap above would never
# run, leaving the server holding the port for the next invocation.
RV=0
./tests/session.sh --url "http://127.0.0.1:$PORT/" --name godav "$@" || RV=$?
exit $RV
