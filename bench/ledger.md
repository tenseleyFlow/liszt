# Carried-exception ledger

Workloads at or behind GNU, each with an owner. A cell leaves the
ledger when the owner sprint lands numbers showing the win, or gets a
permanent entry with rationale. Matrix source: bench/run-matrix.sh.

| workload | dev-box status | owner |
|---|---|---|
| (empty) | | |

Closed entries:
- flat -S 100k: was 75.9ms vs GNU 74.3ms serial; 38.6ms (1.93x win)
  after 09D parallel stat. 2026-07-19.
- flat -t 100k: same shape; cleared by the same change.

Notes:
- The -S/-t gap was per-call statx time in a serial loop (identical
  syscall counts and masks); the pool cleared it. Threshold: parallel
  engages at 400 entries (dev-box crossover 200-400, aspen worker
  scaling n/256 capped at online CPUs; re-measure on nomad-1 before
  trusting the constant there).
