#!/bin/sh
# Deterministic flat perf fixture: N seeded empty files in DEST.
# Usage: mkperf.sh DEST N [SEED]
set -eu

dest="$1"
n="$2"
seed="${3:-42}"

rm -rf "$dest"
mkdir -p "$dest"

awk -v n="$n" -v seed="$seed" 'BEGIN {
    s = seed
    for (i = 0; i < n; i++) {
        s = (s * 1103515245 + 12345) % 2147483648
        len = 4 + s % 8
        t = s
        nm = ""
        for (j = 0; j < len; j++) {
            t = (t * 1103515245 + 12345) % 2147483648
            nm = nm substr("abcdefghijklmnopqrstuvwxyz0123456789", t % 36 + 1, 1)
        }
        printf "f%07d-%s\n", i, nm
    }
}' | (cd "$dest" && xargs touch)
