# Which source produced which number

Four versions of `lease_certified.cpp` are involved. sha256 of the file in each:

| version | sha256 | where | used for |
|---|---|---|---|
| `c5ef787` snapshot | `6cdc662e973c102b2121bf999a58883d5ff5e85779f85588cd269bd5bdae825d` | `lease_certified_c5ef787.cpp` | official 10k four-reader row (`lease 0.249 ms`, Table I-F); binary md5 `64247916100250d5dba32353fbae1aa2` |
| one-reader build, 2026-11-09 21:58 | `035037b2dfdf3c18afeb4bcf0fcddf9ad4bc10e163b004e6a73914a17ca5edb6` | `lease_certified_1r_20261109.cpp` | one-reader row (0.266/0.324 ms), `experiments/results/review_fix_20261109/lease_1r_official10k/`; binary sha256 `5228fb9d1aaeaa5fe45bb4ff0281c42a0554b460967b93591becfc60e411dc53` on llm_01. Not a git commit: it is the run host's working copy between `c5ef787` and `bf84bb0`. |
| git `1cc536c` (same file content since `afc91b0`) | `42e6e224381e75d8f38c12b17203a9ba175c7c6251e1f27f9a7c89837d44852f` | git history; public repo `LeaseServe-Experiments` at `b3f3f14` ships this content | paired-latency, protocol-control, budget-guard, reader-scaling experiments |
| `1cc536c` + online-stats patch | `e447bfb1c7fc3b77584e4fb35d5e22427ebfa86aae15e794e38a7c74d3171198` | `../lease_certified.cpp` in this package | online probe, `experiments/results/lease_online_fair30k_k1_20261111/` |

Diffs:

- `c5ef787_vs_head.diff`: snapshot to `1cc536c`. Adds measurement instrumentation (`DUMP_LAT`,
  `SKIP_VERIFY`, `certified_reads` / `verified` / `budget_fallbacks` summary fields, reader/QPS
  timing), the budget guard, and the package build comment. The publication protocol is unchanged
  and the instrumentation is env-gated, with one exception: the budget guard is an always-on
  read-path branch that answers exactly when `phi > 1 - 1/rho` or `k*rho > m_max`. For the archived
  k=10 rows it cannot fire (rho=15 puts the phi bound at 93.3%), so those rows keep the snapshot's
  read path.
- `head_1cc536c_vs_online_stats.diff`: `1cc536c` to the shipped file. Adds `upd_last`, `inwin_n`,
  `inwin_p50/p95`, `post_p50/p95` to the summary; no read-path change.

## Algorithm 1 against the shipped kernel (`../lease_certified.cpp`, reader lambda)

| Algorithm 1 | kernel | note |
|---|---|---|
| line 1, pin `e` | `pin = st->version.load()` | the role version is the epoch; one load per read |
| line 3, lease lookup | `st->lease.load()` + ref count | the kernel loads the lease *before* the pin. The order does not matter: the certificate is evaluated on that (lease, pin) pair, and a pin outside the interval falls back |
| line 4, `e` in `[a_L, omega_L]` | `covered = pin >= L->a && pin <= omega` | `PRED=1` switches to `pin == L->a`, `NOLOG=1` sets omega = a |
| line 6, `phi` | `1 - live / (graph nodes + ring_n)` | `live` is the current visible count (O(1)); the kernel reads at the current epoch, so current = pinned |
| line 7, guard | `phi <= 1 - 1/mfac && k*mfac <= mmax`, else exact | `mfac` = rho, `mmax` = m_max (5,000 in the paper) |
| line 9, `m` | `ceil(k*mfac)`, then clamped to the graph size | the `min(m, mmax)` after the guard is unreachable; the only effective clamp is the node count |
| lines 10-12 | graph search + ring scan with `ep <= pin` and `visible(d, pin)`, deduplicated | |
| line 13 | `visible(d, pin)` filter on graph candidates, top-k | |
