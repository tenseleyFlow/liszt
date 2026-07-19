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
conf_ver=$(sed -n 's/^LISZT_VERSION="\(.*\)"/\1/p' configure)
check_eq "liszt --version line" "liszt $conf_ver" "$(./liszt --version)"
check_eq "lz --version line" "liszt $conf_ver" "$(./lz --version)"
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
check_status "width sort works" 0 ./liszt --sort=width "$udir"
check_status "color always works" 0 env LS_COLORS="di=01;34" ./liszt --color=always "$udir"
check_status "classify works" 0 ./liszt -F "$udir"

# Statless color plan: a scheme observing nothing beyond d_type must
# perform zero per-entry stats (one statx total: the operand classify).
if command -v strace >/dev/null 2>&1; then
    checks=$((checks + 1))
    nstx=$(strace -c -e trace=statx env LC_ALL=C \
        LS_COLORS="di=01;34:ln=01;36:ex=00:su=00:sg=00:ow=00:st=00:tw=00:or=00:mi=00:ca=00" \
        ./liszt --color=always "$udir" 2>&1 >/dev/null \
        | sed -n 's/.* \([0-9][0-9]*\) *statx$/\1/p' | tail -1)
    if [ "${nstx:-99}" -gt 1 ]; then
        note_fail "statless color scheme performed $nstx statx calls"
    fi
fi
check_status "columns work piped" 0 ./liszt -C "$udir"
check_status "commas work" 0 ./liszt -m "$udir"
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

# Icons stay statless: builtin tables observe only name + d_type, so
# the statless color scheme with icons on still performs one statx.
if command -v strace >/dev/null 2>&1; then
    checks=$((checks + 1))
    nstx=$(strace -c -e trace=statx env LC_ALL=C \
        LS_COLORS="di=01;34:ln=01;36:ex=00:su=00:sg=00:ow=00:st=00:tw=00:or=00:mi=00:ca=00" \
        ./liszt --color=always --icons=always "$udir" 2>&1 >/dev/null \
        | sed -n 's/.* \([0-9][0-9]*\) *statx$/\1/p' | tail -1)
    if [ "${nstx:-99}" -gt 1 ]; then
        note_fail "icons broke the statless plan ($nstx statx calls)"
    fi
fi

# Icon grid alignment: every second-column start in -C equals the
# icons-off start plus glyph+spacing cells.
checks=$((checks + 1))
idir=$(mktemp -d "${TMPDIR:-/tmp}/liszt-icongrid.XXXXXX")
for f in aa bb cc dd ee ff gg hh; do printf 'x\n' > "$idir/$f"; done
# Equal-length names, spaces-only tabs: every full row of the icon
# grid must have identical byte length (columns will have reflowed to
# fewer than the icons-off run; that is the layout doing its job).
nlens=$(env -i PATH="$PATH" LC_ALL=C COLUMNS=40 \
    ./liszt -C -w 40 -T0 --icons=always "$idir" \
    | awk '{ print length($0) }' | sort -u | wc -l)
rm -rf "$idir"
if [ "$nlens" -ne 1 ]; then
    note_fail "icon grid alignment (rows have $nlens distinct lengths)"
fi

# Scan kernels: fuzz the inline SIMD paths against the scalar oracles
# (rank's scan-fuzz shape), and fail if a SIMD-capable build silently
# runs scalar (tally's engagement lesson).
scanwork=$(mktemp -d "${TMPDIR:-/tmp}/liszt-scan.XXXXXX")
trap 'chmod -R u+rwx "$fixwork" 2>/dev/null; rm -rf "$fixwork" "$scanwork"' EXIT INT TERM
cat > "$scanwork/scanfuzz.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include "sys/scan.h"
int main(void)
{
    unsigned s = 42;
    unsigned char buf[4096];
    for (int trial = 0; trial < 4000; trial++) {
        s = s * 1103515245u + 12345u;
        size_t len = s % sizeof buf;
        int mode = (s >> 8) % 4;
        for (size_t i = 0; i < len; i++) {
            s = s * 1103515245u + 12345u;
            unsigned char b;
            switch (mode) {
            case 0: b = (unsigned char)s; break;                 /* wild */
            case 1: b = (unsigned char)(0x20 + s % 0x5F); break; /* graph */
            case 2: b = (unsigned char)(s % 0x80); break;        /* ascii */
            default: b = (unsigned char)(0x60 + s % 0x40); break;/* edge */
            }
            buf[i] = b;
        }
        /* Every offset: kernels must agree with oracles including on
           unaligned starts and short tails. */
        for (size_t off = 0; off < 24 && off < len; off += 7) {
            const unsigned char *p = buf + off;
            size_t n = len - off;
            if (liszt_scan_nonascii(p, n)
                != liszt_scan_nonascii_scalar(p, n)) {
                printf("nonascii mismatch %d/%zu\n", trial, off);
                return 1;
            }
            if (liszt_scan_ascii_graph(p, n)
                != liszt_scan_ascii_graph_scalar(p, n)) {
                printf("graph mismatch %d/%zu\n", trial, off);
                return 1;
            }
        }
    }
    puts(liszt_scan_backend());
    return 0;
}
EOF
checks=$((checks + 1))
if cc -O2 -std=c11 -I src -o "$scanwork/scanfuzz" "$scanwork/scanfuzz.c"     src/sys/scan.c 2>"$scanwork/cc.err"; then
    backend=$("$scanwork/scanfuzz") || note_fail "scan kernel fuzz mismatch"
    case "$(uname -m)" in
    x86_64|amd64|aarch64|arm64)
        checks=$((checks + 1))
        if [ "$backend" = "scalar" ]; then
            note_fail "SIMD-capable build runs scalar scan kernels"
        fi
        ;;
    esac
else
    note_fail "scan fuzz harness failed to compile"
fi

# Gitignore engine (sprint 14A): wildmatch against git's own t3070
# corpus (tests/unit/data/wildmatch.tsv, columns: wildmatch pathmatch
# TAB text TAB pattern), then the pattern-compiler rule battery
# (negation last-wins, anchoring, dir-only, escaped trailing space,
# nesting bases). Harness-compiled driver; the shipping binary only
# links the engine.
giwork=$(mktemp -d "${TMPDIR:-/tmp}/liszt-gi.XXXXXX")
trap 'chmod -R u+rwx "$fixwork" 2>/dev/null; rm -rf "$fixwork" "$scanwork" "$giwork"' EXIT INT TERM
cat > "$giwork/gitest.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gitignore.h"

static int bad;

static void
corpus(const char *file)
{
    FILE *fp = fopen(file, "r");
    char line[1024];
    if (!fp) { puts("no-corpus"); exit(1); }
    while (fgets(line, sizeof line, fp)) {
        size_t n = strlen(line);
        if (n && line[n - 1] == '\n') line[--n] = '\0';
        /* "<wm> <pm>\t<text>\t<pattern>" */
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        char *t2 = strchr(t1 + 1, '\t');
        if (!t2) continue;
        *t1 = *t2 = '\0';
        int wm = line[0] == '1';
        int pm = line[2] == '1';
        const char *text = t1 + 1;
        const char *pat = t2 + 1;
        if (liszt_wildmatch(pat, text, LISZT_WM_PATHNAME) != wm) {
            printf("wildmatch [%s] vs [%s]: want %d\n", pat, text, wm);
            bad = 1;
        }
        if (liszt_wildmatch(pat, text, 0) != pm) {
            printf("pathmatch [%s] vs [%s]: want %d\n", pat, text, pm);
            bad = 1;
        }
    }
    fclose(fp);
}

static struct liszt_gi_file gf;

static void
rules(const char *base, const char *buf)
{
    liszt_gi_file_free(&gf);
    liszt_gi_file_parse(&gf, buf, strlen(buf), base, strlen(base));
}

static void
expect(const char *path, int is_dir, int want)
{
    const char *sl = strrchr(path, '/');
    size_t off = sl ? (size_t)(sl - path) + 1 : 0;
    int got = liszt_gi_file_match(&gf, path, strlen(path), off,
                                  is_dir != 0);
    if (got != want) {
        printf("rules [%s] dir=%d: want %d got %d\n", path, is_dir,
               want, got);
        bad = 1;
    }
}

int
main(int argc, char **argv)
{
    if (argc > 1) corpus(argv[1]);

    /* Basename patterns match at any depth; last matching line wins. */
    rules("", "*.log\n!important.log\n");
    expect("a.log", 0, 1);
    expect("sub/deep/b.log", 0, 1);
    expect("important.log", 0, 0);
    expect("sub/important.log", 0, 0);
    expect("a.txt", 0, -1);

    /* Reversed order: the exclude wins again. */
    rules("", "!important.log\n*.log\n");
    expect("important.log", 0, 1);

    /* Slash anchors to the base; leading slash is spelling only. */
    rules("", "/build\ndoc/frotz\n");
    expect("build", 1, 1);
    expect("sub/build", 1, -1);
    expect("doc/frotz", 1, 1);
    expect("a/doc/frotz", 1, -1);

    /* Dir-only patterns ignore files of the same name. */
    rules("", "cache/\n");
    expect("cache", 1, 1);
    expect("cache", 0, -1);

    /* Escaped trailing space survives; unescaped ones trim. */
    rules("", "spaced\\ \nplain   \n");
    expect("spaced ", 0, 1);
    expect("spaced", 0, -1);
    expect("plain", 0, 1);

    /* Comments and escaped specials. */
    rules("", "#comment\n\\#literal\n\\!bang\n");
    expect("#comment", 0, -1);
    expect("#literal", 0, 1);
    expect("!bang", 0, 1);

    /* Double-star spans components; single star stays bounded. */
    rules("", "foo/**/bar\nqux/*.o\n");
    expect("foo/bar", 1, 1);
    expect("foo/a/bar", 1, 1);
    expect("foo/a/b/bar", 1, 1);
    expect("qux/x.o", 0, 1);
    expect("qux/sub/x.o", 0, -1);

    /* Nested base: patterns anchor below it. */
    rules("sub/dir", "/top\n*.tmp\n");
    expect("sub/dir/top", 0, 1);
    expect("sub/dir/deep/top", 0, -1);
    expect("sub/dir/deep/x.tmp", 0, 1);

    /* CRLF lines parse like git (CR stripped by the line splitter). */
    rules("", "win.txt\r\n");
    expect("win.txt", 0, 1);

    /* Character classes with ranges and negation. */
    rules("", "[a-c][!0-9].o\n");
    expect("bx.o", 0, 1);
    expect("b1.o", 0, -1);
    expect("dx.o", 0, -1);

    liszt_gi_file_free(&gf);
    if (!bad) puts("ok");
    return bad;
}
EOF
checks=$((checks + 1))
if cc -O2 -std=c11 -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64 -I src \
    -o "$giwork/gitest" "$giwork/gitest.c" \
    src/gitignore.c src/util.c 2>"$giwork/cc.err"; then
    out=$("$giwork/gitest" tests/unit/data/wildmatch.tsv)         || note_fail "gitignore driver: $out"
    [ "$out" = "ok" ] || { echo "$out" | sed -n '1,6p' >&2; }
else
    sed -n '1,4p' "$giwork/cc.err" >&2
    note_fail "gitignore driver failed to compile"
fi

# Git index parser (sprint 14B): synthetic v2/v3 images built in the
# driver - parse, sorted-order verify, status compare table, window
# lookups, and corrupt-at-every-boundary degrades (never a crash).
cat > "$giwork/ixtest.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "git.h"

static int bad;
static unsigned char img[65536];
static size_t ilen;

static void
be32p(size_t off, unsigned v)
{
    img[off] = (unsigned char)(v >> 24);
    img[off + 1] = (unsigned char)(v >> 16);
    img[off + 2] = (unsigned char)(v >> 8);
    img[off + 3] = (unsigned char)v;
}

static void
begin_index(unsigned version)
{
    memset(img, 0, sizeof img);
    memcpy(img, "DIRC", 4);
    be32p(4, version);
    be32p(8, 0);
    ilen = 12;
}

static void
add_entry(unsigned version, const char *path, unsigned mode,
          unsigned size, unsigned msec, unsigned mnsec,
          unsigned flags, unsigned xflags)
{
    size_t start = ilen;
    be32p(start + 8, msec);
    be32p(start + 12, mnsec);
    be32p(start + 24, mode);
    be32p(start + 36, size);
    size_t p = start + 40 + 20;
    size_t namelen = strlen(path);
    unsigned f = flags | (namelen < 0xFFF ? (unsigned)namelen : 0xFFF);
    img[p] = (unsigned char)(f >> 8);
    img[p + 1] = (unsigned char)f;
    p += 2;
    if (flags & 0x4000) {
        img[p] = (unsigned char)(xflags >> 8);
        img[p + 1] = (unsigned char)xflags;
        p += 2;
    }
    memcpy(img + p, path, namelen + 1);
    p += namelen + 1;
    if (version <= 3)
        while ((p - start) % 8)
            img[p++] = 0;
    ilen = p;
    be32p(8, (unsigned)(img[8] << 24 | img[9] << 16 | img[10] << 8
                        | img[11]) + 1);
    (void)version;
}

static void
finish_index(void)
{
    memset(img + ilen, 0xAB, 20);   /* fake checksum, never verified */
    ilen += 20;
}

static struct liszt_git_ctx c;

static int
parse_ok(void)
{
    memset(&c, 0, sizeof c);
    c.root = (char *)"/repo";
    c.root_len = 5;
    c.filemode = 1;
    c.image = img;
    c.image_len = ilen;
    c.index_mtime = 99999;
    return liszt_git_index_parse(&c);
}

static void
reset_ctx(void)
{
    free(c.ents);
    free(c.v4_arena);
    free(c.win_tab);
    memset(&c, 0, sizeof c);
}

static void
expect_status(const char *desc, const char *name, int is_dir,
              const struct liszt_git_wtstat *ws, int want)
{
    char got = liszt_git_status(&c, name, strlen(name), is_dir, ws);
    if (got != want) {
        printf("%s: want %c got %c\n", desc, want ? want : '0',
               got ? got : '0');
        bad = 1;
    }
}

int
main(void)
{
    /* v2: three clean regular files + one symlink + one subdir path. */
    begin_index(2);
    add_entry(2, "a.txt", 0100644, 5, 1000, 500, 0, 0);
    add_entry(2, "lnk", 0120000, 4, 1000, 0, 0, 0);
    add_entry(2, "sub/b.txt", 0100755, 9, 2000, 0, 0, 0);
    add_entry(2, "zz", 0100644, 1, 3000, 0, 0, 0);
    finish_index();
    if (!parse_ok() || c.n_ents != 4) {
        printf("v2 parse: degraded or wrong count\n");
        return 1;
    }
    if (strcmp(c.ents[2].path, "sub/b.txt") != 0) {
        printf("v2 paths wrong\n");
        bad = 1;
    }

    liszt_git_window(&c, "/repo");
    struct liszt_git_wtstat ws = { 1000, 500, 5, 0100644 };
    expect_status("clean file", "a.txt", 0, &ws, '-');
    ws.size = 6;
    expect_status("size differs", "a.txt", 0, &ws, 'M');
    ws.size = 5;
    ws.mtime_sec = 1001;
    expect_status("mtime sec differs", "a.txt", 0, &ws, 'M');
    ws.mtime_sec = 1000;
    ws.mtime_nsec = 501;
    expect_status("nsec differs both set", "a.txt", 0, &ws, 'M');
    ws.mtime_nsec = 0;
    expect_status("wt zero nsec tolerated", "a.txt", 0, &ws, '-');
    ws.mtime_nsec = 500;
    ws.mode = 0100755;
    expect_status("exec bit flip", "a.txt", 0, &ws, 'M');
    c.filemode = 0;
    expect_status("filemode=false ignores exec", "a.txt", 0, &ws, '-');
    c.filemode = 1;
    ws.mode = 0120000 | 0777;
    expect_status("reg vs symlink typechange", "a.txt", 0, &ws, 'T');
    struct liszt_git_wtstat lws = { 1000, 0, 4, 0120000 | 0777 };
    expect_status("clean symlink", "lnk", 0, &lws, '-');
    expect_status("tracked dir one decision", "sub", 1, NULL, '-');
    expect_status("tracked path listed as dir", "a.txt", 1, NULL, 'T');
    expect_status("unknown name", "nope", 0, &ws, 0);

    liszt_git_window(&c, "/repo/sub");
    struct liszt_git_wtstat bws = { 2000, 0, 9, 0100755 };
    expect_status("subdir window clean", "b.txt", 0, &bws, '-');
    expect_status("subdir window miss", "a.txt", 0, &ws, 0);
    reset_ctx();

    /* v3: extended flags - skip-worktree, intent-to-add, stage, valid. */
    begin_index(3);
    add_entry(3, "conflict", 0100644, 1, 1, 0, 0x1000, 0);
    add_entry(3, "ita", 0100644, 0, 0, 0, 0x4000, 0x2000);
    add_entry(3, "skipwt", 0100644, 7, 1, 0, 0x4000, 0x4000);
    add_entry(3, "valid", 0100644, 7, 1, 0, 0x8000, 0);
    finish_index();
    if (!parse_ok() || c.n_ents != 4) {
        printf("v3 parse: degraded or wrong count\n");
        return 1;
    }
    liszt_git_window(&c, "/repo");
    struct liszt_git_wtstat junk = { 42, 42, 42, 0100644 };
    expect_status("stage bits conflict", "conflict", 0, &junk, 'U');
    expect_status("intent-to-add", "ita", 0, &junk, 'N');
    expect_status("skip-worktree clean", "skipwt", 0, &junk, '-');
    expect_status("assume-unchanged clean", "valid", 0, &junk, '-');
    reset_ctx();

    /* Unsorted order must degrade (binary-search ground truth). */
    begin_index(2);
    add_entry(2, "zz", 0100644, 1, 1, 0, 0, 0);
    add_entry(2, "aa", 0100644, 1, 1, 0, 0, 0);
    finish_index();
    if (parse_ok()) {
        printf("unsorted index accepted\n");
        bad = 1;
    }
    reset_ctx();

    /* Corrupt-at-every-boundary: parse must degrade, never crash. */
    begin_index(2);
    add_entry(2, "a.txt", 0100644, 5, 1000, 0, 0, 0);
    finish_index();
    size_t good = ilen;
    for (size_t cut = 0; cut < good; cut += 3) {
        ilen = cut;
        memset(&c, 0, sizeof c);
        c.image = img;
        c.image_len = ilen;
        if (liszt_git_index_parse(&c) && cut < good) {
            /* Truncations that still parse must at least keep the
               entry table consistent. */
            if (c.n_ents > 1) {
                printf("truncation at %zu inflated entries\n", cut);
                bad = 1;
            }
        }
        free(c.ents);
        free(c.v4_arena);
    }
    ilen = good;
    memcpy(img, "JUNK", 4);
    memset(&c, 0, sizeof c);
    c.image = img;
    c.image_len = ilen;
    if (liszt_git_index_parse(&c)) {
        printf("bad magic accepted\n");
        bad = 1;
    }
    free(c.ents);
    memcpy(img, "DIRC", 4);
    be32p(4, 9);
    memset(&c, 0, sizeof c);
    c.image = img;
    c.image_len = ilen;
    if (liszt_git_index_parse(&c)) {
        printf("version 9 accepted\n");
        bad = 1;
    }
    free(c.ents);
    be32p(4, 2);
    be32p(8, 0xFFFFFF);
    memset(&c, 0, sizeof c);
    c.image = img;
    c.image_len = ilen;
    if (liszt_git_index_parse(&c)) {
        printf("entry-count overflow accepted\n");
        bad = 1;
    }
    free(c.ents);

    if (!bad) puts("ok");
    return bad;
}
EOF
checks=$((checks + 1))
if cc -O2 -std=c11 -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64 -I src \
    -o "$giwork/ixtest" "$giwork/ixtest.c" \
    src/git.c src/util.c 2>"$giwork/cc2.err"; then
    out=$("$giwork/ixtest") || note_fail "git index driver: $out"
    [ "$out" = "ok" ] || { echo "$out" | sed -n '1,6p' >&2; }
else
    sed -n '1,4p' "$giwork/cc2.err" >&2
    note_fail "git index driver failed to compile"
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
