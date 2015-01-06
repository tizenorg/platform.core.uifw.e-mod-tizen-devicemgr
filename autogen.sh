#!/bin/sh
# Run this to generate all the initial makefiles, etc.

#rm -rf autom4te.cache
#rm -f aclocal.m4 ltmain.sh

#echo "Running autoreconf..." ; autoreconf -v --install || exit 1
#echo "Running configure..." ; ./configure --enable-maintainer-mode

set  -x
aclocal
autoconf
libtoolize --copy --force
autoheader
automake --foreign --add-missing --copy

