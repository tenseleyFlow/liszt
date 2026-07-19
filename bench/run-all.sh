#!/bin/sh
# Perf suite master. Per-workload scripts land with their sprints; each
# writes its own file under bench/results/. Exit 77 from a script means
# skipped (no oracle), which does not fail the suite.
set -u
cd "$(dirname "$0")/.." || exit 1

rc=0
for s in smoke; do
    sh "bench/run-$s.sh"
    st=$?
    if [ "$st" -ne 0 ] && [ "$st" -ne 77 ]; then
        echo "bench: run-$s.sh failed ($st)" >&2
        rc=1
    fi
done
exit $rc
