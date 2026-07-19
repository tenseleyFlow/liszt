#!/bin/sh
# Syscall budget assertions (strace -c): the stat economy is a parity
# surface of its own. Budgets are per-entry shapes, not absolute counts,
# so fixture size changes do not rot them.
#
#   names lane (-U -1):        zero per-entry stats (getdents only)
#   -l lane:                   exactly 1 statx + 1 llistxattr per entry
#   statless color scheme:     exactly 1 statx total (the operand)
#
# Linux-only (strace); exit 77 elsewhere. Exit 1 on a blown budget.
set -u
cd "$(dirname "$0")/.." || exit 1

command -v strace >/dev/null 2>&1 || {
    echo "bench/syscalls: no strace; skipping" >&2
    exit 77
}
[ "$(uname -s)" = "Linux" ] || exit 77

work=$(mktemp -d "${TMPDIR:-/tmp}/liszt-sys.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT INT TERM

n=500
sh bench/mkperf.sh "$work/flat" "$n" 42 >/dev/null

count() {    # count SYSCALL invocations of "$@"
    sc="$1"; shift
    strace -f -c -U calls -e "trace=$sc" -o "$work/c.txt" "$@" \
        > /dev/null 2> /dev/null
    # strace -c summary: the calls column of the syscall row; absent = 0.
    awk -v s="$sc" '$NF == s { print $1; found = 1 }
                    END { if (!found) print 0 }' "$work/c.txt" | tail -1
}

fails=0
budget() {   # budget NAME ACTUAL OP LIMIT
    name="$1"; actual="$2"; op="$3"; limit="$4"
    if [ "$actual" "$op" "$limit" ]; then
        echo "bench/syscalls: OK   $name ($actual)"
    else
        echo "bench/syscalls: BLOWN $name: $actual ! $op $limit" >&2
        fails=1
    fi
}

# Names lane: no per-entry metadata at all. A handful of fixed statx
# calls are allowed (operand classification, locale files are not statx).
s=$(count statx env -i LC_ALL=C ./liszt -U1 "$work/flat")
budget "names statx fixed" "$s" -le 3
g=$(count getdents64 env -i LC_ALL=C ./liszt -U1 "$work/flat")
budget "names getdents present" "$g" -ge 1

# -l lane: 1 statx per entry + operand overhead; 1 llistxattr per entry.
s=$(count statx env -i LC_ALL=C TZ=UTC0 ./liszt -l "$work/flat")
budget "-l statx per entry" "$s" -le $((n + 5))
budget "-l statx not short" "$s" -ge "$n"
x=$(count llistxattr env -i LC_ALL=C TZ=UTC0 ./liszt -l "$work/flat")
budget "-l llistxattr per entry" "$x" -le $((n + 5))

# Statless color scheme: classification comes from d_type alone. The
# builtin defaults must be zeroed explicitly - an unmentioned ex= keeps
# its default and forces per-entry mode stats (GNU behaves the same).
s=$(count statx env -i LC_ALL=C \
    LS_COLORS="di=01;34:ln=01;36:ex=00:su=00:sg=00:ow=00:st=00:tw=00:or=00:mi=00:ca=00" \
    ./liszt --color=always "$work/flat")
budget "statless color statx" "$s" -le 1

exit $fails
