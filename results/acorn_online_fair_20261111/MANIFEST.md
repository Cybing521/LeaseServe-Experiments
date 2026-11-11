# ACORN online, fair protocol (2026-11-11)

Host: Ryzen 9 9950X, 1 reader + 1 writer, 200k SIFT / ERBAC. **All 10,000 queries target the
churn role** (role 49), so every query sees the update stream; earlier 10-role runs diluted it.

Driver: `acorn_online_fair.cpp`: writer priority; `members` updated on grant; 500 random query
IDs (not every 200th) checked against their pinned bitmap; e2e timer includes bitmap copy + ACL
lock, search-only timer reported separately.

Protocol: warmup over all 10k -> barrier -> 50 changes at 30 ms; 3 seeds.

| run | p50 search | p95 | e2e p50 | recall | overlaps |
|---|---:|---:|---:|---:|---:|
| ef32 s0 | 0.17525 | 0.21602 | 0.17769 | 0.9484 | 38/50 |
| ef32 s1 | 0.18756 | 0.23271 | 0.19005 | 0.9412 | 30/50 |
| ef32 s2 | 0.18847 | 0.23510 | 0.19085 | 0.9540 | 39/50 |
| ef256 s0 | 0.93754 | 1.13201 | 0.94007 | 0.9998 | 42/50 |
| ef256 s1 | 0.79657 | 0.98828 | 0.79892 | 0.9992 | 44/50 |
| ef256 s2 | 0.79823 | 0.98597 | 0.80067 | 0.9988 | 46/50 |

All updates finish before the ACORN read phase ends (`upd_last` 1.47 s, `read_last` 2.75--10.5 s).
Updates are not byte-identical to the lease stream; same count and cadence.
