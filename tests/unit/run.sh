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

# Sorted listings landed in sprint 02: bare invocations work.
check_status "bare invocation lists cwd" 0 ./liszt
check_status "default-sort operand works" 0 ./liszt /tmp
check_status "unknown option exits 2" 2 ./liszt --bogus
check_status "argmatch error exits 1 (GNU quirk)" 1 ./liszt --format=bogus
check_status "unsupported sort word exits 2" 2 ./liszt --sort=width

# Functional sprint-01 surface.
udir=$(mktemp -d "${TMPDIR:-/tmp}/liszt-unit.XXXXXX")
trap 'rm -rf "$udir"' EXIT INT TERM
printf 'x\n' > "$udir/bb"
printf 'x\n' > "$udir/aa"
printf 'x\n' > "$udir/.dot"
check_status "-U1 lists a directory" 0 ./liszt -U1 "$udir"
check_status "permuted options accepted" 0 ./liszt "$udir" -U1
# $(( )) normalizes BSD wc's space-padded output.
n_default=$(( $(./liszt -U1 "$udir" | wc -l) ))
n_all=$(( $(./liszt -Ua1 "$udir" | wc -l) ))
n_almost=$(( $(./liszt -UA1 "$udir" | wc -l) ))
check_eq "-a adds . .. and dotfiles" "$((n_default + 3))" "$n_all"
check_eq "-A adds dotfiles only" "$((n_default + 1))" "$n_almost"
fmt_out=$(./liszt -U --format=single-column "$udir")
one_out=$(./liszt -U1 "$udir")
check_eq "--format=single-column equals -1" "$one_out" "$fmt_out"
err=$(./liszt /liszt-no-such 2>&1 >/dev/null)
case "$err" in
liszt:*) : ;;
*) note_fail "diagnostic prefix: got [$err]" ;;
esac
checks=$((checks + 1))

# lz diagnostics carry the lz program name.
err=$(./lz /liszt-no-such 2>&1 >/dev/null)
case "$err" in
lz:*) : ;;
*) note_fail "lz diagnostic prefix: got [$err]" ;;
esac
checks=$((checks + 1))

# Plan selection and debug surface.
p=$(LC_ALL=C LISZT_DEBUG_PLAN=1 ./liszt "$udir" 2>&1 >/dev/null)
case "$p" in *radix-bytes*) : ;; *) note_fail "C plan: [$p]" ;; esac
checks=$((checks + 1))
p=$(LISZT_FORCE_SCALAR=1 LISZT_DEBUG_PLAN=1 ./liszt "$udir" 2>&1 >/dev/null)
case "$p" in *scalar*) : ;; *) note_fail "forced scalar plan: [$p]" ;; esac
checks=$((checks + 1))
check_status "verify oracle passes" 0 env LISZT_DEBUG_VERIFY=1 ./liszt "$udir"

# Fixture generator is deterministic: same seed, same manifest.
fixwork=$(mktemp -d "${TMPDIR:-/tmp}/liszt-fix.XXXXXX")
trap 'chmod -R u+rwx "$fixwork" 2>/dev/null; rm -rf "$fixwork"' EXIT INT TERM
h1=$(sh tests/fixtures/generate.sh "$fixwork/a" 42 | sed -n 's/^MANIFEST_SHA256 //p')
h2=$(sh tests/fixtures/generate.sh "$fixwork/b" 42 | sed -n 's/^MANIFEST_SHA256 //p')
h3=$(sh tests/fixtures/generate.sh "$fixwork/a" 7 | sed -n 's/^MANIFEST_SHA256 //p')
check_eq "fixture generator deterministic per seed" "$h1" "$h2"
checks=$((checks + 1))
if [ -z "$h1" ] || [ "$h1" = "$h3" ]; then
    note_fail "fixture generator seed variation (seed42=$h1 seed7=$h3)"
fi

# Makefile SRC list matches the files on disk (unwired sources fail loudly).
listed=$(sed -n '/^SRC =/,/^$/p' Makefile | grep -o 'src/[a-z_/]*\.c' | sort)
ondisk=$(ls src/*.c src/sys/*.c | sort)
check_eq "Makefile SRC matches src/*.c" "$ondisk" "$listed"

if [ "$fails" -gt 0 ]; then
    echo "tests/unit: $fails/$checks checks failed" >&2
    exit 1
fi
echo "tests/unit: $checks checks passed"
