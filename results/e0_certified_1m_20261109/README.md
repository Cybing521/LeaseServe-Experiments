# Contract arms at 1M (2026-11-09)

Seven-arm contract driver (`experiments/mechanism/e0_certified_driver.py`) run at n = 1,000,000
on ryzen-host (32 cores), against the **current** kernel source, not an older snapshot.

- kernel: `experiments/cpp/lease_certified.cpp`, sha256
  `6cdc662e973c102b2121bf999a58883d5ff5e85779f85588cd269bd5bdae825d`
  (frozen copy built as `lease_certified_wt`, g++ -O2 -march=native, exit 0, no warnings)
- data: `data/sift1m/base.f32` (1M x 128), `data/sift1m/queries_10k.f32`,
  `acl/erbac100_1m/roles.bin` (100 roles), `acl/erbac100_1m/qroles_head10.bin`
- command:

```
python3 experiments/mechanism/e0_certified_driver.py \
  --bin experiments/cpp/lease_certified_wt \
  --base data/sift1m/base.f32 --queries data/sift1m/queries_10k.f32 \
  --roles acl/erbac100_1m/roles.bin --qroles acl/erbac100_1m/qroles_head10.bin \
  --n 1000000 --nq 200 --changes 60 \
  --out results/e0_certified_1m_wt
```

Result: **7/7 PASS**, wall clock about 5.5 minutes (each arm builds ten ~77k-document leases).

| arm | reads | p50 | p95 | leaks | missing | cert_fb | verdict |
|---|---|---|---|---|---|---|---|
| full | 200 | 0.228 | 0.316 | 0 | 2 | 0 | PASS |
| versioneq | 200 | 14.06 | 21.77 | 0 | 0 | 200 | PASS |
| nolog | 200 | 14.20 | 21.39 | 0 | 0 | 200 | PASS |
| nocert | 200 | 0.233 | 0.329 | 0 | 2 | 0 | PASS |
| full_hole | 200 | 0.267 | 15.66 | 0 | 2 | 77 | PASS |
| nocert_hole | 200 | 0.231 | 0.318 | 0 | 2 | 0 | PASS |
| regrant_window | 200 | 0.237 | 0.331 | 0 | 2 | 0 | PASS |

Notes, stated so they are not over-read:

- `missing = 2` in the approximate arms is **ann**, not auth: `auth = 0` and `leaks = 0` in every
  arm. The driver does not assert `missing == 0`; the paper's zero-missing claims are for the
  exact-certified configuration, not for this budget.
- In this run the injected hole (`full_hole`) did **not** surface as an auth miss: both the gated
  and ungated arms end with the same two ann misses. The 1M evidence is that the certificate
  *fires* (77 of 200 reads fall back) where the ungated arm answers all of them. The silent-loss
  evidence remains the 200k counterexample (93/100 and 149 auth misses).
- `regrant_violations` and `live_mismatch` are printed to stdout, not written into `summary.txt`.
  They were re-captured by re-running that arm with stdout redirected:
  `regrant_stdout/stdout.txt` shows `regrant_violations=0 live_mismatch=0` over 2,000 changes.
- An earlier run of the same driver against an older kernel snapshot (sha256 46a3e1e0…, before the
  `SCRIPT`/`ever_mu` changes) produced 0.231/14.60/77 fallbacks: the arms are stable across the two
  kernel versions, but only this run corresponds to the shipped source.
