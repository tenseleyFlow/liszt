# Carried-exception ledger

Workloads at or behind GNU, each with an owner. A cell leaves the
ledger when the owner sprint lands numbers showing the win, or gets a
permanent entry with rationale. Matrix source: bench/run-matrix.sh.

| workload | dev-box status | owner |
|---|---|---|
| (empty) | | |

Extension lanes (v0.2, dev box, 100k flat, 20 runs):
- icons cost: -1 20.2 -> 21.9ms (+8.4%); --color -1 34.0 -> 35.8ms
  (+5.3%, at the 5% gate). vs eza --icons --color -1: 182.2ms ->
  liszt 5.1x faster. GNU-parity matrix re-run: no lane regressed.
- tree (100k-node nested fixture, 1100 dirs, 10 runs): --tree 14.2ms
  vs -R -1 13.4ms (self-relative ratio 1.057, gate 1.15); --tree -l
  150.9ms; eza -T 230.2ms -> liszt 16.2x faster. fd ceiling: identical
  output under ulimit -n 16 (one dirfd at any depth). GNU-parity
  matrix re-run: no lane regressed.
- git (200k-file one-commit repo, 10 runs): -l 130.2ms -> -l --git
  142.8ms (ratio 1.097, gate 2.0; 63ns/entry for the whole status
  path); v4 index 141.9ms (prefix decompression in the noise);
  --git-ignore -1 44.8ms. eza -l --git 1.583s -> liszt 11.1x faster.
  Syscalls: +32 constant per repo, +0 per entry (strace-asserted).
- color=full (100k -l, 10 runs): 71ms vs --color=always 62.5ms
  (ratio 1.14, gate 1.25; was 1.56 before the perf pass: batched
  funnel writes, cached gid - 100k getgid syscalls - and memoized
  styled mode strings). vs eza -l --color: 276.7ms -> 4.4x faster
  with the full theme on. 2026-07-19.
- themes (100k -l, 10 runs): --theme=dracula 1.09x the --color=full
  lane (gate 1.10; 24-bit payloads are ~2.5x the escape bytes).
  2026-07-19.
- v0.3.0 release lanes: dev box full/always 1.155, theme/full 0.994,
  full vs eza -l 4.5x; nomad-1 full/always 1.014, theme/full 1.018,
  full vs eza 8.0x. GNU matrix all lanes >= 1.08 (dev) / >= 1.0
  (nomad). Artifacts: bench/release/*-v0.3.0-*. 2026-07-20.
- v0.2.0 release lanes, nomad-1 (M5 Pro, APFS): icons color-lane
  ratio 1.042, 10.4x vs eza; --tree 41.0ms vs -R -1 58.8ms (0.696),
  eza -T 522.6ms = 12.7x; -l --git 382.5ms vs -l 342.7ms (1.116),
  eza -l --git 5.729s = 15.0x; v4 index 352.2ms. Full GNU matrix all
  ten lanes >= 1.02x. Artifacts: bench/release/*-v0.2.0-*.

Closed entries:
- macOS -l 100k: was 286ms uncapped-pool vs gls 239ms (E-core
  contention on APFS metadata locks); 208ms (1.14x win) after capping
  Darwin workers at hw.perflevel0 P-cores. 2026-07-19.
- macOS dired -l: same cause; 232ms vs gls 274ms after the cap.
- flat -S 100k: was 75.9ms vs GNU 74.3ms serial; 38.6ms (1.93x win)
  after 09D parallel stat. 2026-07-19.
- flat -t 100k: same shape; cleared by the same change.

Notes:
- Darwin worker sweep (nomad-1, -l 100k): 2->323, 3->262, 4->226,
  6->210, 8->271, 10->274, uncapped(12+)->286ms. Minimum sits exactly
  at P-core count; E-cores actively hurt APFS metadata concurrency.
- LTO probe: <1% on every lane (mat's 11% came from cross-TU inlining
  liszt does not need); not kept. Cold-cache -l on the dev box:
  53ms vs GNU 146ms.
- The -S/-t gap was per-call statx time in a serial loop (identical
  syscall counts and masks); the pool cleared it. Threshold: parallel
  engages at 400 entries (dev-box crossover 200-400, aspen worker
  scaling n/256 capped at online CPUs; re-measure on nomad-1 before
  trusting the constant there).
