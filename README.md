# LeaseServe Experiments

Experiment code, ACL inputs, and raw results for coverage-certified vector-search
leases (ICDE 2027 Research Track submission).

## Supplemental material

This repository is the primary entry point for the submission's artifact.

- **Code, ACL inputs, raw results, reproduction scripts** — here. `MANIFEST.md`
  maps every table in the paper to the directory that produced it.
- **Written supplement, 16 pages of measurement detail** —
  <https://github.com/Cybing521/LeaseServe-Supplement>

The paper is self-contained: all lemmas, theorems and propositions are stated and
argued in its 12 pages, and the supplement repeats none of them. This repository
contains no manuscript source or paper text.

## Layout

```
cpp/                     C++ kernel, baseline drivers, ACL inputs
  lease_certified.cpp    certified lease kernel (read path, writer, WAL, tests hooks)
  acorn_acl.cpp          ACORN driver used for the 10k table
  curator_acl.cpp        Curator driver used for the 10k table
  curator_acl_precomputed.cpp  same, precomputed-truth protocol control
  exact_concurrent*.cpp  AVX-512 exact-scan path of IV-a
  arms_compare*.cpp      five-arm and four-arm same-runtime comparisons
  veda_lattice.cpp       Veda-style lattice baseline
  source_snapshots/      kernel snapshot that produced the official rows, plus its diff
  gen_erbac_acl.py       ERBAC ACL generator
  acl/erbac200k/         roles.bin, qroles_10000.bin, qroles_all.bin
  test/                  tiny fixture that the service tests use
  third_party/hnswlib/   stock hnswlib headers (MIT)
hnswlib_fix/             patch for ASan/TSan builds only (one header, see the paper's IV-d)
boundary/                controlled A-E boundary harness and its raw matrices
mechanism/               test and statistics scripts
service/                 WAL sequencer and its lifecycle tests
results/                 raw run outputs (summary.txt, cmd.txt, logs)
scripts/                 reproduction shell scripts
MANIFEST.md              result -> claim index
```

## Build

```
g++ -O3 -march=native -std=c++17 -I cpp/third_party \
    cpp/lease_certified.cpp -o lease_certified -pthread
```

Tests, from the repository root (`lease_certified` built as above):

```
python3 mechanism/script_regression.py --bin ./lease_certified   # 23 checks
python3 mechanism/e0_contract_tests.py                           # 9
python3 mechanism/e0_certified_tests.py                          # 14
python3 service/test_promotion_restart.py --bin ./lease_certified # 6
python3 service/test_wal_recovery.py --bin ./lease_certified      # 18
```

These scripts locate the tree in either layout (`experiments/cpp/...` in the paper's
project, `cpp/...` here), so the same files run in both.

Curator's incremental grant/revoke path is measured in
`results/curator_incremental_20261110/` (driver and log). The online probe is single-role
(role 49), 1R+1W, 30 ms: ACORN `results/acorn_online_fair30k_20261111/` (timing-only rerun
`results/acorn_online_fair30k_clean_20261111/`, sample control
`results/acorn_online_fair30k_samplectl_20261111/`), Curator
`results/curator_online_fair2_20261111/`, and the lease run the supplement reports,
`results/lease_online_fair30k_k1_20261111/` (K=1, so all 50 changes hit the queried role).
It is not a 10-role workload or long-term ACL drift.

That lease run ships each read's pin and returned documents (`k1_30_s<seed>.txt`) and the
committed change stream (`k1_chg_s<seed>.txt`), so recall is recomputed offline, without
kernel code:

```
python3 mechanism/replay_online_truth.py base.f32 queries_30k.f32 \
    cpp/acl/erbac200k/roles.bin results/lease_online_fair30k_k1_20261111/k1_30_s0.txt \
    results/lease_online_fair30k_k1_20261111/k1_chg_s0.txt
```

It folds the log into V(r,e) per pin in float64, scores ties by distance, and attributes a
miss to `auth` only when the document is not a node of the materialized graph (the run does
no merge). `results/lease_online_fair30k_k1_20261111/MANIFEST.md` records the three seeds,
the timing, and why the earlier float32 script reported 140/142/146 instead of 130/129/129.

`cpp/source_snapshots/README.md` maps each reported row to the exact source file (sha256)
that produced it and lines Algorithm 1 up against the kernel's reader. The one-reader 10k
row (0.266 ms) ships with its command in `results/review_fix_20261109/lease_1r_official10k/`.

The kernel uses AVX-512 for the exact scan; `-march=native` on a non-AVX-512 host
builds the scalar fallback. A budget guard answers exactly when `phi > 1-1/rho` or
`rho*k > MMAX` (default 5000), rather than capping the graph search.

## Data

The 1M SIFT base is not bundled (512 MB). Download the ann-benchmarks file
`sift-128-euclidean.hdf5`, or use any `1,000,000 x 128` float32 base with the same
row order; `BASE` below points to it.

Bundled inputs:

- `results/ab_stream_official10k_20261109/queries10k_real.f32`
  (10,000 x 128 float32, md5 `2b294e6cdaafcaf9a1e6dd35ba43fb31`)
- `cpp/acl/erbac200k/roles.bin` (100 roles, 200k documents, md5
  `dbcf99d67fff42f9caf26d58994c5b21`)
- `cpp/acl/erbac200k/qroles_10000.bin` (one role per query, md5
  `de4038ffd576df0f1f9687e3edd72469`)
- `results/ab_stream_official10k_20261109/changes100.txt` (100 changes, md5
  `7bd83ae974948840fa834285a099f4a4`)

## Run the official 10k table

```
BASE=/path/to/sift1m/base.f32 scripts/reproduce_official10k.sh
```

The script runs the lease on the post-change snapshot and writes to
`results/official10k_repro/`. The archived rows are in
`results/ab_stream_official10k_20261109/` (lease, ACORN ef=32/256, Curator
se=800/1600/2400/3200; `cmd.txt` records the exact environment).

## Baselines

`cpp/acorn_acl.cpp` and `cpp/curator_acl.cpp` are the drivers used for the
baseline rows. They require the ACORN and Curator (v2) libraries, which are not
bundled. The archived `cmd.txt` files give the arguments.

## Tests

```
python3 mechanism/script_regression.py --bin ./lease_certified
python3 mechanism/e0_contract_tests.py
python3 mechanism/e0_certified_tests.py
python3 mechanism/paired_bootstrap.py --lease <lat.txt> --base name=<lat.txt> --out bootstrap.json
```

The regression script generates its own random data and runs the independent
oracle; it expects `--bin` and a writable temporary directory.

## Results

`results/` is organized by experiment; `MANIFEST.md` maps each paper table to
the directory and gives the measured ratios. Absolute latencies are per-host;
compare ratios, not absolute milliseconds, across the three hosts.

## Hosts

Three AVX-512 machines were used: Xeon Platinum 8352V, Ryzen 9 9950X, and Xeon
Gold 6330. Host names are normalized in `cmd.txt` and manifests to
`xeon-host`, `ryzen-host`, and `gold-host`.

## License

MIT for the code in this repository; `cpp/third_party/hnswlib` retains its MIT
license.

## Data sources

Raw corpora are not in this repository (size). Every one is a public download;
the table gives the exact source and how our inputs were derived from it, so the
bundled `results/` rows can be rebuilt from scratch.

| Corpus | Public source | How our input was built |
|---|---|---|
| SIFT1M (1M x 128) | TEXMEX/INRIA, `sift.tar.gz` — <http://corpus-texmex.irisa.fr/> | `sift_base.fvecs` converted to `base.f32`; md5 of our base `0caeb02f1cc236dbc1c97391dae37013` |
| GIST1M (1M x 960) | TEXMEX/INRIA, `gist.tar.gz` — <http://corpus-texmex.irisa.fr/> | used unchanged for the full-coverage row |
| SIFT10M (10M x 128) | Hugging Face `Nanvivi/sift10m-modify` — <https://huggingface.co/datasets/Nanvivi/sift10m-modify> | first 10M rows, uint8 to float32; queries are the first 500 official SIFT queries |
| Wikipedia 768d (1M x 768) | Hugging Face `wikimedia/wikipedia`, config `20231101.en` — <https://huggingface.co/datasets/wikimedia/wikipedia> | paragraphs embedded with `all-mpnet-base-v2` by `real768/gen_real768_1m.py` |
| OpenAlex ACL | OpenAlex snapshot — <https://docs.openalex.org/download-all-data/openalex-snapshot> | author-affiliation graph; the derived ACL is bundled as `cpp/openalex_acl.json` |
| 10M Gaussian | none, generated | `mechanism/gen_synth_vectors.py`, seeded and byte-identical across runs |

Summary/log records for these rows are under `results/oa_acl_20261107/`, `results/oa_replay_20261107/`,
`results/sift10m_*`, `results/real10m_768_20261107/`, `results/real768_1m_20261107/`,
`results/p4_20261107/`, `results/certified_20261108/`, `results/sweep10m_20261106/`.
