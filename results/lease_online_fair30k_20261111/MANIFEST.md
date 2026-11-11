# Lease online, 30k-query read phase (2026-11-11)

Goal: cover all 50 changes at 30 ms (1.5 s) inside the lease read phase. The official 10k query
set is repeated three times (queries_30k.f32, qroles_30k.bin: all role 49), so the read phase is
1.66 s instead of 0.55 s. Per-query latency is unchanged; this only lengthens the observation
window. `make_queries.py` regenerates the two inputs from the official 10k files.

Command (Ryzen 9950X, 1R+1W):

```
SKIP_VERIFY=1 CHANGE_US=30000 WARMUP=1 CERT=1 WRITERS=1 MMAX=5000 \
  ./lease_online base.f32 queries_30k.f32 roles.bin qroles_30k.bin \
  200000 128 30000 10 10 50 1 <seed> lease30k_s<seed> 15
```

| seed | p50 | p95 | p99 | read_s | QPS |
|---:|---:|---:|---:|---:|---:|
| 0 | 0.056225 | 0.063940 | 0.067327 | 1.66187 | 18,069 |
| 1 | 0.056036 | 0.063719 | 0.066855 | 1.65824 | 18,108 |
| 2 | 0.056235 | 0.063810 | 0.066876 | 1.66141 | 18,073 |

`hist_rows=50` in every seed; all 50 changes land inside the 1.66 s read phase. `verified=0`,
so `missing=0` is the disabled-oracle placeholder, not an online recheck.
