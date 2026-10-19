#!/bin/sh
# prof.sh SECS K N COUNTS: KITE_PROF=1 phase profiles of COUNT tests at once, printed per test.
secs=$1 k=$2 n=$3 counts=$4
eng=${KITE_BIN:-/tmp/lb/kite-prst}
for c in $counts; do
    w=$(mktemp -d /tmp/prof.XXXXXX); pids=
    i=0
    while [ $i -lt $c ]; do
        mkdir $w/$i
        (cd $w/$i && KITE_PROF=1 exec $eng "$k*2^$((n + 2 * i))-1" -d -fermat a 3 -t 1 -time progress 5 > out 2>&1) &
        pids="$pids $!"; i=$((i + 1))
    done
    sleep $secs
    for p in $pids; do kill $p; done
    wait
    for f in $w/*/out; do echo "== count $c $(basename $(dirname $f))"; sed -n '/phase profile/,$p' $f; done
    rm -rf $w
done
