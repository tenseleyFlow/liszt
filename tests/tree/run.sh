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
        COLUMNS=80 LS_COLORS="${LSC:-}" LISZT_DEBUG_VERIFY=1 "$@"
}
EXTRA_ENV=
LSC=

# run_tree SLUG desc locale wantrc -- args...
# Runs from inside the fixture: relative operands keep $work out of the
# pinned bytes. LISZT_TREE_REGEN=1 (authoring only) rewrites expected
# files instead of asserting; review the diff before committing.
run_tree() {
    slug="$1" desc="$2" lc="$3" wantrc="$4"
    shift 4
    [ "$1" = "--" ] && shift
    cases=$((cases + 1))
    exp="tests/tree/expected/$slug.out"
    experr="tests/tree/expected/$slug.err"
    (cd "$fx" && run_pinned "$lc" "$work/liszt.uut" "$@") \
        > "$work/u.out" 2> "$work/u.raw"
    urc=$?
    normprog < "$work/u.raw" > "$work/u.err"
    if [ "${LISZT_TREE_REGEN:-0}" = "1" ]; then
        cp "$work/u.out" "$exp"
        if [ -s "$work/u.err" ]; then
            cp "$work/u.err" "$experr"
        else
            rm -f "$experr"
        fi
        return 0
    fi
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
# Deterministic: fixed ASCII names (C and C.UTF-8 collate identically),
# fixed sizes and touch -d mtimes where a case sorts on them, explicit
# chmod. Nothing fs-dependent (inodes, blocks, dir sizes, owners) enters
# pinned bytes - those ride structural checks.
fx="$work/fx"
mkdir -p "$fx/top/a1/b1/c1" "$fx/top/a1/zlast" "$fx/top/a2" \
    "$fx/top/mid_empty" "$fx/chain/l1/l2/l3" "$fx/sorts" \
    "$fx/quoting/sp ace" "$fx/links/real" "$fx/looped/sub" \
    "$fx/guard/denied" "$fx/exec"
printf 'd1\n' > "$fx/top/a1/b1/c1/d1.txt"
printf 'c2\n' > "$fx/top/a1/b1/c2.txt"
printf 'b2\n' > "$fx/top/a1/b2.txt"
printf 'only\n' > "$fx/top/a2/only.txt"
printf 'zz\n' > "$fx/top/zz.txt"
printf 'leaf\n' > "$fx/chain/l1/l2/l3/leaf"
# -S/-t material: size order and mtime order both differ from name order.
LC_ALL=C awk 'BEGIN { for (i = 0; i < 100; i++) printf "x" }' \
    > "$fx/sorts/big.bin"
LC_ALL=C awk 'BEGIN { for (i = 0; i < 300; i++) printf "x" }' \
    > "$fx/sorts/mid.bin"
LC_ALL=C awk 'BEGIN { for (i = 0; i < 200; i++) printf "x" }' \
    > "$fx/sorts/sml.bin"
touch -d '2020-01-01 00:00:01' "$fx/sorts/big.bin"
touch -d '2020-01-03 00:00:03' "$fx/sorts/mid.bin"
touch -d '2020-01-02 00:00:02' "$fx/sorts/sml.bin"
printf 'q\n' > "$fx/quoting/sp ace/it's.txt"
printf 'q\n' > "$fx/quoting/sp ace/q\"uote"
printf 'f\n' > "$fx/links/real/f.txt"
printf 'x\n' > "$fx/links/file.txt"
ln -s real "$fx/links/lnk"
ln -s .. "$fx/looped/sub/back"
printf 'trap\n' > "$fx/guard/denied/trap.txt"
printf 'ok\n' > "$fx/guard/ok.txt"
chmod 000 "$fx/guard/denied"
printf '#!/bin/sh\n' > "$fx/exec/runme"
chmod 755 "$fx/exec/runme"
printf 'p\n' > "$fx/exec/plain.txt"
# Alignment fixture: the root list is widest (12000-byte file), so
# inherited column maxima keep one glyph column for the whole tree.
mkdir -p "$fx/align/a1/b1"
LC_ALL=C awk 'BEGIN { for (i = 0; i < 12000; i++) printf "x" }' \
    > "$fx/align/big.bin"
printf 'a\n' > "$fx/align/afile"
printf 'b\n' > "$fx/align/a1/bfile"
printf 'c\n' > "$fx/align/a1/b1/cfile"

# --- Parser error lanes (13A) --------------------------------------------
# Child flags without --tree: exit 2, empty stdout.
run_tree err-level-alone "level without tree" C 2 \
    -- --level=2 "top"
run_tree err-limit-alone "tree-limit without tree" C 2 \
    -- --tree-limit=3 "top"
run_tree err-glyphs-alone "tree-glyphs without tree" C 2 \
    -- --tree-glyphs=ascii "top"

# Invalid values: numeric flags exit 2 (tabsize shape); word flags exit
# 1 (the argmatch machinery's pinned GNU quirk - same as --icons=bogus).
run_tree err-level-junk "non-numeric level" C 2 \
    -- --tree --level=x "top"
run_tree err-level-neg "negative level" C 2 \
    -- --tree --level=-1 "top"
run_tree err-level-range "ERANGE level" C 2 \
    -- --tree --level=99999999999999999999999 "top"
run_tree err-limit-junk "non-numeric tree-limit" C 2 \
    -- --tree --tree-limit=2x "top"
run_tree err-glyphs-junk "bad tree-glyphs word" C 1 \
    -- --tree --tree-glyphs=bogus "top"

# run_tree_u8: unicode-glyph cases need a real UTF-8 locale.
run_tree_u8() {
    if [ -n "$utf8_locale" ]; then
        slug="$1" desc="$2" wantrc="$3"
        shift 3
        run_tree "$slug" "$desc" "$utf8_locale" "$wantrc" "$@"
    fi
}

# --- Core matrix (13B) ----------------------------------------------------
# Glyph selection: auto follows the locale codeset; explicit words win.
run_tree_u8 basic "unicode glyphs via auto" 0 -- --tree top
run_tree ascii-auto "ascii glyphs via auto (C)" C 0 -- --tree top
run_tree unicode-forced "unicode forced under C" C 0 \
    -- --tree --tree-glyphs=unicode top
run_tree_u8 ascii-forced "ascii forced under UTF-8" 0 \
    -- --tree --tree-glyphs=ascii top
run_tree glyphs-abbrev "unique value abbreviation (u)" C 0 \
    -- --tree --tree-glyphs=u top
run_tree glyphs-ambig "ambiguous value abbreviation (a)" C 1 \
    -- --tree --tree-glyphs=a top

# --level: boundary dirs shown, never entered.
run_tree level1 "level 1" C 0 -- --tree --level=1 top
run_tree level2 "level 2" C 0 -- --tree --level=2 top
run_tree level3 "level 3" C 0 -- --tree --level=3 top

# Last-child edges: single-child chains, empty dir last and middle.
run_tree_u8 chain "single-child chain" 0 -- --tree chain
run_tree u-chain "-U over the chain" C 0 -- --tree -U chain

# Dot entries render, never descend.
run_tree_u8 dot-a "-a dot entries" 0 -- --tree -a top/a2

# Indicators and slashes.
run_tree_u8 classify "-F indicators" 0 -- --tree -F links
run_tree_u8 classify-exec "-F exec star" 0 -- --tree -F exec
run_tree_u8 slash-p "-p dir slashes" 0 -- --tree -p top/a1

# Sort surface per sibling list.
run_tree rev "-r reverse" C 0 -- --tree -r top
run_tree size-sort "-S size order" C 0 -- --tree -S sorts
run_tree time-sort "-t mtime order" C 0 -- --tree -t sorts
run_tree_u8 gdf "group-directories-first" 0 \
    -- --tree --group-directories-first top

# Quoting-heavy names at depth.
run_tree_u8 quoting "shell-escape quoting" 0 \
    -- --tree --quoting-style=shell-escape quoting
run_tree quoting-c "shell-escape quoting (C)" C 0 \
    -- --tree --quoting-style=shell-escape quoting

# Color: fixed scheme (empty LS_COLORS disables coloring, GNU parity);
# the pinned bytes prove glyphs stay outside every color region.
LSC='di=01;34:ln=01;36:ex=01;32'
run_tree_u8 color "pinned fixed-scheme color" 0 \
    -- --tree --color=always links
LSC=

# Symlinks: shown by default, descended under -L, cycles exit 2.
run_tree_u8 symlink-default "symlink not descended" 0 -- --tree links
run_tree_u8 symlink-deref "-L descends symlink" 0 -- --tree -L links
run_tree loop "-L cycle exits 2" C 2 -- --tree -L looped

# Operand shapes.
run_tree multi "multiple tree operands" C 0 -- --tree top/a2 chain
run_tree mix "files before trees" C 0 -- --tree top/zz.txt top/a2
run_tree dwins "-d wins, tree inert" C 0 -- --tree -d top
run_tree zero "--zero smoke" C 0 -- --tree --zero top/a2

# --tree-limit (13C): cap after sort, summary as final sibling, capped
# subdirs never walked.
run_tree_u8 limit-basic "limit 2 with summary" 0 \
    -- --tree --tree-limit=2 top
run_tree limit-nested "limit 1 caps every level" C 0 \
    -- --tree --tree-limit=1 top
run_tree limit-exact "limit equal to count, no summary" C 0 \
    -- --tree --tree-limit=4 top
run_tree limit-onemore "singular summary" C 0 \
    -- --tree --tree-limit=3 top
run_tree limit-zero-eol "summary honors --zero eol" C 0 \
    -- --tree --zero --tree-limit=1 top/a1
# Proof capped subdirs are not walked: reversed order caps away the
# permission trap - any walk would diagnose on stderr.
run_tree limit-notwalked "capped trap not walked" C 0 \
    -- --tree -r --tree-limit=1 guard

# Unreadable subdir: entry line prints, descent diagnoses, exit 1.
run_tree denied "permission-denied subdir" C 1 -- --tree guard

# --- Structural checks (fs-dependent fields never enter pinned bytes) ----
# -l/-i/-s: same tree shape as the flag-free run (line-count equality),
# and every non-root line carries a branch glyph.
tree_struct() {
    desc="$1"
    shift
    cases=$((cases + 1))
    (cd "$fx" && run_pinned C "$work/liszt.uut" --tree "$@" top) \
        > "$work/s.out" 2>/dev/null
    (cd "$fx" && run_pinned C "$work/liszt.uut" --tree top) \
        > "$work/p.out" 2>/dev/null
    lines_s=$(wc -l < "$work/s.out")
    lines_p=$(wc -l < "$work/p.out")
    ok=1
    [ "$lines_s" -eq "$lines_p" ] || ok=0
    LC_ALL=C awk 'NR > 1 && !/\|-- / && !/`-- / { bad = 1 }
                  END { exit bad }' "$work/s.out" || ok=0
    if [ "$ok" -ne 1 ]; then
        echo "TREE CASE FAIL [struct: $desc] ($lines_s vs $lines_p lines)" >&2
        fails=$((fails + 1))
    fi
}
tree_struct "-l long format" -l
tree_struct "-i inodes" -i
tree_struct "-s blocks" -s

# -l glyph-column alignment: child lists inherit parent column maxima,
# so with a root-widest fixture every branch glyph sits at
# pmin + 4 * depth-steps - all positions congruent mod 4.
cases=$((cases + 1))
(cd "$fx" && run_pinned C "$work/liszt.uut" --tree -l align) \
    > "$work/al.out" 2>/dev/null
if ! LC_ALL=C awk '
    {
        b = index($0, "|-- ")
        l = index($0, "`-- ")
        p = b && l ? (b < l ? b : l) : (b ? b : l)
        if (!p) next
        if (!pmin || p < pmin) pmin = p
        pos[NR] = p
    }
    END {
        for (i in pos)
            if ((pos[i] - pmin) % 4 != 0) exit 1
    }' "$work/al.out"; then
    echo "TREE CASE FAIL [-l glyph-column alignment]" >&2
    sed -n '1,6p' "$work/al.out" >&2
    fails=$((fails + 1))
fi

# Hyperlink self-identity: strip the OSC 8 wrappers, get the plain run.
cases=$((cases + 1))
(cd "$fx" && run_pinned C "$work/liszt.uut" --tree --hyperlink=always top) \
    > "$work/hl.out" 2>/dev/null
(cd "$fx" && run_pinned C "$work/liszt.uut" --tree top) \
    > "$work/plain.out" 2>/dev/null
LC_ALL=C awk 'BEGIN { e = sprintf("%c", 27) }
    { gsub(e "\\]8;;[^" e "]*" e "\\\\", ""); print }' \
    "$work/hl.out" > "$work/hl.stripped"
if ! cmp -s "$work/hl.stripped" "$work/plain.out"; then
    echo "TREE CASE FAIL [hyperlink strip identity]" >&2
    diff -u "$work/plain.out" "$work/hl.stripped" | sed -n '1,8p' >&2
    fails=$((fails + 1))
fi

echo "tests/tree: $cases cases, $fails failures"
[ "$fails" -eq 0 ]
