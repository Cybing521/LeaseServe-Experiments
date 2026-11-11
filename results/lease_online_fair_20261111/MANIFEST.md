# LeaseServe online, service timing (2026-11-11)

Host: Ryzen 9 9950X, 1 reader + 1 writer, 200k SIFT / ERBAC. All 10k queries target role 49,
the same churn role as ACORN; 50 changes at 30 ms; 3 seeds; `SKIP_VERIFY=1`.

| seed | p50 | p95 | p99 | read_s |
|---:|---:|---:|---:|---:|
| 0 | 0.056165 | 0.063990 | 0.067356 | 0.553 |
| 1 | 0.056636 | 0.064612 | 0.068178 | 0.559 |
| 2 | 0.056285 | 0.064041 | 0.067496 | 0.555 |

The full read phase is 0.55 s, so 50 changes at 30 ms (1.5 s) mostly land after the readers
finish; this is a same-cadence query-path comparison, not a fully overlapped run. The ACORN
read phase is 3--10 s and contains all 50 changes.
