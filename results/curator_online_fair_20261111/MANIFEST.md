# Curator online, 1R+1W, 30 ms, se=3200

Host: Ryzen 9950X. 200k SIFT, ERBAC ACL, 10k queries, 50 changes at 30 ms.
Driver: `curator_online_fair.cpp` (hierarchical, niter=20, nlist=64, nprobe=32, se=3200).
Warmup over all queries; barrier; one reader and one writer. Query timing includes the shared
lock and search (e2e); writer applies grant/revoke under the exclusive lock and records each
batch. Truth: 500 random query IDs, exact scan of the visible set at the pin; recall 0.9996.

```
curator_online n=200000 nq=10000 k=10 ef=3200 changes=50 threads=1 p50=2.31234 p95=2.68118
p99=2.80864 updates=50 u_p99=2.55112 overlap_reads=95 overlapping_updates=48 granted=20
revoked=29 missing=2 truth_checked=5000 recall=0.9996
```

One seed; the pool is 500 documents toggled repeatedly (not long-term drift).
