# Run-time source snapshot vs shipped HEAD

- `lease_certified_c5ef787.cpp`: the source at commit `c5ef787`, sha256
  `6cdc662e973c102b2121bf999a58883d5ff5e85779f85588cd269bd5bdae825d`. The official
  10k rows (`lease 0.249 ms`) were produced by a binary built from this tree
  (binary md5 `64247916100250d5dba32353fbae1aa2`).
- `c5ef787_vs_head.diff`: the full diff to the shipped HEAD (sha256
  `42e6e224381e75d8f38c12b17203a9ba175c7c6251e1f27f9a7c89837d44852f`). It adds
  measurement instrumentation (`DUMP_LAT`, `SKIP_VERIFY`, `certified_reads` /
  `verified` / `budget_fallbacks` summary fields, reader/QPS timing), the budget guard, and the
  package build comment. The publication protocol is unchanged and the instrumentation is
  env-gated, with one exception: the budget guard is an always-on read-path branch that answers
  exactly when `phi > 1 - 1/rho` or `k*rho > m_max`. For the archived k=10 rows it cannot fire
  (rho=15 puts the phi bound at 93.3%), so those rows keep the snapshot's read path.

Every official table row uses the snapshot; the instrumented HEAD is used for the
paired-latency, protocol-control, budget-guard and reader-scaling experiments,
which are labelled as such in the supplement.
