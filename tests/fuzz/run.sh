#!/bin/sh
# Differential fuzzer: random trees x random accepted flag sets, liszt vs
# the pinned GNU ls oracle, comparing exit status, stdout bytes, and
# normalized stderr.
#
# Deterministic per (FUZZ_SEED, trial) on a given box; awk's rand()
# differs across awk implementations, so repro requires the same box
# (rank's precedent; tally's C generator is the upgrade path if
# cross-platform repro ever matters).
#
# Diagnostics render through the quotearg port (quoteaf) since 04F, so
# missing-operand names may take any shape; existing-file names use
# every class - stdout passes raw bytes through.
#
# Env: FUZZ_TRIALS (default 100), FUZZ_SEED (default 42).
# Exit: 0 pass, 1 fail, 77 skip (no oracle).
set -u
cd "$(dirname "$0")/../.." || exit 1

trials="${FUZZ_TRIALS:-100}"
seed="${FUZZ_SEED:-42}"

oracle=$(sh scripts/find-gnu-ls.sh) || {
    echo "tests/fuzz: no GNU coreutils ls oracle found; skipping" >&2
    exit 77
}

oracle_is_pin=0
"$oracle" --version | sed -n 1p | grep -q " 9\.11$" && oracle_is_pin=1

utf8_locale=""
for loc in en_US.UTF-8 en_US.utf8 C.UTF-8 C.utf8; do
    if locale -a 2>/dev/null | grep -qix "$loc"; then
        utf8_locale="$loc"
        break
    fi
done

work=$(mktemp -d "${TMPDIR:-/tmp}/liszt-fuzz.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT INT TERM
faildir="tests/.work/fuzz-failures"

cp ./liszt "$work/liszt.uut"
FUZZ_LSC=$(dircolors -b 2>/dev/null | sed -n "s/^LS_COLORS='\(.*\)';\$/\1/p")

normprog() {
    sed -e 's/^[^:][^:]*:/PROG:/' \
        -e "s|Try '[^']* --help'|Try 'PROG --help'|"
}

# Emit a trial plan: TREE lines (octal-escaped creation commands) then one
# ARGS line. Consumed line-by-line below.
gen_plan() {
    awk -v seed="$seed" -v trial="$1" -v pin="$oracle_is_pin" 'BEGIN {
        srand(seed + trial)
        r = int(rand() * 1000000)

        # Deviation D1: liszt intentionally diverges from GNU when raw
        # control bytes meet width-using layout (see .docs/deviations.md),
        # so those combinations are excluded from differential trials.
        # The layout/quoting shape is drawn FIRST so name classes can
        # avoid raw control bytes when D1 would fire.
        fmtpick = rand()
        stylepick = rand()
        si = 0
        if (stylepick < 0.35) si = int(rand() * 10) + 1
        qpick = rand()
        widthsort = rand()
        layoutish = (fmtpick >= 0.5 && fmtpick < 0.85) || widthsort < 0.1
        styled = si >= 4    # styles that escape control bytes
        qmark = qpick < 0.2
        avoid_ctl = layoutish && !styled && !qmark

        alpha = "abcdefghijklmnopqrstuvwxyz0123456789"
        nfiles = int(rand() * 25)
        for (i = 0; i < nfiles; i++) {
            cls = int(rand() * 7)
            len = 1 + int(rand() * 12)
            name = ""
            if (cls == 0 || cls == 1) {          # plain
                for (j = 0; j < len; j++)
                    name = name substr(alpha, int(rand() * 36) + 1, 1)
            } else if (cls == 2) {               # blanks mixed in
                for (j = 0; j < len; j++) {
                    if (rand() < 0.3) name = name " "
                    else name = name substr(alpha, int(rand() * 36) + 1, 1)
                }
                sub(/^ /, "x", name)             # avoid confusing ls args
            } else if (cls == 3 && avoid_ctl) {  # D1: reroll to plain
                for (j = 0; j < len; j++)
                    name = name substr(alpha, int(rand() * 36) + 1, 1)
            } else if (cls == 3) {               # control bytes
                name = "c"
                ctl = "\\011:\\033:\\007:\\013"
                split(ctl, cs, ":")
                for (j = 0; j < len; j++) {
                    if (rand() < 0.4) name = name cs[int(rand() * 4) + 1]
                    else name = name substr(alpha, int(rand() * 36) + 1, 1)
                }
            } else if (cls == 4) {               # utf8
                pick = int(rand() * 4)
                if (pick == 0) name = "u\\303\\251-" i        # e-acute
                else if (pick == 1) name = "u\\346\\227\\245" i
                else if (pick == 2) name = "u\\355\\225\\234" i
                else name = "u\\360\\237\\216\\274" i
            } else if (cls == 5) {               # dotfiles / backups
                if (rand() < 0.5) name = "." substr(alpha, int(rand()*26)+1, 1) i
                else name = substr(alpha, int(rand()*26)+1, 1) i "~"
            } else {                             # dash-leading
                name = "-" substr(alpha, int(rand() * 26) + 1, 1) i
            }
            print "F " name
        }
        if (rand() < 0.5) print "D subdir"
        if (rand() < 0.4) print "F subdir/inner" int(rand() * 10)
        if (rand() < 0.4) print "L goodlink subdir"
        if (rand() < 0.3) print "L deadlink zz-nothing"

        # Sort surface (sprint 02): every implemented word plus -r,
        # -f, and grouping.
        p = rand()
        avoid_ctl = 0
        if (p < 0.25) flags = "-U"
        else if (p < 0.35) flags = "-t"
        else if (p < 0.45) flags = "-S"
        else if (p < 0.55) flags = "-v"
        else if (p < 0.62) flags = "-X"
        else if (p < 0.68) flags = pin ? "-f" : "-U"
        else if (p < 0.74) flags = "--sort=version"
        else flags = ""             # default name sort
        if (rand() < 0.3) flags = flags " -r"
        if (rand() < 0.25) flags = flags " --group-directories-first"
        p = fmtpick
        if (p < 0.2) flags = flags " -1"
        else if (p < 0.3) flags = flags " --format=single-column"
        else if (p < 0.5) flags = flags " -l"
        else if (p < 0.65) flags = flags " -C -w " int(rand() * 100)
        else if (p < 0.75) flags = flags " -x -w " int(rand() * 100)
        else if (p < 0.85) flags = flags " -m -w " int(rand() * 100)
        # else: piped default resolves to one-per-line
        if (si > 0) {
            split("literal shell shell-always shell-escape " \
                  "shell-escape-always c c-maybe escape locale clocale",
                  qsty, " ")
            flags = flags " --quoting-style=" qsty[si]
        }
        if (qmark) flags = flags " -q"
        if (widthsort < 0.1) flags = flags " --sort=width"
        p = rand()
        if (p < 0.25) flags = flags " --color=always"
        else if (p < 0.3) flags = flags " --color=auto"
        p = rand()
        if (p < 0.15) flags = flags " -F"
        else if (p < 0.25) flags = flags " -p"
        else if (p < 0.32) flags = flags " --file-type"
        p = rand()
        if (p < 0.2) flags = flags " -R"
        else if (p < 0.28) flags = flags " -d"
        p = rand()
        if (p < 0.12) flags = flags " -H"
        else if (p < 0.24) flags = flags " -L"
        p = rand()
        if (p < 0.35) flags = flags " -a"
        else if (p < 0.6) flags = flags " -A"
        # Long-lane extras compose with any format (frills) or -l.
        p = rand()
        if (p < 0.1) flags = flags " -u"
        else if (p < 0.2) flags = flags " -c"
        else if (p < 0.25) flags = flags " --time=birth"
        p = rand()
        if (p < 0.08) flags = flags " --time-style=iso"
        else if (p < 0.16) flags = flags " --time-style=full-iso"
        else if (p < 0.22) flags = flags " --full-time"
        p = rand()
        if (p < 0.1) flags = flags " -B"
        else if (p < 0.18) flags = flags " -I u*"
        else if (p < 0.24) flags = flags " --hide=?*4"
        p = rand()
        if (p < 0.08) flags = flags " --zero"
        else if (p < 0.16) flags = flags " -D"
        else if (p < 0.24) flags = flags " --hyperlink=always"
        if (rand() < 0.2) flags = flags " -s"
        if (rand() < 0.2) flags = flags " -i"
        if (rand() < 0.15) flags = flags " -n"
        if (rand() < 0.15) flags = flags " -h"
        if (rand() < 0.1) flags = flags " -G"
        sub(/^ /, "", flags)

        # Operands.
        ops = "TREE"
        p = rand()
        if (p < 0.15) ops = "TREE TREE/subdir"
        else if (p < 0.3) ops = "FIRSTFILE TREE"
        else if (p < 0.4) ops = "MISSING TREE"
        else if (p < 0.5) ops = "-- TREE"

        style = (rand() < 0.25) ? "permuted" : "normal"
        # Flags after "--" are operands; that voids -U and needs sprint 02.
        if (ops ~ /^--/) style = "normal"
        lc = (rand() < 0.5) ? "C" : "UTF8"
        print "ARGS " lc " " style " " flags " | " ops
    }'
}

fails=0
t=0
while [ "$t" -lt "$trials" ]; do
    t=$((t + 1))
    tree="$work/t$t"
    rm -rf "$tree"
    mkdir -p "$tree"
    firstfile=""

    plan=$(gen_plan "$t")
    argline=""
    while IFS= read -r line; do
        kind=${line%% *}
        rest=${line#* }
        case "$kind" in
        F)
            name=$(printf '%b' "$rest")
            case "$name" in */*) mkdir -p "$tree/$(dirname "$name")" ;; esac
            printf 'x\n' > "$tree/$name" 2>/dev/null || true
            [ -n "$firstfile" ] || firstfile="$tree/$name"
            ;;
        D) mkdir -p "$tree/$rest" ;;
        L) ln -s "${rest#* }" "$tree/${rest%% *}" 2>/dev/null || true ;;
        ARGS) argline="$rest" ;;
        esac
    done << EOF
$plan
EOF

    lc=${argline%% *}; argline=${argline#* }
    style=${argline%% *}; argline=${argline#* }
    flags=${argline%% | *}
    ops=${argline#* | }
    [ "$lc" = "UTF8" ] && lc="${utf8_locale:-C}" || lc="C"

    # Expand operand tokens.
    set --
    for tok in $ops; do
        case "$tok" in
        TREE) set -- "$@" "$tree" ;;
        TREE/subdir) set -- "$@" "$tree/subdir" ;;
        FIRSTFILE) set -- "$@" "${firstfile:-$tree}" ;;
        MISSING) set -- "$@" "$work/zz-missing-$t" ;;
        --) set -- "$@" -- ;;
        esac
    done
    if [ -z "$flags" ]; then
        : # default sort, no flag words
    fi
    # set -f: flag words now carry glob patterns (-I u*) that must reach
    # the tools literally, not expanded against the script's cwd.
    set -f
    if [ "$style" = "permuted" ]; then
        set -- "$@" $flags
    else
        set -- $flags "$@"
    fi
    set +f

    env -i PATH="$PATH" LC_ALL="$lc" TZ=UTC0 COLUMNS=80 \
        LS_COLORS="$FUZZ_LSC" LISZT_DEBUG_VERIFY=1 \
        "$work/liszt.uut" "$@" > "$work/u.out" 2> "$work/u.raw"
    urc=$?
    env -i PATH="$PATH" LC_ALL="$lc" TZ=UTC0 COLUMNS=80 \
        LS_COLORS="$FUZZ_LSC" \
        "$oracle" "$@" > "$work/o.out" 2> "$work/o.raw"
    orc=$?
    normprog < "$work/u.raw" > "$work/u.err"
    normprog < "$work/o.raw" > "$work/o.err"

    if [ "$urc" -ne "$orc" ] || ! cmp -s "$work/u.out" "$work/o.out" \
        || ! cmp -s "$work/u.err" "$work/o.err"; then
        fails=$((fails + 1))
        mkdir -p "$faildir"
        cp -R "$tree" "$faildir/trial-$t" 2>/dev/null || true
        {
            echo "seed=$seed trial=$t lc=$lc"
            echo "args: $*"
            echo "rc uut=$urc oracle=$orc"
        } > "$faildir/trial-$t.repro"
        echo "FUZZ FAIL trial $t (seed $seed, lc $lc): args: $*" >&2
        diff "$work/o.out" "$work/u.out" | head -6 >&2
        diff "$work/o.err" "$work/u.err" | head -6 >&2
        echo "repro saved under $faildir/trial-$t*" >&2
        if [ "$fails" -ge 3 ]; then
            echo "tests/fuzz: aborting after 3 failures" >&2
            exit 1
        fi
    fi
done

if [ "$fails" -gt 0 ]; then
    echo "tests/fuzz: $fails/$trials trials failed (seed $seed)" >&2
    exit 1
fi
echo "tests/fuzz: $trials trials passed (seed $seed, oracle $oracle)"
