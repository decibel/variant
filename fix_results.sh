#!/bin/sh

cmd='^(ok |\\set ECHO 0|1\.\.[0-9]+|$)'
out=`egrep -v "$cmd" results/*`

if [ -z "$out" ]; then
    echo "No errors found; copying results $out"
    cp results/*.out test/expected
    git status -s test/expected
else
    echo "Errors found:"
    egrep -v "$cmd" results/*
fi
