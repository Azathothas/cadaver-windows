#!/bin/sh
# Builds and runs the unit tests for lib/netrc.c.
#
#   ./tests/netrc.sh
#
# Needs no server, no network and no cadaver binary: it compiles
# tests/netrctest.c against lib/netrc.c and runs the result.  Under a
# second, and the thing to run after touching the .netrc parsing.
#
# What that parser gets wrong is invisible in use -- a password it
# mangles produces an authentication failure that names neither the file
# nor the character -- so it is worth checking directly rather than only
# through a session.
#
# It uses win32/config.h, so it works whether or not configure has been
# run.  CC may be overridden.

set -e

srcdir=`dirname "$0"`/..
cd "$srcdir"

CC=${CC-gcc}
WORK=netrctest-work
OUT=$WORK/netrctest

rm -rf "$WORK"
mkdir -p "$WORK"

echo "-- Building --"
$CC -g -O2 -Wall -Wstrict-prototypes -Wmissing-declarations -Wshadow \
    -DHAVE_CONFIG_H -Iwin32 -Ilib -Ineon/src \
    -o "$OUT" tests/netrctest.c lib/netrc.c

echo "-- Running --"
# The test writes its scratch .netrc into the directory it is given,
# rather than leaving one in the tree.
"$OUT" "$WORK"
