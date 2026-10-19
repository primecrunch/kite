#!/bin/sh
# xt2.sh ENGINE TEMPLATE OUT: start TEMPLATE recording, then 10 tests of 2009*2^n-1 (240K), 50 s.
eng=$1 tmpl=$2 out=$3
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
rm -rf "$out"
sudo -E xcrun xctrace record --template "$tmpl" --all-processes --time-limit 50s --output "$out" > "$out.log" 2>&1 &
xp=$!
sleep 6
w=$(mktemp -d /tmp/xt.XXXXXX); pids=; i=0
while [ $i -lt 10 ]; do
    mkdir $w/$i
    (cd $w/$i && exec $eng "2009*2^$((3322280 + 2 * i))-1" -d -fermat a 3 -t 1 -time progress 5 > out 2>&1) &
    pids="$pids $!"; i=$((i + 1))
done
sleep 8; for p in $pids; do echo "$p $(vmmap $p 2>/dev/null | awk '/^__TEXT/ {print $2; exit}')"; done > "$out.slides"
wait $xp
echo "xctrace exit $? pids$pids"
for p in $pids; do kill $p; done; wait; rm -rf $w
