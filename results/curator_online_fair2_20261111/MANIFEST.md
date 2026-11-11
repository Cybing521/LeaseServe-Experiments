# Curator online, fair protocol (2026-11-11)

Host: Ryzen 9950X, 1 reader + 1 writer, 200k SIFT/ERBAC, se=3200, nlist=64, nprobe=32.
All 10k queries target role 49 (the churn role), same as ACORN/lease. 50 changes at 30 ms.
Truth: 500 random query IDs, exact scan of the **current** visible set at the pin; the writer
also updates `ids[role]`, so the truth is not the static input ACL.

Command:

```
./curator_online_fair2 base.f32 queries10k.f32 roles.bin qroles_r43.bin \
  200000 128 10000 10 64 32 3200 <seed>
```

| seed | p50 | p95 | p99 | updates | u_p99 | recall |
|---:|---:|---:|---:|---:|---:|---:|
| 0 | 1.75116 | 2.05898 | 2.23504 | 50 | 1.96884 | 1.0000 |
| 1 | 1.74594 | 2.05319 | 2.23154 | 50 | 1.81756 | 0.9992 |
| 2 | 1.74829 | 2.04670 | 2.19285 | 50 | 1.74181 | 0.9996 |

Overlap: 45--47 of 50 updates; the read phase is ~17 s so all updates are inside it.
The pool is 500 documents toggled repeatedly, not long-term ACL drift.
