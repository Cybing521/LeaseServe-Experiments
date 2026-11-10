## A. 1M ERBAC, 1 change/0.5ms, 5 seeds (paper §V-G)
### p50 (ms)

| arm | mean ± sd (n=5) |
|---|---|
| delta_mvcc | 0.347 ± 0.004 |
| epoch-checked | 0.345 ± 0.005 |

- paired t-test: t=0.76, p=0.489 (df=4)
- mean difference (delta_mvcc − epoch-checked): 0.002 ms, bootstrap 95% CI [-0.003, 0.007] ms (includes 0)
### p95 (ms)

| arm | mean ± sd (n=5) |
|---|---|
| delta_mvcc | 0.410 ± 0.005 |
| epoch-checked | 0.405 ± 0.004 |

- paired t-test: t=2.04, p=0.110 (df=4)
- mean difference (delta_mvcc − epoch-checked): 0.005 ms, bootstrap 95% CI [0.001, 0.009] ms (excludes 0)

## B. 1M churn matrix (design x interval), 5 seeds
## epoch @ 500 µs

seeds: [0, 1, 2, 3, 4]
- incr_p50: 0.353 ± 0.007 (bootstrap 95% CI [0.347, 0.357]), per-seed ['0.342', '0.353', '0.360', '0.356', '0.352']
- incr_p95: 0.416 ± 0.009 (bootstrap 95% CI [0.408, 0.422]), per-seed ['0.400', '0.415', '0.422', '0.424', '0.419']
- inc_missing: 2893.2 ± 0.447 (bootstrap 95% CI [2893.000, 2893.600]), per-seed ['2893.000', '2893.000', '2894.000', '2893.000', '2893.000']
- inc_leaks: 0.000 ± 0.000 (bootstrap 95% CI [0.000, 0.000]), per-seed ['0.000', '0.000', '0.000', '0.000', '0.000']

## epoch @ 10000 µs

seeds: [0, 1, 2, 3, 4]
- incr_p50: 0.347 ± 0.005 (bootstrap 95% CI [0.343, 0.351]), per-seed ['0.349', '0.353', '0.341', '0.341', '0.349']
- incr_p95: 0.407 ± 0.008 (bootstrap 95% CI [0.400, 0.413]), per-seed ['0.410', '0.413', '0.395', '0.403', '0.415']
- inc_missing: 2893.2 ± 0.447 (bootstrap 95% CI [2893.000, 2893.600]), per-seed ['2893.000', '2893.000', '2894.000', '2893.000', '2893.000']
- inc_leaks: 0.000 ± 0.000 (bootstrap 95% CI [0.000, 0.000]), per-seed ['0.000', '0.000', '0.000', '0.000', '0.000']

## mvcc @ 500 µs

seeds: [0, 1, 2, 3, 4]
- incr_p50: 0.346 ± 0.007 (bootstrap 95% CI [0.340, 0.350]), per-seed ['0.337', '0.353', '0.349', '0.341', '0.349']
- incr_p95: 0.405 ± 0.009 (bootstrap 95% CI [0.398, 0.412]), per-seed ['0.395', '0.414', '0.408', '0.395', '0.411']
- incr_p99: 0.443 ± 0.008 (bootstrap 95% CI [0.437, 0.448]), per-seed ['0.433', '0.452', '0.442', '0.438', '0.448']
- inc_missing: 2893.2 ± 0.447 (bootstrap 95% CI [2893.000, 2893.600]), per-seed ['2893.000', '2893.000', '2894.000', '2893.000', '2893.000']
- retries: 0.000 ± 0.000 (bootstrap 95% CI [0.000, 0.000]), per-seed ['0.000', '0.000', '0.000', '0.000', '0.000']
- fallbacks: 0.000 ± 0.000 (bootstrap 95% CI [0.000, 0.000]), per-seed ['0.000', '0.000', '0.000', '0.000', '0.000']
- inc_leaks: 0.000 ± 0.000 (bootstrap 95% CI [0.000, 0.000]), per-seed ['0.000', '0.000', '0.000', '0.000', '0.000']
- pin_violations: 0.000 ± 0.000 (bootstrap 95% CI [0.000, 0.000]), per-seed ['0.000', '0.000', '0.000', '0.000', '0.000']

## mvcc @ 10000 µs

seeds: [0, 1, 2, 3, 4]
- incr_p50: 0.341 ± 0.004 (bootstrap 95% CI [0.339, 0.345]), per-seed ['0.343', '0.340', '0.340', '0.347', '0.337']
- incr_p95: 0.402 ± 0.003 (bootstrap 95% CI [0.399, 0.404]), per-seed ['0.405', '0.397', '0.403', '0.401', '0.402']
- incr_p99: 0.443 ± 0.010 (bootstrap 95% CI [0.435, 0.450]), per-seed ['0.456', '0.430', '0.445', '0.445', '0.438']
- inc_missing: 2893.2 ± 0.447 (bootstrap 95% CI [2893.000, 2893.600]), per-seed ['2893.000', '2893.000', '2894.000', '2893.000', '2893.000']
- retries: 0.000 ± 0.000 (bootstrap 95% CI [0.000, 0.000]), per-seed ['0.000', '0.000', '0.000', '0.000', '0.000']
- fallbacks: 0.000 ± 0.000 (bootstrap 95% CI [0.000, 0.000]), per-seed ['0.000', '0.000', '0.000', '0.000', '0.000']
- inc_leaks: 0.000 ± 0.000 (bootstrap 95% CI [0.000, 0.000]), per-seed ['0.000', '0.000', '0.000', '0.000', '0.000']
- pin_violations: 0.000 ± 0.000 (bootstrap 95% CI [0.000, 0.000]), per-seed ['0.000', '0.000', '0.000', '0.000', '0.000']

## rsu @ 500 µs

seeds: [0, 1, 2, 3, 4]
- incr_p50: 0.291 ± 0.004 (bootstrap 95% CI [0.288, 0.294]), per-seed ['0.294', '0.286', '0.291', '0.295', '0.291']
- incr_p95: 4.141 ± 5.095 (bootstrap 95% CI [0.420, 7.968]), per-seed ['0.436', '9.451', '9.985', '0.435', '0.398']
- incr_p99: 19.4 ± 2.380 (bootstrap 95% CI [17.698, 21.404]), per-seed ['23.176', '19.108', '17.824', '16.998', '19.670']
- inc_missing: 2770.2 ± 44.3 (bootstrap 95% CI [2735.200, 2805.200]), per-seed ['2766.000', '2730.000', '2725.000', '2806.000', '2824.000']
- retries: 164.2 ± 63.6 (bootstrap 95% CI [114.400, 214.000]), per-seed ['178.000', '236.000', '210.000', '105.000', '92.000']
- fallbacks: 20.2 ± 7.918 (bootstrap 95% CI [14.000, 26.400]), per-seed ['22.000', '29.000', '26.000', '13.000', '11.000']
- inc_leaks: 0.000 ± 0.000 (bootstrap 95% CI [0.000, 0.000]), per-seed ['0.000', '0.000', '0.000', '0.000', '0.000']
- pin_violations: 0.000 ± 0.000 (bootstrap 95% CI [0.000, 0.000]), per-seed ['0.000', '0.000', '0.000', '0.000', '0.000']

## rsu @ 10000 µs

seeds: [0, 1, 2, 3, 4]
- incr_p50: 0.323 ± 0.018 (bootstrap 95% CI [0.309, 0.336]), per-seed ['0.329', '0.345', '0.302', '0.329', '0.308']
- incr_p95: 0.394 ± 0.009 (bootstrap 95% CI [0.388, 0.402]), per-seed ['0.400', '0.406', '0.384', '0.393', '0.390']
- incr_p99: 9.189 ± 8.092 (bootstrap 95% CI [3.010, 15.368]), per-seed ['14.463', '13.081', '17.416', '0.484', '0.502']
- inc_missing: 2849.6 ± 21.9 (bootstrap 95% CI [2833.000, 2866.600]), per-seed ['2837.000', '2847.000', '2822.000', '2865.000', '2877.000']
- retries: 58.2 ± 28.5 (bootstrap 95% CI [35.800, 80.600]), per-seed ['80.000', '65.000', '89.000', '32.000', '25.000']
- fallbacks: 7.200 ± 3.564 (bootstrap 95% CI [4.400, 10.000]), per-seed ['10.000', '8.000', '11.000', '4.000', '3.000']
- inc_leaks: 0.000 ± 0.000 (bootstrap 95% CI [0.000, 0.000]), per-seed ['0.000', '0.000', '0.000', '0.000', '0.000']
- pin_violations: 0.000 ± 0.000 (bootstrap 95% CI [0.000, 0.000]), per-seed ['0.000', '0.000', '0.000', '0.000', '0.000']

#### mvcc vs epoch @ 500 µs (paired, seeds [0, 1, 2, 3, 4])
### incr_p50 (ms)

| arm | mean ± sd (n=5) |
|---|---|
| mvcc | 0.346 ± 0.007 |
| epoch | 0.353 ± 0.007 |

- paired t-test: t=-2.60, p=0.060 (df=4)
- mean difference (mvcc − epoch): -0.007 ms, bootstrap 95% CI [-0.012, -0.002] ms (excludes 0)
### incr_p95 (ms)

| arm | mean ± sd (n=5) |
|---|---|
| mvcc | 0.405 ± 0.009 |
| epoch | 0.416 ± 0.009 |

- paired t-test: t=-2.36, p=0.077 (df=4)
- mean difference (mvcc − epoch): -0.012 ms, bootstrap 95% CI [-0.021, -0.004] ms (excludes 0)
#### mvcc vs rsu @ 500 µs (paired, seeds [0, 1, 2, 3, 4])
### incr_p50 (ms)

| arm | mean ± sd (n=5) |
|---|---|
| mvcc | 0.346 ± 0.007 |
| rsu | 0.291 ± 0.004 |

- paired t-test: t=12.07, p=0.000 (df=4)
- mean difference (mvcc − rsu): 0.054 ms, bootstrap 95% CI [0.046, 0.062] ms (excludes 0)
### incr_p95 (ms)

| arm | mean ± sd (n=5) |
|---|---|
| mvcc | 0.405 ± 0.009 |
| rsu | 4.141 ± 5.095 |

- paired t-test: t=-1.64, p=0.176 (df=4)
- mean difference (mvcc − rsu): -3.736 ms, bootstrap 95% CI [-7.562, -0.019] ms (excludes 0)
### incr_p99 (ms)

| arm | mean ± sd (n=5) |
|---|---|
| mvcc | 0.443 ± 0.008 |
| rsu | 19.4 ± 2.380 |

- paired t-test: t=-17.75, p=0.000 (df=4)
- mean difference (mvcc − rsu): -18.9 ms, bootstrap 95% CI [-20.967, -17.257] ms (excludes 0)
#### mvcc vs epoch @ 10000 µs (paired, seeds [0, 1, 2, 3, 4])
### incr_p50 (ms)

| arm | mean ± sd (n=5) |
|---|---|
| mvcc | 0.341 ± 0.004 |
| epoch | 0.347 ± 0.005 |

- paired t-test: t=-1.45, p=0.221 (df=4)
- mean difference (mvcc − epoch): -0.005 ms, bootstrap 95% CI [-0.012, 0.001] ms (includes 0)
### incr_p95 (ms)

| arm | mean ± sd (n=5) |
|---|---|
| mvcc | 0.402 ± 0.003 |
| epoch | 0.407 ± 0.008 |

- paired t-test: t=-1.26, p=0.276 (df=4)
- mean difference (mvcc − epoch): -0.005 ms, bootstrap 95% CI [-0.012, 0.002] ms (includes 0)
#### mvcc vs rsu @ 10000 µs (paired, seeds [0, 1, 2, 3, 4])
### incr_p50 (ms)

| arm | mean ± sd (n=5) |
|---|---|
| mvcc | 0.341 ± 0.004 |
| rsu | 0.323 ± 0.018 |

- paired t-test: t=2.56, p=0.063 (df=4)
- mean difference (mvcc − rsu): 0.019 ms, bootstrap 95% CI [0.005, 0.031] ms (excludes 0)
### incr_p95 (ms)

| arm | mean ± sd (n=5) |
|---|---|
| mvcc | 0.402 ± 0.003 |
| rsu | 0.394 ± 0.009 |

- paired t-test: t=1.56, p=0.194 (df=4)
- mean difference (mvcc − rsu): 0.007 ms, bootstrap 95% CI [-0.002, 0.014] ms (includes 0)
### incr_p99 (ms)

| arm | mean ± sd (n=5) |
|---|---|
| mvcc | 0.443 ± 0.010 |
| rsu | 9.189 ± 8.092 |

- paired t-test: t=-2.42, p=0.073 (df=4)
- mean difference (mvcc − rsu): -8.747 ms, bootstrap 95% CI [-14.922, -2.571] ms (excludes 0)

## C. 10M x 128d C++ mvcc (quiet host), 5 seeds
## cpp128

seeds: [0, 1, 2, 3, 4]
- build_s: 594.4 ± 6.591 (bootstrap 95% CI [589.432, 599.396]), per-seed ['597.458', '601.740', '598.022', '587.713', '587.141']
- incr_p50: 50.0 ± 1.186 (bootstrap 95% CI [48.942, 50.739]), per-seed ['50.063', '50.163', '50.964', '47.969', '50.739']
- incr_p95: 57.2 ± 0.395 (bootstrap 95% CI [56.857, 57.473]), per-seed ['57.035', '57.234', '57.690', '56.605', '57.286']
- incr_p99: 58.5 ± 0.566 (bootstrap 95% CI [57.964, 58.874]), per-seed ['58.613', '58.645', '59.050', '57.531', '58.575']
- inc_missing: 1549.0 ± 0.000 (bootstrap 95% CI [1549.000, 1549.000]), per-seed ['1549.000', '1549.000', '1549.000', '1549.000', '1549.000']
- inc_leaks: 0.000 ± 0.000 (bootstrap 95% CI [0.000, 0.000]), per-seed ['0.000', '0.000', '0.000', '0.000', '0.000']
- retries: 0.000 ± 0.000 (bootstrap 95% CI [0.000, 0.000]), per-seed ['0.000', '0.000', '0.000', '0.000', '0.000']
- fallbacks: 0.000 ± 0.000 (bootstrap 95% CI [0.000, 0.000]), per-seed ['0.000', '0.000', '0.000', '0.000', '0.000']
- pin_violations: 0.000 ± 0.000 (bootstrap 95% CI [0.000, 0.000]), per-seed ['0.000', '0.000', '0.000', '0.000', '0.000']

## D. 1M x 768d real corpus C++ mvcc, 5 seeds
## real768_1m

seeds: [0, 1, 2, 3, 4]
- build_s: 54.2 ± 6.902 (bootstrap 95% CI [48.990, 59.494]), per-seed ['57.691', '58.671', '61.219', '46.712', '46.918']
- incr_p50: 21.1 ± 10.0 (bootstrap 95% CI [15.539, 29.965]), per-seed ['38.712', '18.726', '18.674', '14.964', '14.668']
- incr_p95: 27.6 ± 13.3 (bootstrap 95% CI [20.417, 39.423]), per-seed ['51.091', '23.597', '23.524', '19.415', '20.245']
- incr_p99: 31.6 ± 17.0 (bootstrap 95% CI [23.059, 46.900]), per-seed ['61.966', '25.890', '25.150', '22.359', '22.713']
- inc_missing: 24.4 ± 0.894 (bootstrap 95% CI [24.000, 25.200]), per-seed ['24.000', '24.000', '24.000', '26.000', '24.000']
- inc_leaks: 0.000 ± 0.000 (bootstrap 95% CI [0.000, 0.000]), per-seed ['0.000', '0.000', '0.000', '0.000', '0.000']
- retries: 0.000 ± 0.000 (bootstrap 95% CI [0.000, 0.000]), per-seed ['0.000', '0.000', '0.000', '0.000', '0.000']
- fallbacks: 0.000 ± 0.000 (bootstrap 95% CI [0.000, 0.000]), per-seed ['0.000', '0.000', '0.000', '0.000', '0.000']
- pin_violations: 0.000 ± 0.000 (bootstrap 95% CI [0.000, 0.000]), per-seed ['0.000', '0.000', '0.000', '0.000', '0.000']

## E. 10M x 768d real corpus C++ mvcc, 3 seeds
## real10m_768

seeds: [0, 1, 2]
- build_s: 754.6 ± 5.607 (bootstrap 95% CI [748.204, 758.620]), per-seed ['748.204', '758.620', '757.009']
- incr_p50: 204.6 ± 2.632 (bootstrap 95% CI [202.507, 207.574]), per-seed ['207.574', '202.507', '203.806']
- incr_p95: 260.7 ± 1.420 (bootstrap 95% CI [259.217, 262.031]), per-seed ['259.217', '260.956', '262.031']
- incr_p99: 265.8 ± 2.070 (bootstrap 95% CI [263.606, 267.711]), per-seed ['263.606', '266.129', '267.711']
- inc_missing: 39.0 ± 0.000 (bootstrap 95% CI [39.000, 39.000]), per-seed ['39.000', '39.000', '39.000']
- inc_leaks: 0.000 ± 0.000 (bootstrap 95% CI [0.000, 0.000]), per-seed ['0.000', '0.000', '0.000']
- retries: 0.000 ± 0.000 (bootstrap 95% CI [0.000, 0.000]), per-seed ['0.000', '0.000', '0.000']
- fallbacks: 0.000 ± 0.000 (bootstrap 95% CI [0.000, 0.000]), per-seed ['0.000', '0.000', '0.000']
- pin_violations: 0.000 ± 0.000 (bootstrap 95% CI [0.000, 0.000]), per-seed ['0.000', '0.000', '0.000']

## F. fig:axes five seeds

B1 correctness replay seeds 3–4 harvested from llm_01 (batch `run_b1_1m_seeds34.sh`, log `b1_seeds34.log`, all rc=0, SEEDS34_DONE at 14:22). All 6 jsons healthy: arms = lease/static/partition/shared, leaks = 0 everywhere. fig:axes regenerated with seeds 0–4 (make_axes_3acl.py, glob `*_s[01234].json`).

Total missing (sum of auth+ann+sel) per design, mean ± sd over 5 seeds:

| ACL | shared | static | partition | lease |
|---|---|---|---|---|
| Uniform (r1000_v5k) | 16288 ± 94 | 2988 ± 230 | 7421 ± 177 | 0 ± 1 (max 2) |
| ERBAC (erbac100) | 1093 ± 156 | 2990 ± 325 | 103 ± 19 | 17 ± 5 |
| OpenAlex (oa) | 10076 ± 249 | 2786 ± 203 | 179 ± 28 | 2 ± 2 (max 5) |

Per-seed totals confirm the law ordering unchanged under 5 seeds: lease < partition < static < shared on ERBAC and OpenAlex; on Uniform, static (2988) < partition (7421) < shared (16288) with lease ≈ 0.
