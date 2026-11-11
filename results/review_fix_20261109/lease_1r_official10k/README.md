# One-reader lease on the official SIFT1M 10k queries

Check for the tenth-review W1 (the paper's lease rows use four readers; ACORN/Curator are
single-threaded). Same inputs as `experiments/results/ab_stream_official10k_20261109/lease_real2/`:

```
CHANGES_OUT=stream.txt WARMUP=100 CERT=1 CHANGE_US=500 DELTA_CAP=4096 MERGE_EVERY=0 \
DROP_GRANT=0 WRITERS=1 MMAX=5000 \
lease_certified_new base.f32 queries10k_real.f32 roles.bin qroles_10000.bin \
  200000 128 10000 10 10 100 1 0 out 15
```

Host llm_01, 2026-11-10 00:0x; build 15.2 s.

| config | p50 | p95 | missing | recall@10 |
|---|---|---|---|---|
| lease, 4 readers (`lease_real2`) | 0.2488 | 0.2949 | 46 | 0.99954 |
| lease, 1 reader (this run) | 0.2656 | 0.3240 | 46 | 0.99954 |
| ACORN ef=32, 1 thread | 0.3081 | 0.3639 | — | 0.95035 |
| ACORN ef=256, 1 thread | 1.7417 | 1.9922 | — | 0.99943 |
| Curator se=800, 1 thread | 0.9599 | 1.2672 | — | 0.97177 |
| Curator se=3200, 1 thread | 6.4871 | 7.6018 | — | 0.99978 |

Matched one-reader ratios: ACORN ef=32 1.16x, Curator se=800 3.61x, ACORN ef=256 6.56x,
Curator se=3200 24.4x. The paper's 1.24x / 3.86x / 7.00x / 26.1x use the four-reader lease; the
4-reader row is 6.7% faster than one reader, so the ordering and the qualitative claim hold but the
paper's ratios are the four-reader-vs-one-thread numbers.

## Provenance (added 2026-11-12)

- `cmd.txt`: the command line and environment written by the binary into its output directory on
  llm_01 (`/root/work/icde/results/review_10th/lease_1r/out/`, 2026-11-10 00:08).
- `summary.txt`: sha256 `2a44bc600690eb9797b8b3f6879249751c7193b4b4d74f1811f0ce56e3c11ba8`, identical
  to the file on the run host. p50=0.265594, p95=0.323998, missing=46 of 100,000.
- Change stream: sha256 `e6de478d...`, identical to
  `../../ab_stream_official10k_20261109/changes100.txt`.
- Source: `experiments/cpp/source_snapshots/lease_certified_1r_20261109.cpp`, sha256 `035037b2...`
  (see that directory's README).
- Single run, one host; no repetition, so no spread is reported for this row.
