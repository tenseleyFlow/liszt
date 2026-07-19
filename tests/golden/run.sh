#!/bin/sh
# Golden parity harness vs pinned GNU ls (coreutils 9.11).
#
# Phase 1 (always): oracle self-test - the oracle vs itself over the smoke
# fixtures, proving harness + fixture determinism and 0/1/2 exit capture
# before any liszt diff is trusted.
# Phase 2 (gated): liszt vs oracle over the case matrix. Gated on
# tests/golden/PARITY_ACTIVE containing the active sprint number; each case
# is tagged with its owning sprint and runs only when tag <= active.
#
# Env pinning: every invocation runs under env -i with LC_ALL from the case,
# TZ=UTC0, COLUMNS=80, LS_COLORS empty (fixed non-empty schemes come with
# sprint 05 cases), stdout piped. tty-side defaults are exercised via
# explicit flags in the cases, never a real tty.
#
# Exit: 0 pass, 1 fail, 77 skip (root, or no oracle).
set -u
cd "$(dirname "$0")/../.." || exit 1

if [ "$(id -u)" -eq 0 ]; then
    echo "tests/golden: refusing to run as root (permission fixtures are meaningless)" >&2
    exit 77
fi

oracle=$(sh scripts/find-gnu-ls.sh) || {
    echo "tests/golden: no GNU coreutils ls oracle found; skipping" >&2
    echo "tests/golden: run scripts/build-gnu-ls.sh to build the pinned 9.11" >&2
    exit 77
}
oracle_version=$("$oracle" --version | sed -n '1s/.*coreutils) //p')
case "$oracle_version" in
9.11) ;;
*) echo "tests/golden: WARNING oracle is coreutils $oracle_version, pin is 9.11" >&2 ;;
esac

# Locale legs: C always; UTF-8 legs when present. Refuse to run without a
# real UTF-8 locale (a silent C-only run once hid a divergence - family
# lesson); LISZT_ALLOW_NO_UTF8=1 overrides for exotic boxes.
utf8_locale=""
for loc in C.UTF-8 C.utf8 en_US.UTF-8 en_US.utf8; do
    if locale -a 2>/dev/null | grep -qix "$loc"; then
        utf8_locale="$loc"
        break
    fi
done
if [ -z "$utf8_locale" ] && [ "${LISZT_ALLOW_NO_UTF8:-0}" != "1" ]; then
    echo "tests/golden: no UTF-8 locale available; refusing (set LISZT_ALLOW_NO_UTF8=1 to override)" >&2
    exit 1
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/liszt-golden.XXXXXX") || exit 1
cleanup() {
    chmod -R u+rwx "$work" 2>/dev/null
    rm -rf "$work"
}
trap cleanup EXIT INT TERM

# Pin the binaries: a concurrent rebuild must not swap the UUT mid-run.
cp ./liszt "$work/liszt.uut" 2>/dev/null || true

fails=0
cases=0

# Normalize the program-name token on stderr on BOTH sides. The only other
# sanctioned normalizations are named oracle-vintage substitutions, added
# case by case as they are discovered - never broad sed.
normprog() {
    sed -e 's/^[^:][^:]*:/PROG:/'
}

run_pinned() {
    lc="$1"
    shift
    env -i PATH="$PATH" LC_ALL="$lc" TZ=UTC0 COLUMNS=80 LS_COLORS= "$@"
}

# --- Phase 1: oracle self-test -------------------------------------------

mkdir -p "$work/smoke/dir/sub" "$work/smoke/dir/unreadable"
printf 'x' > "$work/smoke/dir/a"
printf 'y' > "$work/smoke/dir/.hidden"
printf 'z' > "$work/smoke/dir/sp ace"
chmod 000 "$work/smoke/dir/unreadable"

selftest() {
    desc="$1" lc="$2" wantrc="$3"
    shift 3
    cases=$((cases + 1))
    run_pinned "$lc" "$@" > "$work/o1.out" 2> "$work/o1.err"
    rc1=$?
    run_pinned "$lc" "$@" > "$work/o2.out" 2> "$work/o2.err"
    rc2=$?
    if [ "$rc1" -ne "$rc2" ] || ! cmp -s "$work/o1.out" "$work/o2.out" \
        || ! cmp -s "$work/o1.err" "$work/o2.err"; then
        echo "SELFTEST FAIL (nondeterministic): $desc" >&2
        fails=$((fails + 1))
        return
    fi
    if [ "$rc1" -ne "$wantrc" ]; then
        echo "SELFTEST FAIL (exit): $desc: want $wantrc got $rc1" >&2
        fails=$((fails + 1))
    fi
}

selftest "list dir rc0" C 0 "$oracle" -1 "$work/smoke/dir"
selftest "hidden -a rc0" C 0 "$oracle" -1a "$work/smoke/dir"
selftest "missing operand rc2" C 2 "$oracle" -1 "$work/smoke/nope"
selftest "unreadable subdir under -R rc1" C 1 "$oracle" -R "$work/smoke/dir"
if [ -n "$utf8_locale" ]; then
    selftest "list dir rc0 (utf8)" "$utf8_locale" 0 "$oracle" -1 "$work/smoke/dir"
fi

# Oracle over the full core fixture: same instance, twice, byte-identical.
sh tests/fixtures/generate.sh "$work/fix" 42 >/dev/null
selftest "core fixture -1aR" C 1 "$oracle" -1aR "$work/fix/core"
selftest "core fixture -laR" C 1 "$oracle" -laR "$work/fix/core"
if [ -n "$utf8_locale" ]; then
    selftest "core fixture -1aR (utf8)" "$utf8_locale" 1 "$oracle" -1aR "$work/fix/core"
fi

# Normalization self-test: any program token collapses to PROG:.
for tok in ls gls liszt lz; do
    got=$(printf '%s: cannot access\n' "$tok" | normprog)
    cases=$((cases + 1))
    if [ "$got" != "PROG: cannot access" ]; then
        echo "SELFTEST FAIL: normprog on token $tok -> [$got]" >&2
        fails=$((fails + 1))
    fi
done

# --- Phase 2: liszt vs oracle --------------------------------------------

active=-1
if [ -f tests/golden/PARITY_ACTIVE ]; then
    active=$(cat tests/golden/PARITY_ACTIVE)
fi

# run_case SPRINT name locale wantrc -- flags/operands...
# Compares stdout byte-exact, stderr after normprog, and exit codes between
# liszt.uut and the oracle. wantrc '-' skips the explicit rc assertion (the
# two tools must still agree).
run_case() {
    tag="$1" desc="$2" lc="$3" wantrc="$4"
    shift 4
    [ "$1" = "--" ] && shift
    [ "$tag" -le "$active" ] || return 0
    cases=$((cases + 1))
    run_pinned "$lc" "$work/liszt.uut" "$@" > "$work/u.out" 2> "$work/u.raw"
    urc=$?
    run_pinned "$lc" "$oracle" "$@" > "$work/o.out" 2> "$work/o.raw"
    orc=$?
    normprog < "$work/u.raw" > "$work/u.err"
    normprog < "$work/o.raw" > "$work/o.err"
    ok=1
    [ "$urc" -eq "$orc" ] || ok=0
    cmp -s "$work/u.out" "$work/o.out" || ok=0
    cmp -s "$work/u.err" "$work/o.err" || ok=0
    if [ "$wantrc" != "-" ] && [ "$urc" -ne "$wantrc" ]; then
        ok=0
    fi
    if [ "$ok" -ne 1 ]; then
        echo "CASE FAIL [$desc] (lc=$lc rc uut=$urc oracle=$orc)" >&2
        diff -u "$work/o.out" "$work/u.out" | sed -n '1,12p' >&2
        diff -u "$work/o.err" "$work/u.err" | sed -n '1,12p' >&2
        fails=$((fails + 1))
    fi
}

# Case matrix: populated from sprint 01 on. Tags are owning sprints.
# (No liszt-vs-oracle cases exist at sprint 00; the stubs list nothing.)

if [ "$fails" -gt 0 ]; then
    echo "tests/golden: $fails/$cases failed (oracle: $oracle, coreutils $oracle_version)" >&2
    exit 1
fi
echo "tests/golden: $cases checks passed (oracle: $oracle, coreutils $oracle_version, parity<=$active)"
