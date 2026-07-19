# Carried-exception ledger

Workloads at or behind GNU, each with an owner. A cell leaves the
ledger when the owner sprint lands numbers showing the win, or gets a
permanent entry with rationale. Matrix source: bench/run-matrix.sh.

| workload | dev-box status (2026-07-19 baseline) | owner |
|---|---|---|
| flat -S 100k | 75.9ms vs GNU 74.3ms (~1 sigma; statx-bound, sys-time dominated) | 09D parallel stat |
| flat -t 100k | 74.5ms vs GNU 73.3ms (~2 sigma; same shape) | 09D parallel stat |

Notes:
- Both lanes: identical statx counts (100001) and near-identical masks
  (liszt adds STATX_TYPE, which the kernel fills with MODE anyway);
  liszt already wins user time. The gap is per-call syscall time in a
  serial loop - the parallel stat lane is the designed fix.
