# ACORN online, fair protocol (2026-11-11)

Host: Ryzen 9 9950X, 1 reader + 1 writer, 200k SIFT / ERBAC, one hot role (the query role).
Driver: `acorn_online_fair.cpp` (writer priority; member pool fixed; e2e timer includes bitmap
copy and ACL lock; search-only timer reported separately).

Protocol: untimed warmup over all 10k queries -> barrier -> 50 changes at 30 ms. Truth: 500
query IDs drawn uniformly at random from the 10k (not every 200th), each checked against an exact
scan of its pinned bitmap; 5,000 returned positions per run. 3 seeds.

| run | p50 search | p95 | e2e p50 | recall | updates during read |
|---|---:|---:|---:|---:|---:|
| ef32 s0 | 0.1790 | 0.2165 | 0.1818 | 0.9460 | 38/50 |
| ef32 s1 | 0.1797 | 0.2175 | 0.1823 | 0.9438 | 38/50 |
| ef32 s2 | 0.1800 | 0.2170 | 0.1820 | 0.9448 | 38/50 |
| ef256 s0 | 0.9248 | 1.0721 | 0.9285 | 0.9994 | 45/50 |
| ef256 s1 | 0.9315 | 1.0798 | 0.9352 | 0.9992 | 48/50 |
| ef256 s2 | 1.1299 | 1.2703 | 1.1347 | 0.9992 | 47/50 |

Updates all complete before the read phase ends (`upd_last` 1.47 s, `read_last` 2.7--12.3 s).
`overlap_reads` is the number of read intervals intersecting an update interval; because the
update critical section is microseconds, this is a conservative lower bound (38--48 of 50).

The random change stream is not byte-identical to the lease stream; same count and 30 ms cadence.
