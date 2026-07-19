#!/bin/sh
# Git-status bench: a 200k-file one-commit repo (flat root - the
# harshest per-entry window) plus a v4-index variant. Assertions:
#   - gate: `liszt -l --git` wall <= 2x `liszt -l` on the same repo
#   - syscall ceiling: --git adds a CONSTANT number of syscalls
#     (repo discovery + index mmap), never per-entry work
# eza -l --git is recorded when present - report, not gate (eza #1710
# measured it 550x slower than ls on big repos).
#
# Env: LISZT_BENCH_DIR (default build/bench), LISZT_BENCH_RUNS
# (default 10). Exit: 0 pass, 1 assertion failed, 77 no git.
set -u
cd "$(dirname "$0")/.." || exit 1

export LC_ALL=C

command -v git >/dev/null 2>&1 || {
    echo "bench/run-git: no git binary; skipping" >&2
    exit 77
}

benchdir="${LISZT_BENCH_DIR:-build/bench}"
repo="$benchdir/git-200k"
grepo4="$benchdir/git-200k-v4"

G="git -c user.email=b@liszt.test -c user.name=bench"

if [ ! -d "$repo/.git" ]; then
    echo "bench/run-git: building $repo (200k files, one commit)" >&2
    rm -rf "$repo"
    mkdir -p "$repo"
    awk 'BEGIN {
        s = 42
        for (i = 0; i < 200000; i++) {
            s = (s * 1103515245 + 12345) % 2147483648
            printf "f%06d-%04d\n", i, s % 10000
        }
    }' | (cd "$repo" && xargs touch)
    (cd "$repo" && $G init -q && $G add -A && $G commit -q -m base)
fi
if [ ! -d "$grepo4/.git" ]; then
    echo "bench/run-git: building v4 variant" >&2
    rm -rf "$grepo4"
    cp -R "$repo" "$grepo4"
    (cd "$grepo4" && $G update-index --index-version 4)
fi

stamp=$(date -u +%Y%m%d%H%M%S)
mkdir -p bench/results
out="bench/results/git-$stamp.txt"
{
    echo "kind=git"
    echo "host=$(hostname)"
    echo "os=$(uname -srm)"
    echo "liszt_version=$(./liszt --version 2>/dev/null | sed -n 1p)"
    echo "fixture=$repo entries=200000"
    echo
} > "$out"

fails=0

# --- syscall ceiling ------------------------------------------------------
if command -v strace >/dev/null 2>&1; then
    nplain=$(strace -f -c -U calls -- ./liszt -l "$repo" 2>&1 >/dev/null \
        | tail -1 | awk '{ print $1 }')
    ngit=$(strace -f -c -U calls -- ./liszt -l --git "$repo" \
        2>&1 >/dev/null | tail -1 | awk '{ print $1 }')
    diff=$((ngit - nplain))
    echo "syscalls_plain=$nplain syscalls_git=$ngit diff=$diff" >> "$out"
    if [ "$diff" -le 32 ] && [ "$diff" -ge 0 ]; then
        echo "git_syscalls=PASS (+$diff per repo, +0 per entry)" >> "$out"
    else
        echo "git_syscalls=FAIL (+$diff)" >> "$out"
        fails=$((fails + 1))
    fi
else
    echo "git_syscalls=SKIP (no strace)" >> "$out"
fi

# --- timing + gate --------------------------------------------------------
have_eza=""
command -v eza >/dev/null 2>&1 && have_eza=1

if command -v hyperfine >/dev/null 2>&1; then
    set -- \
        "env LC_ALL=C ./liszt -l $repo" \
        "env LC_ALL=C ./liszt -l --git $repo" \
        "env LC_ALL=C ./liszt -l --git $grepo4" \
        "env LC_ALL=C ./liszt --git-ignore -1 $repo"
    [ -n "$have_eza" ] && set -- "$@" "env LC_ALL=C eza -l --git $repo"
    hyperfine --warmup 2 --runs "${LISZT_BENCH_RUNS:-10}" \
        --export-json "${out%.txt}.json" "$@" >> "$out" 2>&1
    ratio=$(awk 'match($0, /"mean": [0-9.]+/) {
            v = substr($0, RSTART + 8, RLENGTH - 8) + 0
            n++
            if (n == 1) base = v
            if (n == 2) { printf "%.3f", v / base; exit }
        }' "${out%.txt}.json")
    if awk -v r="$ratio" 'BEGIN { exit !(r <= 2.0) }'; then
        echo "git_gate=PASS (git/plain ratio $ratio <= 2.0)" >> "$out"
    else
        echo "git_gate=FAIL (git/plain ratio $ratio > 2.0)" >> "$out"
        fails=$((fails + 1))
    fi
else
    echo "git_gate=SKIP (no hyperfine)" >> "$out"
fi

sed -n '/git_syscalls\|git_gate\|syscalls_/p' "$out"
echo "bench/run-git: results in $out"
[ "$fails" -eq 0 ]
