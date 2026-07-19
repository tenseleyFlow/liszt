#!/bin/sh
# The overview §9 workload matrix: liszt vs the GNU oracle over every
# required warm-cache lane. Writes bench/results/matrix-<stamp>.txt with
# machine, locale availability, and fixture hashes. Fixtures live on
# tmpfs when LISZT_PERF_ROOT points there (default /tmp/liszt-perf-*,
# built by this script when absent).
#
# Gate policy (tally's VM-runner precedent): this script REPORTS; the
# hard gate is a human reading the table on real hardware. Exit 77 = no
# oracle/hyperfine.
set -u
cd "$(dirname "$0")/.." || exit 1

oracle=$(sh scripts/find-gnu-ls.sh) || exit 77
command -v hyperfine >/dev/null 2>&1 || {
    echo "bench/matrix: hyperfine required" >&2
    exit 77
}

utf8=""
for loc in en_US.UTF-8 en_US.utf8 C.UTF-8 C.utf8; do
    locale -a 2>/dev/null | grep -qix "$loc" && { utf8="$loc"; break; }
done

root="${LISZT_PERF_ROOT:-/tmp}"
flat="$root/liszt-perf-100k"
deep="$root/liszt-perf-deep"
tiny="$root/liszt-perf-50"

[ -d "$flat" ] || sh bench/mkperf.sh "$flat" 100000 42
[ -d "$tiny" ] || sh bench/mkperf.sh "$tiny" 50 7
if [ ! -d "$deep" ]; then
    mkdir -p "$deep"
    p="$deep"; i=0
    while [ "$i" -lt 60 ]; do
        p="$p/d$i"; mkdir "$p"
        for f in 1 2 3 4 5; do : > "$p/f$i-$f"; done
        i=$((i + 1))
    done
fi

out="bench/results/matrix-$(date +%Y%m%d%H%M%S).txt"
mkdir -p bench/results
{
    echo "machine: $(uname -srm) $(uname -n)"
    echo "oracle: $oracle ($("$oracle" --version | sed -n 1p))"
    echo "utf8: ${utf8:-none}"
    echo "flat entries: $(ls "$flat" | wc -l)"
} > "$out"

runs="${LISZT_BENCH_RUNS:-10}"
lsc=$(dircolors -b 2>/dev/null | sed -n "s/^LS_COLORS='\(.*\)';\$/\1/p")

row() {
    name="$1"; largs="$2"; genv="$3"
    echo "== $name" >> "$out"
    hyperfine -N --warmup 3 --runs "$runs" \
        "env $genv $oracle $largs" "env $genv ./liszt $largs" 2>&1 \
        | grep -E "Time|faster" >> "$out"
}

echo "== workload matrix (each pair: oracle then liszt)" >> "$out"
row "flat C"        "$flat"                    "LC_ALL=C"
[ -n "$utf8" ] && \
row "flat UTF-8"    "$flat"                    "LC_ALL=$utf8"
row "flat -l"       "-l $flat"                 "LC_ALL=C"
row "flat color -F" "--color=always -F $flat"  "LC_ALL=C LS_COLORS='$lsc'"
row "deep -R"       "-R $deep"                 "LC_ALL=C"
row "flat -S"       "-S $flat"                 "LC_ALL=C"
row "flat -t"       "-t $flat"                 "LC_ALL=C"
row "flat -U"       "-U $flat"                 "LC_ALL=C"
row "tiny 50"       "$tiny"                    "LC_ALL=C"
row "dired -l"      "-D $flat"                 "LC_ALL=C"

echo "bench: wrote $out"
