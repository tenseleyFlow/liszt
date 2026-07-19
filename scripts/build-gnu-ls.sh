#!/bin/sh
# Build the pinned GNU coreutils ls into build/gnu-ls/src/ls and print its
# path. Source preference: local .docs/refs/gnu-coreutils (the exact 9.11
# release tree), else the release tarball fetched from ftp.gnu.org.
# The built binary is refused unless --version reports the pin.
set -eu

pin="9.11"
out="build/gnu-ls"

if [ -x "$out/src/ls" ] && "$out/src/ls" --version | sed -n 1p | grep -q " $pin\$"; then
    printf '%s\n' "$out/src/ls"
    exit 0
fi

src=".docs/refs/gnu-coreutils"
if [ ! -f "$src/configure" ]; then
    mkdir -p build
    tarball="build/coreutils-$pin.tar.xz"
    url="https://ftp.gnu.org/gnu/coreutils/coreutils-$pin.tar.xz"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL -o "$tarball" "$url"
    else
        wget -qO "$tarball" "$url"
    fi
    tar xf "$tarball" -C build
    src="build/coreutils-$pin"
fi

srcabs=$(cd "$src" && pwd)
mkdir -p "$out"
outabs=$(cd "$out" && pwd)

(
    cd "$outabs"
    "$srcabs/configure" --quiet --disable-nls >configure.log 2>&1
    make -s src/ls >make.log 2>&1
) || {
    echo "build-gnu-ls: build failed; see $out/configure.log and $out/make.log" >&2
    exit 1
}

"$out/src/ls" --version | sed -n 1p | grep -q " $pin\$" || {
    echo "build-gnu-ls: built ls does not report version $pin; refusing" >&2
    exit 1
}
printf '%s\n' "$out/src/ls"
