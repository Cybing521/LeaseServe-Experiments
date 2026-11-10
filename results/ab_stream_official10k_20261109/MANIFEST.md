# Official SIFT1M 10k matched-snapshot runs (2026-11-09 22:53-22:58)

These are the raw runs behind Table `tab:stream` and the matched-recall numbers in
§V-E of the paper. Every row below has a `summary.txt` in this directory.

## Conditions

- Host: xeon-host (Xeon 8352V, 16-core cgroup), `OMP_NUM_THREADS=16`, idle.
- Host-speed control: ACORN build 41.5-42.9 s, Curator 47.7-48.4 s, lease build
  14.0 s. An earlier official run at 22:43 had ACORN build 84.8 s (2x slower
  window); its `*_real` rows (lease 0.320, ACORN 0.448) are **not cited**.
- Kernel source: the official rows were produced by the binary built at commit `c5ef787`
  (source sha256 `6cdc662e973c102b2121bf999a58883d5ff5e85779f85588cd269bd5bdae825d`). The shipped
  tree adds only `DUMP_LAT`/`SKIP_VERIFY` and reader/QPS instrumentation (sha256
  `59f1c0da60608b9de74e435b8a42dbc574953165b52776aedc4219290fb8748e`); the read path is unchanged.
  Exact run-time source and full diff: `experiments/cpp/source_snapshots/`.
  Binary md5 `64247916100250d5dba32353fbae1aa2` (official rows).
- ACORN driver md5 `d08686a72bbe8a3419de5e37db4a65ac`; Curator driver md5
  `abb408445c9b5305e1a4204d72b72c72`.

## Inputs (md5)

| file | md5 |
|---|---|
| queries10k_real.f32 (official SIFT1M test set, 10000x128) | 2b294e6cdaafcaf9a1e6dd35ba43fb31 |
| changes100.txt (50 grants / 50 revokes) | 7bd83ae974948840fa834285a099f4a4 |
| qroles_10000.bin | de4038ffd576df0f1f9687e3edd72469 |
| roles.bin (200k ERBAC) | dbcf99d67fff42f9caf26d58994c5b21 |
| stream_real2.txt | identical to changes100.txt (diff empty) |

## Commands (cwd = this directory, base = 1M SIFT, n = 200000, k = 10)

```bash
CHANGES_OUT=stream_real2.txt WARMUP=100 CERT=1 CHANGE_US=500 DELTA_CAP=4096 \
  MERGE_EVERY=0 DROP_GRANT=0 WRITERS=1 MMAX=5000 \
  lease_certified_new base.f32 queries10k_real.f32 roles.bin qroles_10000.bin \
  200000 128 10000 10 10 100 4 0 lease_real2 15

CHANGES_IN=stream_real2.txt acorn_acl_stream base.f32 queries10k_real.f32 \
  roles.bin qroles_10000.bin 200000 128 10000 10 32 12 32 32 acorn_ef32_real2
CHANGES_IN=stream_real2.txt acorn_acl_stream ... 256 acorn_ef256_real2

CHANGES_IN=stream_real2.txt curator_acl_stream base.f32 queries10k_real.f32 \
  roles.bin qroles_10000.bin 200000 128 10000 10 64 32 curator_se800_real2 20 128 800
# se = 1600 / 2400 / 3200 likewise
```

## Paper row -> file

| paper | file |
|---|---|
| lease 0.249 / 0.295 / 0.99954 | lease_real2/summary.txt (p50=0.248818, missing=46) |
| ACORN ef=32 0.308 / 0.364 / 0.95035 | acorn_ef32_real2/summary.txt |
| ACORN ef=256 1.742 / 1.992 / 0.99943 | acorn_ef256_real2/summary.txt |
| Curator se=800 0.960 / 1.267 / 0.97177 | curator_se800_real2/summary.txt |
| Curator se=3200 6.487 / 7.602 / 0.99978 | curator_se3200_real2/summary.txt |

The full read-latency distributions are in the per-run `summary.txt`; per-query
lines are in the corresponding `*.out`.

## Paired per-query test (`paired/`)

The four systems were rerun with `DUMP_LAT=1`; each `lat.txt` has one line per query in
index order, so the bootstrap pairs by query.

- `lease_paired/lat.txt` (p50 0.270 ms), `acorn_ef32_paired/lat.txt` (0.324),
  `acorn_ef256_paired2/lat.txt` (1.741; a first ef=256 run was a 2.0 ms host outlier and
  was replaced), `curator_se800_paired/lat.txt` (0.943).
- `bootstrap.json`: median difference, 95% CI, fraction lease-faster, Wilcoxon p
  (10,000 resamples, seed 0).
- Command: the four commands in the main section with `DUMP_LAT=1` and
  `stream_paired.txt` (identical to `changes100.txt`).

## Reader scaling (`readers/`)

`readers2_{1,2,4,8,16}/summary.txt`: the lease with 1--16 readers, 100 changes, same ACL
and queries. QPS is computed inside the driver from the per-query timings; the wall clock
also covers the offline exact verification used for missing/leak accounting.

## Single-reader point

`../review_fix_20261109/lease_1r_official10k/summary.txt`: p50 0.265594 ms, same stream and
queries, one reader. Ratios against the table: 1.16x (ACORN ef=32), 6.56x (ef=256), 3.61x
(Curator se=800), 24.4x (se=3200). The reader-scaling table's 1-reader point (0.255 ms) is a
separate run and is used only for the scaling curve.

## Measured service throughput (`readers_noverify/`)

`readers_noverify/noverify_{1,2,4,8,16}/summary.txt`: the lease with 1--16 readers, same ACL and
queries, `SKIP_VERIFY=1` so the offline exact oracle is disabled. QPS = `nq / read_s` (wall clock);
the p50 is the service-only query path. With the oracle enabled, the same run takes about 2.2 ms
more per query because its exact scan evicts the graph from cache; the paper's 0.249 ms row is that
instrumented run, so its ratios are conservative. The dedicated instrumented single-reader run is
`../review_fix_20261109/lease_1r_official10k` (0.265594 ms).

## Second host (`secondhost/`)

Same inputs, stream and queries on `ryzen-host` (Ryzen 9950X, 32 cores, AVX-512), staged under
`$REPO/secondhost` with the package's `lease_certified.cpp` + `third_party/hnswlib`
built locally and the same ACORN/Curator binaries. `stream_2h.txt` is byte-identical to
`changes100.txt`. Rows: `lease_2h`, `acorn_ef32_mimo`, `acorn_ef256_mimo`, `curator_se800_mimo`.
Ratios: 1.01x, 6.12x, 3.28x (absolute latencies are ~1.7x lower than the Xeon; only ratios compare).

## Cold-role churn (`coldrole/`)

`cold_default` / `cold_promote`: 200 changes with `ANY_ROLE=1`, 100 queries over all roles,
`K=10`, 200k ERBAC. Default: 25/200 accepted, 175 refused, 0 promotions, p50 0.482/p95 1.895.
`PROMOTE_ON_WRITE=1`: 200/200 accepted, 74 promotions, 45.5 s total build, p50 0.868/p95 4.132.
Both zero missing and zero leaks.

## Third host (`thirdhost/`)

Xeon Gold 6330 (112 cores, AVX-512, 2.0 GHz base), staged from the ann-benchmarks
`sift-128-euclidean.hdf5` train split (md5 of base equals the xeon-host base:
`0caeb02f1cc236dbc1c97391dae37013`) plus the same queries/ACL/stream (stream byte-identical).
Rows: `lease_out`, `acorn_ef32_3h`, `acorn_ef256_3h`, `curator_se800_3h`. Ratios: 1.38x,
7.34x, 3.96x. Across the three hosts the ranges are 1.0--1.4x (0.95 point), 6.1--7.3x
(near-matched) and 3.3--4.0x (Curator se=800).

## QPS fields

The kernel prints two throughput numbers. `qps` is the per-query estimate
`nq * threads * 1000 / sum(lat)`; `read_s` is the measured reader-phase wall clock, so
`nq / read_s` is the service QPS used by the supplement's reader-scaling table. With
`SKIP_VERIFY=1` the `missing`, `auth` and `leaks` counters are not measured; the run prints zeros,
which are placeholders, not observations.
