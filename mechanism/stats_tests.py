#!/usr/bin/env python3
"""P4 statistics: paired t-tests + bootstrap CIs over seed-level results.

Sources (seed-level, all mimo quiet-host AVX-512):
  A. p4_1m_20261106/mvcc1m_s{0..4}.log + epoch1m_s{0..4}.log   (1M ERBAC, 0.5ms)
  B. churn_matrix_20261107/{design}_us{500,10000}_s{0..4}/summary.txt
  C. cpp128_mimo_20261106/cpp128_s{0..4}.log                   (10M x 128d)
  D. real768_1m_20261107/real768_1m_s{0..4}.log                (1M x 768d real)
  E. real10m_768_20261107/real10m_768_s{0..2}.log              (10M x 768d real)

Each file ends with a whitespace-separated key=value summary line; the last
such line in the file is the incremental-phase result.

Usage:
  python3 stats_tests.py --results-root <dir with the families above> [--out report.md]

Defaults to the local harvest layout:
  experiments/results/p4_20261107/{p4_1m,churn_matrix,cpp128,real768_1m,real10m_768}
"""

import argparse
import glob
import os
import re
import sys

import numpy as np
from scipy import stats


def parse_last_kv(path):
    """Return dict of the last key=value line in a log/summary file."""
    best = None
    with open(path, errors="replace") as fh:
        for line in fh:
            if "=" in line and " " in line:
                toks = line.split()
                if all("=" in t for t in toks) and len(toks) >= 4:
                    best = line
    if best is None:
        raise ValueError(f"no key=value summary line in {path}")
    out = {}
    for tok in best.split():
        k, _, v = tok.partition("=")
        out[k] = v
    return out


def num(d, key, cast=float):
    return cast(d[key])


def load_family(root, subdir, pattern, seed_re=r"_s(\d+)"):
    """Load seed-level metric dicts: {seed: dict}."""
    fam = {}
    for path in sorted(glob.glob(os.path.join(root, subdir, pattern))):
        if path.endswith("summary.txt"):
            # matrix dirs: summary.txt inside per-run dir; seed is in dir name
            seed = int(re.search(r"_s(\d+)", os.path.basename(os.path.dirname(path))).group(1))
        else:
            m = re.search(seed_re, os.path.basename(path))
            if not m:
                continue
            seed = int(m.group(1))
        fam[seed] = parse_last_kv(path)
    return fam


def ms(d, key):
    return np.array([num(d[s], key) for s in sorted(d)], dtype=float)


def mean_sd(x):
    return float(np.mean(x)), float(np.std(x, ddof=1))


def fmt(x):
    return f"{x:.3f}" if abs(x) < 10 else f"{x:.1f}"


def paired_block(name_a, name_b, a, b, label, boot=100_000, rng=None):
    """Paired t-test + bootstrap CI of the mean difference a-b. Returns text."""
    assert len(a) == len(b) and len(a) >= 3
    d = a - b
    t, p = stats.ttest_rel(a, b)
    rng = rng or np.random.default_rng(20261107)
    n = len(d)
    idx = rng.integers(0, n, size=(boot, n))
    boots = d[idx].mean(axis=1)
    lo, hi = np.percentile(boots, [2.5, 97.5])
    ma, sa = mean_sd(a)
    mb, sb = mean_sd(b)
    lines = [
        f"### {label}",
        "",
        f"| arm | mean ± sd (n={n}) |",
        "|---|---|",
        f"| {name_a} | {fmt(ma)} ± {fmt(sa)} |",
        f"| {name_b} | {fmt(mb)} ± {fmt(sb)} |",
        "",
        f"- paired t-test: t={t:.2f}, p={p:.3f} (df={n - 1})",
        f"- mean difference ({name_a} − {name_b}): {fmt(float(d.mean()))} ms,"
        f" bootstrap 95% CI [{lo:.3f}, {hi:.3f}] ms"
        f" ({'excludes 0' if lo * hi > 0 else 'includes 0'})",
    ]
    return "\n".join(lines)


def family_block(title, fam, keys):
    lines = [f"## {title}", ""]
    seeds = sorted(fam)
    lines.append(f"seeds: {seeds}")
    for key in keys:
        if not all(key in fam[s] for s in seeds):
            continue
        x = ms(fam, key)
        m, s = mean_sd(x)
        rng = np.random.default_rng(20261107)
        idx = rng.integers(0, len(x), size=(100_000, len(x)))
        boots = x[idx].mean(axis=1)
        lo, hi = np.percentile(boots, [2.5, 97.5])
        lines.append(
            f"- {key}: {fmt(m)} ± {fmt(s)} (bootstrap 95% CI [{lo:.3f}, {hi:.3f}]),"
            f" per-seed {[f'{v:.3f}' for v in x]}"
        )
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results-root", default=os.path.join(os.path.dirname(__file__), "..", "results", "p4_20261107"))
    ap.add_argument("--out", default=None)
    args = ap.parse_args()
    root = args.results_root
    parts = []

    # A. 1M ERBAC 0.5ms: mvcc vs epoch, 5 seeds (paper §V-G)
    mvcc = load_family(root, "p4_1m", "mvcc1m_s*.log")
    epoch = load_family(root, "p4_1m", "epoch1m_s*.log")
    if mvcc and epoch:
        parts.append("## A. 1M ERBAC, 1 change/0.5ms, 5 seeds (paper §V-G)")
        seeds = sorted(set(mvcc) & set(epoch))
        mvcc = {s: mvcc[s] for s in seeds}
        epoch = {s: epoch[s] for s in seeds}
        common_keys = [k for k in ["incr_p50", "incr_p95", "incr_p99"]
                       if all(k in mvcc[s] and k in epoch[s] for s in seeds)]
        labels = {"incr_p50": "p50", "incr_p95": "p95", "incr_p99": "p99"}
        for key in common_keys:
            parts.append(paired_block("delta_mvcc", "epoch-checked", ms(mvcc, key), ms(epoch, key), f"{labels[key]} (ms)"))
        parts.append("")

    # B. churn matrix 3x2, 5 seeds
    designs = ["mvcc", "epoch", "rsu"]
    mats = {}
    for design in designs:
        for us in [500, 10000]:
            fam = load_family(root, "churn_matrix", f"{design}_us{us}_s*/summary.txt")
            if fam:
                mats[(design, us)] = fam
    if mats:
        parts.append("## B. 1M churn matrix (design x interval), 5 seeds")
        for (design, us), fam in sorted(mats.items()):
            parts.append(family_block(f"{design} @ {us} µs", fam, ["incr_p50", "incr_p95", "incr_p99", "inc_missing", "retries", "fallbacks", "inc_leaks", "pin_violations"]))
        # paired contrasts
        for us in [500, 10000]:
            for other in ["epoch", "rsu"]:
                if (design_key := ("mvcc", us)) in mats and (other, us) in mats:
                    a, b = mats[("mvcc", us)], mats[(other, us)]
                    common = sorted(set(a) & set(b))
                    if len(common) >= 3:
                        aa = {s: a[s] for s in common}
                        bb = {s: b[s] for s in common}
                        parts.append(f"#### mvcc vs {other} @ {us} µs (paired, seeds {common})")
                        for key in ["incr_p50", "incr_p95", "incr_p99"]:
                            if not all(key in aa[s] and key in bb[s] for s in common):
                                continue
                            parts.append(paired_block("mvcc", other, ms(aa, key), ms(bb, key), f"{key} (ms)"))
        parts.append("")

    # C. 10M x 128d C++, 5 seeds
    cpp10m = load_family(root, "cpp128", "cpp128_s*.log")
    if cpp10m:
        parts.append("## C. 10M x 128d C++ mvcc (quiet host), 5 seeds")
        parts.append(family_block("cpp128", cpp10m, ["build_s", "incr_p50", "incr_p95", "incr_p99", "inc_missing", "inc_leaks", "retries", "fallbacks", "pin_violations"]))

    # D. 1M x 768d real, 5 seeds
    r768 = load_family(root, "real768_1m", "real768_1m_s*.log")
    if r768:
        parts.append("## D. 1M x 768d real corpus C++ mvcc, 5 seeds")
        parts.append(family_block("real768_1m", r768, ["build_s", "incr_p50", "incr_p95", "incr_p99", "inc_missing", "inc_leaks", "retries", "fallbacks", "pin_violations"]))

    # E. 10M x 768d real, 3 seeds
    r10m = load_family(root, "real10m_768", "real10m_768_s*.log")
    if r10m:
        parts.append("## E. 10M x 768d real corpus C++ mvcc, 3 seeds")
        parts.append(family_block("real10m_768", r10m, ["build_s", "incr_p50", "incr_p95", "incr_p99", "inc_missing", "inc_leaks", "retries", "fallbacks", "pin_violations"]))

    report = "\n".join(parts)
    if args.out:
        with open(args.out, "w") as fh:
            fh.write(report)
        print(f"wrote {args.out}")
    print(report)


if __name__ == "__main__":
    sys.exit(main())
