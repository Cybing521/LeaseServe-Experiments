# ACORN online, 30k queries (2026-11-11)

200k SIFT/ERBAC, all 30k queries target role 49 (the churn role; official 10k set repeated 3×),
1 reader + 1 writer, 50 changes at 30 ms, 3 seeds. Truth: 500 random query IDs, exact scan of
the pinned bitmap (5,000 positions/run). Driver `acorn_online_fair.cpp` (writer priority, member
pool fixed, overlap = full query interval, e2e timer includes bitmap copy + ACL lock).

| run | p50 | p95 | e2e p50 | recall | overlaps |
|---|---:|---:|---:|---:|---:|
| ef32 s0 | 0.138483 | 0.174822 | 0.140898 | 0.9484 | 44/50 |
| ef32 s1 | 0.138272 | 0.174500 | 0.140737 | 0.9408 | 39/50 |
| ef32 s2 | 0.138252 | 0.174320 | 0.140686 | 0.9538 | 45/50 |
| ef256 s0 | 0.756808 | 0.928462 | 0.759192 | 0.9998 | 46/50 |
| ef256 s1 | 0.799808 | 0.970900 | 0.802362 | 0.9992 | 47/50 |
| ef256 s2 | 0.756666 | 0.922478 | 0.759150 | 0.9990 | 46/50 |

Read phase 5.2 s (ef32) / 23.9--25.2 s (ef256); all 50 changes inside it.
