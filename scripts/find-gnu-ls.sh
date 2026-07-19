#!/bin/sh
# Print the path of a GNU coreutils ls oracle on stdout; exit 1 if none.
# Preference: $LISZT_ORACLE, the pinned local build, gls, ls - exact-pin
# versions first, then any GNU coreutils ls (callers may warn on vintage
# drift). BSD/busybox ls is never accepted.
set -u

pin="9.11"

version_line() {
    "$1" --version 2>/dev/null | sed -n 1p
}

resolve() {
    case "$1" in
    /*) [ -x "$1" ] && printf '%s\n' "$1" ;;
    */*) [ -x "$1" ] && printf '%s/%s\n' "$(pwd)" "$1" ;;
    *) command -v "$1" 2>/dev/null ;;
    esac
}

candidates="${LISZT_ORACLE:-} build/gnu-ls/src/ls gls ls"

best=""
for cand in $candidates; do
    path=$(resolve "$cand") || continue
    [ -n "$path" ] || continue
    line=$(version_line "$path")
    case "$line" in
    *"GNU coreutils"*) ;;
    *) continue ;;
    esac
    case "$line" in
    *" $pin") printf '%s\n' "$path"; exit 0 ;;
    esac
    [ -n "$best" ] || best="$path"
done

if [ -n "$best" ]; then
    printf '%s\n' "$best"
    exit 0
fi
exit 1
