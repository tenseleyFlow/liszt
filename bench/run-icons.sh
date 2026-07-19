#!/bin/sh
# Icons bench: 100k flat fixture. Assertions:
#   - gate: --icons=always --color=always -1 <= 1.10x the icons-off
#     run (the real-usage lane; the bare lane is recorded)
#   - target: >= 3x faster than eza --icons --color (recorded when
#     eza is present; report, not gate)
#
# Env: LISZT_BENCH_RUNS (default 10), LISZT_PERF_ROOT (default /tmp).
# Exit: 0 pass, 1 gate failed, 77 no hyperfine.
set -u
cd "$(dirname "$0")/.." || exit 1

export LC_ALL=C

command -v hyperfine >/dev/null 2>&1 || {
    echo "bench/run-icons: hyperfine required" >&2
    exit 77
}

root="${LISZT_PERF_ROOT:-/tmp}"
flat="$root/liszt-perf-100k"
[ -d "$flat" ] || sh bench/mkperf.sh "$flat" 100000 42

lsc=$(dircolors -b 2>/dev/null | sed -n "s/^LS_COLORS='\(.*\)';\$/\1/p")

stamp=$(date -u +%Y%m%d%H%M%S)
mkdir -p bench/results
out="bench/results/icons-$stamp.txt"
{
    echo "kind=icons"
    echo "host=$(hostname)"
    echo "os=$(uname -srm)"
    echo "liszt_version=$(./liszt --version 2>/dev/null | sed -n 1p)"
    echo "fixture=$flat entries=100000"
    echo
} > "$out"

fails=0
have_eza=""
command -v eza >/dev/null 2>&1 && have_eza=1

set -- \
    "env LC_ALL=C LS_COLORS='$lsc' ./liszt --color=always -1 $flat" \
    "env LC_ALL=C LS_COLORS='$lsc' ./liszt --color=always --icons=always -1 $flat" \
    "env LC_ALL=C ./liszt -1 $flat" \
    "env LC_ALL=C ./liszt --icons=always -1 $flat"
[ -n "$have_eza" ] && set -- "$@" \
    "env LC_ALL=C eza --color=always --icons=always -1 $flat"
hyperfine --warmup 2 --runs "${LISZT_BENCH_RUNS:-10}" \
    --export-json "${out%.txt}.json" "$@" >> "$out" 2>&1

ratio=$(awk 'match($0, /"mean": [0-9.]+/) {
        v = substr($0, RSTART + 8, RLENGTH - 8) + 0
        n++
        if (n == 1) base = v
        if (n == 2) { printf "%.3f", v / base; exit }
    }' "${out%.txt}.json")
if awk -v r="$ratio" 'BEGIN { exit !(r <= 1.10) }'; then
    echo "icons_gate=PASS (color-lane ratio $ratio <= 1.10)" >> "$out"
else
    echo "icons_gate=FAIL (color-lane ratio $ratio > 1.10)" >> "$out"
    fails=$((fails + 1))
fi
if [ -n "$have_eza" ]; then
    eza_x=$(awk 'match($0, /"mean": [0-9.]+/) {
            v = substr($0, RSTART + 8, RLENGTH - 8) + 0
            n++
            if (n == 2) mine = v
            if (n == 5) { printf "%.1f", v / mine; exit }
        }' "${out%.txt}.json")
    echo "icons_vs_eza=${eza_x}x" >> "$out"
fi

grep -E "icons_gate|icons_vs_eza" "$out"
echo "bench/run-icons: results in $out"
[ "$fails" -eq 0 ]
