#!/bin/sh
set -e
rm -rf config.cache autom4te*.cache aclocal.m4
printf 'aclocal... '
${ACLOCAL:-aclocal} -I m4 -I neon/macros
printf 'autoheader... '
${AUTOHEADER:-autoheader} -Wall
printf 'autoconf... '
${AUTOCONF:-autoconf} -Wall
echo okay.
rm -rf autom4te*.cache
