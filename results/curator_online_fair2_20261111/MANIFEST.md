# Curator online, 30k queries (2026-11-11)

200k SIFT/ERBAC, se=3200, nlist=64, nprobe=32; all 30k queries target role 49 (official 10k set
repeated 3×), 1 reader + 1 writer, 50 changes at 30 ms, 3 seeds. Truth: 500 random query IDs,
exact scan of the **current** visible set at the pin; the writer also updates `ids[role]`.
Query timer includes the shared lock and search.

```
./curator_online_fair2 base.f32 queries_30k.f32 roles.bin qroles_30k.bin \
  200000 128 30000 10 64 32 3200 <seed>
```

| seed | p50 | p95 | p99 | u_p99 | recall |
|---:|---:|---:|---:|---:|---:|
| 0 | 1.76743 | 2.05515 | 2.25049 | 2.00457 | 1.0000 |
| 1 | 1.72210 | 1.99101 | 2.13495 | 1.59013 | 0.9992 |
| 2 | 1.71544 | 1.99156 | 2.16153 | 1.75970 | 0.9996 |

48--49 of 50 updates overlap; read phase ~52 s. 500-document repeated toggles, not long-term drift.
