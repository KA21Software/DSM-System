#!/bin/bash
# Integration test: runs apps/selftest_dsm on N nodes (processes of this PC) and checks that
# every node reports PASS. Also checks that touching an unallocated page ends with ENOTALLOC.
# Usage: tests/test_cluster.sh [nodes]   (needs `make` first; default 3 nodes)
set -u
cd "$(dirname "$0")/.."
NODES=${1:-3}
T=$(mktemp -d)
sed "s/max_nodes = 8/max_nodes = $NODES/; s#dir = ./logs#dir = $T/logs#; s/node_memory = .*/node_memory = 4M/" conf/dsm.conf > "$T/dsm.conf"

run_cluster() {  # $1 = extra argument for the app ("" or notalloc), $2 = output prefix
    local PIDS=()
    for ((i = 0; i < NODES; i++)); do
        build/apps/selftest_dsm "$T/dsm.conf" "$NODES" $1 > "$T/$2$i.out" 2>&1 < /dev/null &
        PIDS+=($!)
        sleep 1
    done
    local rc=0
    for p in "${PIDS[@]}"; do wait "$p" || rc=1; done
    return $rc
}

FAIL=0
echo "== selftest with $NODES nodes"
if run_cluster "" n; then echo "all processes exited 0"; else echo "some process failed"; FAIL=1; fi
cat "$T"/n*.out | grep -E "FAIL|PASS|t7 block" | sort | uniq -c
[ "$(grep -l 'SELFTEST PASS' "$T"/n*.out | wc -l)" -eq "$NODES" ] || FAIL=1

echo "== access to an unallocated page"
run_cluster notalloc a
grep -q "ENOTALLOC" "$T"/a*.out && echo "ENOTALLOC reported" || { echo "no ENOTALLOC message"; FAIL=1; }

echo "logs en $T/logs"
[ $FAIL -eq 0 ] && echo "TEST_CLUSTER OK" || echo "TEST_CLUSTER FAILED"
exit $FAIL
