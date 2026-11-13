#!/bin/bash
# Demo de E2 en una sola PC: N nodos (procesos) ejecutan apps/mm_dsm.
# Uso: tests/demo_local.sh [N K M [nodos]]   (requiere `make` previo; por defecto 256 256 256 con 2 nodos)
set -u
cd "$(dirname "$0")/.."
N=${1:-256}; K=${2:-256}; M=${3:-256}; NODES=${4:-2}
T=$(mktemp -d)
sed "s/max_nodes = 8/max_nodes = $NODES/; s#dir = ./logs#dir = $T/logs#; s/node_memory = .*/node_memory = 4M/" conf/dsm.conf > "$T/dsm.conf"
PIDS=()
for ((i = 0; i < NODES; i++)); do
    build/apps/mm_dsm "$T/dsm.conf" "$N" "$K" "$M" 0 "$NODES" > "$T/n$i.out" 2>&1 &
    PIDS+=($!)
    sleep 1  # el nodo 0 debe fundar el cluster antes de que lleguen los demas
done
RC=0
for p in "${PIDS[@]}"; do wait "$p" || RC=1; done
cat "$T"/n*.out
echo "logs en $T/logs"
exit $RC
