# LeaseServe online, service timing (2026-11-11)

Host: Ryzen 9 9950X, 1 reader + 1 writer, 200k SIFT / ERBAC, 10k queries, K=10, 50 changes at
30 ms, 3 seeds. `SKIP_VERIFY=1` removes the per-query exact oracle; p50 is the service path
(pin, interval check, candidate filter). `hist_rows=50` confirms all changes applied.

| seed | p50 | p95 | p99 | read_s |
|---:|---:|---:|---:|---:|
| 0 | 0.12137 | 0.14236 | 0.15130 | 1.202 |
| 1 | 0.12003 | 0.14063 | 0.14977 | 1.189 |
| 2 | 0.12243 | 0.14346 | 0.15237 | 1.213 |

The read phase is 1.2 s while 50 changes at 30 ms span 1.5 s, so the last changes land after the
readers finish; the first ~40 are concurrent. This is the same cadence as ACORN, not a
byte-identical event stream.
