#!/bin/bash

x=$(grep ingroup chapters/api/*|awk -F':' '{print $2}'| awk 'NF != 2')
if test -n "$x" ; then
    echo Errors on group definitions
    echo $x
fi

echo
echo "Defined groups"
grep ingroup chapters/api/*|awk -F':' '{print $2}'| awk 'NF == 2'|sort|uniq
echo

for f in ../../build/doc/doxygen/latex/*tex ; do
    x=$(grep $(basename $f .tex) refman.tex)
    if test -z "$x" ; then
	echo Error. $f not included in refman.tex
    fi
done

