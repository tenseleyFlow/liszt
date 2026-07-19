#!/bin/sh
# Unit tests. Run from the repo root: sh tests/unit/run.sh
set -u
LC_ALL=C
export LC_ALL

fails=0
checks=0

note_fail() {
    echo "FAIL: $1" >&2
    fails=$((fails + 1))
}

check_eq() {
    desc="$1" want="$2" got="$3"
    checks=$((checks + 1))
    [ "$want" = "$got" ] || note_fail "$desc: want [$want] got [$got]"
}

check_status() {
    desc="$1" want="$2"
    shift 2
    checks=$((checks + 1))
    "$@" >/dev/null 2>&1
    got=$?
    [ "$want" -eq "$got" ] || note_fail "$desc: want exit $want got $got"
}

[ -x ./liszt ] || { echo "tests/unit: ./liszt missing; run make first" >&2; exit 1; }
[ -x ./lz ] || { echo "tests/unit: ./lz missing; run make first" >&2; exit 1; }

# Version and help exit 0 and carry the canonical name from either binary.
check_eq "liszt --version line" "liszt 0.0.1" "$(./liszt --version)"
check_eq "lz --version line" "liszt 0.0.1" "$(./lz --version)"
check_status "liszt --version exit" 0 ./liszt --version
check_status "liszt --help exit" 0 ./liszt --help
check_status "lz --help exit" 0 ./lz --help
./liszt --help | grep -q "lz" || note_fail "--help mentions lz"
checks=$((checks + 1))

# The version string in configure matches the binary.
conf_ver=$(sed -n 's/^LISZT_VERSION="\(.*\)"/\1/p' configure)
check_eq "configure/binary version agree" "liszt $conf_ver" "$(./liszt --version)"

# Skeleton: anything else is a serious error (exit 2) with a diagnostic.
check_status "bare invocation exits 2" 2 ./liszt
check_status "unknown option exits 2" 2 ./liszt --bogus
check_status "operand exits 2" 2 ./liszt /tmp
err=$(./liszt 2>&1 >/dev/null)
case "$err" in
liszt:*) : ;;
*) note_fail "diagnostic prefix: got [$err]" ;;
esac
checks=$((checks + 1))

# lz diagnostics carry the lz program name.
err=$(./lz 2>&1 >/dev/null)
case "$err" in
lz:*) : ;;
*) note_fail "lz diagnostic prefix: got [$err]" ;;
esac
checks=$((checks + 1))

# Makefile SRC list matches the files on disk (unwired sources fail loudly).
listed=$(sed -n '/^SRC =/,/^$/p' Makefile | grep -o 'src/[a-z_/]*\.c' | sort)
ondisk=$(ls src/*.c src/sys/*.c | sort)
check_eq "Makefile SRC matches src/*.c" "$ondisk" "$listed"

if [ "$fails" -gt 0 ]; then
    echo "tests/unit: $fails/$checks checks failed" >&2
    exit 1
fi
echo "tests/unit: $checks checks passed"
