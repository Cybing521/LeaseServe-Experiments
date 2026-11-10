# 1M certificate matrix, six rows re-run (2026-11-09)

Server `xeon-host`, 22:29-22:41. New kernel (`lease_certified.cpp` with the P0-1 accounting fix),
same 1M ERBAC ACL, 200 changes, 200 reads, 4 readers + 1 writer, seed 0, `CHANGE_US=500`,
`MERGE_EVERY=0`, `SELFCHECK=1`. Each arm dumps `changes.txt` and `queries.txt`; `full_drop0` and
`full_drop5` also have the independent oracle output.

| arm | env | fallback | missing | auth | oracle |
|---|---|---|---|---|---|
| `versioneq_drop0` | `CERT=1 PRED=1 DROP_GRANT=0` | 200/200 | 0 | 0 | not run |
| `nolog_drop0` | `CERT=1 NOLOG=1 DROP_GRANT=0` | 200/200 | 0 | 0 | not run |
| `nocert_drop0` | `CERT=0 DROP_GRANT=0` | 0 | 3 | 0 | not run |
| `full_drop0` | `CERT=1 DROP_GRANT=0` | 0 | 3 | 0 | `leaks=0 missing=3` |
| `full_drop5` | `CERT=1 DROP_GRANT=5` | 181/200 | 0 | 0 | `leaks=0 missing=0` |
| `1m_nocert_drop5` (earlier) | `CERT=0 DROP_GRANT=5` | 0 | 3 | 0 | `leaks=0 missing=3` |

The 11-08 table (`experiments/results/cert_matrix_20261108/`) has the same missing/auth counts and
178/200 fallbacks for `full_drop5`; the three-read difference is reader timing around the frozen
watermark. The re-run's p50 (35.0, 35.1, 2.5, 0.51, 37.5 ms) is not compared with the 11-08
latency: the host was shared with other jobs.

Commands (per arm):

```
env CERT=... PRED=... NOLOG=... DROP_GRANT=... CHANGE_US=500 MERGE_EVERY=0 WARMUP=100 \
  SELFCHECK=1 CHANGES_OUT=... QUERIES_OUT=... \
  ./lease_certified base.f32 queries.f32 acl/erbac100/roles.bin \
  acl/erbac100/qroles_head10.bin 1000000 128 200 10 10 200 4 0 <out> 15
```
