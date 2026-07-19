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

# run_case_tables: cases whose bytes depend on Unicode printability and
# width classification. GNU bundles gnulib's own tables; liszt reads the
# platform libc's, which agree on Linux/FreeBSD but not Darwin. Sprint 07
# ports the gnulib tables and lifts this guard (tracked deviation).
tables_ok=1
[ "$(uname -s)" = "Darwin" ] && tables_ok=0
run_case_tables() {
    if [ "$tables_ok" -eq 1 ]; then
        run_case_pin911 "$@"
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

# Deviation fixture: names whose widths GNU rejects (control byte,
# invalid multibyte) plus plain ASCII - deliberately free of exotic
# code points so expected bytes are identical across platform libcs.
mkdir -p "$work/devdir"
for n in aa bb cc dd ee ff; do printf 'x\n' > "$work/devdir/$n"; done
printf 'x\n' > "$work/devdir/$(printf 'ctl\007x')"
{ printf 'x\n' > "$work/devdir/$(printf 'inv\200x')"; } 2>/dev/null || true

# run_case_dev SLUG SPRINT desc locale wantrc -- args...
# Deviation registry (.docs/deviations.md): liszt must match the pinned
# expected bytes, the oracle must still DIFFER (an upstream fix fails
# loudly), and exit codes must agree.
run_case_dev() {
    slug="$1" tag="$2" desc="$3" lc="$4" wantrc="$5"
    shift 5
    [ "$1" = "--" ] && shift
    [ "$tag" -le "$active" ] || return 0
    [ "$tables_ok" -eq 1 ] || return 0
    cases=$((cases + 1))
    exp="tests/golden/deviations/$slug.out"
    run_pinned "$lc" "$work/liszt.uut" "$@" > "$work/u.out" 2>/dev/null
    urc=$?
    run_pinned "$lc" "$oracle" "$@" > "$work/o.out" 2>/dev/null
    orc=$?
    ok=1
    [ "$urc" -eq "$orc" ] || ok=0
    if [ "$wantrc" != "-" ] && [ "$urc" -ne "$wantrc" ]; then
        ok=0
    fi
    if [ ! -f "$exp" ]; then
        echo "DEV CASE MISSING EXPECTED [$desc]: $exp" >&2
        echo "  (generate with the current binary after review)" >&2
        fails=$((fails + 1))
        return
    fi
    cmp -s "$work/u.out" "$exp" || ok=0
    if cmp -s "$work/u.out" "$work/o.out"; then
        echo "DEVIATION VANISHED [$desc]: oracle now matches liszt;" \
            "re-evaluate .docs/deviations.md" >&2
        fails=$((fails + 1))
        return
    fi
    if [ "$ok" -ne 1 ]; then
        echo "DEV CASE FAIL [$desc] (rc uut=$urc oracle=$orc)" >&2
        diff -u "$exp" "$work/u.out" | sed -n '1,10p' >&2
        fails=$((fails + 1))
    fi
}

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
# The dirs-first split can leave a group range empty: an empty dir with
# -a is dot-dirs only (no files range), a dotless all-file listing has
# no dirs range. Both radix engines must tolerate the empty range.
run_case 2 "group empty dir -a" C 0 -- -1a --group-directories-first "$work/empty"
run_case 2 "group empty dir -a utf8" "$U8" 0 -- -1a --group-directories-first "$work/empty"
run_case 2 "group all-files no dirs" C 0 -- -1 --group-directories-first "$fix/plain"
run_case 2 "group empty dir -aS" C 0 -- -1aS --group-directories-first "$work/empty"
run_case 2 "sorted operands mix" C 2 -- -1 "$fix/times/recent-a" "$work/nope" "$fix/plain" "$fix/times/old-a"
run_case 2 "size-sorted operands" C 0 -- -1S "$fix/sizes/sz512" "$fix/sizes/sz1" "$fix/sizes/sz65536"
run_case_pin911 2 "sort word invalid" C 1 -- --sort=bogus
run_case_pin911 2 "sort word ambiguous" C 1 -- --sort=n
run_case 2 "sorted two dirs" C 0 -- -1 "$fix/links" "$fix/plain"

# 03: the -l lane. Both tools run as the same user on the same tree, so
# owner/group names need no normalization; device goldens stay out (the
# privileged fixture tier owns them - /dev mtimes flake).
run_case 3 "-l times" C 0 -- -l "$fix/times"
run_case 3 "-l times utf8" "$U8" 0 -- -l "$fix/times"
run_case 3 "-l times dict" "$D8" 0 -- -l "$fix/times"
run_case 3 "-l sizes" C 0 -- -l "$fix/sizes"
run_case 3 "-l links -a" C 0 -- -la "$fix/links"
run_case 3 "-l meta -a (acl suffix)" C 0 -- -la "$fix/meta"
run_case 3 "-l shapes -a" C 0 -- -la "$fix/shapes"
run_case 3 "-ln numeric" C 0 -- -ln "$fix/plain"
run_case 3 "-lg no owner" C 0 -- -lg "$fix/times"
run_case 3 "-lo no group" C 0 -- -lo "$fix/times"
run_case 3 "-lG no group" C 0 -- -lG "$fix/times"
run_case 3 "-l --author" C 0 -- -l --author "$fix/times"
run_case 3 "-lh human" C 0 -- -lh "$fix/sizes"
run_case 3 "-l --si" C 0 -- -l --si "$fix/sizes"
run_case 3 "-lk kibibytes" C 0 -- -lk "$fix/sizes"
run_case 3 "block-size 1M" C 0 -- -l --block-size=1M "$fix/sizes"
run_case 3 "block-size KiB" C 0 -- -l --block-size=KiB "$fix/sizes"
run_case 3 "block-size grouped" C 0 -- -l "--block-size='1" "$fix/sizes"
run_case 3 "block-size invalid" C 2 -- -l --block-size=bogus "$fix/sizes"
run_case 3 "-s blocks short" C 0 -- -1s "$fix/sizes"
run_case 3 "-i inode short" C 0 -- -1i "$fix/plain"
run_case 3 "-lsi combined" C 0 -- -lsi "$fix/sizes"
run_case 3 "-l file operands" C 0 -- -l "$fix/times/old-a" "$fix/sizes/sz512"
run_case 3 "-l symlink operand" C 0 -- -l "$fix/links/good"
run_case 3 "-l dangling operand" C 0 -- -l "$fix/links/dangling"
run_case 3 "-lt sorted" C 0 -- -lt "$fix/times"
run_case 3 "-lS sorted" C 0 -- -lS "$fix/sizes"
run_case 3 "-lv versions" C 0 -- -lv "$fix/versions"
run_case 3 "-l sparse" C 0 -- -l "$fix/sizes"
run_case 3 "-l perm -a" C 0 -- -la "$fix/perm"
run_case 3 "-l group-dirs" C 0 -- -la --group-directories-first "$fix/links"

# 03 bespoke: env block size and POSIXLY_CORRECT totals.
if [ 3 -le "$active" ]; then
    for envspec in "LS_BLOCK_SIZE=1M" "BLOCK_SIZE=512" "POSIXLY_CORRECT=1" \
        "BLOCKSIZE=64K" "LS_BLOCK_SIZE=human-readable"; do
        cases=$((cases + 1))
        env -i PATH="$PATH" LC_ALL=C TZ=UTC0 COLUMNS=80 LS_COLORS= \
            LISZT_DEBUG_VERIFY=1 "$envspec" \
            "$work/liszt.uut" -ls "$fix/sizes" > "$work/u.out" 2>/dev/null
        urc=$?
        env -i PATH="$PATH" LC_ALL=C TZ=UTC0 COLUMNS=80 LS_COLORS= \
            "$envspec" \
            "$oracle" -ls "$fix/sizes" > "$work/o.out" 2>/dev/null
        orc=$?
        if [ "$urc" -ne "$orc" ] || ! cmp -s "$work/u.out" "$work/o.out"; then
            echo "CASE FAIL [block-size env $envspec]" >&2
            diff "$work/o.out" "$work/u.out" | head -6 >&2
            fails=$((fails + 1))
        fi
    done
fi

# 04: layout and quoting.
run_case 4 "-C plain w80" C 0 -- -C -w 80 "$fix/plain"
run_case 4 "-C plain w20" C 0 -- -C -w 20 "$fix/plain"
run_case 4 "-C plain w200" C 0 -- -C -w 200 "$fix/plain"
run_case 4 "-C plain w1" C 0 -- -C -w 1 "$fix/plain"
run_case 4 "-C w0 unlimited" C 0 -- -C -w 0 -a "$fix/shapes"
run_case 4 "-C shapes -a" C 0 -- -Ca -w 80 "$fix/shapes"
run_case 4 "-C versions -a utf8" "$U8" 0 -- -Ca -w 80 "$fix/versions"
run_case 4 "-C plain dict" "$D8" 0 -- -Ca -w 80 "$fix/plain"
run_case 4 "-x shapes" C 0 -- -xa -w 80 "$fix/shapes"
run_case 4 "-x versions dict" "$D8" 0 -- -xa -w 80 "$fix/versions"
run_case 4 "-m plain" C 0 -- -m -w 80 "$fix/plain"
run_case 4 "-m versions dict" "$D8" 0 -- -ma -w 80 "$fix/versions"
run_case 4 "-m width20" C 0 -- -ma -w 20 "$fix/shapes"
run_case 4 "-Cs frills" C 0 -- -Csa -w 80 "$fix/sizes"
run_case 4 "-Ci frills" C 0 -- -Ci -w 80 "$fix/plain"
run_case 4 "-C tab0" C 0 -- -Ca -T0 -w 80 "$fix/shapes"
run_case 4 "-C tab3" C 0 -- -Ca -T3 -w 80 "$fix/shapes"
run_case 4 "-w invalid" C 2 -- -C -w bogus
run_case 4 "-T invalid" C 2 -- -C -T bogus
run_case 4 "format across" C 0 -- -a -w 80 --format=across "$fix/shapes"
run_case 4 "format commas" C 0 -- -a -w 80 --format=commas "$fix/shapes"
run_case 4 "format vertical" C 0 -- -a -w 80 --format=vertical "$fix/shapes"
run_case 4 "-C operands mix" C 0 -- -C -w 80 "$fix/times/old-a" "$fix/plain" "$fix/sizes/sz512"
run_case 4 "-m operands" C 0 -- -m -w 80 "$fix/times/old-a" "$fix/sizes/sz512"

for sty in literal shell shell-always shell-escape shell-escape-always \
    c c-maybe escape locale clocale; do
    run_case 4 "style $sty -1" C 0 -- -1a --quoting-style=$sty "$fix/shapes"
    # UTF-8 escaping decisions ride the platform's printability tables;
    # gnulib ships its own, so only the pinned-oracle platforms (whose
    # libc agrees) gate these until sprint 07 ports the uniwidth tables.
    run_case_tables 4 "style $sty -1 dict" "$D8" 0 -- -1a --quoting-style=$sty "$fix/shapes"
    run_case 4 "style $sty -l" C 0 -- -la --quoting-style=$sty "$fix/links"
    run_case 4 "style $sty -C" C 0 -- -Ca -w 80 --quoting-style=$sty "$fix/shapes"
done
run_case 4 "-b escape" C 0 -- -1ab "$fix/shapes"
run_case 4 "-N literal" C 0 -- -1aN "$fix/shapes"
run_case 4 "-Q quote-name" C 0 -- -1aQ "$fix/shapes"
run_case 4 "-q qmark" C 0 -- -1aq "$fix/shapes"
run_case_tables 4 "-q qmark dict" "$D8" 0 -- -1aq "$fix/shapes"
run_case 4 "-q columns" C 0 -- -Caq -w 80 "$fix/shapes"
run_case 4 "-q show-control override" C 0 -- -1aq --show-control-chars "$fix/shapes"
run_case 4 "-lq long qmark" C 0 -- -laq "$fix/shapes"
run_case 4 "sort width" C 0 -- -1a --sort=width "$fix/shapes"
run_case_tables 4 "sort width dict" "$D8" 0 -- -1a --sort=width "$fix/shapes"
run_case 4 "sort width -C" C 0 -- -Ca -w 80 --sort=width "$fix/shapes"
run_case 4 "sort width -r" C 0 -- -1ar --sort=width "$fix/plain"
run_case 4 "weird missing operand quoteaf" C 2 -- -1 "$work/no such\
byte operand"

# 04 deviation registry (D1): rejected widths clamp to 0, GNU wraps.
# (--sort=width needs no case: SIZE_MAX and 0 order identically.)
run_case_dev d1-columns 4 "D1 -C devdir" "$U8" 0 -- -Ca -w 20 "$work/devdir"
run_case_dev d1-across 4 "D1 -x devdir" "$U8" 0 -- -xa -w 20 "$work/devdir"
run_case_dev d1-commas 4 "D1 -m devdir" "$U8" 0 -- -ma -w 12 "$work/devdir"

# 04 bespoke: env-driven width/tabsize/quoting.
if [ 4 -le "$active" ]; then
    for envspec in "COLUMNS=25" "COLUMNS=bogus" "TABSIZE=3" "TABSIZE=0" \
        "TABSIZE=bogus" "QUOTING_STYLE=shell-escape" "QUOTING_STYLE=bogus"; do
        cases=$((cases + 1))
        env -i PATH="$PATH" LC_ALL=C TZ=UTC0 LS_COLORS= \
            LISZT_DEBUG_VERIFY=1 "$envspec" \
            "$work/liszt.uut" -Ca "$fix/plain" > "$work/u.out" 2> "$work/u.raw"
        urc=$?
        env -i PATH="$PATH" LC_ALL=C TZ=UTC0 LS_COLORS= "$envspec" \
            "$oracle" -Ca "$fix/plain" > "$work/o.out" 2> "$work/o.raw"
        orc=$?
        normprog < "$work/u.raw" > "$work/u.err"
        normprog < "$work/o.raw" > "$work/o.err"
        if [ "$urc" -ne "$orc" ] || ! cmp -s "$work/u.out" "$work/o.out" \
            || ! cmp -s "$work/u.err" "$work/o.err"; then
            echo "CASE FAIL [layout env $envspec]" >&2
            diff "$work/o.out" "$work/u.out" | head -6 >&2
            fails=$((fails + 1))
        fi
    done
fi

# 05: color and indicators. Custom schemes pass through run_pinned's
# LS_COLORS override; the default scheme comes from the oracle-side
# dircolors so both tools read identical bytes.
DEFCOLORS=$(dircolors -b 2>/dev/null | sed -n "s/^LS_COLORS='\(.*\)';\$/\1/p")
run_case_color() {
    lsc="$1"
    shift
    tag="$1" desc="$2" lc="$3" wantrc="$4"
    shift 4
    [ "$1" = "--" ] && shift
    [ "$tag" -le "$active" ] || return 0
    cases=$((cases + 1))
    env -i PATH="$PATH" LC_ALL="$lc" TZ=UTC0 COLUMNS=80 LS_COLORS="$lsc" \
        LISZT_DEBUG_VERIFY=1 \
        "$work/liszt.uut" "$@" > "$work/u.out" 2> "$work/u.raw"
    urc=$?
    env -i PATH="$PATH" LC_ALL="$lc" TZ=UTC0 COLUMNS=80 LS_COLORS="$lsc" \
        "$oracle" "$@" > "$work/o.out" 2> "$work/o.raw"
    orc=$?
    normprog < "$work/u.raw" > "$work/u.err"
    normprog < "$work/o.raw" > "$work/o.err"
    ok=1
    [ "$urc" -eq "$orc" ] || ok=0
    cmp -s "$work/u.out" "$work/o.out" || ok=0
    cmp -s "$work/u.err" "$work/o.err" || ok=0
    [ "$wantrc" = "-" ] || [ "$urc" -eq "$wantrc" ] || ok=0
    if [ "$ok" -ne 1 ]; then
        echo "CASE FAIL [$desc] (lc=$lc rc uut=$urc oracle=$orc)" >&2
        diff "$work/o.out" "$work/u.out" | head -6 | cat -v >&2
        fails=$((fails + 1))
    fi
}

run_case_color "$DEFCOLORS" 5 "color default links" C 0 -- --color=always -1a "$fix/links"
run_case_color "$DEFCOLORS" 5 "color default meta" C 0 -- --color=always -1a "$fix/meta"
run_case_color "$DEFCOLORS" 5 "color default versions" C 0 -- --color=always -1 "$fix/versions"
run_case_color "$DEFCOLORS" 5 "color default perm" C 0 -- --color=always -1a "$fix/perm"
run_case_color "$DEFCOLORS" 5 "color -l links" C 0 -- --color=always -la "$fix/links"
run_case_color "$DEFCOLORS" 5 "color -C links" C 0 -- --color=always -Ca -w 60 "$fix/links"
run_case_color "$DEFCOLORS" 5 "color -C sizes" C 0 -- --color=always -C -w 80 "$fix/sizes"
run_case_color "$DEFCOLORS" 5 "color classify" C 0 -- --color=always -1aF "$fix/meta"
run_case_color "$DEFCOLORS" 5 "color utf8 links" "$U8" 0 -- --color=always -1a "$fix/links"
run_case_color "$DEFCOLORS" 5 "color dict versions" "$D8" 0 -- --color=always -1 "$fix/versions"
run_case_color "di=1;35:ln=target:or=41:mi=05;37;41:*.tar.gz=01;31:*.GZ=01;33:ex=00" \
    5 "custom ln=target orphans" C 0 -- --color=always -1a "$fix/links"
run_case_color "di=1;35:ln=target:or=41:mi=05;37;41:*.tar.gz=01;31:*.GZ=01;33:ex=00" \
    5 "custom ln=target -l" C 0 -- --color=always -la "$fix/links"
run_case_color "*.txt=35:*.TXT=36:*.txt=34" 5 "suffix precedence case" C 0 -- --color=always -1 "$fix/versions"
run_case_color "di=01;34:bogus" 5 "malformed trailing entry" C 0 -- --color=always -1 "$fix/plain"
run_case_color "xx=99" 5 "unknown prefix warns" C 0 -- --color=always -1 "$fix/plain"
run_case_color "di=01;34:no=00;36" 5 "norm color" C 0 -- --color=always -1a "$fix/links"
run_case_color "di=01;34:ec=\e[0m" 5 "explicit end code" C 0 -- --color=always -1a "$fix/links"
run_case_color "$DEFCOLORS" 5 "color operands mix" C 0 -- --color=always -1 "$fix/links/good" "$fix/plain"

# 05: indicators without color.
run_case 5 "classify -F meta" C 0 -- -1aF "$fix/meta"
run_case 5 "classify -F links" C 0 -- -1aF "$fix/links"
run_case 5 "slash -p" C 0 -- -1ap "$fix/meta"
run_case 5 "file-type" C 0 -- -1a --file-type "$fix/links"
run_case 5 "indicator-style words" C 0 -- -1a --indicator-style=file-type "$fix/meta"
run_case 5 "classify WHEN never" C 0 -- -1a --classify=never "$fix/meta"
run_case 5 "classify WHEN always" C 0 -- -1a --classify=always "$fix/links"
run_case 5 "-F columns" C 0 -- -CaF -w 80 "$fix/meta"
run_case 5 "-F long" C 0 -- -laF "$fix/links"
run_case 5 "-F sort width regolden" C 0 -- -1a --sort=width -F "$fix/shapes"
run_case 5 "-F operands" C 0 -- -F -1 "$fix/links/good" "$fix/meta/exec"
run_case 5 "-p versions" C 0 -- -1p "$fix/versions"
run_case 5 "color auto piped" C 0 -- --color=auto -1a "$fix/links"

# 06: recursion and the deref family. Deep and wide trees built here;
# unreadable-subdir continuation and symlink cycles ride the core
# fixture.
mkdir -p "$work/deep"
dp="$work/deep"
i=0
while [ "$i" -lt 40 ]; do
    dp="$dp/d$i"
    mkdir "$dp"
    printf 'x\n' > "$dp/f$i"
    i=$((i + 1))
done
mkdir -p "$work/wide"
i=0
while [ "$i" -lt 50 ]; do
    mkdir "$work/wide/w$i"
    printf 'x\n' > "$work/wide/w$i/inner"
    i=$((i + 1))
done

run_case 6 "-R core tree" C 1 -- -R1 "$fix"
run_case 6 "-R core -a" C 1 -- -R1a "$fix"
run_case 6 "-R core utf8" "$U8" 1 -- -R1 "$fix"
run_case 6 "-R core dict" "$D8" 1 -- -R1 "$fix"
run_case 6 "-R -l links" C 0 -- -Rl "$fix/links"
run_case 6 "-R -U" C 0 -- -RU1 "$fix/links"
run_case 6 "-R -t" C 1 -- -Rt1 "$fix"
run_case 6 "-R reverse" C 0 -- -R1r "$fix/links"
run_case 6 "-R columns" C 1 -- -RC -w 80 "$fix"
run_case 6 "-R multi operands" C 0 -- -R1 "$fix/links" "$fix/times"
run_case 6 "-R file+dir operands" C 0 -- -R1 "$fix/times/old-a" "$fix/times"
run_case 6 "-R deep tree" C 0 -- -R1 "$work/deep"
run_case 6 "-R deep -l" C 0 -- -Rl "$work/deep"
run_case 6 "-R wide tree" C 0 -- -R1 "$work/wide"
run_case 6 "-R unreadable continues" C 1 -- -R1 "$fix/perm"
run_case 6 "-RL cycle detect" C 2 -- -RL1 "$fix/links"
run_case 6 "-RL cycle -l" C 2 -- -RLl "$fix/links"
run_case 6 "-RL full tree" C 2 -- -RL1 "$fix"
run_case 6 "-d dir operand" C 0 -- -d1 "$fix/plain"
run_case 6 "-d multiple" C 0 -- -d1 "$fix/plain" "$fix/links" "$fix/times/old-a"
run_case 6 "-dl long" C 0 -- -dl "$fix/plain"
run_case 6 "-d symlink-to-dir" C 0 -- -d1 "$fix/links/gooddir"
run_case 6 "-dR no recursion" C 0 -- -dR1 "$fix/plain"
run_case 6 "-dF classify" C 0 -- -dF "$fix/plain" "$fix/links/gooddir"
run_case 6 "-H symlink-to-dir op" C 0 -- -H1 "$fix/links/gooddir"
run_case 6 "-H dangling op" C 2 -- -H1 "$fix/links/dangling"
run_case 6 "-Hl operands" C 0 -- -Hl "$fix/links/good" "$fix/links/gooddir"
run_case 6 "-L links -a" C 0 -- -L1a "$fix/links"
run_case 6 "-lL dangling errors" C 1 -- -lLa "$fix/links"
run_case 6 "-L dangling operand" C 2 -- -L1 "$fix/links/dangling"
run_case 6 "-L target-time sort" C 1 -- -Lt1 "$fix/links"
run_case 6 "-L classify" C 1 -- -LF1a "$fix/links"
run_case 6 "explicit cl-symlink-to-dir" C 0 -- -1 --dereference-command-line-symlink-to-dir "$fix/links/gooddir"
run_case_color "$DEFCOLORS" 6 "-R color" C 0 -- -R1 --color=always "$fix/links"
run_case_color "$DEFCOLORS" 6 "-L color links" C - -- -L1a --color=always "$fix/links"
# Fuzz trial 20 shape: second operand is an empty dir, so grouping with
# -a leaves the transformed engine's files range empty (was a crash).
run_case_color "$DEFCOLORS" 6 "group -C color empty operand" "$U8" 0 -- --group-directories-first -C -w 42 --color=always -a "$fix/links" "$work/empty"

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
