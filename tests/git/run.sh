#!/bin/sh
# Git-status extension harness: GIT ITSELF IS THE ORACLE. Fixture repos
# are built with the git binary (exit 77 when absent); expected letters
# derive from git status --porcelain=v1 --ignored=matching, git
# ls-files (the tracked-dir one-decision rule), and git check-ignore.
# Every mutation gets an mtime >= 2 s in the past so the racy-clean
# window cannot flake in CI.
#
# Git runs isolated (HOME/XDG redirected, GIT_CONFIG_NOSYSTEM): a user
# core.excludesFile would poison the oracle - liszt deliberately
# ignores it (documented punt).
#
# Exit: 0 pass, 1 fail, 77 skip (no git).
set -u
cd "$(dirname "$0")/../.." || exit 1

command -v git >/dev/null 2>&1 || {
    echo "tests/git: no git binary; skipping" >&2
    exit 77
}

work=$(mktemp -d "${TMPDIR:-/tmp}/liszt-git.XXXXXX") || exit 1
cleanup() {
    chmod -R u+rwx "$work" 2>/dev/null
    rm -rf "$work"
}
trap cleanup EXIT INT TERM

cp ./liszt "$work/liszt.uut" 2>/dev/null || true

fails=0
checks=0

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

old() {
    touch -d '2024-01-01 00:00:00' "$@"
}
old2() {
    touch -d '2024-06-01 00:00:00' "$@"
}

# Expected worktree letter for one repo-relative child.
# expected_letter REPO REL IS_DIR
expected_letter() {
    er=$1 rel=$2 edir=$3
    if [ -n "$(g -C "$er" ls-files -- "$rel" | head -1)" ]; then
        if [ "$edir" = 1 ]; then
            # Tracked directory: one decision, never an aggregate.
            gl=$(g -C "$er" ls-files -s -- "$rel" | head -1)
            case "$gl" in
            160000*) printf '%s' '-' ;;   # gitlink
            *) printf '%s' '-' ;;
            esac
            return
        fi
        pl=$(g -C "$er" status --porcelain=v1 --ignored=matching \
            -- "$rel" | head -1)
        case "$pl" in
        "") printf '%s' '-' ;;
        UU*|AA*|DD*|AU*|UA*|DU*|UD*) printf '%s' U ;;
        *)
            y=$(printf '%s' "$pl" | cut -c2)
            case "$y" in
            'M') printf '%s' M ;;
            'T') printf '%s' T ;;
            '?') printf '%s' N ;;
            '!') printf '%s' I ;;
            ' ') printf '%s' - ;;   # staged-only: cheap tier shows clean
            *) printf '%s' - ;;
            esac
            ;;
        esac
    else
        if g -C "$er" check-ignore -q -- "$rel" 2>/dev/null; then
            printf '%s' I
        else
            printf '%s' N
        fi
    fi
}

# case_dir DESC REPO SUBDIR(or "") -- compare every liszt cell in the
# listing of REPO/SUBDIR against the derived expectation.
case_dir() {
    cd_desc=$1 cd_repo=$2 cd_sub=$3
    cd_dir=$cd_repo${cd_sub:+/$cd_sub}
    checks=$((checks + 1))
    run_uut -l --git "$cd_dir" > "$work/l.out" 2>/dev/null
    # cell/name extraction; symlink lines carry "-> target".
    LC_ALL=C awk 'NR > 1 {
        if (NF >= 4 && $(NF - 1) == "->")
            print $(NF - 3) "\t" $(NF - 2)
        else
            print $(NF - 1) "\t" $NF
    }' "$work/l.out" > "$work/cells.txt"
    ok=1
    while IFS="$(printf '\t')" read -r cell name; do
        rel=${cd_sub:+$cd_sub/}$name
        edir=0
        [ -d "$cd_dir/$name" ] && [ ! -L "$cd_dir/$name" ] && edir=1
        want="-$(expected_letter "$cd_repo" "$rel" "$edir")"
        if [ "$cell" != "$want" ]; then
            echo "GIT CASE FAIL [$cd_desc] $rel:" \
                "liszt '$cell' want '$want'" >&2
            ok=0
        fi
    done < "$work/cells.txt"
    [ "$ok" -eq 1 ] || fails=$((fails + 1))
}

# --- Fixture: the kitchen-sink repo --------------------------------------
repo="$work/repo"
mkdir -p "$repo/sub" "$repo/tracked-dir" "$repo/mixed-dir"
printf 'clean\n' > "$repo/clean.txt"
printf 'v1\n' > "$repo/mod.txt"
printf 'aaaa\n' > "$repo/modsame.txt"
printf 'x\n' > "$repo/chmod.txt"
printf 'target\n' > "$repo/type.txt"
printf 'skip\n' > "$repo/skipwt.txt"
printf 'assume\n' > "$repo/assume.txt"
printf 'in\n' > "$repo/sub/inner.txt"
printf 'keep\n' > "$repo/sub/committed.txt"
printf 'td\n' > "$repo/tracked-dir/file.txt"
printf 'mx\n' > "$repo/mixed-dir/tracked.txt"
old "$repo"/*.txt "$repo/sub"/*.txt "$repo/tracked-dir"/*.txt \
    "$repo/mixed-dir"/*.txt
printf '*.log\n!keep.log\nignored-dir/\n' > "$repo/.gitignore"
printf 'inner.txt\n' > "$repo/sub/.gitignore"
old "$repo/.gitignore" "$repo/sub/.gitignore"
g init -q "$repo"
g -C "$repo" add -A
g -C "$repo" add -f sub/inner.txt
g -C "$repo" commit -q -m base
printf 'excl-pat.txt\n' > "$repo/.git/info/exclude"

# Mutations (mtimes pushed to a second fixed past date).
printf 'v2-longer\n' > "$repo/mod.txt"
printf 'bbbb\n' > "$repo/modsame.txt"      # same size, new mtime
chmod 755 "$repo/chmod.txt"
rm "$repo/type.txt" && ln -s clean.txt "$repo/type.txt"
printf 'untracked\n' > "$repo/untracked.txt"
mkdir -p "$repo/untracked-dir" "$repo/ignored-dir" "$repo/empty-ign"
printf 'u\n' > "$repo/untracked-dir/u.txt"
printf 'i\n' > "$repo/ignored-dir/i.txt"
printf 'log\n' > "$repo/noise.log"
printf 'keep\n' > "$repo/keep.log"
printf 'ex\n' > "$repo/excl-pat.txt"
printf 'im\n' > "$repo/sub/inner-new.txt"  # matched by sub/.gitignore? no: inner.txt only
printf 'mixnew\n' > "$repo/mixed-dir/newfile.txt"
printf 'skip2\n' > "$repo/skipwt.txt"
printf 'assume2\n' > "$repo/assume.txt"
old2 "$repo/mod.txt" "$repo/modsame.txt" "$repo/untracked.txt" \
    "$repo/untracked-dir/u.txt" "$repo/ignored-dir/i.txt" \
    "$repo/noise.log" "$repo/keep.log" "$repo/excl-pat.txt" \
    "$repo/sub/inner-new.txt" "$repo/mixed-dir/newfile.txt" \
    "$repo/skipwt.txt" "$repo/assume.txt"
g -C "$repo" update-index --skip-worktree skipwt.txt
g -C "$repo" update-index --assume-unchanged assume.txt

case_dir "kitchen sink root" "$repo" ""
case_dir "subdir listing" "$repo" "sub"
case_dir "mixed dir" "$repo" "mixed-dir"

# sub/inner.txt is TRACKED despite sub/.gitignore naming it: tracked
# names are never ignored (index first, git's order).
checks=$((checks + 1))
cell=$(run_uut -l --git "$repo/sub" 2>/dev/null | LC_ALL=C awk \
    '$NF == "inner.txt" { print $(NF - 1) }')
if [ "$cell" != "--" ]; then
    echo "GIT CASE FAIL [tracked beats gitignore] got '$cell'" >&2
    fails=$((fails + 1))
fi

# --- conflict repo -------------------------------------------------------
crepo="$work/conflict"
mkdir -p "$crepo"
printf 'base\n' > "$crepo/both.txt"
old "$crepo/both.txt"
g init -q "$crepo"
g -C "$crepo" add -A && g -C "$crepo" commit -q -m base
g -C "$crepo" checkout -q -b side
printf 'side\n' > "$crepo/both.txt" && old2 "$crepo/both.txt"
g -C "$crepo" commit -qam side
g -C "$crepo" checkout -q trunk
printf 'trunk\n' > "$crepo/both.txt" && old2 "$crepo/both.txt"
g -C "$crepo" commit -qam trunk
g -C "$crepo" merge -q side >/dev/null 2>&1 || true
old2 "$crepo/both.txt"
case_dir "conflict repo" "$crepo" ""

# --- intent-to-add (explicit expectation: N, eza-compatible) -------------
irepo="$work/ita"
mkdir -p "$irepo"
printf 'c\n' > "$irepo/c.txt"
old "$irepo/c.txt"
g init -q "$irepo"
g -C "$irepo" add -A && g -C "$irepo" commit -q -m base
printf 'ita\n' > "$irepo/ita.txt"
old2 "$irepo/ita.txt"
g -C "$irepo" add -N ita.txt
checks=$((checks + 1))
cell=$(run_uut -l --git "$irepo" 2>/dev/null | LC_ALL=C awk \
    '$NF == "ita.txt" { print $(NF - 1) }')
if [ "$cell" != "-N" ]; then
    echo "GIT CASE FAIL [intent-to-add is N] got '$cell'" >&2
    fails=$((fails + 1))
fi

# --- gitlink (submodule shape): clean, never recursed --------------------
srepo="$work/subm"
mkdir -p "$srepo/child"
printf 'p\n' > "$srepo/p.txt"
old "$srepo/p.txt"
g init -q "$srepo"
g -C "$srepo" add -A && g -C "$srepo" commit -q -m base
g -C "$srepo" update-index --add --cacheinfo \
    160000,1234567890123456789012345678901234567890,child
printf 'dirty\n' > "$srepo/child/dirty.txt"
checks=$((checks + 1))
cell=$(run_uut -l --git "$srepo" 2>/dev/null | LC_ALL=C awk \
    '$NF == "child" { print $(NF - 1) }')
if [ "$cell" != "--" ]; then
    echo "GIT CASE FAIL [gitlink clean, not recursed] got '$cell'" >&2
    fails=$((fails + 1))
fi

# --- fresh repo (no index yet) -------------------------------------------
frepo="$work/fresh"
mkdir -p "$frepo"
printf 'f\n' > "$frepo/f.txt"
old "$frepo/f.txt"
g init -q "$frepo"
case_dir "fresh repo" "$frepo" ""

# --- worktree add (gitfile + commondir) ----------------------------------
if g -C "$repo" worktree add -q "$work/wt" >/dev/null 2>&1; then
    printf 'wtnew\n' > "$work/wt/wt-new.txt"
    printf 'wl\n' > "$work/wt/wt.log"
    old2 "$work/wt/wt-new.txt" "$work/wt/wt.log"
    case_dir "linked worktree" "$work/wt" ""
fi

# --- bare repo: no worktree of its own ------------------------------------
# Observable as "no column" only when nothing above $work is a repo (a
# stray /tmp/.git would enclose the fixture legitimately).
enclosing=0
g -C "$work" rev-parse --is-inside-work-tree >/dev/null 2>&1 \
    && enclosing=1
brepo="$work/bare"
g init -q --bare "$brepo"
if [ "$enclosing" -eq 0 ]; then
    checks=$((checks + 1))
    run_uut -l --git "$brepo" > "$work/b1.out" 2>/dev/null
    run_uut -l "$brepo" > "$work/b2.out" 2>/dev/null
    if ! cmp -s "$work/b1.out" "$work/b2.out"; then
        echo "GIT CASE FAIL [bare repo shows no column]" >&2
        fails=$((fails + 1))
    fi
else
    echo "tests/git: note: enclosing repo above workdir;" \
        "no-column checks skipped" >&2
fi

# --- nested independent repo: boundaries respected -----------------------
inner="$repo/inner-repo"
mkdir -p "$inner"
printf 'iv\n' > "$inner/iv.txt"
old "$inner/iv.txt"
g init -q "$inner"
g -C "$inner" add -A && g -C "$inner" commit -q -m base
printf 'inew\n' > "$inner/inew.txt"
old2 "$inner/inew.txt"
case_dir "nested repo inner view" "$inner" ""

# --- degraded repo: blank cells, no crash --------------------------------
drepo="$work/degraded"
mkdir -p "$drepo"
printf 'd\n' > "$drepo/d.txt"
old "$drepo/d.txt"
g init -q "$drepo"
g -C "$drepo" add -A && g -C "$drepo" commit -q -m base
printf 'JUNKJUNKJUNKJUNK' > "$drepo/.git/index"
checks=$((checks + 1))
run_uut -l --git "$drepo" > "$work/d1.out" 2>/dev/null
rc=$?
run_uut -l "$drepo" > "$work/d2.out" 2>/dev/null
n1=$(LC_ALL=C awk 'NR>1 { print length($0) }' "$work/d1.out" | sort -u)
n2=$(LC_ALL=C awk 'NR>1 { print length($0) + 3 }' "$work/d2.out" | sort -u)
if [ "$rc" -ne 0 ] || [ "$n1" != "$n2" ]; then
    echo "GIT CASE FAIL [degraded repo blank cells] rc=$rc" >&2
    fails=$((fails + 1))
fi

# --- --git-ignore filtering across formats -------------------------------
# Expected visible set: every child that is tracked or not ignored.
gi_expected() {
    er=$1
    for f in "$er"/* "$er"/.[!.]*; do
        [ -e "$f" ] || [ -L "$f" ] || continue
        b=$(basename "$f")
        [ "$b" = ".git" ] && printf '%s\n' "$b" && continue
        if [ -n "$(g -C "$er" ls-files -- "$b" | head -1)" ]; then
            printf '%s\n' "$b"
        elif ! g -C "$er" check-ignore -q -- "$b" 2>/dev/null; then
            printf '%s\n' "$b"
        fi
    done | LC_ALL=C sort
}
for fmt in -1 -C -l; do
    checks=$((checks + 1))
    run_uut --git-ignore $fmt -a "$repo" 2>/dev/null \
        | tr '\t' '\n' | tr -s ' ' '\n' | LC_ALL=C awk '
            /^total$/ { skip = 2; next }
            skip > 0 { skip--; next }
            NF' > "$work/gi-raw.txt"
    # Formats decorate differently; assert set membership on names.
    ok=1
    gi_expected "$repo" > "$work/gi-want.txt"
    while IFS= read -r want; do
        grep -qx "$(printf '%s' "$want" | sed 's/[][\\.*^$]/\\&/g')" \
            "$work/gi-raw.txt" || ok=0
    done < "$work/gi-want.txt"
    for hid in noise.log ignored-dir excl-pat.txt; do
        grep -qx "$hid" "$work/gi-raw.txt" && ok=0
    done
    if [ "$ok" -ne 1 ]; then
        echo "GIT CASE FAIL [--git-ignore $fmt]" >&2
        fails=$((fails + 1))
    fi
done

# Ignored contents under an ignored dir stay hidden (no re-include).
checks=$((checks + 1))
if run_uut --git-ignore -1 "$repo/ignored-dir" 2>/dev/null \
    | grep -q .; then
    echo "GIT CASE FAIL [ancestor-ignored dir listing not empty]" >&2
    fails=$((fails + 1))
fi

# --- --no-git suppressor -------------------------------------------------
checks=$((checks + 1))
run_uut -l --git --no-git "$repo" > "$work/ng1.out" 2>/dev/null
run_uut -l "$repo" > "$work/ng2.out" 2>/dev/null
if ! cmp -s "$work/ng1.out" "$work/ng2.out"; then
    echo "GIT CASE FAIL [--no-git suppresses]" >&2
    fails=$((fails + 1))
fi

# --- command-line operands -----------------------------------------------
checks=$((checks + 1))
run_uut -l --git "$repo/mod.txt" "$repo/clean.txt" \
    > "$work/op.out" 2>/dev/null
mcell=$(LC_ALL=C awk '$NF ~ /mod.txt$/ { print $(NF - 1) }' "$work/op.out")
ccell=$(LC_ALL=C awk '$NF ~ /clean.txt$/ { print $(NF - 1) }' "$work/op.out")
if [ "$mcell" != "-M" ] || [ "$ccell" != "--" ]; then
    echo "GIT CASE FAIL [operands] mod='$mcell' clean='$ccell'" >&2
    fails=$((fails + 1))
fi

# --- identity: --git off the long format is silently inert ---------------
checks=$((checks + 1))
run_uut --git -1 "$repo" > "$work/i1.out" 2>/dev/null
run_uut -1 "$repo" > "$work/i2.out" 2>/dev/null
if ! cmp -s "$work/i1.out" "$work/i2.out"; then
    echo "GIT CASE FAIL [--git inert outside long]" >&2
    fails=$((fails + 1))
fi

echo "tests/git: $checks checks, $fails failures"
[ "$fails" -eq 0 ]
