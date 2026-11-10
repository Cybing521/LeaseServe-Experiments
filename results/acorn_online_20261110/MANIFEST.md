# ACORN online (fixed driver: warmup-first barrier)

Source: `acorn_online.cpp`, sha256 `bd1b967450a4adf90dd6a31ea276ba7ab33681da98ad82df468a76d68755da47`.
Index: ACORN-gamma M=32, gamma=12, M_beta=32; 200k SIFT, ERBAC ACL.

Order: build -> untimed warmup over all 10k queries -> barrier releases 1 reader and 1 writer
together. Writer applies 50 changes; each update interval and read interval is recorded.

| run | p50 | p95 | p99 | overlap | leaks |
|---|---:|---:|---:|---:|---:|
| Ryzen ef32 s0 | 0.155 | 0.187 | 0.201 | 49/50 | 0 |
| Ryzen ef32 s1 | 0.148 | 0.178 | 0.191 | 50/50 | 0 |
| Ryzen ef32 s2 | 0.149 | 0.181 | 0.194 | 50/50 | 0 |
| Ryzen ef32 truth run | 0.168 | 0.203 | 0.217 | 48/50 | 0 |
| Ryzen ef256 | 0.866 | 0.996 | 1.05 | 28/50 | 0 |
| Xeon ef32 fixed s0/s1 | 0.644/0.491 | 1.142/1.097 | 66.3/58.9 | 50/50 each | 0 |
| Xeon static (load) | 0.636 | 1.176 | 45.8 | — | 0 |

Truth run: missing 21/500, recall 0.958, leaked 0. Offline Ryzen 0.144 (ef32), 0.874 (ef256).
Xeon fixed run has 50/50 overlap but p50 0.49--0.64 under shared-host load 24--29; not used for
latency. Old pre-fix runs in this directory are historical only.

Lease reference at the same 30 ms cadence (Ryzen, 3 seeds): p50 0.133--0.134, p95 0.153,
p99 0.160, missing 46/100k, leaks 0 (`online_lease_20261110/fixed30/`).
