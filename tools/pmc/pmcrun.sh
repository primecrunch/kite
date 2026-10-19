#!/bin/sh
# pmcrun.sh ENGINE K N: system-wide counters over 40 s of 10 tests, plus each test's ms per squaring.
eng=$1 k=$2 n=$3
w=$(mktemp -d /tmp/pmc.XXXXXX); pids=; i=0
while [ $i -lt 10 ]; do
    mkdir $w/$i
    (cd $w/$i && exec $eng "$k*2^$((n + 2 * i))-1" -d -fermat a 3 -t 1 -time progress 5 > out 2>&1) &
    pids="$pids $!"; i=$((i + 1))
done
sleep 20
sudo /tmp/lb/pmc 40 $EVENTS
for p in $pids; do kill $p; done; wait
for f in $w/*/out; do
    tr '\r' '\n' < $f | sed -n 's/.*time per op: \([0-9.]*\) ms.*/\1/p' | tail -n +2 | sort -n |
        awk '{a[NR]=$1} END {if (NR) printf "ms %s\n", a[int((NR+1)/2)]}'
done
rm -rf $w
