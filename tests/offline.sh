#!/bin/sh
# Checks the things that need no server, no network and no Python.
#
#   ./tests/offline.sh
#
# Run it first: if the executable cannot report its own version there is
# no point starting a WebDAV server to find that out.  It takes well
# under a second.
#
# Environment:
#   CADAVER  the executable to test (default: ./cadaver or ./cadaver.exe)

set -e

srcdir=`dirname "$0"`/..
cd "$srcdir"

CADAVER=${CADAVER-./cadaver}
[ -x "$CADAVER" ] || CADAVER=./cadaver.exe
if [ ! -x "$CADAVER" ]; then
    echo "offline.sh: build cadaver first" >&2
    exit 1
fi

VERSION=`cat VERSION`
FAILURES=0
CHECKS=0

# Reports one check.  `expected' and `got' are compared literally, so
# every check says what it wanted rather than just that something was
# wrong.
check() {
    what=$1
    expected=$2
    got=$3

    CHECKS=`expr $CHECKS + 1`
    if [ "$expected" = "$got" ]; then
        printf '  ok   %s\n' "$what"
    else
        printf '  FAIL %s\n       wanted: %s\n       got:    %s\n' \
            "$what" "$expected" "$got"
        FAILURES=`expr $FAILURES + 1`
    fi
}

# Reports a check whose output only has to contain something.
contains() {
    what=$1
    needle=$2
    haystack=$3

    CHECKS=`expr $CHECKS + 1`
    case $haystack in
        *"$needle"*) printf '  ok   %s\n' "$what" ;;
        *)
            printf '  FAIL %s\n       wanted to find: %s\n       in: %s\n' \
                "$what" "$needle" "$haystack"
            FAILURES=`expr $FAILURES + 1`
            ;;
    esac
}

echo "-- Version --"

out=`"$CADAVER" --version 2>&1 || :`
check "--version reports the version in VERSION" \
    "cadaver $VERSION" "`echo \"$out\" | sed -n 1p`"
contains "--version names the bundled neon" "neon 0.37" "$out"

# -V is the short form of the same thing.
short=`"$CADAVER" -V 2>&1 || :`
check "-V and --version agree" "$out" "$short"

echo "-- Usage --"

out=`"$CADAVER" --help 2>&1 || :`
contains "--help shows the synopsis" "Usage: cadaver [OPTIONS] URL" "$out"
contains "--help names the fork" "cadaver-windows" "$out"
contains "--help mentions --tolerant" "--tolerant" "$out"

# The program name in the synopsis is argv[0] with the directory and the
# .exe suffix taken off, which is what a user typed.
contains "the synopsis does not name the executable's path" \
    "Usage: cadaver [OPTIONS] URL" "$out"

out=`"$CADAVER" --no-such-option 2>&1 || :`
contains "an unknown option is refused" "Try \`cadaver --help'" "$out"

out=`"$CADAVER" one two 2>&1 || :`
contains "more than one URL is refused" "Usage: cadaver" "$out"

echo "-- Without a server --"

# A high port that no service registers, so the connection is refused at
# once rather than timing out.  A well-known one would not do: the
# machine this was written on turned out to have something on port 9.
out=`echo quit | "$CADAVER" http://127.0.0.1:47821/ 2>&1 || :`
contains "a refused connection is reported" \
    "Could not connect to \`127.0.0.1' on port 47821" "$out"

out=`echo quit | "$CADAVER" 'not a url' 2>&1 || :`
contains "an unparsable URL is reported" "Could not parse URL" "$out"

out=`echo quit | "$CADAVER" 'ftp://example.com/' 2>&1 || :`
contains "an unsupported scheme is reported" "not supported" "$out"

out=`echo quit | "$CADAVER" 'http://user:pass@example.com/' 2>&1 || :`
contains "credentials in the URL are refused" "User info must not be used" \
    "$out"

echo "-- Local commands with no connection --"

out=`printf 'lpwd\nquit\n' | "$CADAVER" 2>&1 || :`
contains "lpwd works unconnected" "Local directory:" "$out"

out=`printf 'ls\nquit\n' | "$CADAVER" 2>&1 || :`
contains "a remote command explains it needs a connection" \
    "can only be used when connected" "$out"

out=`printf 'help\nquit\n' | "$CADAVER" 2>&1 || :`
contains "help lists the commands" "Available commands:" "$out"
contains "help lists the aliases" "Aliases: rm=delete" "$out"

# End of input is end of session, so a closed stdin must not hang.  All
# that is left is the prompt readline wrote before it read.
out=`"$CADAVER" < /dev/null 2>&1 || :`
check "end of input ends the session" "dav:!>" \
    "`echo \"$out\" | tr -d '\r\n' | sed 's/ *$//'`"

echo
if [ $FAILURES -eq 0 ]; then
    echo "$CHECKS checks, all passed"
    exit 0
fi

echo "$CHECKS checks, $FAILURES failed"
exit 1
