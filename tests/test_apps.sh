#!/bin/bash
# Runs the test applications on N nodes of this PC (mm_dsm; the second app goes here once chosen).
# Usage: tests/test_apps.sh [nodes] [matrix size]
set -u
cd "$(dirname "$0")/.."
NODES=${1:-3}; SIZE=${2:-256}
T=$(mktemp -d)
sed "s/max_nodes = 8/max_nodes = $NODES/; s#dir = ./logs#dir = $T/logs#; s/node_memory = .*/node_memory = 4M/" conf/dsm.conf > "$T/dsm.conf"

run() {  # $1 = output prefix, rest = command with the config path as first argument
    local P=$1; shift
    local PIDS=() rc=0
    for ((i = 0; i < NODES; i++)); do
        "$@" > "$T/$P$i.out" 2>&1 < /dev/null &
        PIDS+=($!)
        sleep 1
    done
    for p in "${PIDS[@]}"; do wait "$p" || rc=1; done
    return $rc
}

FAIL=0
echo "== mm_dsm $SIZE x $SIZE, $NODES nodes"
run mm build/apps/mm_dsm "$T/dsm.conf" $SIZE $SIZE $SIZE 0 $NODES && echo "exit codes 0" || FAIL=1
grep -h "verification\|Time" "$T"/mm*.out


[ $FAIL -eq 0 ] && echo "TEST_APPS OK" || echo "TEST_APPS FAILED (logs en $T)"
exit $FAIL
