#!/bin/sh
# scale.sh SECS COUNTS CASES: per-test ms/sq for kite and prst, ABBA, at each test count.
# Output lines: engine k n count rep ms_test0 ms_test1 ...
secs=$1 counts=$2 cases=$3
kite=${KITE_BIN:-/Users/andy/.crunch/kite-prst} prst=${PRST_BIN:-/Users/andy/.crunch/prst}
work=$(mktemp -d /tmp/scale.XXXXXX)
trap 'kill $(cat $work/pids 2>/dev/null) 2>/dev/null; rm -rf $work' EXIT INT TERM
run() {
    eng=$1 name=$2 k=$3 n=$4 c=$5 rep=$6
    rm -rf $work/r; mkdir $work/r; : > $work/pids
    i=0
    while [ $i -lt $c ]; do
        mkdir $work/r/$i
        (cd $work/r/$i && exec $eng "$k*2^$((n + 2 * i))-1" -d -fermat a 3 -t 1 -time progress 5 > out 2>&1) &
        echo $! >> $work/pids; i=$((i + 1))
    done
    sleep $secs
    kill $(cat $work/pids) 2>/dev/null; wait 2>/dev/null; : > $work/pids
    fft=$(grep -h -o "FFT length [0-9K]*" $work/r/0/out | head -1 | sed "s/FFT length //")
    line="$name $k $n $c $rep ${fft:--}"
    for f in $work/r/*/out; do
        m=$(tr '\r' '\n' < $f | sed -n 's/.*time per op: \([0-9.]*\) ms.*/\1/p' | tail -n +2 | sort -n |
            awk '{a[NR]=$1} END {if (NR) printf "%.4f", a[int((NR+1)/2)]; else print "-"}')
        line="$line $m"
    done
    echo "$line"
}
for kn in $cases; do
    k=${kn%:*} n=${kn#*:}
    for c in $counts; do
        run $kite kite $k $n $c 1; run $prst prst $k $n $c 1
        run $prst prst $k $n $c 2; run $kite kite $k $n $c 2
    done
done
