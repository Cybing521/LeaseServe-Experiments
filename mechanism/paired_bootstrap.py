#!/usr/bin/env python3
"""Paired per-query bootstrap and Wilcoxon test for the official 10k table.

Reads the per-query latency dumps produced with DUMP_LAT=1 (``lat.txt``: one
``index latency_ms`` line per query, in query order) and writes
``bootstrap.json`` next to them.  Each baseline is paired with the lease by
query index, so the reported difference is the median of the per-query
differences, not a difference of medians.

Usage:
  python3 paired_bootstrap.py --lease paired/lease_paired/lat.txt \
      --base acorn_ef32=paired/acorn_ef32_paired/lat.txt \
      --base curator_se800=paired/curator_se800_paired/lat.txt \
      --out paired/bootstrap.json

Resamples: 100000, seed 20261107 (numpy default_rng), 2.5/97.5 percentiles.
Wilcoxon: scipy.stats.wilcoxon, two-sided, exact when n <= 50 else normal
approximation; reported as 0.0 when every difference has one sign.
"""
import argparse
import json
from pathlib import Path

import numpy as np
from scipy import stats


def read_lat(path):
    vals = {}
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) < 2:
                continue
            vals[int(parts[0])] = float(parts[1])
    return vals


def aligned(lease, base):
    idx = sorted(set(lease) & set(base))
    a = np.array([lease[i] for i in idx], dtype=float)
    b = np.array([base[i] for i in idx], dtype=float)
    return a, b


def bootstrap_ci(diff, n_boot, seed):
    rng = np.random.default_rng(seed)
    n = diff.size
    meds = np.empty(n_boot)
    for k in range(n_boot):
        meds[k] = np.median(diff[rng.integers(0, n, n)])
    return float(np.percentile(meds, 2.5)), float(np.percentile(meds, 97.5))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lease", required=True)
    ap.add_argument("--base", action="append", required=True,
                    help="name=path, repeatable")
    ap.add_argument("--out", required=True)
    ap.add_argument("--resamples", type=int, default=100000)
    ap.add_argument("--seed", type=int, default=20261107)
    a = ap.parse_args()

    lease = read_lat(a.lease)
    out = {}
    for spec in a.base:
        name, path = spec.split("=", 1)
        base = read_lat(path)
        la, ba = aligned(lease, base)
        d = la - ba                      # negative: lease faster
        ci = bootstrap_ci(d, a.resamples, a.seed)
        try:
            p = float(stats.wilcoxon(la, ba).pvalue)
        except ValueError:
            p = 0.0
        out[name] = {
            "median_diff_ms": float(np.median(d)),
            "ci95": [ci[0], ci[1]],
            "frac_lease_faster": float(np.mean(d < 0)),
            "frac_lease_slower": float(np.mean(d > 0)),
            "wilcoxon_p": p,
            "lease_p50": float(np.median(la)),
            "base_p50": float(np.median(ba)),
            "n_queries": int(d.size),
            "resamples": a.resamples,
            "seed": a.seed,
        }
    Path(a.out).write_text(json.dumps(out, indent=1, sort_keys=True) + "\n")
    print(json.dumps(out, indent=1, sort_keys=True))


if __name__ == "__main__":
    main()
