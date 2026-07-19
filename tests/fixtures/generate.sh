#!/bin/sh
# Deterministic fixture generator, core (unprivileged) tier.
#
# Usage: sh tests/fixtures/generate.sh DESTDIR [SEED]
# Creates DESTDIR/core and prints "MANIFEST_SHA256 <hash>" on stdout.
#
# Determinism contract: for a given seed (and generation day - see mtimes
# below), two runs produce trees with identical manifests (names, types,
# modes, sizes, mtimes, link targets). readdir order is NOT part of the
# contract; goldens compare both tools on the same instance.
#
# mtimes: the "older" side is pinned absolute (2020-01-15 12:00:00 UTC,
# far past any six-month cutoff). The "recent" side is midnight UTC of the
# generation day - deterministic within a day, and goldens always compare
# both tools against the same instance at the same wall clock.
#
# Skip-cleanly rules: sockets, setfacl, and invalid-multibyte names are
# attempted and silently skipped where the platform/filesystem refuses
# (e.g. APFS rejects invalid UTF-8 names; some filesystems lack ACLs).
# Everything skipped is absent from the manifest, so determinism holds per
# platform, not across platforms.
#
# Privileged tier (NOT generated here, documented for completeness):
# device nodes (mknod), setcap binaries (security.capability), SELinux
# labels, bind-mount cycles. Requires root and platform support; CI and
# default runs skip it. It will live behind LISZT_FIXTURE_PRIVILEGED=1
# when a sprint needs it (03E devices, 08D labels).
set -u

dest="${1:?usage: generate.sh DESTDIR [SEED]}"
seed="${2:-42}"

root="$dest/core"
if [ -d "$root" ]; then
    chmod -R u+rwx "$root" 2>/dev/null || true
fi
rm -rf "$root"
mkdir -p "$root"

old_ts="2020-01-15 12:00:00 UTC"
recent_ts="$(date -u +%Y-%m-%d) 00:00:00 UTC"

# --- plain names and name shapes -----------------------------------------

mkdir -p "$root/plain" "$root/shapes" "$root/meta" "$root/links" \
    "$root/sizes" "$root/times" "$root/perm"

# Seeded bulk names: LCG over a fixed alphabet, 64 files.
awk -v seed="$seed" 'BEGIN {
    s = seed
    for (i = 0; i < 64; i++) {
        s = (s * 1103515245 + 12345) % 2147483648
        len = 3 + s % 10
        name = ""
        t = s
        for (j = 0; j < len; j++) {
            t = (t * 1103515245 + 12345) % 2147483648
            name = name substr("abcdefghijklmnopqrstuvwxyz0123456789", t % 36 + 1, 1)
        }
        print name
    }
}' | sort -u | while IFS= read -r n; do
    printf 'seeded\n' > "$root/plain/$n"
done

# Hidden files and backup shapes.
printf 'h\n' > "$root/plain/.hidden"
printf 'h\n' > "$root/plain/.config"
printf 'b\n' > "$root/plain/notes.txt~"
printf 'b\n' > "$root/plain/.hidden~"

# Blanks, quotes, globs, dashes.
for n in 'sp ace' ' lead' 'trail ' 'two  sp' "sq'uote" 'dq"uote' 'back\slash' \
    'st*ar' 'qu?est' 'brack[et' 'dash-file' '-dashlead' '--ddash' 'tilde~mid' \
    'eq=sign' 'pl+us' 'co:lon' 'semi;colon' 'am&p' 'do$llar' 'ba`ck' \
    'pa(ren' 'pa)ren' 'an<gle' 'an>gle' 'pi|pe' 'ha#sh' 'ex!cl' 'pct%pct'; do
    printf 'w\n' > "$root/shapes/$n"
done

# Control bytes (no slash, no NUL): tab, newline, CR, ESC, bell, DEL, 0x01.
printf 'c\n' > "$root/shapes/$(printf 'tab\tin')"
printf 'c\n' > "$root/shapes/$(printf 'nl\nin')"
printf 'c\n' > "$root/shapes/$(printf 'cr\rin')"
printf 'c\n' > "$root/shapes/$(printf 'esc\033in')"
printf 'c\n' > "$root/shapes/$(printf 'bell\007in')"
printf 'c\n' > "$root/shapes/$(printf 'del\177in')"
printf 'c\n' > "$root/shapes/$(printf 'soh\001in')"

# UTF-8 classes: accented, double-width CJK, combining mark, zero-width
# joiner, RTL, emoji.
printf 'u\n' > "$root/shapes/naïve"
printf 'u\n' > "$root/shapes/日本語ファイル"
printf 'u\n' > "$root/shapes/中文名"
printf 'u\n' > "$root/shapes/한국어"
printf 'u\n' > "$root/shapes/$(printf 'combo e\xcc\x81 mark')"
printf 'u\n' > "$root/shapes/$(printf 'zw\xe2\x80\x8djoin')"
printf 'u\n' > "$root/shapes/עברית"
printf 'u\n' > "$root/shapes/école"
printf 'u\n' > "$root/shapes/$(printf 'emoji \xf0\x9f\x8e\xbc')"

# Invalid multibyte: lone continuation, truncated sequence, overlong-ish,
# stray 0xFF. Skip cleanly where the fs refuses.
for esc in 'inv \x80 lone' 'inv \xc3 trunc' 'inv \xe2\x82 short' 'inv \xff ff'; do
    n=$(printf "$esc")
    { printf 'i\n' > "$root/shapes/$n"; } 2>/dev/null || true
done

# Very long name (200 bytes; below common 255 limits).
long=$(awk 'BEGIN { s = ""; for (i = 0; i < 200; i++) s = s "L"; print s }')
printf 'l\n' > "$root/shapes/$long"

# --- links ----------------------------------------------------------------

printf 't\n' > "$root/links/target"
mkdir -p "$root/links/tdir"
ln -s target "$root/links/good"
ln -s tdir "$root/links/gooddir"
ln -s missing "$root/links/dangling"
ln -s loop-b "$root/links/loop-a"
ln -s loop-a "$root/links/loop-b"
ln -s . "$root/links/self"
ln -s ../links "$root/links/tdir/up"
ln "$root/links/target" "$root/links/hard1"
ln "$root/links/target" "$root/links/hard2"

# --- special file types (skip cleanly) ------------------------------------

mkfifo "$root/meta/fifo" 2>/dev/null || true
if command -v python3 >/dev/null 2>&1; then
    python3 -c '
import socket, sys
try:
    s = socket.socket(socket.AF_UNIX)
    s.bind(sys.argv[1])
except OSError:
    pass
' "$root/meta/socket" 2>/dev/null || true
fi

# setuid/setgid/sticky on our own files/dirs (allowed unprivileged).
printf 's\n' > "$root/meta/suid"
chmod 4755 "$root/meta/suid"
printf 's\n' > "$root/meta/sgid"
chmod 2755 "$root/meta/sgid"
mkdir -p "$root/meta/sticky-dir"
chmod 1777 "$root/meta/sticky-dir"
mkdir -p "$root/meta/ow-dir"
chmod 0777 "$root/meta/ow-dir"
printf 'x\n' > "$root/meta/exec"
chmod 0755 "$root/meta/exec"

# ACL entry where the filesystem permits (mode gains a '+' in ls -l).
printf 'a\n' > "$root/meta/acl"
if command -v setfacl >/dev/null 2>&1; then
    setfacl -m u:nobody:r "$root/meta/acl" 2>/dev/null || true
fi

# --- sizes ----------------------------------------------------------------

# Straddle 512/1024 rounding and human-readable breakpoints.
for sz in 0 1 511 512 513 1023 1024 1025 4095 4096 65536 1048576; do
    head -c "$sz" /dev/zero > "$root/sizes/sz$sz" 2>/dev/null \
        || dd if=/dev/zero of="$root/sizes/sz$sz" bs=1 count="$sz" 2>/dev/null
done
# TiB sparse file.
truncate -s 1T "$root/sizes/sparse-1T" 2>/dev/null || true
truncate -s 1099511627777 "$root/sizes/sparse-1T+1" 2>/dev/null || true

# --- mtimes straddling the six-month cutoff -------------------------------

for n in old-a old-b old-epochish recent-a recent-b; do
    printf 't\n' > "$root/times/$n"
done

# --- permission-shaped directories (chmod applied after mtime pinning) ----

mkdir -p "$root/perm/no-access" "$root/perm/exec-only"
printf 'p\n' > "$root/perm/no-access/inside"
printf 'p\n' > "$root/perm/exec-only/inside"

# --- pin every mtime deterministically ------------------------------------

# Everything (files, dirs, symlinks) gets the pinned old timestamp, then
# the recent-side files are re-touched. Dir mtimes stay old: utimensat on
# a child does not modify the parent. chmod restrictions come after.
find "$root" -depth -print0 | xargs -0 touch -h -d "$old_ts"
touch -d "1970-01-02 00:00:00 UTC" "$root/times/old-epochish"
touch -d "$recent_ts" "$root/times/recent-a" "$root/times/recent-b"

chmod 000 "$root/perm/no-access"
chmod 111 "$root/perm/exec-only"

# --- manifest -------------------------------------------------------------

# Canonical manifest: path, type, mode, size (regular files only - dir
# sizes are fs-dependent), mtime, link target. chmod-000/111 dirs are
# restored for hashing and re-restricted after.
chmod 755 "$root/perm/no-access" "$root/perm/exec-only"
manifest=$(
    cd "$dest" && find core -print | LC_ALL=C sort | while IFS= read -r p; do
        LC_ALL=C stat -c '%n|%F|%a|%Y' "$p" 2>/dev/null || true
        case $(LC_ALL=C stat -c '%F' "$p" 2>/dev/null) in
        "regular file") LC_ALL=C stat -c 'size|%s' "$p" ;;
        "symbolic link") printf 'target|%s\n' "$(readlink "$p")" ;;
        esac
    done
)
chmod 000 "$root/perm/no-access"
chmod 111 "$root/perm/exec-only"

hash=$(printf '%s\n' "$manifest" | sha256sum | cut -d' ' -f1)
printf 'MANIFEST_SHA256 %s\n' "$hash"
