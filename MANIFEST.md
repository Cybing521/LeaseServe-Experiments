# Result manifest

Inputs (md5): queries `2b294e6cdaafcaf9a1e6dd35ba43fb31`; ACL roles
`dbcf99d67fff42f9caf26d58994c5b21`; qroles `de4038ffd576df0f1f9687e3edd72469`;
changes `7bd83ae974948840fa834285a099f4a4`. The SIFT base is not bundled; its md5
on the three hosts was `0caeb02f1cc236dbc1c97391dae37013`.

| Result | Directory |
|---|---|
| Official 10k, Xeon (main table) | `results/ab_stream_official10k_20261109/{lease_real2,acorn_ef32_real2,acorn_ef256_real2,curator_se800_real2,curator_se3200_real2}` |
| Run-to-run repetitions | `results/ab_stream_official10k_20261109/reps/` |
| Reader scaling, oracle off | `results/ab_stream_official10k_20261109/readers_noverify/` |
| Second host (Ryzen) | `results/ab_stream_official10k_20261109/secondhost/` |
| Third host (Gold 6330) | `results/ab_stream_official10k_20261109/thirdhost/` |
| Curator protocol control | `results/ab_stream_official10k_20261109/protocol_control/` |
| Cold-role churn | `results/ab_stream_official10k_20261109/coldrole/` |
| Host diagnosis (mfac, exact) | `results/ab_stream_official10k_20261109/diagnostics/` |

Key ratios (p50, same host): ACORN ef=32 1.20--1.26 (Xeon), 1.01 (Ryzen),
1.38 (Gold); ACORN ef=256 6.91--7.12, 6.12, 7.34; Curator se=800 3.70--3.75,
3.28, 3.96 instrumented, and 2.09--2.70 precomputed; Curator se=3200 23.1
(Xeon) and 22.0 (Gold) instrumented, 23.1/14.5/22.0 precomputed.

| Table IV (coverage arms) | `results/arms_cov_20261108/` |
| 1M certificate suite | `results/e0_certified_1m_20261109/` |
| Return-length sweep | `results/e1_returnlen_20261109/` |
| Multi-writer | `results/multiwriter_20261107/` |
| Repair/refill | `results/repair_20261109/`, `results/repair_refill2_20261109/` |
| PRO-HNSW baseline | `results/official_prohnsw_20261109/` |
| Static-vs-warm | `results/ab_static_vs_warm_20261109/` |

Paths inside `cmd.txt` are normalized: `$REPO` is this repository, `$DATA` is
the corpus directory. Host names are replaced by `xeon-host`, `ryzen-host`,
and `gold-host`.
