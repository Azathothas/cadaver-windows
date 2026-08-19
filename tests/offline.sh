#!/bin/sh
# Checks the things that need no server, no network and no Python.
#
#   ./tests/offline.sh
#
# Run it first: if the executable cannot report its own version there is
# no point starting a WebDAV server to find that out.  It takes well
# under a second.
#
# It also checks the source, for the one rule that keeps --json usable:
# nothing in src/ may write to standard output except through
# src/output.c.  One stray printf would corrupt the document.
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

srcdir=`pwd`

# For the one check that needs to build a long line.  Any interpreter
# will do, and the check is skipped when there is none.
# shellcheck source=tests/python.sh
. tests/python.sh

VERSION=`cat VERSION`
FAILURES=0
CHECKS=0

# Somewhere to write the scratch files a few checks need.
TMP=offline-work
rm -rf "$TMP"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

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

# Reports a check whose output must NOT contain something.
lacks() {
    what=$1
    needle=$2
    haystack=$3

    CHECKS=`expr $CHECKS + 1`
    case $haystack in
        *"$needle"*)
            printf '  FAIL %s\n       did not want to find: %s\n       in: %s\n' \
                "$what" "$needle" "$haystack"
            FAILURES=`expr $FAILURES + 1`
            ;;
        *) printf '  ok   %s\n' "$what" ;;
    esac
}

# Reports a check on an exit status.
status() {
    what=$1
    expected=$2
    shift 2

    # Not a bare invocation: `set -e' is on, and a non-zero status is
    # exactly what several of these are checking for.
    got=0
    "$@" > /dev/null 2>&1 || got=$?

    CHECKS=`expr $CHECKS + 1`
    if [ "$expected" = "$got" ]; then
        printf '  ok   %s\n' "$what"
    else
        printf '  FAIL %s\n       wanted exit %s, got %s\n' \
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

out=`"$CADAVER" --help 2>&1 || :`
contains "--help documents --json" "--json" "$out"
contains "--help documents --trace" "--trace" "$out"
contains "--help documents --clobber" "--clobber" "$out"

echo "-- Exit status --"

# 0 for the two that answer a question about the program, 2 for a
# command line that could not be understood at all.  These used to be
# -1, which Windows reports as 4294967295 and a shell with `set -e' in
# it treats as a failure of something that in fact worked.
status "--version succeeds" 0 "$CADAVER" --version
status "-V succeeds" 0 "$CADAVER" -V
status "--help succeeds" 0 "$CADAVER" --help
status "an unknown option is a usage error" 2 "$CADAVER" --no-such-option
status "more than one URL is a usage error" 2 "$CADAVER" one two
status "an unknown clobber value is a usage error" 2 \
    "$CADAVER" --clobber=maybe
status "--json and --trace=- cannot share standard output" 2 \
    "$CADAVER" --trace=- --json
status "nor in the other order" 2 \
    "$CADAVER" --json --trace=-

# A session reports how many commands failed, capped so that it cannot
# be read as a signalled exit.  Neither of these connects to anything,
# so every remote command fails on that.
printf 'quit\n' > "$TMP/quit.cad"
status "a session with nothing in it succeeds" 0 \
    sh -c "printf 'quit\n' | '$CADAVER'"
status "one failed command exits 1" 1 \
    sh -c "printf 'nosuchcommand\nquit\n' | '$CADAVER'"
status "three failed commands exit 3" 3 \
    sh -c "printf 'nosuchcommand\nnosuchcommand\nls\nquit\n' | '$CADAVER'"
status "a refused connection is a failure" 1 \
    sh -c "printf 'quit\n' | '$CADAVER' http://127.0.0.1:47821/"

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

echo "-- Options --"

# `set' with no argument prints every boolean as on or off, and until
# now that could not be typed back in: a boolean refused a value, so
# `unset' was the only way to turn one off.
out=`printf 'set overwrite off\nset\nquit\n' | "$CADAVER" 2>&1 || :`
contains "a boolean takes off" "overwrite: off" "$out"
out=`printf 'set overwrite off\nset overwrite on\nset\nquit\n' \
    | "$CADAVER" 2>&1 || :`
contains "a boolean takes on" "overwrite: on" "$out"
out=`printf 'set overwrite perhaps\nquit\n' | "$CADAVER" 2>&1 || :`
contains "a boolean refuses anything else" "give it on or off" "$out"
status "and that is a failed command" 1 \
    sh -c "printf 'set overwrite perhaps\nquit\n' | '$CADAVER'"

# Every option with a fixed set of values reports one it does not know.
# searchdepth used to take anything at all and treat it as infinity, so
# a typo silently set the widest possible search.
out=`printf 'set searchdepth 1\nset\nquit\n' | "$CADAVER" 2>&1 || :`
contains "searchdepth accepts 1" "searchdepth: 1" "$out"
out=`printf 'set searchdepth nonsense\nquit\n' | "$CADAVER" 2>&1 || :`
contains "searchdepth refuses a value it does not know" \
    "Invalid value for searchdepth" "$out"
status "and that is a failed command" 1 \
    sh -c "printf 'set searchdepth nonsense\nquit\n' | '$CADAVER'"

# `set' on an option that does not exist reports a failure; `unset' used
# to print the same message and succeed.
status "unset on an unknown option fails" 1 \
    sh -c "printf 'unset nosuchoption\nquit\n' | '$CADAVER'"

echo "-- Quoting --"

# `echo' writes each argument followed by a space, so the number of
# arguments the tokeniser produced is visible in the spacing.  -r rather
# than a pipe, because readline echoes the prompt and the command line
# itself when the input is not a terminal.
quoting() {
    what=$1
    expected=$2
    line=$3

    printf '%s\nquit\n' "$line" > "$TMP/quote.cad"
    got=`"$CADAVER" -r "$TMP/quote.cad" < /dev/null 2>/dev/null | head -1 \
        | tr -d '\r' || :`
    check "$what" "$expected" "$got"
}

# An empty argument and the end of the line used to be the same answer
# from gettoken(), and parse_command() reads the second as the end of
# the command, so `propset res name ""' set nothing and dropped every
# argument after it in silence.
quoting "an empty argument is an argument" "a  b " 'echo a "" b'
quoting "an empty argument alone is one argument" " " 'echo ""'

# A quote quotes only where it opens a token; inside one it is an
# ordinary character.  That is the rule lib/netrc.c uses for a .netrc
# value, and it is what leaves a way to write a quote at all on Windows,
# where a backslash is a path separator rather than an escape.
quoting "a quote opening a token groups it" "a b " 'echo "a b"'
quoting "a quote inside a token is a character" 'a"b"c ' 'echo a"b"c'
quoting "an unterminated quote runs to the end of the line" "a b " 'echo "a b'

# A hash still starts a comment, and still does not inside quotes.
quoting "a hash starts a comment" "one " 'echo one # two'
quoting "a hash inside quotes is a hash" "one # two " 'echo "one # two"'

echo "-- Machine-readable output --"

# One object on standard output and nothing else.  The prompt readline
# writes goes to standard error under --json, so standard output has to
# be a single line beginning and ending with a brace.
out=`printf 'help\nquit\n' | "$CADAVER" --json 2>/dev/null || :`
lines=`printf '%s\n' "$out" | wc -l | tr -d ' '`
check "--json writes exactly one line to standard output" "1" "$lines"
contains "--json names the tool" '"tool":"cadaver"' "$out"
contains "--json carries a summary" '"summary":' "$out"
contains "--json records each command" '"command":"help"' "$out"
contains "--json keeps the human output" '"output":[' "$out"
lacks "--json does not leak the prompt to standard output" "dav:!>" "$out"

# A failure classified without matching prose.
out=`printf 'nosuchcommand\nquit\n' | "$CADAVER" --json 2>/dev/null || :`
contains "--json marks a failed command" '"status":"failed"' "$out"
contains "--json gives a failure a context" '"context":' "$out"

echo "-- Long commands --"

# A command longer than BUFSIZ used to be cut into several: the rcfile
# was read with fgets() into a fixed buffer, and the tokeniser gave up
# on a token that long by returning NULL, which reads as end of line.
# BUFSIZ is 512 on Windows, which a deep path reaches without trying.
long=`"$PYTHON" -c "print('x' * 20000)" 2>/dev/null || :`
if [ -n "$long" ]; then
    printf 'echo %s
quit
' "$long" > "$TMP/long.cad"
    out=`"$CADAVER" -r "$TMP/long.cad" < /dev/null 2>&1 || :`
    got=`printf '%s' "$out" | head -1 | tr -d '
' | wc -c | tr -d ' '`
    # The 20000 characters, and the trailing space `echo' writes after
    # each argument.
    check "a 20000-character command survives whole" "20001" "$got"
else
    echo "  skip a long command needs a Python interpreter"
fi

echo "-- The source --"

# src/output.c decides where output goes.  With --json standard output
# carries the result document, so one write from anywhere else would
# make it something no parser accepts.
#
# There are three ways to write there, and none of them is caught by the
# checks for the other two: the printf family with no stream argument, a
# stdio call naming stdout, and a write(2) to the descriptor.
stray=`grep -nE '(^|[^A-Za-z0-9_])(printf|vprintf|putchar|puts)[[:space:]]*\(' \
    "$srcdir"/src/*.c | grep -v '^.*src/output\.c:' \
    | grep -vE 'fprintf|snprintf|vfprintf|vsnprintf|sprintf|fputs|out_printf|out_vprintf|out_putchar|out_puts' || :`
check "nothing in src/ calls the printf family directly" "" "$stray"

# Anything naming stdout: fputs(x, stdout), fprintf(stdout, ...),
# fwrite(x, 1, n, stdout), fileno(stdout).  Matching the call would miss
# the ones written over two lines, so the name itself is what is
# forbidden.  --trace is the one thing allowed to aim there, and --json
# refuses to share standard output with it.
stray=`grep -n '\bstdout\b' "$srcdir"/src/*.c \
    | grep -v '^.*src/output\.c:' | grep -v 'trace' || :`
check "nothing in src/ names stdout except the trace" "" "$stray"

# And the descriptor underneath it.  `cat' hands STDOUT_FILENO to neon,
# which is the resource itself and is refused under --json, so only a
# write(2) is looked for here.
stray=`grep -nE '(^|[^A-Za-z0-9_])write[[:space:]]*\([[:space:]]*(STDOUT_FILENO|1)[[:space:]]*,' \
    "$srcdir"/src/*.c | grep -v '^.*src/output\.c:' || :`
check "nothing in src/ writes to the standard output descriptor" "" "$stray"

echo
if [ $FAILURES -eq 0 ]; then
    echo "$CHECKS checks, all passed"
    exit 0
fi

echo "$CHECKS checks, $FAILURES failed"
exit 1
