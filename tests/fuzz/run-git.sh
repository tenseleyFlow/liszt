#!/bin/sh
# Git-status differential fuzzer: random repos x random mutations,
# liszt's column vs expectations derived from git itself (porcelain +
# ls-files + batched check-ignore). Safe alnum names; every mutation
# mtime sits years in the past so the racy window cannot flake.
#
# Env: FUZZ_TRIALS (default 25), FUZZ_SEED (default 42).
# Exit: 0 pass, 1 fail, 77 skip (no git).
set -u
cd "$(dirname "$0")/../.." || exit 1

command -v git >/dev/null 2>&1 || {
    echo "tests/fuzz/run-git: no git binary; skipping" >&2
    exit 77
}

trials="${FUZZ_TRIALS:-25}"
seed="${FUZZ_SEED:-42}"

work=$(mktemp -d "${TMPDIR:-/tmp}/liszt-gitfuzz.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT INT TERM
faildir="tests/.work/git-fuzz-failures"

cp ./liszt "$work/liszt.uut"

GIT_ENV="HOME=$work GIT_CONFIG_NOSYSTEM=1 XDG_CONFIG_HOME=$work/xdg"
g() {
    env $GIT_ENV git -c user.email=t@liszt.test -c user.name=liszt \
        -c init.defaultBranch=trunk "$@"
}

run_uut() {
    env -i ${LISZT_PARALLEL_MIN:+LISZT_PARALLEL_MIN=$LISZT_PARALLEL_MIN} \
        PATH="$PATH" LC_ALL=C TZ=UTC0 COLUMNS=80 LS_COLORS= \
        LISZT_DEBUG_VERIFY=1 "$work/liszt.uut" "$@"
}

# Plan lines: "F path", "D path", "IGN pattern", "MUT kind path",
# "LIST subdir-or-." - consumed below.
gen_plan() {
    awk -v seed="$seed" -v trial="$1" 'BEGIN {
        srand(seed + trial * 977)
        alpha = "abcdefghijklmnopqrstuvwxyz0123456789"
        nd = 1 + int(rand() * 3)
        dirs[0] = ""
        ndirs = 1
        for (i = 0; i < nd; i++) {
            parent = dirs[int(rand() * ndirs)]
            len = 1 + int(rand() * 6)
            nm = ""
            for (j = 0; j < len; j++)
                nm = nm substr(alpha, int(rand() * 36) + 1, 1)
            d = parent == "" ? nm : parent "/" nm
            if (d in seen) continue
            seen[d] = 1
            dirs[ndirs++] = d
            print "D " d
        }
        nf = 4 + int(rand() * 16)
        nfiles = 0
        for (i = 0; i < nf; i++) {
            parent = dirs[int(rand() * ndirs)]
            len = 1 + int(rand() * 6)
            nm = ""
            for (j = 0; j < len; j++)
                nm = nm substr(alpha, int(rand() * 36) + 1, 1)
            if (rand() < 0.4) nm = nm ".log"
            else if (rand() < 0.3) nm = nm ".txt"
            f = parent == "" ? nm : parent "/" nm
            if (f in seen) continue
            seen[f] = 1
            files[nfiles++] = f
            print "F " f
        }
        if (rand() < 0.6) print "IGN *.log"
        if (rand() < 0.3) print "IGN !keep.log"
        if (rand() < 0.3 && ndirs > 1) print "IGN " dirs[1] "/"
        for (i = 0; i < nfiles; i++) {
            p = rand()
            if (p < 0.25) print "MUT append " files[i]
            else if (p < 0.35) print "MUT chmod " files[i]
            else if (p < 0.45) print "MUT typechange " files[i]
            else if (p < 0.55) print "MUT new " files[i] "x"
        }
        print "LIST ."
        if (ndirs > 1) print "LIST " dirs[1 + int(rand() * (ndirs - 1))]
    }'
}

fails=0
t=0
while [ "$t" -lt "$trials" ]; do
    t=$((t + 1))
    repo="$work/r$t"
    rm -rf "$repo"
    mkdir -p "$repo"
    lists=""

    plan=$(gen_plan "$t")
    # Creation phase.
    while IFS= read -r line; do
        kind=${line%% *}
        rest=${line#* }
        case "$kind" in
        D) mkdir -p "$repo/$rest" ;;
        F) printf 'seed\n' > "$repo/$rest" ;;
        IGN) printf '%s\n' "$rest" >> "$repo/.gitignore" ;;
        esac
    done << EOF
$plan
EOF
    find "$repo" -exec touch -d '2024-01-01 00:00:00' {} +
    g -C "$repo" init -q
    g -C "$repo" add -A
    g -C "$repo" commit -q -m base --allow-empty
    # Mutation phase (mtimes to a later fixed past date).
    while IFS= read -r line; do
        kind=${line%% *}
        rest=${line#* }
        case "$kind" in
        MUT)
            mk=${rest%% *}
            mp=${rest#* }
            case "$mk" in
            append)
                [ -f "$repo/$mp" ] && printf 'more\n' >> "$repo/$mp" ;;
            chmod)
                [ -f "$repo/$mp" ] && chmod 755 "$repo/$mp" ;;
            typechange)
                if [ -f "$repo/$mp" ]; then
                    rm "$repo/$mp"
                    ln -s nowhere "$repo/$mp"
                fi ;;
            new)
                mkdir -p "$repo/$(dirname "$mp")" 2>/dev/null || true
                printf 'new\n' > "$repo/$mp" ;;
            esac
            ;;
        LIST) lists="$lists $rest" ;;
        esac
    done << EOF
$plan
EOF
    find "$repo" -newer "$repo/.git/HEAD" -exec \
        touch -h -d '2024-06-01 00:00:00' {} + 2>/dev/null || true

    # Oracle state, batched: tracked paths, porcelain map.
    g -C "$repo" ls-files > "$work/tracked.txt"
    g -C "$repo" status --porcelain=v1 --ignored=matching \
        > "$work/porc.txt"

    bad=""
    for sub in $lists; do
        dir="$repo"
        rel=""
        if [ "$sub" != "." ]; then
            dir="$repo/$sub"
            rel="$sub/"
        fi
        [ -d "$dir" ] || continue
        run_uut -l --git "$dir" > "$work/l.out" 2>/dev/null
        LC_ALL=C awk 'NR > 1 {
            if (NF >= 4 && $(NF - 1) == "->")
                print $(NF - 3) "\t" $(NF - 2)
            else
                print $(NF - 1) "\t" $NF
        }' "$work/l.out" > "$work/cells.txt"
        while IFS="$(printf '\t')" read -r cell name; do
            p="$rel$name"
            want=""
            if LC_ALL=C grep -q "^$p\$" "$work/tracked.txt"; then
                pl=$(LC_ALL=C grep -F " $p" "$work/porc.txt" \
                    | LC_ALL=C awk -v p="$p" 'substr($0, 4) == p' \
                    | head -1)
                case "$pl" in
                "") want='-' ;;
                UU*|AA*|DD*|AU*|UA*|DU*|UD*) want=U ;;
                *)
                    case "$(printf '%s' "$pl" | cut -c2)" in
                    M) want=M ;;
                    T) want=T ;;
                    ' ') want='-' ;;
                    *) want='-' ;;
                    esac
                    ;;
                esac
            elif LC_ALL=C grep -q "^$p/" "$work/tracked.txt"; then
                want='-'        # tracked dir: one decision
            else
                if g -C "$repo" check-ignore -q -- "$p" 2>/dev/null; then
                    want=I
                else
                    want=N
                fi
            fi
            if [ "$cell" != "-$want" ]; then
                bad="$p: liszt '$cell' want '-$want' (list $sub)"
                break
            fi
        done < "$work/cells.txt"
        [ -n "$bad" ] && break

        # --git-ignore: visible set = tracked or not-ignored.
        run_uut --git-ignore -1 "$dir" > "$work/gi.out" 2>/dev/null
        while IFS= read -r name; do
            p="$rel$name"
            if ! LC_ALL=C grep -q "^$p\$\|^$p/" "$work/tracked.txt" \
                && g -C "$repo" check-ignore -q -- "$p" 2>/dev/null; then
                bad="--git-ignore shows ignored $p (list $sub)"
                break
            fi
        done < "$work/gi.out"
        [ -n "$bad" ] && break
    done

    if [ -n "$bad" ]; then
        fails=$((fails + 1))
        mkdir -p "$faildir"
        cp -R "$repo" "$faildir/trial-$t" 2>/dev/null || true
        echo "GIT FUZZ FAIL trial $t: $bad" >&2
        echo "  seed=$seed" >&2
    fi
done

echo "tests/fuzz/run-git: $trials trials, $fails failures"
[ "$fails" -eq 0 ]
