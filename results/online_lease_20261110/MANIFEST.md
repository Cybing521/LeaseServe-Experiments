# Lease 1R+1W online (same cadence as ACORN online)

200k SIFT, ERBAC ACL, 10k queries, 50 changes over ~3 s (`CHANGE_US=60000`, `WARMUP=1`),
one reader + one writer, K=10, 3 seeds per host.

| host | p50 | p95 | p99 | missing | leaks |
|---|---:|---:|---:|---:|---:|
| Xeon s0/s1/s2 | 0.340/0.355/0.343 | 0.703/0.949/0.739 | 2.39/3.53/2.49 | 46/100k | 0 |
| Ryzen s0/s1/s2 | 0.133/0.134/0.134 | 0.151/0.152/0.152 | 0.158/0.158/0.159 | 46/100k | 0 |

ACORN online 1R+1W: Xeon 0.334--0.347, Ryzen 0.151. The lease matches ACORN at ef=32;
ACORN ef=256 is 2.096 ms on the Xeon (6.2x the lease).

## noverify

`noverify/xeon.txt` and `noverify/ryzen.txt` are `SKIP_VERIFY=1` runs: read_s 7.37 s (Xeon)
and 1.09 s (Ryzen), against 72.6/15.7 s with the offline oracle. The oracle is accounting, not
service time; do not derive throughput from oracle-inclusive read_s.
