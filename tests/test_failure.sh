#!/bin/bash
# Membership test with 3 idle nodes: one holds a lock and is killed (kill -9). The survivors
# must see it DEAD in about 5 s (nodes command of the console), the waiter must get the lock
# the dead node held, and a node that quits through the console must show as LEFT.
# Usage: tests/test_failure.sh   (needs `make` first)
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d)
sed "s/max_nodes = 8/max_nodes = 4/; s#dir = ./logs#dir = $T/logs#" conf/dsm.conf > "$T/dsm.conf"
export DSM_CONSOLE=1
mkfifo "$T/in0" "$T/in2"

build/apps/idle_dsm "$T/dsm.conf" 60 plain < "$T/in0" > "$T/n0.out" 2>&1 &
P0=$!; exec 8> "$T/in0"; sleep 1
build/apps/idle_dsm "$T/dsm.conf" 60 holder < /dev/null > "$T/n1.out" 2>&1 &
P1=$!; sleep 1
build/apps/idle_dsm "$T/dsm.conf" 60 waiter < "$T/in2" > "$T/n2.out" 2>&1 &
P2=$!; exec 9> "$T/in2"; sleep 3

echo "nodes" >&8; sleep 1
echo "== nodes before the kill (node 0)"; grep -E "^ *[0-9]+ " "$T/n0.out"
kill -9 $P1
echo "kill -9 of node 1 (holds lock 5)"; sleep 9
echo "nodes" >&8; echo "nodes" >&9; echo "stats" >&9; sleep 1
FAIL=0
grep -q "GOT LOCK" "$T/n2.out" && echo "waiter got the dead node's lock: OK" || { echo "waiter did NOT get the lock"; FAIL=1; }
echo "== nodes after the kill (node 0)"; grep -E "^ *[0-9]+ " "$T/n0.out" | tail -3
tail -3 "$T/n0.out" | grep -q "DEAD" || FAIL=1

echo "quit" >&9; sleep 2
echo "nodes" >&8; sleep 1
echo "== nodes after node 2 quit (node 0)"; grep -E "^ *[0-9]+ " "$T/n0.out" | tail -3
tail -3 "$T/n0.out" | grep -q "LEFT" || { echo "node 2 not shown as LEFT"; FAIL=1; }

echo "quit" >&8; sleep 1; kill $P0 $P2 2>/dev/null
[ $FAIL -eq 0 ] && echo "TEST_FAILURE OK" || echo "TEST_FAILURE FAILED (logs en $T)"
exit $FAIL
