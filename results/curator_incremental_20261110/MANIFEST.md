# Curator incremental path

- Host: Xeon Platinum 8352V, 16-core quota, AVX-512.
- Index: Curator v2 `MultiTenantIndexIVFHierarchical`, nlist=64, nprobe=32, k=10,
  niter=20, max_leaf=128, search_ef 800/3200, 200k SIFT base, ERBAC ACL.
- Queries: `qroles_role0.bin` (1,000 queries, role 0); truth is an exact scan of the
  current visible set at each checkpoint.
- Changes: 500-document toggle pool; revoke if present, grant otherwise; single writer.
- Build: 645 s (niter=20). No rebuild between checkpoints.
- Driver: `curator_online_eval.cpp`; raw log: `eval.log`.

| N | update p50/p95 (us) | se800 p50/p95/recall | se3200 p50/p95/recall |
|---:|---:|---:|---:|
| 0 | -- | 0.960/1.240/0.9800 | 6.891/8.017/0.9999 |
| 100 | 1.9/90.3 | 0.951/1.179/0.9805 | 7.003/8.136/0.9999 |
| 1,000 | 0.89/5.1 | 0.963/1.191/0.9800 | 6.984/8.243/0.9999 |
| 10,000 | 0.85/4.6 | 0.907/1.127/0.9800 | 6.830/8.146/0.9999 |

Concurrent 4 readers + 1 writer (se=3200, 10 changes per lock, 100 changes):
query 6.05/8.57/11.53 ms p50/p95/p99; per-batch update 5.96 ms p50, 1.54 s p99;
no errors. At se=100 and one change per lock the writer p99 is 53.6 ms.

Used in supplement `sec:curator-incr`. The 48 s rebuild in the main artifact is the
driver's compaction policy, not a per-change requirement.
