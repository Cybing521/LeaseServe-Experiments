# review_fix_20261109 — evidence for the strict-review P0/P1 fixes

Host: Apple Silicon (arm64), Apple clang, `g++ -O2 -std=c++17`; the kernel runs without
AVX-512 here, so only correctness counts are quoted from this directory. The 1M latency rows
elsewhere in the package come from the x86 servers.

Binary: `g++ -O2 -std=c++17 -I<hnswlib> experiments/cpp/lease_certified.cpp -o lease_certified`.

## Files

| file | command | result |
|---|---|---|
| `script_regression.txt` | `python3 experiments/mechanism/script_regression.py --bin lease_certified` | 23/23 PASS |
| `negative_control_old_rule.txt` | same, against a binary with `materialized = grant_v[d] <= a_L` restored | 20/23 PASS; the three failures are the authorization checks |
| `wal_lifecycle.txt` | `python3 experiments/mechanism/e0_wal_lifecycle.py --bin lease_certified --seqd experiments/service/seqd.py` | 28 checks, ALL PASS (includes B5 stalled commit) |

The other suites: `e0_certified_tests.py` 14/14, `test_wal_recovery.py` 18/18,
`test_promotion_restart.py` 6/6. `contract_interval.txt` is the rewritten
`e0_contract_tests.py`: nine tests against the same interval model (C1-C9, 9/9), replacing the
old version-pinned suite.

`1m_nocert_drop5/` is a server re-run of the 1M `nocert_drop5` arm with the repaired kernel and the
independent oracle: kernel `missing=3 auth=0 leaks=0`, oracle `leaks=0 missing=3`, identical to the
recorded 11-08 run. `200k_oracle/` is a 200k certified run whose oracle cross-check is
`leaks=0 missing=0`. See each directory's `README.md`.

## What the scripted regression runs

One deterministic stream, four arms. Role 0 holds document 0; document 7 is the exact top-1 of
query 0.

```
grant 0 7
revoke 0 7
merge 0
grant 0 7 lost     # state changes, the log entry does not
```

| arm | CERT | fallback | returned top-1 | missing | auth |
|---|---|---|---|---|---|
| cert | 1 | 4/4 | 7 | 0 | 0 |
| nocert | 0 | 0/4 | 0 | 1 | 1 |
| control | 1 | 0/4 | 7 | 0 | 0 |
| nocert_control | 0 | 0/4 | 7 | 0 | 0 |

The `nocert` row is the P0-1 witness: before the fix the kernel reported `missing=1, auth=0`
(the miss was charged to ANN); the independent oracle computes `auth=1`. The same split appears
on a randomized 300-change stream at seed 5 (`missing=1, auth=0` on the old rule, `auth=1` now).

## B5: stalled commit does not stall the fallback

`seqd.py --stall-after 1` acknowledges the first commit and never answers the second. A lost
grant freezes `omega` at 0 so every read falls back; readers start 400 ms in, while the writer
waits on the 1.5 s client timeout. Summary of the run:

```
reads=4 p50=0.000917 p95=0.019667 leaks=0 missing=0 auth=0
cert_fallbacks=4 wal_refused=1
```

The fallback p50 is 0.0009 ms; it does not wait for the writer's state lock. The script runs in
its own thread, so the readers (created after it starts) genuinely overlap the blocked commit: the
process takes 1.55 s, i.e. the 1.5 s timeout plus startup, while the reads complete at 0.0009 ms.
The suite asserts p50 < 500 ms.
