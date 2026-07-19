#!/bin/sh
# Perf smoke: baseline the GNU oracle (enumeration floor, C sort, UTF-8
# collation, -l) plus a liszt startup row, writing a result file under
# bench/results/. Fast by default (5k entries); the dev-box reference
# baseline runs with LISZT_BENCH_N=100000 LISZT_BENCH_DIR=<tmpfs>.
#
# All rows pin their locale explicitly - the twice-burned family rule.
# Exit: 0 recorded, 77 skipped (no oracle).
set -u
cd "$(dirname "$0")/.." || exit 1

export LC_ALL=C

oracle=$(sh scripts/find-gnu-ls.sh) || {
    echo "bench: no GNU ls oracle; skipping" >&2
    exit 77
}
oracle_version=$("$oracle" --version | sed -n '1s/.*coreutils) //p')

utf8_locale=""
for loc in en_US.UTF-8 en_US.utf8 C.UTF-8 C.utf8; do
    if locale -a 2>/dev/null | grep -qix "$loc"; then
        utf8_locale="$loc"
        break
    fi
done

n="${LISZT_BENCH_N:-5000}"
benchdir="${LISZT_BENCH_DIR:-build/bench}"
fixture="$benchdir/flat-$n"
if [ ! -d "$fixture" ]; then
    sh bench/mkperf.sh "$fixture" "$n" 42
fi
if command -v sha256sum >/dev/null 2>&1; then
    hash_stdin() { sha256sum | cut -d' ' -f1; }
elif command -v shasum >/dev/null 2>&1; then
    hash_stdin() { shasum -a 256 | cut -d' ' -f1; }
else
    hash_stdin() { sha256 -q; }
fi
fixture_hash=$(cd "$fixture" && ls -A | LC_ALL=C sort | hash_stdin)

stamp=$(date -u +%Y%m%d%H%M%S)
mkdir -p bench/results
out="bench/results/smoke-$stamp.txt"

{
    echo "kind=smoke"
    echo "host=$(hostname)"
    echo "os=$(uname -srm)"
    echo "cc=$( (${CC:-cc} --version 2>/dev/null || echo unknown) | sed -n 1p)"
    echo "liszt_version=$(./liszt --version 2>/dev/null | sed -n 1p || echo unbuilt)"
    echo "oracle=$oracle"
    echo "oracle_version=$oracle_version"
    echo "fixture=$fixture"
    echo "entries=$n"
    echo "fixture_hash=$fixture_hash"
    echo "utf8_locale=${utf8_locale:-none}"
    echo
} > "$out"

# Rows. Each command pins env explicitly; stdout is discarded (piped, not
# a tty) by the runner.
row_names="oracle_f_C oracle_default_C"
set -- \
    "env LC_ALL=C $oracle -f $fixture" \
    "env LC_ALL=C $oracle $fixture"
if [ -n "$utf8_locale" ]; then
    row_names="$row_names oracle_default_utf8 oracle_l_utf8"
    set -- "$@" \
        "env LC_ALL=$utf8_locale $oracle $fixture" \
        "env LC_ALL=$utf8_locale $oracle -l $fixture"
fi
# Enumeration floor: liszt's first functional lane (sprint 01).
row_names="$row_names oracle_u1_C liszt_u1_C"
set -- "$@" \
    "env LC_ALL=C $oracle -U -1 $fixture" \
    "env LC_ALL=C ./liszt -U -1 $fixture"
# Sorted lanes (sprint 02).
row_names="$row_names liszt_default_C liszt_S_C liszt_t_C"
set -- "$@" \
    "env LC_ALL=C ./liszt $fixture" \
    "env LC_ALL=C ./liszt -S $fixture" \
    "env LC_ALL=C ./liszt -t $fixture"
if [ -n "$utf8_locale" ]; then
    row_names="$row_names liszt_default_utf8"
    set -- "$@" "env LC_ALL=$utf8_locale ./liszt $fixture"
fi
# The -l lane (sprint 03).
row_names="$row_names oracle_l_C liszt_l_C"
set -- "$@" \
    "env LC_ALL=C $oracle -l $fixture" \
    "env LC_ALL=C ./liszt -l $fixture"
# The decoration lane (sprint 05), default dircolors scheme.
lsc=$(dircolors -b 2>/dev/null | sed -n "s/^LS_COLORS='\(.*\)';\$/\1/p")
if [ -n "$lsc" ]; then
    row_names="$row_names oracle_colorF_C liszt_colorF_C"
    # The scheme value carries ';' and '*': it must reach hyperfine's
    # shell single-quoted or the command splits at the first semicolon.
    set -- "$@" \
        "env LC_ALL=C LS_COLORS='$lsc' $oracle --color=always -F $fixture" \
        "env LC_ALL=C LS_COLORS='$lsc' ./liszt --color=always -F $fixture"
fi
row_names="$row_names liszt_startup"
set -- "$@" "./liszt --version"

if command -v hyperfine >/dev/null 2>&1; then
    hyperfine --warmup 2 --runs "${LISZT_BENCH_RUNS:-5}" \
        --export-json "${out%.txt}.json" "$@" >> "$out" 2>&1
else
    # Fallback: three timed runs per row, min kept. BSD date lacks %N, so
    # granularity degrades to whole seconds there.
    case "$(date +%N)" in
    *N*) ns=0 ;;
    *) ns=1 ;;
    esac
    for cmd in "$@"; do
        best=""
        i=0
        while [ "$i" -lt 3 ]; do
            if [ "$ns" -eq 1 ]; then
                t0=$(date +%s%N)
            else
                t0=$(($(date +%s) * 1000000000))
            fi
            sh -c "$cmd" > /dev/null 2>&1
            if [ "$ns" -eq 1 ]; then
                t1=$(date +%s%N)
            else
                t1=$(($(date +%s) * 1000000000))
            fi
            dt=$(((t1 - t0) / 1000000))
            if [ -z "$best" ] || [ "$dt" -lt "$best" ]; then
                best=$dt
            fi
            i=$((i + 1))
        done
        echo "cmd=[$cmd] min_ms=$best" >> "$out"
    done
fi

echo "bench: wrote $out"
