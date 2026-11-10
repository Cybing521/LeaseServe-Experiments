# ACORN online (1 reader + 1 writer)

Status: **complete for the single-reader baseline**; 4 concurrent readers do not scale in
this build. The paper's ACORN rows remain matched-snapshot; this file measures the online
1R+1W path only.

- ACORN-gamma: M=32, gamma=12, M_beta=32; ef=32/256; 200k SIFT, ERBAC ACL.
- 1 reader + 1 writer, 50 ACL changes over ~0.75 s (`CHANGE_US=15000`), 10k queries.
- Each read copies the role bitmap (200 KB), pins that snapshot, and searches the copy;
  the copy is outside the timed search, as the offline driver builds its filter outside timing.
- No index rebuild: an ACL change flips one bitmap bit.

| run | p50 | p95 | p99 | leaks | updates |
|---|---:|---:|---:|---:|---:|
| Xeon ef32 s0 | 0.347 | 0.452 | 0.726 | 0 | 50 |
| Xeon ef32 s1 | 0.334 | 0.414 | 0.566 | 0 | 50 |
| Xeon ef32 s2 | 0.343 | 0.429 | 0.553 | 0 | 50 |
| Xeon ef32 offline | 0.360 | 0.427 | 0.479 | 0 | 0 |
| Xeon ef256 online | 2.096 | 2.689 | 19.6 | 0 | 50 |
| Xeon ef256 offline | 2.080 | 3.294 | 34.5 | 0 | 0 |
| Ryzen ef32 online | 0.151 | 0.182 | 0.195 | 0 | 50 |
| Ryzen ef32 offline | 0.144 | — | — | 0 | 0 |
| Xeon ef32 4R static | 0.921 | 2.440 | 82.7 | 0 | 0 |

Offline numbers are the same-session ACORN driver; the paper's table uses an earlier run
(0.308/0.364). The 4R static row shows the 82 ms tail is present without a writer, so the
online baseline is single-reader. Driver: `acorn_online.cpp`.
