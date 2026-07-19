#!/bin/sh
# Tree-mode self-differential fuzzer. No oracle knows --tree, so the
# lane attacks structure with safe names (alnum only - reconstruction
# stays unambiguous) and checks invariants instead of bytes:
#   1 prefix-strip reconstruction == find over the same tree
#   2 per-dir sibling sequence == flat `liszt -1 <same flags>` - the
#     parity-proven lane as ordering oracle
#   3 last-branch glyph iff final sibling; depth steps never jump
#   4 --level output == full output filtered by depth; --tree-limit
#     shown+summarized counts reconcile against the flat lane
#
# Deterministic per (FUZZ_SEED, trial) on a given box (awk rand()
# differs across implementations; rank's precedent).
# Env: FUZZ_TRIALS (default 50), FUZZ_SEED (default 42).
# Exit: 0 pass, 1 fail.
set -u
cd "$(dirname "$0")/../.." || exit 1

trials="${FUZZ_TRIALS:-50}"
seed="${FUZZ_SEED:-42}"

work=$(mktemp -d "${TMPDIR:-/tmp}/liszt-treefuzz.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT INT TERM
faildir="tests/.work/tree-fuzz-failures"

cp ./liszt "$work/liszt.uut"

run_uut() {
    env -i ${LISZT_PARALLEL_MIN:+LISZT_PARALLEL_MIN=$LISZT_PARALLEL_MIN} \
        PATH="$PATH" LC_ALL=C TZ=UTC0 COLUMNS=80 LS_COLORS= \
        LISZT_DEBUG_VERIFY=1 "$work/liszt.uut" "$@"
}

# Trial plan: D/F/L creation lines, then "ARGS <sortflags> | <lvl> <lim>".
gen_plan() {
    awk -v seed="$seed" -v trial="$1" 'BEGIN {
        srand(seed + trial * 131)
        alpha = "abcdefghijklmnopqrstuvwxyz0123456789"
        ndirs_total = 1
        dirs[0] = ""
        ndirs = 1 + int(rand() * 8)
        for (i = 0; i < ndirs; i++) {
            parent = dirs[int(rand() * ndirs_total)]
            len = 1 + int(rand() * 8)
            name = ""
            for (j = 0; j < len; j++)
                name = name substr(alpha, int(rand() * 36) + 1, 1)
            d = parent == "" ? name : parent "/" name
            if (d in seen) continue
            seen[d] = 1
            dirs[ndirs_total++] = d
            print "D " d
        }
        if (rand() < 0.5) {
            chain = "chainz"
            depth = 2 + int(rand() * 6)
            for (i = 0; i < depth; i++) chain = chain "/c" i
            print "D " chain
        }
        nf = int(rand() * 30)
        for (i = 0; i < nf; i++) {
            parent = dirs[int(rand() * ndirs_total)]
            len = 1 + int(rand() * 8)
            name = ""
            for (j = 0; j < len; j++)
                name = name substr(alpha, int(rand() * 36) + 1, 1)
            f = parent == "" ? name : parent "/" name
            if (f in seen) continue
            seen[f] = 1
            print "F " f
        }
        # Symlink to a dir: rendered, never descended (find agrees).
        if (rand() < 0.4 && ndirs_total > 1) print "L slnk " dirs[1]

        p = rand()
        if (p < 0.15) flags = "-r"
        else if (p < 0.3) flags = "-S"
        else if (p < 0.45) flags = "-t"
        else if (p < 0.55) flags = "-v"
        else if (p < 0.65) flags = "--group-directories-first"
        else if (p < 0.72) flags = "-X"
        else flags = ""
        if (rand() < 0.25) flags = flags " -a"
        sub(/^ /, "", flags)
        lvl = (rand() < 0.5) ? 1 + int(rand() * 4) : 0
        lim = (rand() < 0.5) ? 1 + int(rand() * 3) : 0
        print "ARGS " flags " | " lvl " " lim
    }'
}

# Parse one --tree output (ascii glyphs, C locale): emits
#   PATH <depth> <path>     every entry except . / .. and summaries
#   SIB <parent>|<name>     sibling sequences in emission order
#   SUM <parent>|<count>    per-dir summary counts
# and enforces invariant 3 (glyph algebra + depth steps). Exit 1 on any
# structural violation.
parse_tree() {
    LC_ALL=C awk '
    NR == 1 { stack[0] = $0; prevdepth = 0; next }
    {
        line = $0
        d = 0
        while (1) {
            u = substr(line, 1, 4)
            if (u == "|   " || u == "    ") { line = substr(line, 5); d++ }
            else break
        }
        u = substr(line, 1, 4)
        if (u == "|-- ") glyph = "mid"
        else if (u == "`-- ") glyph = "last"
        else { print "VIOLATION badline: " $0; exit 1 }
        name = substr(line, 5)
        d++
        if (d > prevdepth + 1) { print "VIOLATION depthjump: " $0; exit 1 }
        # A new line at depth d closes every deeper list: each must have
        # ended with the last-branch glyph.
        for (d2 = d + 1; d2 <= maxd; d2++) {
            if ((d2 in lastglyph) && lastglyph[d2] == "mid") {
                print "VIOLATION unclosed list at depth " d2; exit 1
            }
            delete lastglyph[d2]
        }
        if ((d in lastglyph) && lastglyph[d] == "last") {
            print "VIOLATION sibling after last-branch: " $0; exit 1
        }
        lastglyph[d] = glyph
        if (d > maxd) maxd = d
        if (name ~ /^\.\.\. [0-9]+ more$/) {
            n = name
            sub(/^\.\.\. /, "", n)
            sub(/ more$/, "", n)
            if (glyph != "last") {
                print "VIOLATION summary not last: " $0; exit 1
            }
            print "SUM " stack[d - 1] "|" n
            prevdepth = d - 1
            next
        }
        print "SIB " stack[d - 1] "|" name
        if (name != "." && name != "..") {
            full = stack[d - 1] "/" name
            print "PATH " d " " full
            stack[d] = full
        }
        prevdepth = d
    }
    END {
        for (d2 = 1; d2 <= maxd; d2++)
            if ((d2 in lastglyph) && lastglyph[d2] == "mid") {
                print "VIOLATION unclosed list at depth " d2; exit 1
            }
    }'
}

fails=0
t=0
while [ "$t" -lt "$trials" ]; do
    t=$((t + 1))
    tree="$work/t$t"
    rm -rf "$tree"
    mkdir -p "$tree"

    plan=$(gen_plan "$t")
    sortflags=""
    lvl=0
    lim=0
    while IFS= read -r line; do
        kind=${line%% *}
        rest=${line#* }
        case "$kind" in
        D) mkdir -p "$tree/$rest" ;;
        F) printf 'x\n' > "$tree/$rest" ;;
        L) ln -s "${rest#* }" "$tree/${rest%% *}" ;;
        ARGS)
            sortflags=${rest%% | *}
            [ "$sortflags" = "$rest" ] && sortflags=""
            lvllim=${rest#* | }
            lvl=${lvllim%% *}
            lim=${lvllim#* }
            ;;
        esac
    done << EOF
$plan
EOF

    bad=""

    # Full run: parse (invariant 3 inline), reconstruction, flat oracle.
    run_uut --tree $sortflags "$tree" > "$work/full.out" 2>/dev/null
    if ! parse_tree < "$work/full.out" > "$work/full.parsed"; then
        bad="parse/glyph invariant (full run)"
    fi

    if [ -z "$bad" ]; then
        sed -n 's/^PATH [0-9]* //p' "$work/full.parsed" | sort \
            > "$work/got.paths"
        { printf '%s\n' "$tree"; sed -n 's/^PATH [0-9]* //p' \
            "$work/full.parsed"; } | sort > "$work/got.withroot"
        find "$tree" | sort > "$work/find.paths"
        if ! cmp -s "$work/got.withroot" "$work/find.paths"; then
            bad="reconstruction != find"
        fi
    fi

    if [ -z "$bad" ]; then
        for dir in $(find "$tree" -type d); do
            LC_ALL=C awk -v p="SIB $dir|" 'index($0, p) == 1 {
                print substr($0, length(p) + 1) }' "$work/full.parsed" \
                > "$work/tree.sibs"
            run_uut -1 $sortflags "$dir" > "$work/flat.sibs" 2>/dev/null
            if ! cmp -s "$work/tree.sibs" "$work/flat.sibs"; then
                bad="sibling sequence != flat -1 ($dir)"
                break
            fi
        done
    fi

    # --level algebra: level output == full output filtered by depth.
    if [ -z "$bad" ] && [ "$lvl" -gt 0 ]; then
        run_uut --tree --level="$lvl" $sortflags "$tree" \
            > "$work/lvl.out" 2>/dev/null
        if ! parse_tree < "$work/lvl.out" > "$work/lvl.parsed"; then
            bad="parse/glyph invariant (level run)"
        elif ! sed -n 's/^PATH //p' "$work/lvl.parsed" | sort \
                > "$work/lvl.paths" \
            || ! sed -n 's/^PATH //p' "$work/full.parsed" \
                | LC_ALL=C awk -v n="$lvl" '$1 <= n' | sort \
                > "$work/lvl.want" \
            || ! cmp -s "$work/lvl.paths" "$work/lvl.want"; then
            bad="--level=$lvl filter algebra"
        fi
    fi

    # --tree-limit reconciliation: shown == first K of flat, summary
    # count == remainder; uncapped dirs match flat exactly.
    if [ -z "$bad" ] && [ "$lim" -gt 0 ]; then
        run_uut --tree --tree-limit="$lim" $sortflags "$tree" \
            > "$work/lim.out" 2>/dev/null
        if ! parse_tree < "$work/lim.out" > "$work/lim.parsed"; then
            bad="parse/glyph invariant (limit run)"
        else
            for dir in $(find "$tree" -type d); do
                LC_ALL=C awk -v p="SIB $dir|" 'index($0, p) == 1 {
                    print substr($0, length(p) + 1) }' "$work/lim.parsed" \
                    > "$work/lim.sibs"
                summ=$(LC_ALL=C awk -v p="SUM $dir|" 'index($0, p) == 1 {
                    print substr($0, length(p) + 1) }' "$work/lim.parsed")
                run_uut -1 $sortflags "$dir" > "$work/flat.sibs" \
                    2>/dev/null
                nflat=$(wc -l < "$work/flat.sibs")
                nshown=$(wc -l < "$work/lim.sibs")
                # Capped-away dirs never appear as parents at all.
                if [ "$nshown" -eq 0 ] && [ -z "$summ" ]; then
                    continue
                fi
                if [ "$nflat" -le "$lim" ]; then
                    if [ -n "$summ" ] \
                        || ! cmp -s "$work/lim.sibs" "$work/flat.sibs"
                    then
                        bad="limit: uncapped dir mismatch ($dir)"
                        break
                    fi
                else
                    head -n "$lim" "$work/flat.sibs" > "$work/flat.top"
                    if [ "$nshown" -ne "$lim" ] \
                        || ! cmp -s "$work/lim.sibs" "$work/flat.top" \
                        || [ "$summ" != "$((nflat - lim))" ]; then
                        bad="limit: capped dir mismatch ($dir)"
                        break
                    fi
                fi
            done
        fi
    fi

    if [ -n "$bad" ]; then
        fails=$((fails + 1))
        mkdir -p "$faildir"
        cp -R "$tree" "$faildir/trial-$t" 2>/dev/null || true
        echo "TREE FUZZ FAIL trial $t: $bad" >&2
        echo "  seed=$seed flags='$sortflags' lvl=$lvl lim=$lim" >&2
    fi
done

echo "tests/fuzz/run-tree: $trials trials, $fails failures"
[ "$fails" -eq 0 ]
