#!/bin/sh
# Color/theme bench: 100k -l lanes. Gates:
#   - --color=full <= 1.25x --color=always (the metadata theme)
#   - --theme=dracula <= 1.10x --color=full (24-bit payloads only)
# eza -l --color is recorded when present (report, not gate).
#
# Env: LISZT_BENCH_RUNS (default 10), LISZT_PERF_ROOT (default /tmp).
# Exit: 0 pass, 1 gate failed, 77 no hyperfine.
set -u
cd "$(dirname "$0")/.." || exit 1

export LC_ALL=C

command -v hyperfine >/dev/null 2>&1 || {
    echo "bench/run-color: hyperfine required" >&2
    exit 77
}

root="${LISZT_PERF_ROOT:-/tmp}"
flat="$root/liszt-perf-100k"
[ -d "$flat" ] || sh bench/mkperf.sh "$flat" 100000 42

lsc=$( (dircolors -b 2>/dev/null || gdircolors -b 2>/dev/null) \
    | sed -n "s/^LS_COLORS='\(.*\)';\$/\1/p")

stamp=$(date -u +%Y%m%d%H%M%S)
mkdir -p bench/results
out="bench/results/color-$stamp.txt"
{
    echo "kind=color"
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
    "env LC_ALL=C LS_COLORS='$lsc' ./liszt --color=always -l $flat" \
    "env LC_ALL=C LS_COLORS='$lsc' ./liszt --color=full -l $flat" \
    "env LC_ALL=C LS_COLORS='$lsc' ./liszt --theme=dracula -l $flat"
[ -n "$have_eza" ] && set -- "$@" \
    "env LC_ALL=C eza -l --color=always $flat"
hyperfine --warmup 2 --runs "${LISZT_BENCH_RUNS:-10}" \
    --export-json "${out%.txt}.json" "$@" >> "$out" 2>&1

means=$(awk 'match($0, /"mean": [0-9.]+/) {
        printf "%s ", substr($0, RSTART + 8, RLENGTH - 8) }' \
    "${out%.txt}.json")
set -- $means
full_ratio=$(awk -v a="$1" -v b="$2" 'BEGIN { printf "%.3f", b / a }')
theme_ratio=$(awk -v a="$2" -v b="$3" 'BEGIN { printf "%.3f", b / a }')
if awk -v r="$full_ratio" 'BEGIN { exit !(r <= 1.25) }'; then
    echo "full_gate=PASS (full/always $full_ratio <= 1.25)" >> "$out"
else
    echo "full_gate=FAIL (full/always $full_ratio > 1.25)" >> "$out"
    fails=$((fails + 1))
fi
if awk -v r="$theme_ratio" 'BEGIN { exit !(r <= 1.10) }'; then
    echo "theme_gate=PASS (theme/full $theme_ratio <= 1.10)" >> "$out"
else
    echo "theme_gate=FAIL (theme/full $theme_ratio > 1.10)" >> "$out"
    fails=$((fails + 1))
fi
if [ -n "$have_eza" ] && [ -n "${4:-}" ]; then
    eza_x=$(awk -v m="$2" -v e="$4" 'BEGIN { printf "%.1f", e / m }')
    echo "full_vs_eza=${eza_x}x" >> "$out"
fi

grep -E "full_gate|theme_gate|full_vs_eza" "$out"
echo "bench/run-color: results in $out"
[ "$fails" -eq 0 ]
