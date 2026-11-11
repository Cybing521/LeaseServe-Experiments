# Lease online, 30k queries, one role, all 50 changes on it (2026-11-11 22:30; replay rescored 2026-11-12)

Why this run exists: the earlier 30k lease probe (`lease_online_fair30k_20261111/`) promoted ten
head roles (K=10) while every query targets role 49. The change stream picks uniformly among the
promoted roles, so only **2 of the 50 changes** landed on the queried role (visible in that run's
`CHANGES_OUT`: 2 lines for role 49), whereas the ACORN and Curator drives change their queried role
50 times. This run sets K=1, so all 50 changes target role 49 and the churn matches.

Protocol: 200k SIFT ERBAC ACL, role 49, 1 reader + 1 writer, 50 changes at 30 ms (the first commits before the
reader starts, the other 49 during the 1.67--1.68 s read phase; last change at 1.476 s), 500-document toggle pool, 3 seeds, `SKIP_VERIFY=1`. The run records each read's
pin and returned documents (`QUERIES_OUT`) and every committed change (`CHANGES_OUT`), so recall is
scored offline on the same run whose latency is reported.

Build: `g++ -O3 -march=native -std=c++17 -I experiments/cpp/third_party lease_certified.cpp -o
lease_online_v2 -pthread` (source `experiments/cpp/lease_certified.cpp`, the version with
`QUERIES_OUT`; the server copy before this edit is kept as `lease_certified.cpp.bak2211`).

```
SKIP_VERIFY=1 CHANGE_US=30000 WARMUP=1 CERT=1 WRITERS=1 MMAX=5000 \
  QUERIES_OUT=k1_30_s<seed>.txt CHANGES_OUT=k1_chg_s<seed>.txt \
  ./lease_online_v2 base.f32 queries_30k.f32 roles.bin qroles_30k.bin \
  200000 128 30000 10 1 50 1 <seed> k1_30_s<seed> 15
```

| seed | p50 | p95 | p99 | read_s | qps | in-window n | in-window p50/p95 | post p50/p95 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0.056507 | 0.063990 | 0.066956 | 1.671 | 17,974 | 26,517 | 0.056466/0.063920 | 0.056777/0.064491 |
| 1 | 0.056837 | 0.064341 | 0.067126 | 1.680 | 17,875 | 26,345 | 0.056857/0.064331 | 0.056656/0.064431 |
| 2 | 0.056727 | 0.064161 | 0.067086 | 1.677 | 17,914 | 26,394 | 0.056737/0.064141 | 0.056637/0.064301 |

Timing, as recorded: `WARMUP=1` commits change 1 before the reader starts, so the first recorded pin
is 1 (never 0) and **49 of the 50 changes commit during the read phase** (`straddled=49`,
`upd_last=1.476 s` of `read_s=1.671-1.680 s`). About 520-540 reads pin each of epochs 1..49 and
about 3,650 pin epoch 50. `cert_fallbacks=0`, `budget_fallbacks=0`, `leaks=0`, `merges=0` in the
kernel counters.

## Offline recall (same run)

Raw records (shipped): `k1_30_s<seed>.txt` (QUERIES_OUT; one line per read: `q pin role n doc...`)
and `k1_chg_s<seed>.txt` (CHANGES_OUT; `epoch kind role doc`; 20 grants + 30 revokes in seed 0).

```
python3 experiments/mechanism/replay_online_truth.py base.f32 queries_30k.f32 roles.bin \
    k1_30_s<seed>.txt k1_chg_s<seed>.txt > replay_s<seed>.txt
```

The script folds the committed event stream into V(r,e) at each recorded pin and recomputes the
exact top-10 with numpy in float64 (no kernel code). SIFT is integer-valued, so distances are exact
and scoring is tie-aware: a returned document is a hit iff its distance is <= the 10th exact
distance (30 reads per seed have a tie at rank 10).

Attribution uses the materialized set, not grant times. The run has `merges=0`, so G is the role
set at build and W(L) = G + log. `auth` = a missed exact-top-10 document that is NOT a node of G
(it could only have come from the log); `ann_or_sel` = a missed document that is a node of G.
`log_only_truth` counts exact-top-10 documents that are not in G, i.e. the cases where auth could
be non-zero; without them auth=0 would be vacuous.

| seed | missing / checked | recall | auth | ann_or_sel | leaks | log-only truth / returned | pins |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 130 / 300,000 | 0.999567 | 0 | 130 | 0 | 201 / 201 | 50 |
| 1 | 129 / 300,000 | 0.999570 | 0 | 129 | 0 | 312 / 312 | 50 |
| 2 | 129 / 300,000 | 0.999570 | 0 | 129 | 0 | 235 / 235 | 50 |

The kernel's own verified run (`../lease_online_fair30k_20261111/`, 129/300,000 in each seed)
agrees.

Superseded: `superseded/replay_v1_s*.txt` report 140/142/146. That first script computed distances
in float32 through the |q|^2+|v|^2-2qv expansion, whose rounding misorders documents near rank 10 and
counted 10/13/17 correct answers as misses (rerun 2026-11-12: v1 still prints 140/142/146 on these
files; the float64 script gives 130 on seed 0 with or without tie handling, so ties are not the cause), and its `auth` test used the grant epoch instead of membership in G.
The supplement now quotes the table above. Latency rows are unaffected.
