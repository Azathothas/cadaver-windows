#!/bin/sh
# Builds and runs the unit tests for lib/glob.c.
#
#   ./tests/glob.sh
#
# Needs no server, no network and no cadaver binary: it compiles
# tests/globtest.c against lib/glob.c and runs the result.  A second or
# two, and the first thing worth running after touching the globbing.
#
# It uses win32/config.h, so it works whether or not configure has been
# run.  CC may be overridden.

set -e

srcdir=`dirname "$0"`/..
cd "$srcdir"

CC=${CC-gcc}
WORK=globtest-work
OUT=$WORK/globtest

rm -rf "$WORK"
mkdir -p "$WORK"

echo "-- Building --"
$CC -g -O2 -Wall -Wstrict-prototypes -Wmissing-declarations -Wshadow \
    -DHAVE_CONFIG_H -Iwin32 -Ilib -Ineon/src \
    -o "$OUT" tests/globtest.c lib/glob.c

echo "-- Running --"
# The test creates its own directory and works inside it, so give it one
# under the scratch directory rather than leaving files in the tree.
"$OUT" "`pwd`/$WORK/tree"
