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
oracle_is_pin=0
case "$oracle_version" in
9.11) oracle_is_pin=1 ;;
*) echo "tests/golden: WARNING oracle is coreutils $oracle_version, pin is 9.11;" \
        "pin-sensitive cases will be skipped (build the pin:" \
        "scripts/build-gnu-ls.sh)" >&2 ;;
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
# Dictionary-collation locale for the hard-locale sort legs (C.UTF-8 is
# byte-order; en_US exercises real collation tables).
dict_locale=""
for loc in en_US.UTF-8 en_US.utf8; do
    if locale -a 2>/dev/null | grep -qix "$loc"; then
        dict_locale="$loc"
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
    sed -e 's/^[^:][^:]*:/PROG:/' \
        -e "s|Try '[^']* --help'|Try 'PROG --help'|"
}

run_pinned() {
    lc="$1"
    shift
    env -i PATH="$PATH" LC_ALL="$lc" TZ=UTC0 COLUMNS=80 LS_COLORS= \
        LISZT_DEBUG_VERIFY=1 "$@"
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

cases=$((cases + 1))
if [ "$(printf "Try '/usr/sbin/ls --help' for more information.\n" | normprog)" \
    != "Try 'PROG --help' for more information." ]; then
    echo "SELFTEST FAIL: normprog Try line" >&2
    fails=$((fails + 1))
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

# run_case_pin911: like run_case, but only against the exact pinned
# oracle - for behaviors that changed between coreutils vintages (-f
# last-wins, the --sort word table).
run_case_pin911() {
    if [ "$oracle_is_pin" -eq 1 ]; then
        run_case "$@"
    fi
}

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

# Phase-2 fixtures beyond the generated core tree.
fix="$work/fix/core"
mkdir -p "$work/empty" "$work/dotonly"
printf 'x\n' > "$work/dotonly/.a"
printf 'x\n' > "$work/dotonly/.b"

# Case matrix. Tags are owning sprints. All cases pipe stdout and pin env
# via run_pinned; -U throughout until sprint 02 lands sorting.
U8="${utf8_locale:-C}"

# 01: single-directory enumeration and ignore modes.
run_case 1 "empty dir" C 0 -- -U1 "$work/empty"
run_case 1 "empty dir -a" C 0 -- -Ua1 "$work/empty"
run_case 1 "dotonly default" C 0 -- -U1 "$work/dotonly"
run_case 1 "dotonly -a" C 0 -- -Ua1 "$work/dotonly"
run_case 1 "dotonly -A" C 0 -- -UA1 "$work/dotonly"
run_case 1 "plain" C 0 -- -U1 "$fix/plain"
run_case 1 "plain -a" C 0 -- -Ua1 "$fix/plain"
run_case 1 "plain -A" C 0 -- -UA1 "$fix/plain"
run_case 1 "shapes -a" C 0 -- -Ua1 "$fix/shapes"
run_case 1 "shapes -a utf8" "$U8" 0 -- -Ua1 "$fix/shapes"
run_case 1 "links -a" C 0 -- -Ua1 "$fix/links"
run_case 1 "meta -a" C 0 -- -Ua1 "$fix/meta"
run_case 1 "sizes" C 0 -- -U1 "$fix/sizes"
run_case 1 "times" C 0 -- -U1 "$fix/times"
run_case 1 "perm -a" C 0 -- -Ua1 "$fix/perm"

# 01: operands, headers, ordering, diagnostics.
run_case 1 "two dirs" C 0 -- -U1 "$fix/links" "$fix/plain"
run_case 1 "file then dir" C 0 -- -U1 "$fix/times/old-a" "$fix/plain"
run_case 1 "dir then file" C 0 -- -U1 "$fix/plain" "$fix/times/old-a"
run_case 1 "two files" C 0 -- -U1 "$fix/times/old-a" "$fix/times/recent-a"
run_case 1 "missing operand" C 2 -- -U1 "$work/nope"
run_case 1 "missing plus dir" C 2 -- -U1 "$work/nope" "$fix/plain"
run_case 1 "empty operand" C 2 -- -U1 ""
run_case 1 "unreadable dir operand" C 2 -- -U1 "$fix/perm/no-access"
run_case 1 "unreadable among dirs" C 2 -- -U1 "$fix/perm/no-access" "$fix/plain"
run_case 1 "symlink-to-dir operand" C 0 -- -U1 "$fix/links/gooddir"
run_case 1 "symlink-to-file operand" C 0 -- -U1 "$fix/links/good"
run_case 1 "dangling symlink operand" C 0 -- -U1 "$fix/links/dangling"
run_case 1 "dashdash" C 0 -- -U1 -- "$fix/plain"
run_case 1 "permuted options" C 0 -- "$fix/plain" -U1
run_case 1 "format word equals -1" C 0 -- -U --format=single-column "$fix/plain"

# 02: sorted listings across the locale matrix; engines oracle-checked
# by the harness environment below (LISZT_DEBUG_VERIFY exported).
D8="${dict_locale:-C}"
run_case 2 "sorted plain" C 0 -- -1 "$fix/plain"
run_case 2 "sorted plain utf8" "$U8" 0 -- -1 "$fix/plain"
run_case 2 "sorted plain dict" "$D8" 0 -- -1 "$fix/plain"
run_case 2 "sorted shapes -a" C 0 -- -1a "$fix/shapes"
run_case 2 "sorted shapes -a utf8" "$U8" 0 -- -1a "$fix/shapes"
run_case 2 "sorted shapes -a dict" "$D8" 0 -- -1a "$fix/shapes"
run_case 2 "sorted reverse dict" "$D8" 0 -- -1r "$fix/shapes"
run_case 2 "sorted links -a" C 0 -- -1a "$fix/links"
run_case_pin911 2 "-f is -aU" C 0 -- -f1 "$fix/plain"
run_case_pin911 2 "-f last-wins with sort word" C 0 -- -f1 --sort=name "$fix/plain"
run_case 2 "version sort" C 0 -- -1v "$fix/versions"
run_case 2 "version sort -a" C 0 -- -1va "$fix/versions"
run_case 2 "version reverse" C 0 -- -1vr "$fix/versions"
run_case 2 "extension sort" C 0 -- -1X "$fix/plain"
run_case 2 "extension sort dict" "$D8" 0 -- -1X "$fix/versions"
run_case 2 "size sort" C 0 -- -1S "$fix/sizes"
run_case 2 "size sort links -a" C 0 -- -1Sa "$fix/links"
run_case 2 "size reverse" C 0 -- -1Sr "$fix/sizes"
run_case 2 "time sort" C 0 -- -1t "$fix/times"
run_case 2 "time reverse" C 0 -- -1tr "$fix/times"
run_case 2 "time sort dict" "$D8" 0 -- -1t "$fix/times"
run_case 2 "group dirs first" C 0 -- -1a --group-directories-first "$fix/links"
run_case 2 "group dirs -r" C 0 -- -1ar --group-directories-first "$fix/links"
run_case 2 "group with -U disabled" C 0 -- -1aU --group-directories-first "$fix/links"
run_case 2 "group with -S" C 0 -- -1aS --group-directories-first "$fix/links"
run_case 2 "sorted operands mix" C 2 -- -1 "$fix/times/recent-a" "$work/nope" "$fix/plain" "$fix/times/old-a"
run_case 2 "size-sorted operands" C 0 -- -1S "$fix/sizes/sz512" "$fix/sizes/sz1" "$fix/sizes/sz65536"
run_case_pin911 2 "sort word invalid" C 1 -- --sort=bogus
run_case_pin911 2 "sort word ambiguous" C 1 -- --sort=n
run_case 2 "sorted two dirs" C 0 -- -1 "$fix/links" "$fix/plain"

# 01: parser diagnostics (getopt-layer exit 2, argmatch-layer exit 1).
run_case 1 "unrecognized long" C 2 -- --bogus
run_case 1 "invalid short" C 2 -- -Y
run_case 1 "format missing arg" C 2 -- --format
run_case 1 "long noarg with value" C 2 -- --all=x
run_case 1 "ambiguous long" C 2 -- --f
run_case 1 "format invalid word" C 1 -- -U --format=bogus "$fix/plain"
run_case 1 "format ambiguous word" C 1 -- -U --format=v "$fix/plain"
run_case 1 "format invalid word utf8" "$U8" 1 -- -U --format=bogus "$fix/plain"

# 01: bespoke shapes run_case cannot express.
if [ 1 -le "$active" ]; then
    # Implicit "." (operandless), from inside a fixture dir.
    cases=$((cases + 1))
    (cd "$fix/plain" && run_pinned C "$work/liszt.uut" -U1) \
        > "$work/u.out" 2> "$work/u.raw"
    urc=$?
    (cd "$fix/plain" && run_pinned C "$oracle" -U1) \
        > "$work/o.out" 2> "$work/o.raw"
    orc=$?
    if [ "$urc" -ne "$orc" ] || ! cmp -s "$work/u.out" "$work/o.out"; then
        echo "CASE FAIL [implicit dot]" >&2
        fails=$((fails + 1))
    fi
    # POSIXLY_CORRECT stops option parsing at the first operand.
    cases=$((cases + 1))
    env -i PATH="$PATH" LC_ALL=C TZ=UTC0 COLUMNS=80 LS_COLORS= \
        POSIXLY_CORRECT=1 "$work/liszt.uut" -U1 "$fix/plain" -a \
        > "$work/u.out" 2>/dev/null
    urc=$?
    env -i PATH="$PATH" LC_ALL=C TZ=UTC0 COLUMNS=80 LS_COLORS= \
        POSIXLY_CORRECT=1 "$oracle" -U1 "$fix/plain" -a \
        > "$work/o.out" 2>/dev/null
    orc=$?
    if [ "$urc" -ne "$orc" ] || ! cmp -s "$work/u.out" "$work/o.out"; then
        echo "CASE FAIL [posixly-correct stop]" >&2
        fails=$((fails + 1))
    fi
fi

if [ "$fails" -gt 0 ]; then
    echo "tests/golden: $fails/$cases failed (oracle: $oracle, coreutils $oracle_version)" >&2
    exit 1
fi
echo "tests/golden: $cases checks passed (oracle: $oracle, coreutils $oracle_version, parity<=$active)"
