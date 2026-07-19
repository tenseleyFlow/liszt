#!/bin/sh
# Tree-mode bench: 100k-node nested fixture (~1100 dirs x ~90 files,
# depth <= 6, seeded). Two machine-checkable assertions plus a table:
#   - fd ceiling: --tree completes under `ulimit -n 16` with identical
#     output (one dirfd live at any depth - the eza failure class)
#   - self-relative gate: --tree wall <= 1.15x `liszt -R -1` over the
#     same fixture (needs hyperfine; reported SKIP without it)
# eza --tree and tree(1) are recorded when present - report, not gate.
#
# Env: LISZT_BENCH_DIR (default build/bench), LISZT_BENCH_RUNS (default
# 10 hyperfine runs). Exit: 0 pass, 1 assertion failed.
set -u
cd "$(dirname "$0")/.." || exit 1

export LC_ALL=C

benchdir="${LISZT_BENCH_DIR:-build/bench}"
fixture="$benchdir/tree-100k"

if [ ! -d "$fixture" ]; then
    echo "bench/run-tree: building $fixture (100k nodes)" >&2
    mkdir -p "$fixture"
    awk 'BEGIN {
        s = 42
        ndir = 1100
        dirs[0] = ""
        for (i = 1; i < ndir; i++) {
            s = (s * 1103515245 + 12345) % 2147483648
            parent = dirs[s % i]
            d = parent
            depth = gsub(/\//, "/", d)
            if (parent != "") depth++
            if (depth >= 6) parent = ""
            name = sprintf("d%04d", i)
            dirs[i] = parent == "" ? name : parent "/" name
            print "D " dirs[i]
        }
        for (i = 0; i < ndir; i++) {
            pre = dirs[i] == "" ? "" : dirs[i] "/"
            for (j = 0; j < 90; j++)
                printf "F %sf%02d-%04d\n", pre, j, i
        }
    }' > "$fixture.plan"
    sed -n 's/^D //p' "$fixture.plan" \
        | (cd "$fixture" && xargs mkdir -p)
    sed -n 's/^F //p' "$fixture.plan" \
        | (cd "$fixture" && xargs touch)
    rm -f "$fixture.plan"
fi

nodes=$(find "$fixture" | wc -l)

stamp=$(date -u +%Y%m%d%H%M%S)
mkdir -p bench/results
out="bench/results/tree-$stamp.txt"
{
    echo "kind=tree"
    echo "host=$(hostname)"
    echo "os=$(uname -srm)"
    echo "liszt_version=$(./liszt --version 2>/dev/null | sed -n 1p)"
    echo "fixture=$fixture nodes=$nodes"
    echo
} > "$out"

fails=0

# --- fd ceiling assertion -------------------------------------------------
./liszt --tree "$fixture" > "$benchdir/tree.unlimited" 2>/dev/null
rc_unlim=$?
(
    ulimit -n 16 2>/dev/null || exit 77
    exec ./liszt --tree "$fixture" > "$benchdir/tree.fd16" 2>/dev/null
)
rc16=$?
if [ "$rc16" -eq 77 ]; then
    echo "fd_ceiling=SKIP (ulimit -n unsupported)" >> "$out"
elif [ "$rc_unlim" -eq 0 ] && [ "$rc16" -eq 0 ] \
    && cmp -s "$benchdir/tree.unlimited" "$benchdir/tree.fd16"; then
    echo "fd_ceiling=PASS (identical output under ulimit -n 16)" >> "$out"
else
    echo "fd_ceiling=FAIL (rc=$rc16 vs $rc_unlim)" >> "$out"
    fails=$((fails + 1))
fi
lines=$(wc -l < "$benchdir/tree.unlimited")
echo "tree_lines=$lines" >> "$out"
rm -f "$benchdir/tree.unlimited" "$benchdir/tree.fd16"

# --- timing table + self-relative gate ------------------------------------
have_eza=""
command -v eza >/dev/null 2>&1 && have_eza=1
have_tree=""
command -v tree >/dev/null 2>&1 && have_tree=1

if command -v hyperfine >/dev/null 2>&1; then
    set -- \
        "env LC_ALL=C ./liszt -R -1 $fixture" \
        "env LC_ALL=C ./liszt --tree $fixture" \
        "env LC_ALL=C ./liszt --tree -l $fixture"
    [ -n "$have_eza" ] && set -- "$@" "env LC_ALL=C eza -T $fixture"
    [ -n "$have_tree" ] && set -- "$@" "env LC_ALL=C tree $fixture"
    hyperfine --warmup 2 --runs "${LISZT_BENCH_RUNS:-10}" \
        --export-json "${out%.txt}.json" "$@" >> "$out" 2>&1
    # JSON results keep command order: [0]=-R -1, [1]=--tree.
    ratio=$(awk 'match($0, /"mean": [0-9.]+/) {
            v = substr($0, RSTART + 8, RLENGTH - 8) + 0
            n++
            if (n == 1) base = v
            if (n == 2) { printf "%.3f", v / base; exit }
        }' "${out%.txt}.json")
    if awk -v r="$ratio" 'BEGIN { exit !(r <= 1.15) }'; then
        echo "tree_gate=PASS (tree/R1 ratio $ratio <= 1.15)" >> "$out"
    else
        echo "tree_gate=FAIL (tree/R1 ratio $ratio > 1.15)" >> "$out"
        fails=$((fails + 1))
    fi
else
    echo "tree_gate=SKIP (no hyperfine)" >> "$out"
fi

sed -n '/fd_ceiling\|tree_gate\|tree_lines/p' "$out"
echo "bench/run-tree: results in $out"
[ "$fails" -eq 0 ]
