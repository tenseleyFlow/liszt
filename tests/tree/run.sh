#!/bin/sh
# Tree-mode extension harness. No oracle exists (GNU ls 9.11 exits 2 on
# --tree), so every case byte-compares against reviewed expected files
# under tests/tree/expected/: <slug>.out always, <slug>.err when stderr
# is expected (normalized via normprog), exit code asserted always.
#
# Env pinning matches the golden harness: env -i, LC_ALL per case,
# TZ=UTC0, COLUMNS=80, LS_COLORS empty, stdout piped,
# LISZT_DEBUG_VERIFY=1 so the sort oracle rides along.
#
# Expected bytes are generated once and REVIEWED, never blindly
# regenerated on failure.
#
# Exit: 0 pass, 1 fail, 77 skip (root).
set -u
cd "$(dirname "$0")/../.." || exit 1

if [ "$(id -u)" -eq 0 ]; then
    echo "tests/tree: refusing to run as root (permission fixtures are meaningless)" >&2
    exit 77
fi

utf8_locale=""
for loc in C.UTF-8 C.utf8 en_US.UTF-8 en_US.utf8; do
    if locale -a 2>/dev/null | grep -qix "$loc"; then
        utf8_locale="$loc"
        break
    fi
done

work=$(mktemp -d "${TMPDIR:-/tmp}/liszt-tree.XXXXXX") || exit 1
cleanup() {
    chmod -R u+rwx "$work" 2>/dev/null
    rm -rf "$work"
}
trap cleanup EXIT INT TERM

cp ./liszt "$work/liszt.uut" 2>/dev/null || true

fails=0
cases=0

normprog() {
    sed -e 's/^[^:][^:]*:/PROG:/' \
        -e "s|Try '[^']* --help'|Try 'PROG --help'|"
}

run_pinned() {
    lc="$1"
    shift
    env -i ${EXTRA_ENV:-} \
        ${LISZT_PARALLEL_MIN:+LISZT_PARALLEL_MIN=$LISZT_PARALLEL_MIN} \
        PATH="$PATH" LANGUAGE=C LC_ALL="$lc" TZ=UTC0 \
        COLUMNS=80 LS_COLORS= LISZT_DEBUG_VERIFY=1 "$@"
}
EXTRA_ENV=

# run_tree SLUG desc locale wantrc -- args...
run_tree() {
    slug="$1" desc="$2" lc="$3" wantrc="$4"
    shift 4
    [ "$1" = "--" ] && shift
    cases=$((cases + 1))
    exp="tests/tree/expected/$slug.out"
    experr="tests/tree/expected/$slug.err"
    run_pinned "$lc" "$work/liszt.uut" "$@" > "$work/u.out" 2> "$work/u.raw"
    urc=$?
    normprog < "$work/u.raw" > "$work/u.err"
    ok=1
    [ "$urc" -eq "$wantrc" ] || ok=0
    cmp -s "$work/u.out" "$exp" || ok=0
    if [ -f "$experr" ]; then
        cmp -s "$work/u.err" "$experr" || ok=0
    else
        [ -s "$work/u.err" ] && ok=0
    fi
    if [ "$ok" -ne 1 ]; then
        echo "TREE CASE FAIL [$desc] (lc=$lc rc=$urc want=$wantrc)" >&2
        diff -u "$exp" "$work/u.out" 2>/dev/null | sed -n '1,15p' >&2
        if [ -f "$experr" ]; then
            diff -u "$experr" "$work/u.err" 2>/dev/null | sed -n '1,8p' >&2
        else
            sed -n '1,4p' "$work/u.err" >&2
        fi
        fails=$((fails + 1))
    fi
}

# --- Fixture --------------------------------------------------------------
# Deterministic: fixed names, touch -d mtimes, explicit chmod. Extended by
# later subsprints; 13A needs only a directory to point flags at.
fx="$work/fx"
mkdir -p "$fx/top"
printf 'x\n' > "$fx/top/a"

# --- Parser error lanes (13A) --------------------------------------------
# Child flags without --tree: exit 2, empty stdout.
run_tree err-level-alone "level without tree" C 2 \
    -- --level=2 "$fx/top"
run_tree err-limit-alone "tree-limit without tree" C 2 \
    -- --tree-limit=3 "$fx/top"
run_tree err-glyphs-alone "tree-glyphs without tree" C 2 \
    -- --tree-glyphs=ascii "$fx/top"

# Invalid values: numeric flags exit 2 (tabsize shape); word flags exit
# 1 (the argmatch machinery's pinned GNU quirk - same as --icons=bogus).
run_tree err-level-junk "non-numeric level" C 2 \
    -- --tree --level=x "$fx/top"
run_tree err-level-neg "negative level" C 2 \
    -- --tree --level=-1 "$fx/top"
run_tree err-level-range "ERANGE level" C 2 \
    -- --tree --level=99999999999999999999999 "$fx/top"
run_tree err-limit-junk "non-numeric tree-limit" C 2 \
    -- --tree --tree-limit=2x "$fx/top"
run_tree err-glyphs-junk "bad tree-glyphs word" C 1 \
    -- --tree --tree-glyphs=bogus "$fx/top"

echo "tests/tree: $cases cases, $fails failures"
[ "$fails" -eq 0 ]
