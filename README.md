# liszt

[![ci](https://github.com/tenseleyFlow/liszt/actions/workflows/ci.yml/badge.svg)](https://github.com/tenseleyFlow/liszt/actions/workflows/ci.yml)

(noun) : Franz?!

A from-scratch C11 reimplementation of GNU `ls(1)`. Byte-identical output
(parity target: coreutils 9.11), faster on every workload measured. Also
installs as `lz`, which is easier to type than `ls`.

## Why

ls spends its time in a handful of hot loops: collation, per-entry stat
calls, quoting analysis, and LS_COLORS suffix matching. liszt keeps GNU's
exact output semantics - decode_switches staging, the stat economy of
check_stat, quotearg's control flow, dired byte accounting - and swaps
the engines underneath: MSD radix sorts (byte, strxfrm-transformed, and
numeric-key) with the scalar comparator kept as a verification oracle,
SIMD span kernels in width and multibyte scanning, a folded-final-byte
suffix table for color classification, and a thread pool over the
per-entry stat phase with structurally deterministic output.

A golden suite diffs liszt against a pinned GNU ls built from source -
stdout, stderr, and exit codes, across locales, quoting styles, and
terminal-dependent defaults - plus a differential fuzzer and
syscall-budget assertions (`-U` does zero per-entry stats; `-l` does
exactly one statx and one llistxattr per entry; statless color schemes
stat once).

## Measured

hyperfine over the nine required workloads, warm cache, 100k-entry
directories, vs GNU ls 9.11 built from source on the same machine. Linux
box: x86-64, tmpfs. macOS box: Apple M5 Pro, APFS. Full runs with error
bars and machine metadata are stored under `bench/release/`.

| workload | vs ls 9.11 (x86-64) | vs ls 9.11 (arm64) |
|---|---|---|
| 100k flat, `LC_ALL=C` | 1.7x | 1.5x |
| 100k flat, UTF-8 locale | 1.3x | 2.0x |
| 100k `-l` | 2.7x | 1.1x |
| 100k `--color=always -F` | 3.2x | 2.0x |
| deep tree `-R` | 1.2x | 1.0x |
| 100k `-S` | 1.9x | 1.6x |
| 100k `-t` | 1.8x | 1.9x |
| 100k `-U` | 1.3x | 1.2x |
| 50-entry dir (startup) | 1.0x | 1.0x |
| 100k `--dired -l` | 2.6x | 1.2x |

## Install

Homebrew (macOS, Linuxbrew):

    brew install tenseleyflow/tap/liszt

Arch, from the [AUR](https://aur.archlinux.org/packages/liszt):

    paru -S liszt           # or: yay -S liszt

or clone `https://aur.archlinux.org/liszt.git` and run `makepkg -si`.
Arch's `mtools` package owns `/usr/bin/lz`, so the Arch package installs
`liszt` and its man page but not the `lz` alias; create your own
(`ln -s liszt ~/.local/bin/lz`) if you don't use mtools. The same
`PKGBUILD` ships in `packaging/`. Release tarballs with checksums are on
the [releases page](https://github.com/tenseleyFlow/liszt/releases).

From source:

    ./configure
    make
    make check                  # unit + golden parity + fuzz smoke
                                # (builds GNU ls 9.11 once as the oracle)
    make install                # liszt + lz + man pages

Requires a C11 compiler, GNU make, and libc only. Runs on Linux (glibc
and musl), macOS, and FreeBSD.

## Parity notes

The full GNU ls 9.11 flag surface is implemented and pinned: every
short flag and every `long_options[]` entry is exercised against the
reference build with byte-identical stdout and matching exit codes.
Diagnostics reproduce GNU's quoting (quotef vs quoteaf, locale quotes,
argmatch listings) and its exit-code split (argmatch errors exit 1,
getopt errors exit 2).

One intentional deviation is carried, with its own regression registry
(`tests/golden/deviations/`): GNU assigns a rejected display width into
an unsigned type before its clamp, so names containing control bytes or
invalid multibyte sequences in multibyte locales wrap GNU's column
arithmetic. liszt honors the intended clamp (width 0). Quoted styles
and `-q` are unaffected, and the registry asserts the oracle still
differs so an upstream fix fails loudly.

Platform notes: Unicode width decisions mirror gnulib's per-platform
REPLACE_WCWIDTH probe (configure runs the same conformance test);
printability is libc `iswprint` everywhere, matching `c32isprint`'s
dispatch. Birth times use `statx` with GNU's `?` fallback where the
filesystem has none.

## Extensions

v0.2 adds the modern-replacement surface, all off by default: with no
extension flag, output stays byte-identical to GNU ls and the whole
parity matrix runs unchanged. Flags use eza-compatible names and match
by exact spelling only (no abbreviation).

- `--icons[=WHEN]` - Nerd-Font icons from compiled-in tables (933
  curated mappings), resolved from the name and dirent type alone: no
  stat, no directory reads. `LS_ICONS` overrides per suffix, filename,
  directory, or filetype label; `LISZT_ICONS_OSC66=1` wraps glyphs in
  kitty's text-sizing protocol for terminal-guaranteed width.
- `--tree` - structural tree with `--level=N`, `--tree-limit=N`
  ("... K more" collapsing), `--tree-glyphs=unicode|ascii|auto`. One
  directory descriptor open at any depth: a 100k-node tree lists under
  `ulimit -n 16` with identical bytes. The full sort/filter/color/icon
  surface applies per sibling list; `-l` works with metadata on the
  left.
- `--git` - a status column with zero dependencies: liszt parses
  `.git/index` itself (v2-v4, SHA-256 repos, linked worktrees) and
  compares stat data - nothing is hashed, adding a constant ~30
  syscalls per repo and none per entry. `--git-ignore` hides ignored
  entries through a from-scratch wildmatch engine; `--no-git`
  suppresses. Limits are documented in liszt(1), not glossed over:
  staged-only changes read as clean.

Measured against eza (the fastest of the modern replacements), same
machines as above, artifacts under `bench/release/`:

| workload | vs eza (x86-64) | vs eza (arm64) |
|---|---|---|
| 100k `--icons --color -1` | 4.3x | 10.4x |
| 100k-node `--tree` | 16.2x | 12.7x |
| 200k-file repo `-l --git` | 11.1x | 15.0x |

Extension cost against plain liszt on the same lanes stays gated:
icons within 10% on the color lane, `--tree` within 15% of `-R -1`,
`--git` within 2x of `-l` (measured ~10%).

## Debug surface

`LISZT_DEBUG_PLAN` (selected engines), `LISZT_DEBUG_STATS` (radix and
memo counters), `LISZT_DEBUG_VERIFY` (optimized engines cross-checked
against the scalar oracle; the test harness sets it on every run),
`LISZT_FORCE_SCALAR`, `LISZT_PARALLEL_MIN`, `LISZT_PARALLEL_WORKERS`.

## License

GPL-3.0-or-later. Portions are ported from GNU coreutils and gnulib
(mpsort, uniwidth tables, quotearg control flow); see source headers.
