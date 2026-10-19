#!/bin/sh
# pe.sh ENGINE K N COUNT: per-process P/E counters between 25 s and 85 s of COUNT tests.
eng=$1 k=$2 n=$3 c=$4
w=$(mktemp -d /tmp/pe.XXXXXX); pids=; i=0
while [ $i -lt $c ]; do
    mkdir $w/$i
    (cd $w/$i && exec $eng "$k*2^$((n + 2 * i))-1" -d -fermat a 3 -t 1 -time progress 5 > out 2>&1) &
    pids="$pids $!"; i=$((i + 1))
done
sleep 25; /tmp/lb/rus $pids | sed 's/^/a /'
sleep 60; /tmp/lb/rus $pids | sed 's/^/b /'
for p in $pids; do kill $p; done; wait; rm -rf $w
